//
// Realms of the Haunting DBASE100 command interpreter. Engine-free; see
// roth_dbase100_interp.h for what is ported and from where.
//
// Citations are to ROTH.C, roth_c/src/engine/dbase100.c, unless another
// file is named. ROTH.C's goto labels are quoted (L_1d47b and so on) where
// a rule is easier to check against them.
//

#include "roth_dbase100_interp.h"

#include <string.h>

namespace roth
{

namespace
{

// The loop exits of execute_dbase100_chain that are not a return.
//   L_1d47b "skip or continue"  -> NEXT
//   L_1d672 "count and continue" -> COUNT  (acc++, then L_1d47b)
enum Step { NEXT, COUNT };

const uint32_t REQUEUED = 0xffffffffu;   // dbase100.c:952

} // namespace

//==========================================================================
//
// Names
//
//==========================================================================

const char *Dbase100OpcodeName(uint8_t opcode)
{
	// The dispatch switch, dbase100.c:643-652.
	switch (opcode)
	{
	case 0x01: return "if-flag";
	case 0x02: return "if-count>";
	case 0x03: return "if-count==";
	case 0x04: return "set-flag";
	case 0x05: return "say";
	case 0x07: return "cutscene";
	case 0x08: return "choice";
	case 0x0a: return "end";
	case 0x0b: return "random";
	case 0x0d: return "if-flag-next";
	case 0x0e: return "fullscreen";
	case 0x10: return "count";
	case 0x11: return "give";
	case 0x19: return "sfx";
	case 0x1a: return "music";
	case 0x1b: return "pending-topic";
	case 0x1c: return "caller-gate";
	case 0x1d: return "jump";
	case 0x23: return "raw-by-index";
	case 0x26: return "health";
	case 0x2d: return "ui-latch";
	case 0x36: return "raw-by-id";
	case 0x37: return "value-adjust";
	default:   return nullptr;
	}
}

bool IsDbase100MarkerOpcode(uint8_t opcode)
{
	return opcode == 0x09 || opcode == 0x0c;   // dbase100.c:517, 95
}

//==========================================================================
//
// Setup
//
//==========================================================================

void Dbase100Interp::Attach(const Dbase100 *db, Dbase100Sink *sink)
{
	mDb = (db != nullptr && db->IsLoaded()) ? db : nullptr;
	mSink = sink;
	mState = Dbase100InterpState();
	mDepth = 0;
	mSteps = 0;
	ClearDiagnostics();
	if (mDb != nullptr)
	{
		// init_game_databases, game_core.c:420-432: the record count is header
		// +0x0C; size = ((count + 0x20) & ~0x1f) >> 3; zero-filled.
		const int bytes = Dbase100::FlagBitmapBytes(mDb->Header().unk0C);
		mState.g_dbase100_record_bitmap.assign((size_t)(bytes > 0 ? bytes : 0), 0);
	}
}

void Dbase100Interp::SetSink(Dbase100Sink *sink)
{
	mSink = sink;
}

void Dbase100Interp::ResetDialogueState()
{
	Dbase100InterpState fresh;
	fresh.g_dbase100_record_bitmap.swap(mState.g_dbase100_record_bitmap);
	fresh.g_value_reduction_factor = mState.g_value_reduction_factor;
	mState = fresh;
}

void Dbase100Interp::ClearDiagnostics()
{
	mUnhandled.clear();
	mAnomalies.clear();
	mOperandZeroSkips = 0;
	mOutOfImageReads = 0;
}

int Dbase100Interp::AnomalyTotal() const
{
	int n = 0;
	for (const auto &kv : mAnomalies) n += kv.second;
	return n;
}

//==========================================================================
//
// Image reads. ROTH.C dereferences a pointer; this is the one place that
// decides what a read outside the loaded file gives.
//
//==========================================================================

uint32_t Dbase100Interp::Word(uint32_t offset)
{
	if (mDb == nullptr || (uint64_t)offset + 4 > (uint64_t)mDb->Size())
	{
		// [unverified] ROTH.C reads whatever follows its heap block. 0 decodes
		// as "operand 0", which the main loop skips.
		mOutOfImageReads++;
		Anomaly("command word read outside the image");
		return 0;
	}
	const uint8_t *p = mDb->Image() + offset;
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t Dbase100Interp::Byte(uint32_t offset)
{
	if (mDb == nullptr || (uint64_t)offset + 1 > (uint64_t)mDb->Size())
	{
		mOutOfImageReads++;
		Anomaly("command byte read outside the image");   // [unverified], as Word()
		return 0;
	}
	return mDb->Image()[offset];
}

//==========================================================================
//
// The flag bitmap. Byte = idx >> 3, mask = g_bit_mask_lut[idx & 7], and that
// table is {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80} (ROTH.C data/obj3_owned.c:
// 480-485), so mask = 1 << (idx & 7).
//
// ROTH.C does not bound the index: a flag past the bitmap reads and writes
// the heap after it. [unverified] Here such an index is counted, reads as
// clear and is never stored. Retail scripts stay inside 448 bits.
//
//==========================================================================

int32_t Dbase100Interp::TestDbase100RecordFlag(uint32_t idx)
{
	std::vector<uint8_t> &bm = mState.g_dbase100_record_bitmap;
	if (bm.empty()) return -1;                              // raw_commands.c:82
	const size_t byte = (size_t)((int32_t)idx >> 3);        // sar, raw_commands.c:83
	if ((int32_t)idx < 0 || byte >= bm.size())
	{
		Anomaly("flag index outside the bitmap");
		return 0;
	}
	return (bm[byte] & (1u << (idx & 7))) ? -1 : 0;         // raw_commands.c:85
}

uint32_t Dbase100Interp::Dbase100BitmapTestSet(uint32_t idx)
{
	std::vector<uint8_t> &bm = mState.g_dbase100_record_bitmap;
	if (bm.empty()) return 0xffffffffu;                     // renderer.c:11531-11532
	const size_t byte = (size_t)((int32_t)idx >> 3);
	if ((int32_t)idx < 0 || byte >= bm.size())
	{
		Anomaly("flag index outside the bitmap");
		return 0;
	}
	const uint8_t mask = (uint8_t)(1u << (idx & 7));
	if (bm[byte] & mask) return 0;                          // renderer.c:11535-11536
	bm[byte] |= mask;
	return 0xffffffffu;                                     // renderer.c:11537-11538
}

uint32_t Dbase100Interp::Dbase100BitmapTestClear(uint32_t idx)
{
	std::vector<uint8_t> &bm = mState.g_dbase100_record_bitmap;
	if (bm.empty()) return 0xffffffffu;                     // renderer.c:11546-11547
	const size_t byte = (size_t)((int32_t)idx >> 3);
	if ((int32_t)idx < 0 || byte >= bm.size())
	{
		Anomaly("flag index outside the bitmap");
		return 0;
	}
	const uint8_t mask = (uint8_t)(1u << (idx & 7));
	if ((bm[byte] & mask) == 0) return 0;                   // renderer.c:11550-11551
	bm[byte] &= (uint8_t)~mask;
	return 0xffffffffu;                                     // renderer.c:11552-11553
}

//==========================================================================
//
// filter_dbase100_active_records, dbase100.c:41-100.
//
// Cuts the records after an op 0x0b into segments, each ending at a KEEP
// opcode. out[i] = {first record, record count}; out[activeCount] is the
// segment still open when the scan stopped.
//
// Two things to know, both as ROTH.C has them:
//   - a failed op 0x0d sets cond_flag and NOTHING clears it again
//     (dbase100.c:68-72: the only reset sits inside `if (cond_flag == 0)`),
//     so every KEEP record after the first failed gate is dropped;
//   - the operand is not looked at: an operand-0 word still counts.
//
//==========================================================================

int32_t Dbase100Interp::FilterDbase100ActiveRecords(uint32_t &cursor, int32_t count,
	std::vector<Segment> &out, int32_t &activeCount)
{
	out.assign((size_t)FILTER_SEGMENTS, Segment());
	size_t seg = 0;                         // ROTH.C's moving output pointer
	int32_t active = 0;
	bool condFailed = false;

	out[0].count = 0;                       // dbase100.c:52-53
	out[0].chain = cursor;

	for (;;)
	{
		const uint32_t word = Word(cursor);
		const uint32_t operand = word & 0xffffffu;
		const bool ifNot = (word & 0x80000000u) != 0;
		const int opcode = (int)((word >> 24) & 0x7f);
		cursor += 4;                        // dbase100.c:61

		switch (opcode)
		{
		case 0x05: case 0x07: case 0x0e: case 0x11: case 0x1a: case 0x1d:
		case 0x23: case 0x26: case 0x2d: case 0x36: case 0x37:
			// KEEP, dbase100.c:65-76: close the open segment with this record
			// in it and open the next one after it. With the gate failed the
			// open segment is thrown away and restarted instead.
			if (!condFailed)
			{
				if (seg + 1 < (size_t)FILTER_SEGMENTS)
				{
					seg++;
					active++;
					out[seg - 1].count += 1;
				}
				else
				{
					// [unverified] ROTH.C's 0x400-byte stack array has room for
					// 128 entries and no bound; past it the frame is overwritten.
					Anomaly("random list has more segments than ROTH.C's buffer holds");
				}
			}
			out[seg].count = 0;
			out[seg].chain = cursor;
			break;

		case 0x0d:
		{
			// IF, dbase100.c:78-85: the gate fails when the flag does not
			// match the polarity. The record is counted into the segment.
			const int32_t r = TestDbase100RecordFlag(operand);
			if (ifNot) { if (r != 0) condFailed = true; }
			else       { if (r == 0) condFailed = true; }
			out[seg].count += 1;
			break;
		}

		default:
			out[seg].count += 1;            // SKIP, dbase100.c:87-90
			break;
		}

		count--;                            // dbase100.c:94-98
		if (opcode == 0x0c || count <= 0)
		{
			activeCount = active;
			return count;
		}
	}
}

//==========================================================================
//
// finalize_dbase100_chain, dbase100.c:305-320.
//
//==========================================================================

uint32_t Dbase100Interp::FinalizeDbase100Chain(uint32_t arg)
{
	if (mState.g_dbase100_pending_topic != 0)
	{
		const uint32_t r = Sink().LoadDbase300Chunk(mState.g_dbase100_pending_topic);   // :308
		if (r == 0)
		{
			if (Sink().AudioSeqState() == 2)                                    // :310
			{
				Sink().SetAudioSeqState(0);                                     // :311
				Sink().FinalizeAudioSequenceRef();                              // :312
				Sink().EmitMusicSequenceEvent(Sink().MusicMasterVolume());      // :313
			}
			Sink().ProcessAudioSequenceChunk();                                 // :315
		}
		mState.g_dbase100_pending_topic = 0;                                    // :317
	}
	return arg;
}

//==========================================================================
//
// finish_dialogue_record_eval, dbase100.c:291-296.
//
//==========================================================================

void Dbase100Interp::FinishDialogueRecordEval()
{
	if (mState.g_dialogue_action_queue_count == 0 && mState.g_dbase100_choice_cursor == 0
		&& Sink().CutsceneOverlayActive() != 0)
	{
		Sink().ExitCutsceneOverlayMode();       // :294
	}
	mState.g_dbase100_queue_active = 0;         // :295
}

//==========================================================================
//
// The entries.
//
//==========================================================================

// eval_dialogue_record_by_id, dbase100.c:545-555.
//
// The table pointer ROTH.C keeps is biased by -4 (game_core.c:416-418), so
// entry(id) = u32 at actionTable - 4 + id * 4: ids are 1-based, and the
// bound is `id > count` -> none (signed, :549), so id == count is valid.
// ROTH.C has no lower bound: id 0 reads the dword BEFORE the table (in
// retail, the last inventory table entry) and runs whatever that offset
// holds as a record. That is reproduced, and counted.
uint32_t Dbase100Interp::EvalDialogueRecordById(uint32_t id)
{
	if (mDb == nullptr) return 0;                                           // :547
	const DbHeader &h = mDb->Header();
	if ((int32_t)id > (int32_t)h.actionCount) return 0;                     // :549
	if ((int32_t)id < 1)
		Anomaly("eval_dialogue_record_by_id called with an id below 1");
	const int64_t slot = (int64_t)h.actionTable - 4 + (int64_t)(int32_t)id * 4;
	uint32_t entry = 0;
	if (slot >= 0 && slot <= 0xffffffffll) entry = Word((uint32_t)slot);
	else Word(0xffffffffu);                                                 // counted as out of image
	if (entry == 0) return 0;                                               // :552
	return EvalOrQueueDialogueRecordCommands(entry, 0);                     // :553-554
}

// eval_or_queue_dialogue_record_commands, dbase100.c:427-447.
uint32_t Dbase100Interp::EvalOrQueueDialogueRecordCommands(uint32_t record, uint32_t flag)
{
	if (mState.g_dbase100_queue_active != 0 && mState.g_dialogue_action_queue_count != 0)
		RunDialogueActionQueue();                                           // :431-432

	// The header's low 16 bits are the byte length; count = len / 4 - 1.
	// A length of 0 gives -1 (:433-436).
	int32_t count = (int32_t)(Word(record) & 0xffffu);
	count >>= 2;
	const uint32_t chain = record + 4;
	count -= 1;

	if (mState.g_dialogue_action_queue_count == 0)
		return ExecuteDbase100Chain(chain, count, flag);                    // :437-438
	if ((uint32_t)mState.g_dialogue_action_queue_count > 7) return 0;       // :439, full: dropped
	Dbase100QueueEntry &q = mState.g_dialogue_action_queue[mState.g_dialogue_action_queue_count];
	q.chain = chain;                                                        // :442-444
	q.count = count;
	q.flags = flag;
	mState.g_dialogue_action_queue_count++;                                 // :445
	return 1;
}

// eval_dialogue_record_condition_with_cleanup, dbase100.c:569-576.
uint32_t Dbase100Interp::EvalDialogueRecordConditionWithCleanup(uint32_t record, uint32_t flag)
{
	const uint32_t r = EvalOrQueueDialogueRecordCommands(record, flag);
	FinishDialogueRecordEval();
	return r;
}

//==========================================================================
//
// The deferred-action queue.
//
//==========================================================================

// The block run_dialogue_action_queue repeats before the loop and after
// each entry (dbase100.c:455-459, 472-476).
void Dbase100Interp::ForceEndWhileBusy()
{
	if (mState.g_dialogue_busy_flag == 0) return;
	Sink().DialogueVoiceForceEnd(0, 0);
	mState.g_dbase100_queue_active = 0;
	if (mState.g_voice_stream_state != 1)
	{
		mState.g_voice_stream_state = 0;
		mState.g_dialogue_busy_flag = 0;
		mState.g_move_freeze_gate = 0;
		mState.g_active_dialogue_context = 0;
	}
}

// run_dialogue_action_queue, dbase100.c:452-480: drain everything.
void Dbase100Interp::RunDialogueActionQueue()
{
	int32_t &draining = mState.g_dbase100_choice_record_indices[Dbase100InterpState::QUEUE_DRAINING];
	draining = 1;                                                           // :454
	ForceEndWhileBusy();                                                    // :455-459
	int passes = 0;
	while (mState.g_dialogue_action_queue_count != 0)                       // :460, 477
	{
		if (++passes > MAX_QUEUE_PASSES)
		{
			// [unverified] ROTH.C loops for as long as the head entry keeps
			// re-queueing itself. Not reachable with ROTH.C's own
			// dialogue_voice_force_end, which always leaves state 1.
			Anomaly("run_dialogue_action_queue did not drain");
			break;
		}
		const Dbase100QueueEntry head = mState.g_dialogue_action_queue[0];  // :463-465
		const uint32_t r = ExecuteDbase100Chain(head.chain, head.count, head.flags);
		if (r != REQUEUED)                                                  // :467
		{
			mState.g_dialogue_action_queue_count--;                         // :468
			int32_t n = mState.g_dialogue_action_queue_count;
			if (n > Dbase100InterpState::QUEUE_SLOTS - 1)
			{
				Anomaly("dialogue action queue count beyond its 8 slots");
				n = Dbase100InterpState::QUEUE_SLOTS - 1;
			}
			for (int32_t i = 0; i < n; i++)                                 // :469-470
				mState.g_dialogue_action_queue[i] = mState.g_dialogue_action_queue[i + 1];
		}
		ForceEndWhileBusy();                                                // :472-476
	}
	draining = 0;                                                           // :479
}

// advance_dialogue_action_queue, dbase100.c:484-497: run and pop ONE entry.
void Dbase100Interp::AdvanceDialogueActionQueue()
{
	if (mState.g_dialogue_busy_flag != 0) return;                           // :486
	if (mState.g_dialogue_action_queue_count == 0) return;                  // :487
	const Dbase100QueueEntry head = mState.g_dialogue_action_queue[0];
	const uint32_t r = ExecuteDbase100Chain(head.chain, head.count, head.flags);
	if (r == REQUEUED) return;                                              // :493
	mState.g_dialogue_action_queue_count--;                                 // :494
	if (mState.g_dialogue_action_queue_count == 0)
	{
		FinishDialogueRecordEval();                                         // :495
		return;
	}
	int32_t n = mState.g_dialogue_action_queue_count;
	if (n > Dbase100InterpState::QUEUE_SLOTS - 1 || n < 0)
	{
		Anomaly("dialogue action queue count beyond its 8 slots");
		n = n < 0 ? 0 : Dbase100InterpState::QUEUE_SLOTS - 1;
	}
	for (int32_t i = 0; i < n; i++)                                         // :496
		mState.g_dialogue_action_queue[i] = mState.g_dialogue_action_queue[i + 1];
}

// REQUEUE-A, dbase100.c:946-952: park the CURRENT record (the cursor has
// already moved past it) in queue slot 0, all three fields, and return -1.
uint32_t Dbase100Interp::RequeueA(uint32_t cur, int32_t count, uint32_t flags)
{
	mState.g_dialogue_action_queue[0].chain = cur - 4;                      // :947
	mState.g_dialogue_action_queue[0].count = count + 1;                    // :948
	mState.g_dialogue_action_queue[0].flags = flags;                        // :949
	return RequeueTail();
}

// L_1d4fe, dbase100.c:950-952.
uint32_t Dbase100Interp::RequeueTail()
{
	if (mState.g_dialogue_action_queue_count == 0)
		mState.g_dialogue_action_queue_count++;
	return REQUEUED;
}

//==========================================================================
//
// Choices.
//
//==========================================================================

// execute_dialogue_branch, dbase100.c:502-528. `index` is the VISIBLE choice
// (0-based). Walk on from the choice cursor: each 0x08 adds one to the
// target (a nested menu's lines), each 0x09 takes one off, and the 0x09 met
// at zero is the branch: run from the record after it to the end.
//
// The opcode here is the FULL top byte (:512), so 0x88 / 0x89 are neither.
uint32_t Dbase100Interp::ExecuteDialogueBranch(uint32_t index)
{
	if (mState.g_dbase100_choice_cursor == 0) return index;                 // :504
	uint32_t cur = mState.g_dbase100_choice_cursor;
	int32_t remaining = mState.g_dbase100_choice_remaining;
	const uint32_t mode = mState.g_dbase100_choice_mode;
	uint32_t target = 0;
	if (index < (uint32_t)Dbase100InterpState::CHOICE_INDEX_EXTENT)
		target = (uint32_t)mState.g_dbase100_choice_record_indices[index];  // :508
	else
		Anomaly("dialogue branch index outside the choice table");          // [unverified] ROTH.C reads past it
	mState.g_dbase100_choice_cursor = 0;                                    // :509
	if (remaining <= 0) return target;                                      // :510
	for (;;)
	{
		const uint32_t op = (Word(cur) >> 24) & 0xff;                       // :512
		remaining--;
		cur += 4;
		if (op == 8)
		{
			target++;                                                       // :516
		}
		else if (op == 9)
		{
			if (target == 0)
			{
				ExecuteDbase100Chain(cur, remaining, mode);                 // :519
				FinishDialogueRecordEval();                                 // :520
				return 0;
			}
			target--;                                                       // :523
		}
		if (remaining == 0) break;                                          // :525
	}
	return target;
}

// close_dialogue_and_run_branch, dbase100.c:532-540.
void Dbase100Interp::CloseDialogueAndRunBranch()
{
	mState.g_active_dialogue_context = 0;
	mState.g_move_freeze_gate = 0;
	mState.g_dialogue_busy_flag = 0;
	const uint32_t idx = (uint32_t)mState.g_choice_saved_index;             // :537
	mState.g_choice_selected_index = -1;                                    // :538
	ExecuteDialogueBranch(idx);
}

// The choice-menu builder, H_1d4c3, dbase100.c:665-733. Entered on op 0x08,
// or on an op 0x0d whose NEXT record is an op 0x08 (:656).
//
// Returns true when execute_dbase100_chain must return `ret` at once (none
// of these returns runs finalize); false for L_1d672, "count and go on".
bool Dbase100Interp::BuildChoiceMenu(uint32_t &cur, int32_t &count, uint32_t flags,
	int32_t &sub_idx, uint32_t &ret)
{
	if (flags & 2) { ret = 1; return true; }                                // :666 scan: a choice exists
	int32_t visible = 0;                                                    // edi
	bool stop = false;
	if (mState.g_dialogue_busy_flag != 0)                                   // :669
	{
		ret = RequeueA(cur, count, flags);
		return true;
	}
	int32_t textspace = 0x800;                                              // :670
	int slot = 0;                                                           // :671
	cur -= 4;                                                               // :672 re-read this record
	mState.g_dbase100_queue_active = 0;                                     // :673
	bool ifFailed = false;
	count++;                                                                // :675
	uint8_t text_buf[0xc00];                                                // :622
	uint32_t textw = 0;                                                     // :676
	text_buf[0] = 0;

	if (count != 0)                                                         // :677
	{
		for (;;)
		{
			const uint32_t sw = Word(cur);                                  // :679
			const int subop = (int)((sw >> 24) & 0x7f);
			const bool prevFailed = ifFailed;                               // :681-682
			ifFailed = false;
			if (subop != 8)
			{
				if (subop == 0xd)
				{
					// A gate on the NEXT choice (:684-690). Note the 16-bit
					// flag index here, against 24 bits in the main loop.
					const uint32_t arg = sw & 0xffffu;
					const bool hold = (sw & 0x80000000u)
						? (TestDbase100RecordFlag(arg) == 0)
						: (TestDbase100RecordFlag(arg) != 0);
					if (!hold) ifFailed = true;
					cur += 4;
					count--;                                                // L_1d597
				}
				else
				{
					stop = true;                                            // :692
				}
			}
			else
			{
				if (!prevFailed)                                            // :696
				{
					uint32_t r = Sink().ReadNextDialogueLine(text_buf + textw,
						(uint32_t)textspace, sw & 0xffffffu, 0);            // :698
					if (r > (uint32_t)textspace)
					{
						// ROTH.C's reader clamps to maxlen (:390); a sink that
						// does not is cut here rather than trusted.
						Anomaly("ReadNextDialogueLine returned more than maxlen");
						r = (uint32_t)textspace;
					}
					if (r != 0)                                             // :699
					{
						textw += r;
						textspace -= (int32_t)r;                            // :702
						visible++;
						text_buf[textw - 1] = 0x0a;                         // :704 '\n' over the last byte
						text_buf[textw] = 0;                                // :706
						if (slot < Dbase100InterpState::CHOICE_INDEX_EXTENT)
							mState.g_dbase100_choice_record_indices[slot] = sub_idx;   // :708
						else
							Anomaly("more visible choices than the choice table's extent");   // [unverified]
						slot++;
					}
				}
				cur += 4;                                                   // L_1d5db, :711-712
				sub_idx++;
				count--;                                                    // L_1d597
			}
			if (count <= 0) { ret = 1; return true; }                       // :716
			if (stop) break;                                                // :717
		}
	}

	// L_1d600: the menu is built.
	if (visible == 0) return false;                                         // :719
	mState.g_dbase100_choice_remaining = count;                             // :720-723
	mState.g_dbase100_choice_cursor = cur;
	mState.g_dbase100_choice_count = visible;
	mState.g_dbase100_choice_mode = flags;
	if (visible == 1)
	{
		// Exactly one choice: no menu, its branch runs now (:728-733).
		mState.g_dialogue_busy_flag = 0;
		mState.g_move_freeze_gate = 0;
		mState.g_active_dialogue_context = 0;
		ExecuteDialogueBranch(0);
		ret = (uint32_t)visible;
		return true;
	}
	if (Sink().Dbase100OpenDialogueWindow(text_buf, textw) == 0) return false;   // :725
	mState.g_dialogue_busy_flag = 1;                                        // :726
	ret = 1;
	return true;
}

// The gate ops 0x0e and 0x07 share (dbase100.c:913-915, 928-930): busy, or
// an inspect popup that must close first. Both halves are always evaluated.
bool Dbase100Interp::PanelBlocks()
{
	uint32_t e = (mState.g_dialogue_busy_flag != 0) ? 1u : 0u;
	if (Sink().InventoryPanelOpen() != 0 && Sink().InspectPopupActive() == 1)
	{
		Sink().ArmInventoryPanelExit();
		e++;
	}
	return e != 0;
}

//==========================================================================
//
// execute_dbase100_chain, dbase100.c:607-953.
//
// chain   image offset of the first command word
// count   records to run. The loop tests it AFTER each record, so 0 and
//         negative counts still run one (:625-637).
// flags   bit 1 = scan only: an effectful opcode returns 1 instead of acting
//         bit 0 = what op 0x1c tests
//
// Returns the effect accumulator through finalize_dbase100_chain, EXCEPT:
//   1           scan mode met an effect; or a choice menu was built / the
//               record ended inside the builder (no finalize)
//   -1          re-queued behind a busy line or movie (no finalize)
//   1 or 0      op 0x0a / a failed gate (through finalize)
//
// Opcodes that act even in scan mode, exactly as ROTH.C has them: 0x02,
// 0x03 (they query), 0x19, 0x23, 0x26, 0x2d, 0x36, 0x37.
//
//==========================================================================

uint32_t Dbase100Interp::ExecuteDbase100Chain(uint32_t chain, int32_t count, uint32_t flags)
{
	if (mDb == nullptr)
	{
		Anomaly("interpreter run with no database attached");
		return 0;
	}
	if (mDepth >= MAX_DEPTH)
	{
		// [unverified] ROTH.C recurses until its stack gives out.
		Anomaly("chain nesting deeper than the guard allows");
		return 0;
	}
	struct DepthScope
	{
		int &d;
		explicit DepthScope(int &depth) : d(depth) { d++; }
		~DepthScope() { d--; }
	} scope(mDepth);
	if (mDepth == 1) mSteps = 0;

	const DbHeader &h = mDb->Header();
	uint32_t cur = chain;
	int32_t acc = 0;            // [ebp-4]
	int32_t sub_idx = 0;        // [ebp-0x10]

	for (;;)
	{
		if (++mSteps > MAX_STEPS)
		{
			// [unverified] ROTH.C has no budget: a record that jumps to itself
			// (op 0x1d) hangs the game. Stopped here without finalize.
			Anomaly("chain ran past the step budget");
			return (uint32_t)acc;
		}

		// L_1d44b, dbase100.c:625-633.
		const uint32_t word = Word(cur);
		cur += 4;
		count--;
		const bool ifNot = (word & 0x80000000u) != 0;
		const uint32_t operand = word & 0xffffffu;
		const int opcode = (int)((word >> 24) & 0x7f);
		Step step = NEXT;

		if (operand == 0)
		{
			mOperandZeroSkips++;                                            // :633
		}
		else if ((uint32_t)(opcode - 1) > 0x36u)
		{
			mUnhandled[opcode]++;                                           // :641, outside 1..0x37
		}
		else
		{
			const uint32_t operand16 = operand & 0xffffu;                   // :642
			switch (opcode)
			{
			case 0x0d:
			{
				// :655-662. The NEXT record's opcode byte decides first: a
				// plain 0x08 there hands this gate to the choice builder.
				if (Byte(cur + 3) == 8)
				{
					uint32_t ret = 0;
					if (BuildChoiceMenu(cur, count, flags, sub_idx, ret)) return ret;
					step = COUNT;
					break;
				}
				// Flag matches the polarity: the next record runs. Otherwise
				// it is stepped over.
				const int32_t r = TestDbase100RecordFlag(operand);
				if (!ifNot) { if (r != 0) break; }
				else        { if (r == 0) break; }
				cur += 4;                                                   // :659
				count--;                                                    // L_1d4c0
				break;
			}

			case 0x08:
			{
				uint32_t ret = 0;
				if (BuildChoiceMenu(cur, count, flags, sub_idx, ret)) return ret;
				step = COUNT;
				break;
			}

			case 0x10:
				step = COUNT;                                               // :735-737
				break;

			case 0x0a:
				return FinalizeDbase100Chain(1);                            // :740-742

			case 0x0b:
			{
				// :745-758. Segment what follows, pick one segment, run it.
				// Its result is dropped.
				step = COUNT;
				if (count == 0) break;                                      // :746
				std::vector<Segment> segs;
				int32_t active = 0;
				count = FilterDbase100ActiveRecords(cur, count, segs, active);
				if (active == 0) break;                                     // :752
				const uint32_t idx = (uint32_t)Sink().RngNextIndexForCount(active) & 0xffffu;   // :753
				if (idx >= (uint32_t)active)
				{
					// [unverified] ROTH.C indexes its stack array with whatever
					// came back. Its own RNG returns (count * rng16) >> 16,
					// always below count (renderer.c:11141).
					Anomaly("RngNextIndexForCount returned an index outside the list");
					break;
				}
				ExecuteDbase100Chain(segs[idx].chain, segs[idx].count, flags);   // :756
				break;
			}

			case 0x04:
				// :761-765.
				if (flags & 2) return 1;
				if (!ifNot) Dbase100BitmapTestSet(operand);
				else        Dbase100BitmapTestClear(operand);
				step = COUNT;
				break;

			case 0x11:
				// :768-772. The id is the operand's low 16 bits, signed.
				if (flags & 2) return 1;
				if (!ifNot) Sink().GiveItem((int32_t)(int16_t)operand, 0);
				else        Sink().RemoveItem((int32_t)(int16_t)operand);
				step = COUNT;
				break;

			case 0x1b:
				// :775-779.
				if (flags & 2) break;
				acc++;
				mState.g_dbase100_pending_topic = Sink().CurrentDbase300ChunkId();
				break;

			case 0x37:
				// :782-784. Not gated by scan mode, not counted.
				mState.g_value_reduction_factor += (int32_t)(int16_t)operand;
				break;

			case 0x1a:
				// :787-800.
				if (flags & 2) break;
				if (mState.g_dialogue_busy_flag != 0)
				{
					// REQUEUE-B (:789-792): count, then chain; the slot's flags
					// are left as they were.
					mState.g_dialogue_action_queue[0].count = count + 1;
					mState.g_dialogue_action_queue[0].chain = cur - 4;
					return RequeueTail();
				}
				if (Sink().AudioSeqState() == 2)                            // :794-797
				{
					Sink().FinalizeAudioSequenceRef();
					Sink().SetAudioSeqState(0);
				}
				step = COUNT;
				if (Sink().LoadDbase300Chunk(operand) != 0) break;          // :798
				Sink().ProcessAudioSequenceChunk();                         // :799
				break;

			case 0x01:
			{
				// :803-808. Go on when the flag matches the polarity; else
				// end the chain with 0 (NOT the accumulator).
				const int32_t r = TestDbase100RecordFlag(operand);
				if (!ifNot) { if (r != 0) break; }
				else        { if (r == 0) break; }
				return FinalizeDbase100Chain(0);
			}

			case 0x02:
			{
				// :811-818. Unsigned compare of the count with operand >> 16.
				const uint32_t cnt = Sink().QueryPlayerInventory((int32_t)(int16_t)operand, 0);
				const uint32_t thr = operand >> 16;
				if (!ifNot) { if (cnt > thr) break; }
				else        { if (cnt <= thr) break; }
				return FinalizeDbase100Chain(0);
			}

			case 0x03:
			{
				// :821-832. If: EQUAL ends the chain (with cnt ^ thr, i.e. 0).
				// If-NOT: equal goes on, unequal ends it.
				const uint32_t cnt = Sink().QueryPlayerInventory((int32_t)(int16_t)operand, 0);
				const uint32_t thr = operand >> 16;
				if (!ifNot)
				{
					if (cnt != thr) break;
					return FinalizeDbase100Chain(cnt ^ thr);
				}
				if (cnt == thr) break;
				return FinalizeDbase100Chain(0);
			}

			case 0x23:
				// :835-838. Not gated by scan mode, not counted.
				if (operand16 == 0) break;
				Sink().ResetCommandChainNoSource((uint16_t)operand16);
				break;

			case 0x36:
				// :841-844. As 0x23.
				if (operand16 == 0) break;
				Sink().RunObjectCommandsById((uint16_t)operand16);
				break;

			case 0x19:
				// :847-850. Counted first; not gated by scan mode.
				acc++;
				Sink().PlaySoundEffect(operand - 1, 0xfcfe);
				break;

			case 0x05:
			{
				// :853-864.
				if (flags & 2) return 1;
				step = COUNT;
				// While the queue is being drained a line is not spoken (:855).
				if (mState.g_dbase100_choice_record_indices[Dbase100InterpState::QUEUE_DRAINING] != 0) break;
				if (mState.g_active_dialogue_context != 0 || mState.g_dialogue_busy_flag != 0)
				{
					if (mState.g_dbase100_queue_active != 0) RunDialogueActionQueue();   // :856-858
				}
				if (mState.g_dialogue_busy_flag != 0) return RequeueA(cur, count, flags);   // :859
				FinishDialogueRecordEval();                                 // :860
				uint8_t alt_text[0x3fc];                                    // :623
				uint32_t r = Sink().ReadNextDialogueLine(alt_text, 0x3fc, operand, 1);   // :861
				if (r > 0x3fc)
				{
					Anomaly("ReadNextDialogueLine returned more than maxlen");
					r = 0x3fc;
				}
				if (r == 0) break;
				if (Sink().Dbase100OpenDialogueWindowAlt(alt_text, r) == 0) break;   // :862
				mState.g_dialogue_busy_flag = 1;                            // :863
				break;
			}

			case 0x1c:
				// :867-871. Go on iff caller flag bit 0 matches the polarity
				// (If: set); else end with 0. No bitmap involved.
				if (flags & 2) break;
				if (!ifNot) { if ((flags & 1) == 0) break; }
				else        { if ((flags & 1) != 0) break; }
				return FinalizeDbase100Chain(0);

			case 0x1d:
			{
				// :874-885. Jump: carry on in action record `operand`. The
				// accumulator is kept, the sub-record index starts over.
				if (operand > h.actionCount) break;                         // :877
				const uint32_t entry = Word(h.actionTable + (operand - 1) * 4);   // :878, biased table
				if (entry == 0) break;                                      // :879
				cur = entry + 4;                                            // :881
				count = (int32_t)((Word(entry) & 0xffffu) >> 2);            // :882
				sub_idx = 0;                                                // :883
				count--;                                                    // L_1d4c0
				break;
			}

			case 0x2d:
			{
				// :888-898. Sub-codes 4 and 7 only; not gated by scan mode.
				const uint32_t sc = operand & 0xffu;
				if (sc < 4) break;
				if (sc == 4) { Sink().SetLatch83b20(); break; }
				if (sc == 7)
				{
					if (Sink().InventoryPanelOpen() == 0) break;
					Sink().SetLatch80b2c();
				}
				break;
			}

			case 0x26:
			{
				// :901-907. Not gated by scan mode, not counted.
				const int32_t v = (int32_t)(int16_t)operand;
				if (v <= 0) { Sink().ZeroHealthAndFlagDeath(); break; }
				Sink().AdvanceClamp8a0f0((uint32_t)v, Sink().HealthClampMax());
				break;
			}

			case 0x0e:
				// :910-919.
				if (flags & 2) return 1;
				if (PanelBlocks()) return RequeueA(cur, count, flags);
				acc++;
				Sink().ShowFullscreenImage(operand);
				break;

			case 0x07:
			{
				// :922-942.
				if (flags & 2) return 1;
				if (mState.g_active_dialogue_context != 0 || mState.g_dialogue_busy_flag != 0)
				{
					if (mState.g_dbase100_queue_active != 0) RunDialogueActionQueue();   // :924-926
				}
				if (PanelBlocks()) return RequeueA(cur, count, flags);      // :927-931
				step = COUNT;
				const uint32_t idx = operand - 1;                           // :934
				if (idx >= h.cutsceneCount) break;                          // :935
				if (h.field30 != 2) break;                                  // :936
				if (Byte(h.cutsceneTable + idx * (uint32_t)DB_CUTSCENE_STRIDE) == 0) break;   // :937-938, no name
				Sink().PlayRecordGdvCutscene((int)idx);                     // :939
				Sink().SetLatch83b20();                                     // :941
				break;
			}

			default:
				mUnhandled[opcode]++;                                       // :651
				break;
			}
		}

		if (step == COUNT) acc++;                                           // L_1d672, :735-737
		if (count > 0) continue;                                            // L_1d47b, :635
		return FinalizeDbase100Chain((uint32_t)acc);                        // :636-638
	}
}

} // namespace roth
