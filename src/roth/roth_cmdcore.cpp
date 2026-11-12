//
// The RAW map commands that are pure game state. See roth_cmdcore.h.
//
// Every function names the ROTH.C function it follows (raw_commands.c unless
// another file is given) and the lines the rule is on. Nothing here is engine
// code and nothing here guesses: where the original reads memory this port does
// not model, the comment says which bytes and what is done instead.
//
// Record offsets are ROTH.C's: +0x02 modifier, +0x06 flags, +0x07 sub-flags,
// +0x08 key, +0x0a.. operands. Command::Word/Byte address the same bytes.
//

#include "roth_cmdcore.h"

#include <math.h>
#include <string.h>

namespace roth
{

namespace
{

// The two fields of a record that Command also keeps as named copies.
void SetRecWord(Command &rec, int off, uint16_t v)
{
	rec.SetWord(off, v);
	if (off == 0x08) rec.key = v;
	else if (off == 0x0a) rec.aux = v;
}

// The collectors take a 0xc8-word buffer whose first two words are a header,
// so at most 0xc6 matches fit (renderer.c:1602-1625, :11222-11249).
const size_t kHeaderedCap = 0xc8 - 2;
// resolve_command_objects has no header: 0xc8 matches (:317-357).
const size_t kObjectCap = 0xc8;

} // namespace

void CmdCore::Attach(Map *map, CmdHost *host, uint32_t *commandRng, uint16_t *pendingRecord)
{
	mMap = map;
	mHost = host != nullptr ? host : &mNullHost;
	mCommandRng = commandRng != nullptr ? commandRng : &mOwnRng;
	mPending = pendingRecord != nullptr ? pendingRecord : &mOwnPending;
}

void CmdCore::BeginLevel()
{
	interrupt = 0;
	stepMode = 0;
	stepDelta = 0;
	damageScale = 0;
	activeTexmap = activeFace = -1;
	sourceSector = sourceIndex = -1;
	speedRequest = speedShift = 0;
	// clear_list_field30 (renderer.c:10806): the list head is dropped and every
	// slot's busy byte cleared. The serials are kept so a presenter never
	// mistakes a new level's particle for the old one in the same slot.
	for (int i = 0; i < PARTICLES; i++)
	{
		const uint32_t serial = particles[i].serial;
		particles[i] = CmdParticle();
		particles[i].serial = serial;
	}
}

void CmdCore::NewGame()
{
	health = 0x800;                 // game_core.c:828-830, the cap when unset
	damageFlash = 0;
	hudDirty = true;                // game_core.c:838
}

//==========================================================================
//
// The collectors.
//
//==========================================================================

// collect_raw_state_matches (renderer.c:11222): every EXTENDED texture-map
// record whose id word (+0x0c) is the key, in file order.
std::vector<int> CmdCore::TexmapsById(uint16_t key) const
{
	std::vector<int> out;
	if (mMap == nullptr) return out;
	for (size_t i = 0; i < mMap->textureMaps.size(); i++)
	{
		const TextureMap &tm = mMap->textureMaps[i];
		if (!tm.extended || tm.faceID != key) continue;
		out.push_back((int)i);
		if (out.size() >= kHeaderedCap) break;
	}
	return out;
}

// The word at +0x0c of a texture-map record. An extended record keeps its id
// there. A plain record is ten bytes long, so the same read lands on bytes 2-3
// of the record that follows it: that record's mid texture. gather_faces_by_id
// makes exactly that read without testing the extended bit (:101, :108).
uint16_t CmdCore::TexmapWordC(int tmi) const
{
	if (tmi < 0 || (size_t)tmi >= mMap->textureMaps.size()) return 0;
	const TextureMap &tm = mMap->textureMaps[tmi];
	if (tm.extended) return tm.faceID;
	if ((size_t)tmi + 1 < mMap->textureMaps.size()) return mMap->textureMaps[tmi + 1].midTexture;
	return 0;       // past the last record: the section that follows, not modelled
}

// gather_faces_by_id (:92-115). Despite the name it returns texture-map
// records. A key names every record carrying that id. A key of 0 names the
// record the player just used, and the record on the far side of the same wall
// when both carry the same word at +0x0c.
std::vector<int> CmdCore::GatherFaces(uint16_t id) const
{
	if (id != 0) return TexmapsById(id);
	std::vector<int> out;
	if (mMap == nullptr || activeTexmap < 0) return out;          // :97
	out.push_back(activeTexmap);
	if (activeFace >= 0 && (size_t)activeFace < mMap->faces.size())
	{
		const Face &f = mMap->faces[activeFace];
		if (f.sisterFaceOffset != 0xffff && f.sister >= 0)        // :105
		{
			const int other = mMap->faces[f.sister].textureMap;
			if (other >= 0 && TexmapWordC(activeTexmap) == TexmapWordC(other))   // :108
				out.push_back(other);
		}
	}
	return out;
}

// geom_find_matches (renderer.c:1602): every sector whose id (+0x14) is the key.
std::vector<int> CmdCore::SectorsById(uint16_t key) const
{
	std::vector<int> out;
	if (mMap == nullptr) return out;
	for (size_t i = 0; i < mMap->sectors.size(); i++)
	{
		if (mMap->sectors[i].commandID != key) continue;
		out.push_back((int)i);
		if (out.size() >= kHeaderedCap) break;
	}
	return out;
}

// collect_connected_geometry_group (:866-881) over walk_connected_geom
// (:839-859). A depth-first flood from the first sector the key names: across
// every face whose flag byte (+0x0a) has bit 3 clear and which has a sister,
// into the sister's sector. The visited mark is bit 2 of the sector's +0x16,
// cleared on every sector first (:822-831) and left set on the ones reached,
// which is map state and so is written here too.
std::vector<int> CmdCore::ConnectedSectors(uint16_t key)
{
	std::vector<int> out;
	if (mMap == nullptr) return out;
	int start = -1;
	for (size_t i = 0; i < mMap->sectors.size(); i++)             // find_geometry_record :50
		if (mMap->sectors[i].commandID == key) { start = (int)i; break; }
	if (start < 0) return out;
	for (Sector &s : mMap->sectors) s.flags2 &= (uint16_t)~0x0004;

	int budget = (int)kHeaderedCap;
	struct Walker
	{
		Map &m; std::vector<int> &out; int &budget;
		void Visit(int si)
		{
			Sector &s = m.sectors[si];
			s.flags2 |= 0x0004;                                    // :841
			if (budget <= 0) return;                               // :842
			budget--;
			out.push_back(si);
			// `do { } while (--count > 0)` on a signed byte: one face is always
			// looked at, even when the sector says it has none (:847-858).
			int count = (int)(int8_t)s.faceCount;
			int fi = s.firstFaceIndex;
			do
			{
				if (fi >= 0 && (size_t)fi < m.faces.size())
				{
					const Face &f = m.faces[fi];
					if (!(f.collisionFlags & 0x08) && f.sisterFaceOffset != 0xffff && f.sister >= 0)
					{
						const int next = m.faces[f.sister].sector;
						if (next >= 0 && !(m.sectors[next].flags2 & 0x0004)) Visit(next);
					}
				}
				fi++;
			} while (--count > 0);
		}
	} w{ *mMap, out, budget };
	w.Visit(start);
	return out;
}

// resolve_command_objects (:317-357). Key 0 is the one source object; any other
// key is every object carrying it, sectors in order and objects in each
// sector's own order.
std::vector<CmdCore::ObjRef> CmdCore::ResolveObjects(uint16_t id) const
{
	std::vector<ObjRef> out;
	if (mMap == nullptr) return out;
	if (id == 0)
	{
		if (sourceSector >= 0 && (size_t)sourceSector < mMap->objects.size()
			&& sourceIndex >= 0 && (size_t)sourceIndex < mMap->objects[sourceSector].size())
			out.push_back(ObjRef{ sourceSector, sourceIndex });
		return out;
	}
	for (size_t si = 0; si < mMap->objects.size(); si++)
		for (size_t oi = 0; oi < mMap->objects[si].size(); oi++)
		{
			if (mMap->objects[si][oi].commandID != id) continue;
			out.push_back(ObjRef{ (int)si, (int)oi });
			if (out.size() >= kObjectCap) return out;
		}
	return out;
}

// find_secondary_state_record_by_key (:1155-1177): the first object with the id.
bool CmdCore::FindObjectByKey(uint16_t key, ObjRef &out) const
{
	if (mMap == nullptr) return false;
	for (size_t si = 0; si < mMap->objects.size(); si++)
		for (size_t oi = 0; oi < mMap->objects[si].size(); oi++)
			if (mMap->objects[si][oi].commandID == key) { out = ObjRef{ (int)si, (int)oi }; return true; }
	return false;
}

// find_face_record (:64-74): the first face that wears a texture-map record.
int CmdCore::FindFaceByTexmap(int textureMap) const
{
	if (mMap == nullptr || textureMap < 0) return -1;
	for (size_t i = 0; i < mMap->faces.size(); i++)
		if (mMap->faces[i].textureMap == textureMap) return (int)i;
	return -1;
}

// A texture-map record's slot by the byte offset the original adds: +2 mid,
// +4 upper, +6 lower.
uint16_t *CmdCore::TexmapSlot(int tmi, unsigned dirOff)
{
	if (tmi < 0 || (size_t)tmi >= mMap->textureMaps.size()) return nullptr;
	TextureMap &tm = mMap->textureMaps[tmi];
	switch (dirOff)
	{
	case 2:  return &tm.midTexture;
	case 4:  return &tm.upperTexture;
	case 6:  return &tm.lowerTexture;
	default: return nullptr;
	}
}

//==========================================================================
//
// The count family: 0x15, 0x1f, 0x21, 0x22, and 0x1e which drives them.
//
// A count record is a gate with a memory. Each time the chain reaches it the
// counter (word +0x0a) takes one step, and the chain goes on past it only on
// the step that lands on the target (word +0x0c). Until then the handler leaves
// the interrupt at 1, which stops the chain where it stands.
//
//==========================================================================

// rawcmd_flush_pending_command_record (:179-188): the "not there yet" exit. A
// record latched by 0x36 runs now unless an item was just auto-selected, and
// the step mode goes back to plain counting either way.
int CmdCore::FlushPending()
{
	int ret = CMD_NOTHING;
	if (*mPending != 0)
	{
		if (!mHost->ItemAutoSelected())
		{
			const uint16_t pending = *mPending;
			mHost->RunPendingRecord(pending);
			*mPending = 0;
		}
		else ret = CMD_LATCHED;          // the record pointer itself, :181
	}
	stepMode = 0;
	return ret;
}

// The "done" exit at 0x31944 (:204-206): the chain may go on.
int CmdCore::Done()
{
	interrupt = 0;
	stepMode = 0;
	*mPending = 0;
	return CMD_ACTED;
}

// What each member of the family does with the new count, between the step and
// the compare. The value painted is always count - 1 + word[+0x10].
void CmdCore::PaintStep(Command &rec, uint16_t nv, Paint paint)
{
	if (paint == PaintNone) return;
	const uint16_t key = rec.Word(0x0e);
	const uint16_t value = (uint16_t)(nv - 1 + rec.Word(0x10));
	const uint8_t flags = rec.Byte(0x06);

	if (paint == PaintTexmaps)
	{
		// rawcmd_step_count_apply_primary (:686-698): one slot of every
		// texture-map record with the id. Flag 1 the lower, 2 the upper, else mid.
		const unsigned dirOff = (flags & 1) ? 6u : (flags & 2) ? 4u : 2u;
		for (int tmi : TexmapsById(key))
		{
			uint16_t *slot = TexmapSlot(tmi, dirOff);
			if (slot == nullptr) continue;
			*slot = value;
			mHost->TexmapChanged(tmi);
		}
	}
	else if (paint == PaintObjects)
	{
		// rawcmd_step_count_apply_secondary (:780-788): the texture word (+4) of
		// every object with the id.
		for (size_t si = 0, n = 0; si < mMap->objects.size() && n < kObjectCap; si++)
			for (size_t oi = 0; oi < mMap->objects[si].size() && n < kObjectCap; oi++)
			{
				Object &o = mMap->objects[si][oi];
				if (o.commandID != key) continue;
				n++;
				o.textureIndex = (uint8_t)(value & 0xff);
				o.textureSource = (uint8_t)(value >> 8);
				mHost->ObjectChanged((int)si, (int)oi);
			}
	}
	else
	{
		// rawcmd_step_count_apply_faces (:909-932). Sectors, not faces: the
		// flood when the record's own bit 2 is set, else every sector with the
		// id. Flag 1 picks the ceiling texture (+6), otherwise the floor (+8);
		// flags 1 and 2 together go through the sector's mid-platform (+0x18)
		// to its top texture (platform +6) and skip a sector that has none.
		const std::vector<int> group = (rec.modifier & 0x04) ? ConnectedSectors(key) : SectorsById(key);
		const bool indirect = (flags & 3) == 3;
		for (int si : group)
		{
			Sector &s = mMap->sectors[si];
			if (indirect)
			{
				if (s.platformOffset == 0 || s.platformIndex < 0) continue;
				mMap->platforms[s.platformIndex].topTexture = value;
				mHost->PlatformChanged(s.platformIndex);
			}
			else
			{
				if (flags & 1) s.ceilingTexture = value; else s.floorTexture = value;
				mHost->SectorFlatChanged(si);
			}
		}
	}
}

// rawcmd_step_count_command (:194-209) and its three painting twins. The step
// is chosen by the mode 0x1e left behind (:130-150):
//   0  count + 1
//   1  count - delta, and when that goes negative AS A 16-BIT VALUE, + the limit
//   2  count + delta
//   3  count ^ delta
int CmdCore::StepCount(Command &rec, uint16_t seed, Paint paint)
{
	uint16_t nv;
	switch (stepMode)
	{
	case 1:
		nv = (uint16_t)(seed - stepDelta);
		if ((int16_t)nv < 0) nv = (uint16_t)(nv + rec.Word(0x08));
		break;
	case 2:  nv = (uint16_t)(seed + stepDelta); break;
	case 3:  nv = (uint16_t)(seed ^ stepDelta); break;
	default: nv = (uint16_t)(seed + 1); break;
	}
	SetRecWord(rec, 0x0a, nv);
	PaintStep(rec, nv, paint);
	if (nv == rec.Word(0x0c)) return Done();
	return FlushPending();
}

// cmd_count (:216-233) and the identical wrappers at :710, :800, :945.
int CmdCore::CountFamily(Command &rec, Paint paint)
{
	interrupt = 1;                                        // :218
	const uint16_t current = rec.Word(0x0a);
	const uint8_t flags = rec.Byte(0x06);
	uint16_t seed = current;
	if (current >= rec.Word(0x08))                        // at or past the limit, unsigned
	{
		if (!(flags & 0x20)) return FlushPending();       // no wrap bit: it stays stuck
		seed = 0;
	}
	// The test for "already at the target" uses the count as it stood BEFORE
	// the wrap put the seed back to 0 (:226).
	if (current == rec.Word(0x0c) && (flags & 0x08))
	{
		(void)StepCount(rec, seed, paint);                // step once more ...
		return Done();                                    // ... and let the chain through
	}
	return StepCount(rec, seed, paint);
}

int CmdCore::Count(Command &rec)                   { return CountFamily(rec, PaintNone); }
int CmdCore::TextureChangeCount(Command &rec)      { return CountFamily(rec, PaintTexmaps); }
int CmdCore::AnimateFacegroupTexture(Command &rec) { return CountFamily(rec, PaintFaces); }
int CmdCore::CountAddlArg(Command &rec)            { return CountFamily(rec, PaintObjects); }

// cmd_modify_count (:4001-4045). Sets the step (delta from +0x0a, mode from the
// low two flag bits, plus one, so never the plain +1), then runs the count
// record at index +0x08 for one step. If that step reached the target the
// count's own NEXT is queued to run as a chain of its own; either way this
// chain goes on. With an index of 0 the mode and delta are simply left set for
// whichever count record the chain meets next.
int CmdCore::ModifyCount(Command &rec)
{
	stepDelta = rec.Word(0x0a);
	stepMode = (uint32_t)(rec.Byte(0x06) & 3) + 1;
	const uint16_t index = rec.Word(0x08);
	if (index == 0) return CMD_NOTHING;                   // :4006, mode and delta stay
	Command *target = nullptr;
	if (mMap != nullptr && (size_t)(index - 1) < mMap->commands.size())
		target = &mMap->commands[index - 1];
	if (target != nullptr)
	{
		const uint8_t tb = target->opcode;                // the raw byte, unmasked (:4010)
		if (tb == 0x15 || tb == 0x21 || tb == 0x22 || tb == 0x1f)
		{
			int r = 0;
			(void)Dispatch(tb, *target, r);
			if (interrupt != 0) { interrupt = 0; return CMD_ACTED; }      // still counting
			if (mHost->QueueDeferred(target->linkIndex)) { interrupt = 0; return CMD_ACTED; }
			// queue full: fall into the clearing exit
		}
	}
	interrupt = 0;                                        // :4042-4044
	stepMode = 0;
	return CMD_NOTHING;
}

//==========================================================================
//
// The texture oddments.
//
//==========================================================================

// cmd_sync_facegroup_texture (0x14, :1032-1053). Every texture-map record the
// key names takes the reference record's slot value plus one. The reference is
// the first record with id +0x0a, or the first match when +0x0a is 0 or is the
// key itself. No retail map carries this opcode.
int CmdCore::SyncFacegroupTexture(Command &rec)
{
	const uint16_t av = rec.Word(0x0a), key = rec.Word(0x08);
	int ref = -1;
	if (av != 0 && av != key)
	{
		const std::vector<int> r = TexmapsById(av);       // find_raw_state_record
		if (!r.empty()) ref = r[0];
	}
	const std::vector<int> group = GatherFaces(key);
	if (group.empty()) return CMD_NOTHING;
	if (ref < 0) ref = group[0];
	const uint8_t flags = rec.Byte(0x06);
	const unsigned dirOff = (flags & 1) ? 6u : (flags & 2) ? 4u : 2u;
	const uint16_t value = (uint16_t)(*TexmapSlot(ref, dirOff) + 1);
	bool changed = false;
	for (int tmi : group)
	{
		uint16_t *slot = TexmapSlot(tmi, dirOff);
		if (*slot == value) continue;
		*slot = value;
		changed = true;
		mHost->TexmapChanged(tmi);
	}
	if (!changed) return CMD_NOTHING;
	if (flags & 0x10) Disable(rec);
	return CMD_ACTED;
}

// cmd_cycle_texture (0x1c, :973-1027). The sector twin: every sector the key
// names (or floods to, with record bit 2) takes the reference sector's texture
// plus one. Flags below 2 write the sector itself, ceiling for flag 1 and floor
// otherwise; flags 2 and 3 write its mid-platform, underside for flag 1 and top
// otherwise, skipping sectors with no platform and taking the value from the
// first platform met when the reference has none.
int CmdCore::CycleTexture(Command &rec)
{
	const uint16_t av = rec.Word(0x0a), key = rec.Word(0x08);
	int ref = -1;
	if (av != 0 && av != key)
		for (size_t i = 0; i < mMap->sectors.size(); i++)         // find_geometry_record
			if (mMap->sectors[i].commandID == av) { ref = (int)i; break; }
	if (key == 0) return CMD_NOTHING;                              // :983
	const std::vector<int> group = (rec.modifier & 0x04) ? ConnectedSectors(key) : SectorsById(key);
	if (group.empty()) return CMD_NOTHING;
	if (ref < 0) ref = group[0];
	const uint8_t flags = rec.Byte(0x06);
	bool changed = false;
	if ((flags & 3) < 2)
	{
		const bool ceiling = (flags & 1) != 0;
		const Sector &rs = mMap->sectors[ref];
		const uint16_t value = (uint16_t)((ceiling ? rs.ceilingTexture : rs.floorTexture) + 1);
		for (int si : group)
		{
			Sector &s = mMap->sectors[si];
			uint16_t &slot = ceiling ? s.ceilingTexture : s.floorTexture;
			if (slot == value) continue;
			slot = value;
			changed = true;
			mHost->SectorFlatChanged(si);
		}
	}
	else
	{
		const bool underside = (flags & 1) != 0;
		int value = -1;                                            // not taken yet
		const Sector &rs = mMap->sectors[ref];
		if (rs.platformOffset != 0 && rs.platformIndex >= 0)
		{
			const MidPlatform &rp = mMap->platforms[rs.platformIndex];
			value = (int)(underside ? rp.undersideTexture : rp.topTexture) + 1;
		}
		for (int si : group)
		{
			const Sector &s = mMap->sectors[si];
			if (s.platformOffset == 0 || s.platformIndex < 0) continue;
			MidPlatform &p = mMap->platforms[s.platformIndex];
			uint16_t &slot = underside ? p.undersideTexture : p.topTexture;
			if (value < 0) value = (int)slot + 1;
			if (slot == (uint16_t)value) continue;
			slot = (uint16_t)value;
			changed = true;
			mHost->PlatformChanged(s.platformIndex);
		}
	}
	if (!changed) return CMD_NOTHING;
	if (flags & 0x10) Disable(rec);
	return CMD_ACTED;
}

// cmd_cycle_object_texture (0x20, :1184-1206). The object twin: every object
// the key names takes the reference object's texture word plus one.
int CmdCore::CycleObjectTexture(Command &rec)
{
	const uint16_t av = rec.Word(0x0a), key = rec.Word(0x08);
	ObjRef ref{ -1, -1 };
	bool haveRef = false;
	if (av != 0 && av != key) haveRef = FindObjectByKey(av, ref);
	const std::vector<ObjRef> group = ResolveObjects(key);
	if (group.empty()) return CMD_NOTHING;
	if (!haveRef) ref = group[0];
	const Object &ro = mMap->objects[ref.sector][ref.index];
	const uint16_t value = (uint16_t)((uint16_t)(ro.textureIndex | (ro.textureSource << 8)) + 1);
	bool changed = false;
	for (const ObjRef &r : group)
	{
		Object &o = mMap->objects[r.sector][r.index];
		if ((uint16_t)(o.textureIndex | (o.textureSource << 8)) == value) continue;
		o.textureIndex = (uint8_t)(value & 0xff);
		o.textureSource = (uint8_t)(value >> 8);
		changed = true;
		mHost->ObjectChanged(r.sector, r.index);
	}
	if (!changed) return CMD_NOTHING;
	if (rec.Byte(0x06) & 0x10) Disable(rec);
	return CMD_ACTED;
}

// cmd_smash_face_texture (0x2e, :361-389). Realms keeps a wall's whole and
// broken pictures as an even/odd pair of texture numbers, and this flips bit 0
// of one slot of every texture-map record gathered: flags & 3 picks the slot
// (mid, lower, upper, mid). Without flag 0x20 the bit is forced to 1 (broken);
// flag 4 then forces it to 0 (whole); flag 0x20 alone toggles. Nothing gathered,
// or nothing changed, sets the interrupt to 2: the "next step failed" answer
// that sends the chain to the index 0x38 latched.
int CmdCore::SmashFaceTexture(Command &rec)
{
	const std::vector<int> group = GatherFaces(rec.Word(0x08));
	if (group.empty()) { interrupt = 2; return CMD_NOTHING; }
	const uint8_t fl = rec.Byte(0x06);
	static const uint8_t dirOff[4] = { 2, 6, 4, 2 };
	bool changed = false;
	for (int tmi : group)
	{
		uint16_t *slot = TexmapSlot(tmi, dirOff[fl & 3]);
		const uint16_t cur = *slot;
		uint8_t low = (uint8_t)((cur & 0xff) ^ 1);
		if (!(fl & 0x20)) low |= 1;
		if (fl & 0x04) low &= 0xfe;
		const uint16_t now = (uint16_t)((cur & 0xff00) | low);
		if (now == cur) continue;
		*slot = now;
		changed = true;
		mHost->TexmapChanged(tmi);
	}
	if (!changed) { interrupt = 2; return CMD_NOTHING; }
	return CMD_ACTED;       // 0xffff in the original: non-zero with bit 0 set
}

//==========================================================================
//
// The player effects.
//
//==========================================================================

// apply_damage_to_player (renderer.c:9999-10026). Health runs 0..0x800.
//   amount 0      takes ALL the health there is, and ignores the falloff
//   amount > 200  is a big hit: (amount - 199) * 128
//   scaleFlag     multiplies by the frame step, for damage dealt every frame
//   falloff       0..255, takes that many 256ths back off; nothing left, no hit
// Then the pain flash rises by half of (damage + 32) -- unless the flash is
// already past that, or the hit is enormous -- and is capped at 0x164.
void CmdCore::ApplyDamageToPlayer(uint32_t amount, uint8_t scaleFlag, uint32_t falloff)
{
	if (amount == 0)
	{
		amount = (uint32_t)health;
		falloff = 0;
	}
	else
	{
		if (amount > 0xc8) amount = (amount - 0xc7) << 7;
		if (scaleFlag != 0) amount = (uint32_t)((int32_t)amount * frameTimeScale);
	}
	if (falloff != 0)
	{
		const uint32_t cut = (uint32_t)((int32_t)falloff * (int32_t)amount) >> 8;
		amount -= cut;
		if ((int32_t)amount <= 0) return;
	}
	hudDirty = true;
	const uint32_t bumped = amount + 0x20;
	if ((int32_t)bumped < 0x35c6 && (int32_t)bumped >= damageFlash)
	{
		const uint32_t pain = (uint32_t)damageFlash + (bumped >> 1);
		damageFlash = pain >= 0x164 ? 0x164 : (int32_t)pain;
	}
	const int32_t left = (int32_t)((uint32_t)health - amount);
	health = left < 0 ? 0 : left;
	mHost->PlayerDamaged((int32_t)amount);
}

// advance_clamp_8a0f0 (renderer.c:9818-9824): healing, with an unsigned cap.
void CmdCore::AdvanceClampHealth(uint32_t amount, uint32_t max)
{
	hudDirty = true;
	uint32_t v = (uint32_t)health + amount;
	if (v >= max) v = max;
	health = (int32_t)v;
}

// update_frame_time_scale's flash block (game_core.c:337-342): four per frame
// tick, never below zero.
void CmdCore::DecayDamageFlash()
{
	if (damageFlash == 0) return;
	damageFlash -= frameTimeScale << 2;
	if (damageFlash < 0) damageFlash = 0;
}

// point_segment_distance_sq (:123-150). Squared distance from a point to the
// LINE through a wall, after a box test that throws out anything further than
// `radius` from the wall's bounding box on either axis. Returns the distance
// squared PLUS ONE, or 0x7ffff for "not near". All coordinates are 16-bit and
// the differences wrap as 16-bit, as the original's do.
uint32_t CmdCore::PointSegmentDistanceSq(uint16_t radius, int16_t ax, int16_t ay,
	int16_t bx, int16_t by, int16_t px, int16_t py)
{
	const int16_t lo = (int16_t)(uint16_t)(0u - (uint32_t)radius);
	const int16_t hi = (int16_t)radius;
	{
		const int16_t d1 = (int16_t)(bx - px), d2 = (int16_t)(ax - px);
		const int16_t mx = d1 > d2 ? d1 : d2, mn = d1 > d2 ? d2 : d1;
		if (mx <= lo || mn >= hi) return 0x7ffff;
	}
	{
		const int16_t d1 = (int16_t)(by - py), d2 = (int16_t)(ay - py);
		const int16_t mx = d1 > d2 ? d1 : d2, mn = d1 > d2 ? d2 : d1;
		if (mx <= lo || mn >= hi) return 0x7ffff;
	}
	const int32_t segY = (int16_t)(by - ay), segX = (int16_t)(bx - ax);
	const int32_t len2 = segY * segY + segX * segX;
	if (len2 == 0) return 0x7ffff;      // a zero-length wall divides by zero in the original
	const int32_t cross = (int32_t)(int16_t)(px - bx) * segY - (int32_t)(int16_t)(py - by) * segX;
	const int32_t dist2 = (int32_t)(((int64_t)cross * cross) / len2);
	if ((uint32_t)dist2 > (uint32_t)radius * (uint32_t)radius) return 0x7ffff;
	return (uint32_t)(dist2 + 1);
}

// point_to_wall_distance_sq (:4226-4235): the player against a face's two ends.
uint32_t CmdCore::PointToWallDistanceSq(int face, uint16_t radius)
{
	if (mMap == nullptr || face < 0 || (size_t)face >= mMap->faces.size()) return 0x7ffff;
	const Face &f = mMap->faces[face];
	if (f.vertex1 < 0 || f.vertex2 < 0) return 0x7ffff;
	const Vertex &a = mMap->vertices[f.vertex1], &b = mMap->vertices[f.vertex2];
	int16_t px = 0, py = 0;
	mHost->PlayerXY(px, py);
	return PointSegmentDistanceSq(radius, a.x, a.y, b.x, b.y, px, py);
}

// cmd_apply_damage (0x33, :4245-4275). Byte +7 is the damage; word +0x0a a
// radius, 0 for "no distance test". With a radius the hit comes from the object
// that started the chain, or with none from the wall the player used, reaches
// twice the radius and fades to nothing at that edge. The record's one-shot
// bit is applied and -1 returned whether or not the player was in range.
int CmdCore::ApplyDamage(Command &rec)
{
	const uint16_t radius = rec.Word(0x0a);
	uint32_t falloff = 0;
	bool hit = true;
	if (radius != 0)
	{
		const uint32_t reach2 = (uint32_t)(radius * 2) * (uint32_t)(radius * 2);
		uint32_t dist2 = 0;
		if (sourceSector >= 0 && (size_t)sourceSector < mMap->objects.size()
			&& sourceIndex >= 0 && (size_t)sourceIndex < mMap->objects[sourceSector].size())
		{
			const Object &o = mMap->objects[sourceSector][sourceIndex];
			int16_t px = 0, py = 0;
			mHost->PlayerXY(px, py);
			const int16_t dx = (int16_t)((uint16_t)o.x - (uint16_t)px);
			const int16_t dy = (int16_t)((uint16_t)o.y - (uint16_t)py);
			dist2 = (uint32_t)((int32_t)dx * dx + (int32_t)dy * dy);
			if (dist2 >= reach2) hit = false;
		}
		else if (activeFace < 0) hit = false;
		else
		{
			dist2 = PointToWallDistanceSq(activeFace, (uint16_t)(radius * 2 + 0x1c));
			if (dist2 > 0x7ff00u || dist2 >= reach2) hit = false;
		}
		if (hit) falloff = (uint32_t)(((uint64_t)0x100u * dist2) / reach2);
	}
	if (hit) ApplyDamageToPlayer(rec.Byte(0x07), damageScale, falloff);
	if (rec.Byte(0x06) & 0x10) Disable(rec);
	return CMD_ACTED;
}

// cmd_face_emits_damage (0x35, :4282-4300). A wall that hurts: the first wall
// of the group within 2 * radius + 28 of the player deals byte +7 in full, with
// no falloff and no frame scaling. A record that is armed or registered
// (modifier 0x21) does nothing. Walls gathered but none near still answers -1.
int CmdCore::FaceEmitsDamage(Command &rec)
{
	if (rec.modifier & 0x21) return CMD_NOTHING;
	const std::vector<int> group = GatherFaces(rec.Word(0x08));
	if (group.empty()) return CMD_NOTHING;
	const uint16_t radius = (uint16_t)(rec.Word(0x0a) * 2 + 0x1c);
	for (int tmi : group)
	{
		// A record no face wears makes the original measure against whatever is
		// at offset 0 of the map; that is treated as out of range here.
		const int face = FindFaceByTexmap(tmi);
		if (face < 0) continue;
		if (PointToWallDistanceSq(face, radius) <= 0x7ff00u)
		{
			ApplyDamageToPlayer(rec.Byte(0x07), 0, 0);
			if (rec.Byte(0x06) & 0x10) Disable(rec);
			return CMD_ACTED;
		}
	}
	return CMD_ACTED;
}

// cmd_player_rotation (0x3f, :565-584). The angle is -2 * byte +7, in the
// player's own 512-to-the-turn units.
//   flag 1 clear  the player is turned TO that angle
//   flag 1 set    the player is turned BY it, times the frame step
//   flag 2        the amount is first scaled by a random 16-bit fraction, and
//                 the record then disables itself
int CmdCore::PlayerRotation(Command &rec)
{
	uint32_t amount = 0u - (uint32_t)rec.Byte(0x07);
	amount += amount;
	const uint8_t flags = rec.Byte(0x06);
	if (flags & 0x02)
	{
		*mCommandRng = *mCommandRng * 0x5e5u + 0x29u;
		amount = (amount * (*mCommandRng & 0xffffu)) >> 16;
	}
	uint16_t angle = (uint16_t)amount;
	if (flags & 0x01)
		angle = (uint16_t)((uint16_t)(amount * (uint32_t)frameTimeScale) + mHost->PlayerAngle());
	mHost->SetPlayerAngle(angle);
	if (flags & 0x02) Disable(rec);
	return CMD_ACTED;
}

// cmd_set_player_speed_reduction (0x41, :444-450). Byte +7 is a right-shift for
// the player's movement this frame: 1 halves it, 2 quarters it. It is a REQUEST
// that lasts one frame (LatchSpeedReduction), so a slow zone has to keep asking.
int CmdCore::SetPlayerSpeedReduction(Command &rec)
{
	speedRequest = rec.Byte(0x07);
	if (rec.Byte(0x06) & 0x10) Disable(rec);
	return CMD_ACTED;
}

void CmdCore::LatchSpeedReduction()
{
	speedShift = speedRequest;
	speedRequest = 0;
}

//==========================================================================
//
// Particles.
//
//==========================================================================

// scan_sector_edges_at_y (collision_physics.c:91-150). A point is inside a
// sector when its x lies between the smallest and the largest x at which the
// sector's walls cross the point's own y. A wall counts when its first end is
// exactly on that y, or when its two ends are strictly either side of it; the
// crossing is interpolated in 16-bit integers. A sector that claims no walls
// still has one looked at, because the loop tests its count at the bottom.
bool CmdCore::PointInSector(int sector, int16_t qx, int16_t qy) const
{
	if (mMap == nullptr || sector < 0 || (size_t)sector >= mMap->sectors.size()) return false;
	const Sector &s = mMap->sectors[sector];
	// The comparison at the end is on absolute 16-bit coordinates, so it is made
	// in the file's own, as the original makes it: a map the reader recentred
	// (Map::wrapShiftX/Y, RAQUIA2 alone) is put back for the test.
	const int16_t sx = (int16_t)mMap->wrapShiftX, sy = (int16_t)mMap->wrapShiftY;
	qx = (int16_t)(qx - sx); qy = (int16_t)(qy - sy);
	int16_t maxX = (int16_t)0x8000, minX = 0x7fff;
	int count = (int)(int8_t)s.faceCount;
	int fi = s.firstFaceIndex;
	do
	{
		if (fi >= 0 && (size_t)fi < mMap->faces.size())
		{
			const Face &f = mMap->faces[fi];
			if (f.vertex1 >= 0 && f.vertex2 >= 0)
			{
				Vertex a = mMap->vertices[f.vertex1], b = mMap->vertices[f.vertex2];
				a.x = (int16_t)(a.x - sx); a.y = (int16_t)(a.y - sy);
				b.x = (int16_t)(b.x - sx); b.y = (int16_t)(b.y - sy);
				const int16_t da = (int16_t)(a.y - qy), db = (int16_t)(b.y - qy);
				bool crossed = false;
				int16_t cx = 0;
				if (da == 0) { cx = a.x; crossed = true; }
				else if (da > 0 && db < 0)
				{
					const int16_t span = (int16_t)(da - db);
					cx = (int16_t)((int16_t)(((int32_t)(int16_t)(qy - b.y) * (int16_t)(a.x - b.x)) / span) + b.x);
					crossed = true;
				}
				else if (da < 0 && db > 0)
				{
					const int16_t span = (int16_t)(db - da);
					cx = (int16_t)((int16_t)(((int32_t)(int16_t)(qy - a.y) * (int16_t)(b.x - a.x)) / span) + a.x);
					crossed = true;
				}
				if (crossed)
				{
					if (cx >= maxX) maxX = cx;
					if (cx <= minX) minX = cx;
				}
			}
		}
		fi++;
	} while (--count > 0);
	return minX <= qx && qx <= maxX;
}

// locate_sector_at_position (collision_physics.c:1003-1018): the hint sector
// first, then the sectors through its two-sided walls in wall order
// (search_sector_neighbors, :919-944), then every sector in order
// (search_sector_global, :953-981). The full search also wants the height to be
// between the floor and the ceiling -- compared UNSIGNED, as the original
// does -- and when no sector passes that, the LAST one whose outline holds the
// point is the answer.
int CmdCore::LocateSector(int x, int y, int z, int hintSector) const
{
	if (mMap == nullptr) return -1;
	const int16_t qx = (int16_t)x, qy = (int16_t)y;
	if (hintSector >= 0 && (size_t)hintSector < mMap->sectors.size())
	{
		if (PointInSector(hintSector, qx, qy)) return hintSector;
		const Sector &s = mMap->sectors[hintSector];
		int count = (int)(int8_t)s.faceCount;
		int fi = s.firstFaceIndex;
		do
		{
			if (fi >= 0 && (size_t)fi < mMap->faces.size())
			{
				const Face &f = mMap->faces[fi];
				if (f.sisterFaceOffset != 0xffff && f.sister >= 0)
				{
					const int next = mMap->faces[f.sister].sector;
					if (next >= 0 && PointInSector(next, qx, qy)) return next;
				}
			}
			fi++;
		} while (--count > 0);
	}
	int best = -1;
	const uint16_t uz = (uint16_t)z;
	for (size_t i = 0; i < mMap->sectors.size(); i++)
	{
		if (!PointInSector((int)i, qx, qy)) continue;
		best = (int)i;
		const Sector &s = mMap->sectors[i];
		if (!(uz > (uint16_t)s.ceilingHeight) && !(uz < (uint16_t)s.floorHeight)) return (int)i;
	}
	return best;
}

// The table at 0x72080: sin(2*pi*i/512) * 16384. The original's 512 words are
// the truncation of that value plus a little under 0.03 -- three entries of the
// first quarter sit one above the plain truncation -- and the other three
// quarters are exact mirrors. tools/cmdcore_test compares all 512 with the
// table read out of the running original.
int16_t CmdCore::Sine512(unsigned index)
{
	static int16_t table[512];
	static bool built = false;
	if (!built)
	{
		for (int i = 0; i <= 128; i++)
			table[i] = (int16_t)(sin(2.0 * 3.14159265358979323846 * i / 512.0) * 16384.0 + 0.02);
		for (int i = 129; i <= 256; i++) table[i] = table[256 - i];
		for (int i = 257; i < 512; i++) table[i] = (int16_t)-table[i - 256];
		built = true;
	}
	return table[index & 0x1ff];
}

// rng_next (renderer.c:10797): a 16-bit generator of its own, apart from the
// command RNG.
uint16_t CmdCore::ParticleRngNext()
{
	particleRng = (uint16_t)(particleRng * 0x5e5 + 0x29);
	return particleRng;
}

// alloc_particle (:3640-3655): the first slot that is not busy, zeroed.
int CmdCore::AllocParticle()
{
	for (int i = 0; i < PARTICLES; i++)
	{
		if (particles[i].busy) continue;
		const uint32_t serial = particles[i].serial + 1;
		particles[i] = CmdParticle();
		particles[i].serial = serial;
		particles[i].busy = true;
		return i;
	}
	return -1;
}

// spawn_particle (:5827-5859): one particle thrown up from a point. Upward
// speed 0x2e630 plus four times a random word; a random heading whose top
// seven bits are also the sideways speed; 300 frame ticks to live.
int CmdCore::SpawnParticle(int16_t x, uint16_t texture, int16_t y, int16_t z)
{
	const int slot = AllocParticle();
	if (slot < 0) return -1;
	CmdParticle &p = particles[slot];
	p.texture = texture;
	p.x = x; p.y = y; p.z = z;
	p.xacc = (int32_t)((uint32_t)(uint16_t)x << 16);
	p.yacc = (int32_t)((uint32_t)(uint16_t)y << 16);
	p.zacc = (int32_t)((uint32_t)(uint16_t)z << 16);
	p.sector = LocateSector(x, y, z, -1);
	p.flags = 2;
	p.renderType = 0x10;
	const uint32_t r1 = ParticleRngNext();
	p.zvel = (int32_t)((r1 << 2) + 0x2e630u);
	const uint32_t h = ParticleRngNext();
	const int32_t push = (int32_t)(h >> 9);
	p.rotation = (uint8_t)((h + 0x100) >> 1);
	p.life = 0x12c;
	p.xvel = ((int32_t)Sine512(h) * push) >> 3;
	p.yvel = ((int32_t)Sine512(h + 0x80) * push) >> 3;
	return slot;
}

// spawn_particle_on_edge (:6033-6080): one particle at a random point along a
// wall, between a quarter of the way up the room and the ceiling, drifting
// rather than thrown (vertical speed a random word minus 0x8000), lit as its
// sector (light byte 0x80), 140 frame ticks to live. The sector is the wall's
// own, so nothing is located.
int CmdCore::SpawnParticleOnEdge(int face, uint16_t texture)
{
	if (mMap == nullptr || face < 0 || (size_t)face >= mMap->faces.size()) return -1;
	const Face &f = mMap->faces[face];
	if (f.vertex1 < 0 || f.vertex2 < 0 || f.sector < 0) return -1;
	const int slot = AllocParticle();
	if (slot < 0) return -1;
	CmdParticle &p = particles[slot];
	const Vertex &va = mMap->vertices[f.vertex1], &vb = mMap->vertices[f.vertex2];
	const Sector &sec = mMap->sectors[f.sector];

	const uint32_t t = ParticleRngNext();
	const int32_t ix = (int32_t)va.x + ((int32_t)((uint32_t)((int32_t)vb.x - (int32_t)va.x) * t) >> 16);
	const int32_t iy = (int32_t)va.y + ((int32_t)((uint32_t)((int32_t)vb.y - (int32_t)va.y) * t) >> 16);
	// The "z range" the original is handed is the sector record itself: its
	// first word is the ceiling and its second the floor (:4895).
	const int32_t zTop = sec.ceilingHeight, zBottom = sec.floorHeight;
	const int32_t span = zTop - zBottom;
	const uint32_t rz = ParticleRngNext();
	int32_t rise = ((int32_t)((uint32_t)span * 3u)) >> 2;
	rise = (int32_t)((uint32_t)rise * rz);
	const int32_t iz = zBottom + (span >> 2) + (rise >> 16);

	p.light = 0x80;
	p.texture = texture;
	p.x = (int16_t)ix; p.y = (int16_t)iy; p.z = (int16_t)iz;
	p.xacc = (int32_t)((uint32_t)ix << 16);
	p.zacc = (int32_t)((uint32_t)iz << 16);
	p.yacc = (int32_t)((uint32_t)iy << 16);
	p.flags = 2;
	p.renderType = 0x10;
	p.sector = f.sector;
	const uint32_t rv = ParticleRngNext();
	p.zvel = (int32_t)(rv + 0xffff8000u);
	const uint32_t h = ParticleRngNext();
	const int32_t push = (int32_t)(h >> 9);
	p.rotation = (uint8_t)((h + 0x100) >> 1);
	p.life = 0x8c;
	p.xvel = ((int32_t)Sine512(h) * push) >> 3;
	p.yvel = ((int32_t)Sine512(h + 0x80) * push) >> 3;
	return slot;
}

// cmd_particle_effect (0x2d, :4885-4911). Byte +7 particles (at least one) of
// the texture word at +8. When the chain was started from a wall they appear
// along that wall; otherwise at the object that started it, at the object's own
// height. A wall record with no face behind it spawns nothing and answers with
// the record's address; no source at all answers 0.
int CmdCore::ParticleEffect(Command &rec)
{
	const uint16_t texture = rec.Word(0x08);
	int count = rec.Byte(0x07);
	if (activeTexmap >= 0)
	{
		if (activeFace < 0) return CMD_LATCHED;
		do { SpawnParticleOnEdge(activeFace, texture); } while (--count > 0);
		return CMD_ACTED;
	}
	if (mMap == nullptr || sourceSector < 0 || (size_t)sourceSector >= mMap->objects.size()
		|| sourceIndex < 0 || (size_t)sourceIndex >= mMap->objects[sourceSector].size())
		return CMD_NOTHING;
	const Object &o = mMap->objects[sourceSector][sourceIndex];
	do { SpawnParticle(o.x, texture, o.y, o.z); } while (--count > 0);
	return CMD_ACTED;
}

// tick_particles (:5762-5820). A particle's life runs down by the frame step
// and the slot is freed when it reaches zero. A moving one rises by its vertical
// speed, which gravity (0x1900 a tick) pulls down; on reaching its sector's
// floor it stops there with the speed reversed and halved. Outside every sector
// the floor is far below the world. Then it is located again.
void CmdCore::TickParticles()
{
	const int32_t dt = frameTimeScale;
	for (int i = 0; i < PARTICLES; i++)
	{
		CmdParticle &p = particles[i];
		if (!p.busy) continue;
		const int32_t life = (int32_t)p.life - dt;
		if (life <= 0) { p.busy = false; continue; }
		p.life = (uint16_t)life;
		if (p.isStatic == 0)
		{
			int32_t floor = (int32_t)0xf0000000u;
			if (p.sector >= 0 && mMap != nullptr && (size_t)p.sector < mMap->sectors.size())
				floor = (int32_t)((uint32_t)(int32_t)mMap->sectors[p.sector].floorHeight << 16);
			int32_t zacc = (int32_t)((uint32_t)p.zacc + (uint32_t)dt * (uint32_t)p.zvel);
			int32_t zvel = (int32_t)((uint32_t)p.zvel - (uint32_t)dt * 0x1900u);
			if (zacc < floor)
			{
				zvel = (int32_t)(0u - (uint32_t)zvel);
				zacc = floor;
				zvel >>= 1;
			}
			p.xacc = (int32_t)((uint32_t)p.xacc + (uint32_t)p.xvel * (uint32_t)dt);
			p.yacc = (int32_t)((uint32_t)p.yacc + (uint32_t)p.yvel * (uint32_t)dt);
			p.zacc = zacc;
			p.zvel = zvel;
			p.x = (int16_t)(p.xacc >> 16);
			p.z = (int16_t)(p.zacc >> 16);
			p.y = (int16_t)(p.yacc >> 16);
		}
		p.sector = LocateSector(p.x, p.y, p.z, p.sector);
	}
}

//==========================================================================

bool CmdCore::Dispatch(uint8_t opcode, Command &rec, int &result)
{
	switch (opcode)
	{
	case 0x14: result = SyncFacegroupTexture(rec); return true;
	case 0x15: result = Count(rec); return true;
	case 0x1c: result = CycleTexture(rec); return true;
	case 0x1e: result = ModifyCount(rec); return true;
	case 0x1f: result = TextureChangeCount(rec); return true;
	case 0x20: result = CycleObjectTexture(rec); return true;
	case 0x21: result = AnimateFacegroupTexture(rec); return true;
	case 0x22: result = CountAddlArg(rec); return true;
	case 0x2d: result = ParticleEffect(rec); return true;
	case 0x2e: result = SmashFaceTexture(rec); return true;
	case 0x33: result = ApplyDamage(rec); return true;
	case 0x35: result = FaceEmitsDamage(rec); return true;
	case 0x3f: result = PlayerRotation(rec); return true;
	case 0x41: result = SetPlayerSpeedReduction(rec); return true;
	default:   return false;
	}
}

} // namespace roth
