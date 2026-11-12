//
// Realms of the Haunting's dialogue, around the interpreter. See
// roth_dialogue.h for what is here and where each rule comes from.
//

#include "roth_dialogue.h"

#include <string.h>

namespace roth
{

//==========================================================================
//
// The font's advance widths.
//
// measure_font_char_advance (renderer.c:337-348) reads g_font_descriptor
// (0x70f12), which is ROTH.EXE's own initialised data, not one of the
// player's game files, so the 256 widths it yields are transcribed, as the
// runtime's light patterns are. They are the ORIGINAL ENGINE'S ANSWERS, read
// out of the running original by tools/dialogue_test's probe ("adv"), not
// derived by hand: 0 for the control bytes 0x00-0x0d, the default advance 3
// for a character with no glyph, else the glyph's low nibble plus one.
//
// Only the widths are needed: they decide where a line breaks, how many
// pages a spoken line has and when "..." cuts it, which is content. The
// glyph bitmaps are presentation and are not copied.
//
//==========================================================================

static const uint8_t kFontAdvance[256] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3, 3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
	3,2,4,7,6,6,7,2,4,4,7,6,3,4,2,5, 5,4,5,5,6,5,5,6,5,5,2,3,5,5,5,6,
	5,6,6,6,6,5,4,6,6,4,4,6,5,8,6,6, 6,6,6,6,5,6,6,8,6,6,6,3,5,3,6,6,
	3,5,5,5,5,5,4,5,5,2,3,5,2,6,5,5, 5,5,4,5,4,5,6,6,6,5,5,5,3,5,5,6,
	6,5,5,5,5,5,5,5,5,5,5,3,4,3,6,6, 5,8,8,5,5,5,5,5,5,6,6,7,3,7,5,5,
	5,2,5,5,5,6,6,6,6,3,3,3,3,2,3,3, 3,3,3,3,3,6,6,6,3,3,3,3,3,6,6,3,
	3,3,3,3,3,3,5,6,3,3,3,3,3,3,3,5, 6,7,5,5,5,4,4,4,4,3,3,3,3,3,4,3,
	6,6,6,6,5,6,6,6,7,6,6,6,5,6,6,2, 4,3,3,3,3,7,3,2,5,3,3,4,4,4,3,3,
};

int FontCharAdvance(uint8_t ch)
{
	return kFontAdvance[ch];
}

std::vector<std::string> TextLine::Rows() const
{
	std::vector<std::string> rows;
	std::string cur;
	for (size_t i = 0; i < text.size(); )
	{
		const uint8_t c = text[i++];
		if (c == 0) break;
		if (c == 1) { i += 1; continue; }           // 01 <colour>      (text_font.c:330-340)
		if (c == 2) { i += 2; continue; }           // 02 <lo> <hi>     (text_font.c:341-343)
		if (c == 0x0a || c == 0x0d) { rows.push_back(cur); cur.clear(); continue; }
		if (c < 0x0d) continue;                     // other control bytes draw nothing
		cur += (char)c;
	}
	rows.push_back(cur);
	return rows;
}

//==========================================================================
//
// layout_timed_message_text -- dialogue_ui.c:344-480.
//
// A transcription with indices in place of pointers. The original writes
// into a fixed global buffer; `buf` grows instead. Every assignment, in the
// original's order, so the bytes of each record are the original's bytes --
// the differential test compares them.
//
//==========================================================================

int LayoutTimedMessageText(std::vector<TextLine> &out, const uint8_t *src0, int maxw, int maxLines, uint8_t color)
{
	out.clear();
	std::vector<uint8_t> buf;
	auto put = [&buf](size_t at, uint8_t v) { if (buf.size() <= at) buf.resize(at + 1, 0); buf[at] = v; };

	const uint8_t *src = src0;
	const int32_t textw = maxw - 0xc;                   // :349
	int32_t run_width = 0, wrap_cnt = 0, height = 0;
	int32_t min_x = maxw, max_w = 0, line_cnt = 1;
	int32_t brk_width = 0, brk_lw = 0, last_adv = 0, centerx = 0;
	size_t line_start = 0, brk_out = 0;
	const uint8_t *brk_src = src0;
	uint8_t ch = 0;

	auto emit = [&](size_t wr)                          // :423-427, 471-475
	{
		TextLine l;
		l.x0 = min_x;
		l.x1 = max_w + 1;
		l.height = height;
		l.len = (int32_t)(wr - line_start);
		if (buf.size() < wr) buf.resize(wr, 0);
		l.text.assign(buf.begin() + (ptrdiff_t)line_start, buf.begin() + (ptrdiff_t)wr);
		out.push_back(l);
	};

	size_t wr = 0;                                      // :366-370: the first record's header
	put(wr, 1); put(wr + 1, color); wr += 2;
	put(wr, 2);
	size_t xctl = wr;
	put(wr + 1, 0); put(wr + 2, 0); wr += 3;

	for (;;)
	{
		ch = *src; src++;
		if (ch == 0x5e && *src == 0x5e) { ch = 0xa; src++; }                // "^^" is a newline, :374
		if (ch == 0xa)
		{
			if (run_width == 0) { ch = *src; src++; }                       // :376
			else { brk_out = wr; brk_src = src; brk_width = run_width; }    // :377
		}
		const int32_t cur = ch;

		if (cur <= 9)                                                       // :381-388
		{
			if (cur == 1)
			{
				put(wr, ch); wr++;
				put(wr, *src); src++; wr++;
			}
			if (ch == 0) break;
			continue;
		}

		last_adv = FontCharAdvance((uint8_t)cur);                           // :390
		run_width += last_adv;

		if (run_width >= textw || cur == 0xa)                               // :393
		{
			brk_lw = brk_width;
			src = brk_src;
			if (brk_width == 0) return 0;                                   // :397-398: a word too wide
			wr = brk_out + 1;
			if (cur == 0xa)
			{
				wrap_cnt = 0;
				put(brk_out, 0);
			}
			else if (line_cnt < 0xe && maxLines <= wrap_cnt + 1)            // :403-407: cut with "..."
			{
				wrap_cnt = 0;
				put(brk_out, 0x2e); put(brk_out + 1, 0x2e); put(brk_out + 2, 0x2e); put(brk_out + 3, 0);
				wr = brk_out + 4;
				brk_lw = brk_width + 6;
			}
			else
			{
				wrap_cnt++;
				put(brk_out, 0xa);
			}

			centerx = (maxw - brk_lw) >> 1;                                 // :413
			if (min_x > centerx) min_x = centerx;
			put(xctl + 1, (uint8_t)centerx);
			put(xctl + 2, (uint8_t)(centerx >> 8));
			ch = 0x61;
			centerx += brk_lw + 2;
			if (max_w < centerx) max_w = centerx;
			height += 0xb;

			if (wrap_cnt == 0)                                              // :422-434
			{
				emit(wr);
				line_cnt++;
				max_w = 0;
				height = 0;
				line_start = wr;
				put(wr, 1); put(wr + 1, color); wr += 2;
				min_x = textw;
			}
			run_width = 0;                                                  // :436-441
			put(wr, 2);
			brk_width = 0;
			put(wr + 1, 0); put(wr + 2, 0);
			xctl = wr;
			wr += 3;
			continue;
		}

		put(wr, ch); wr++;                                                  // :446
		if (cur == 0x20)                                                    // :449-453
		{
			brk_out = wr - 1;
			brk_width = run_width - last_adv;
			brk_src = src;
		}
		if (ch == 0) break;
	}

	if (run_width != 0)                                                     // :458-465
	{
		height += 0xb;
		centerx = (maxw - run_width) >> 1;
		if (min_x > centerx) min_x = centerx;
		put(xctl + 1, (uint8_t)centerx);
		put(xctl + 2, (uint8_t)(centerx >> 8));
		centerx += run_width + 2;
		if (max_w < centerx) max_w = centerx;
	}
	else if (wrap_cnt == 0)
	{
		line_cnt--;                                                         // :467
	}

	// :471-476. The original always writes this last record; it is only
	// COUNTED when it holds something.
	if ((int)out.size() < line_cnt) emit(wr);
	return line_cnt;
}

//==========================================================================
//
// The timed-message cache (dialogue_ui.c:273-334): 32 most-recently-used
// short texts. It exists to save a file seek, but a hit also skips the
// "voice sample count = 0" store of a real read (dbase100.c:389), so it is
// kept, to stay exact.
//
//==========================================================================

// lookup_cached_timed_message, dialogue_ui.c:281-311.
int32_t DialogueUi::CacheLookup(uint8_t *dest, int32_t key)
{
	if (!mCacheBuilt) { mCache.assign(32, CacheNode()); mCacheBuilt = true; return 0; }   // init_freelist_820c1
	for (size_t i = 0; i < mCache.size(); i++)
	{
		if (mCache[i].key != key) continue;
		const CacheNode n = mCache[i];
		mCache.erase(mCache.begin() + (ptrdiff_t)i);
		mCache.insert(mCache.begin(), n);
		g_timed_message_color = n.color;
		for (int32_t k = 0; k < n.len; k++) dest[k] = n.text[k];
		return n.len;
	}
	return 0;
}

// store_cached_timed_message, dialogue_ui.c:316-334: reuse the oldest node.
void DialogueUi::CacheStore(const uint8_t *textIn, int32_t key, int32_t len, uint8_t color)
{
	if (!mCacheBuilt || mCache.empty()) return;
	CacheNode n;
	n.key = key;
	n.len = (int16_t)len;
	n.color = color;
	for (int32_t k = 0; k < len && k < (int32_t)sizeof(n.text); k++) n.text[k] = textIn[k];
	mCache.pop_back();
	mCache.insert(mCache.begin(), n);
}

//==========================================================================

void DialogueUi::Bind(Dbase100Interp *interp, const Dbase100 *db)
{
	mInterp = interp;
	mDb = db;
}

//==========================================================================
//
// read_next_dialogue_line -- dbase100.c:371-403.
//
// A DBASE400 entry is { u32 voice clip, u16 text length, u8 colour, u8 },
// then the text. flag 1 primes the entry's voice clip; flag 0 goes through
// the cache.
//
//==========================================================================

uint32_t DialogueUi::ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag)
{
	if (dest == nullptr) return 0;
	if (flag == 0)                                                          // :373-378
	{
		const int32_t cached = CacheLookup(dest, (int32_t)voiceOffset);
		if (cached != 0) return (uint32_t)cached;
	}
	if (text.empty()) return 0;                                             // :379, no script handle
	if ((uint64_t)voiceOffset + 8 > text.size()) return 0;                  // past the file: nothing is read
	const uint8_t *hdr = text.data() + voiceOffset;
	uint32_t len = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8);              // :384-385
	const uint8_t color = hdr[6];                                           // :386
	g_timed_message_color = color;
	if (color == 0) g_timed_message_color = 0x20;                           // :388
	g_voice_sample_count = 0;                                               // :389
	if (len > maxlen) len = maxlen;                                         // :390
	const uint64_t avail = text.size() - ((uint64_t)voiceOffset + 8);
	if (len > avail) len = (uint32_t)avail;                                 // a short read at the file's end
	memcpy(dest, hdr + 8, len);                                             // :392
	if (flag != 0)
	{
		const uint32_t clip = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
		if (clip != 0) PrimeVoiceClip(clip);                                // :393-395
	}
	else if (len <= 0x14)
	{
		CacheStore(dest, (int32_t)voiceOffset, (int32_t)len, g_timed_message_color);   // :396-401
	}
	return len;
}

// resolve_dbase100_text -- dbase100.c:408-423.
std::string DialogueUi::ResolveDbase100Text(int index, uint32_t flag)
{
	uint32_t off = 0;
	if (mDb != nullptr && index < mDb->InterfaceCount() && mDb->InterfaceEntry(index, off) && off != 0)
	{
		uint8_t buf[0x64] = {};
		const uint32_t n = ReadNextDialogueLine(buf, 0x62, off, flag);
		size_t len = 0;
		while (len < n && buf[len] != 0) len++;
		return std::string((const char *)buf, len);
	}
	return "Missing Text";                                                  // :420-422
}

std::string DialogueUi::LineText(uint32_t voiceOffset, uint32_t maxlen)
{
	std::vector<uint8_t> buf(maxlen + 2, 0);
	const uint32_t n = ReadNextDialogueLine(buf.data(), maxlen, voiceOffset, 0);
	size_t len = 0;
	while (len < n && buf[len] != 0) len++;
	return std::string((const char *)buf.data(), len);
}

//==========================================================================
//
// prime_voice_clip -- audio.c:1208-1292, as far as the dialogue reads it:
// the clip header's sample count and rate (which time the line), and the
// stream state. Decoding and the sound driver are the sound system's.
//
//==========================================================================

uint32_t DialogueUi::PrimeVoiceClip(uint32_t clip)
{
	if (!speechAvailable) return 0;                                         // :1210
	if (St().g_voice_stream_state == 1) return 0;                           // :1211
	uint8_t hdr[0x2c];
	if (!SpeechHeader(clip, hdr)) return 0;                                 // :1241, no file
	auto u32at = [&hdr](int o) { return (uint32_t)hdr[o] | ((uint32_t)hdr[o + 1] << 8) | ((uint32_t)hdr[o + 2] << 16) | ((uint32_t)hdr[o + 3] << 24); };
	const uint32_t total = u32at(0x28);                                     // :1244
	g_voice_rate = (int32_t)u32at(0x18);                                    // :1245
	int32_t divi = (int16_t)((uint16_t)hdr[0x20] | ((uint16_t)hdr[0x21] << 8));   // :1246
	if (divi == 0) divi = 1;
	g_voice_sample_count = (int32_t)(total / (uint32_t)divi);               // :1248
	if (total == 0) return 0;                                               // :1264-1268: nothing decoded
	St().g_voice_stream_state = 1;                                          // :1289
	// The stand-in clock: the clip's own length in 70 Hz ticks, the unit the
	// original itself converts it to (dialogue_ui.c:1235).
	voiceClockTicks = g_voice_rate > 0 ? (int32_t)(((int64_t)g_voice_sample_count * 0x46) / g_voice_rate) : 0;
	VoiceStarted(clip, (uint32_t)g_voice_sample_count, (uint32_t)g_voice_rate);
	return 1;
}

void DialogueUi::VoiceEnded()
{
	if (St().g_voice_stream_state == 1) St().g_voice_stream_state = 2;      // audio.c:1178
}

void DialogueUi::TickVoiceClock(int ticks)
{
	if (St().g_voice_stream_state != 1) return;
	voiceClockTicks -= ticks;
	if (voiceClockTicks <= 0) { voiceClockTicks = 0; VoiceEnded(); }
}

//==========================================================================
//
// The two windows.
//
//==========================================================================

static std::vector<uint8_t> Terminated(const uint8_t *t, uint32_t bytes)
{
	std::vector<uint8_t> s;
	if (t != nullptr) s.assign(t, t + bytes);
	s.push_back(0);
	s.push_back(0);     // the layout peeks one byte past a '^' and past a 0x0a
	return s;
}

// dbase100_open_dialogue_window -- dialogue_ui.c:1178-1218. The choice menu:
// one record per choice (the builder ends each with '\n', dbase100.c:704),
// each wrapped to at most 10 lines.
uint32_t DialogueUi::Dbase100OpenDialogueWindow(const uint8_t *t, uint32_t bytes)
{
	const std::vector<uint8_t> src = Terminated(t, bytes);
	const int ctx = LayoutTimedMessageText(mSegments, src.data(), screenPitch, 10, g_timed_message_color);   // :1180
	St().g_active_dialogue_context = ctx;
	if (ctx == 0) return 0;                                                 // :1183
	g_dialogue_segment_index = 0;
	g_dialogue_line_dwell = 0;
	St().g_move_freeze_gate = GATE_MENU;                                    // :1187
	g_dialogue_reveal_ramp = 4;
	// :1189-1212 anchor the panel and work out its rectangle: presentation.
	g_dialogue_panel_color = g_timed_message_color;                         // :1213
	mMarkerRow = -1;        // fresh records: none carries the marker
	return 1;
}

// dbase100_open_dialogue_window_alt -- dialogue_ui.c:1224-1247. A spoken line:
// pages of at most 3 wrapped lines. With a clip, the whole line lasts the
// clip (in 70 Hz ticks) and a page gets a share of that; without, it waits.
uint32_t DialogueUi::Dbase100OpenDialogueWindowAlt(const uint8_t *t, uint32_t bytes)
{
	if (g_voice_sample_count != 0 && subtitlesOff) return 1;                // :1226-1227
	const std::vector<uint8_t> src = Terminated(t, bytes);
	const int ctx = LayoutTimedMessageText(mSegments, src.data(), screenPitch, 3, g_timed_message_color);    // :1228
	St().g_active_dialogue_context = ctx;
	if (ctx == 0) return 1;                                                 // :1231
	g_dialogue_segment_index = 0;
	if (g_voice_sample_count != 0)
	{
		// :1235. The divisor is the rate the clip header carries.
		g_dialogue_line_dwell = g_voice_rate != 0 ? (int32_t)(((int64_t)g_voice_sample_count * 0x46) / g_voice_rate) : 0;
		int32_t total = 0;
		for (int i = 0; i < ctx && i < (int)mSegments.size(); i++) total += mSegments[i].len;   // :1236-1238
		g_dialogue_dwell_divisor = total;
		// :1240. g_dialogue_timer_rate (0x82809) IS record 0's length field
		// (0x827fd + 0xc), so the first page's share is len[0] / total.
		const int32_t rate = mSegments.empty() ? 0 : mSegments[0].len;
		St().g_move_freeze_gate = total != 0 ? (int32_t)(((int64_t)g_dialogue_line_dwell * rate) / total) : 0;
	}
	else
	{
		g_dialogue_line_dwell = 0;
		St().g_move_freeze_gate = GATE_WAIT;                                // :1243
	}
	g_dialogue_reveal_ramp = 4;
	return 1;
}

//==========================================================================
//
// Per frame.
//
//==========================================================================

// voice_stream_pump -- audio.c:1300-1327, minus the buffer refill.
void DialogueUi::VoiceStreamPump()
{
	Dbase100InterpState &s = St();
	if (s.g_voice_stream_state == 2)                                        // :1315-1319
	{
		s.g_voice_stream_state = 0;
		s.g_move_freeze_gate = 0;
		g_dialogue_segment_index = 0x64;
	}
	if (s.g_dialogue_busy_flag != 0)                                        // :1320-1324
	{
		if (s.g_voice_stream_state != 0) return;
		if (s.g_active_dialogue_context != 0) return;
		s.g_dialogue_busy_flag = 0;
	}
	mInterp->AdvanceDialogueActionQueue();                                  // :1325
}

// render_text_ui -- dialogue_ui.c:971-1014, the part that is not drawing:
// the timed message's countdown and the spoken line's page sequencer.
void DialogueUi::TickTextUi(int frameTicks)
{
	Dbase100InterpState &s = St();
	if (g_timed_message_line_count != 0)                                    // :973-974; :902-906
	{
		g_timed_message_timer -= frameTicks;
		if (g_timed_message_timer < 1) g_timed_message_line_count = 0;
	}

	if (s.g_active_dialogue_context == 0) return;                           // :976-980

	if (g_dialogue_reveal_ramp < 0x20)                                      // :983-987
	{
		g_dialogue_reveal_ramp += frameTicks;
		if (g_dialogue_reveal_ramp > 0x20) g_dialogue_reveal_ramp = 0x20;
	}

	if (s.g_move_freeze_gate == GATE_MENU) return;                          // :989-992: the menu just draws

	if (s.g_move_freeze_gate != GATE_WAIT) s.g_move_freeze_gate -= frameTicks;   // :996-997

	if (s.g_move_freeze_gate < 1)                                           // :999-1014
	{
		if (g_dialogue_segment_index + 1 == s.g_active_dialogue_context && s.g_voice_stream_state == 1)
		{
			s.g_move_freeze_gate = 0;                                       // hold the last page while the clip plays
		}
		else
		{
			g_dialogue_segment_index++;
			if (g_dialogue_line_dwell == 0)
				s.g_move_freeze_gate = GATE_WAIT;
			else
			{
				// :1007. As ROTH.C has it: record 0's length, for every page.
				const int32_t rate = mSegments.empty() ? 0 : mSegments[0].len;
				s.g_move_freeze_gate = g_dialogue_dwell_divisor != 0
					? (int32_t)(((int64_t)g_dialogue_line_dwell * rate) / g_dialogue_dwell_divisor) : 0;
			}
			if (s.g_active_dialogue_context <= g_dialogue_segment_index)
			{
				s.g_active_dialogue_context = 0;
				s.g_move_freeze_gate = 0;
				return;
			}
		}
	}
}

//==========================================================================
//
// Input.
//
//==========================================================================

// The highlight IS the record's colour byte: the original overwrites the
// byte after a record's leading 01 with g_map_menu_marker_selected, and puts
// the normal colour back on every other record first (dialogue_ui.c:126-132,
// 247-253, 261).
void DialogueUi::StampMarker(std::vector<TextLine> &lines, int count, uint8_t normal, int row)
{
	for (int i = 0; i < count && i < (int)lines.size(); i++)
		if (lines[(size_t)i].text.size() >= 2 && lines[(size_t)i].text[0] == 1) lines[(size_t)i].text[1] = normal;
	if (row >= 0 && row < (int)lines.size() && lines[(size_t)row].text.size() >= 2) lines[(size_t)row].text[1] = markerSelected;
	mMarkerRow = row;
}

// The shared repaint tail of choice_select_prev / _next, dialogue_ui.c:106-133:
// the dialogue's own records, or -- with no dialogue up and the topics shown
// -- the item topic lines.
#define ROTH_REPAINT_SELECTED() do { \
	std::vector<TextLine> *lines = &mSegments; \
	int32_t cnt = s.g_active_dialogue_context; \
	uint8_t normal = (uint8_t)g_dialogue_panel_color; \
	if (cnt == 0) { \
		if (g_choice_interaction_mode == 1) { lines = &mChoiceLines; cnt = g_choice_line_count; normal = mTopicColor; } \
		if (cnt == 0) break; \
	} \
	s.g_choice_saved_index = s.g_choice_selected_index;                  /* :123 */ \
	StampMarker(*lines, cnt, normal, s.g_choice_selected_index); \
} while (0)

void DialogueUi::ChoiceSelectPrev()
{
	Dbase100InterpState &s = St();
	const int32_t ctx = s.g_active_dialogue_context;
	int32_t count;
	if (ctx != 0)
	{
		count = ctx;
		if (s.g_move_freeze_gate != GATE_MENU) return;                      // :144-145
	}
	else count = g_choice_line_count;

	if (s.g_choice_selected_index == -1) s.g_choice_selected_index = 0;     // :151-152
	else
	{
		s.g_choice_selected_index -= 1;
		if (s.g_choice_selected_index < 0) s.g_choice_selected_index = count - 1;   // :156-157
	}
	ROTH_REPAINT_SELECTED();
}

void DialogueUi::ChoiceSelectNext()
{
	Dbase100InterpState &s = St();
	const int32_t ctx = s.g_active_dialogue_context;
	int32_t count;
	if (ctx != 0)
	{
		count = ctx;
		if (s.g_move_freeze_gate != GATE_MENU) return;                      // :170-171
	}
	else count = g_choice_line_count;

	if (s.g_choice_selected_index == -1) s.g_choice_selected_index = 0;
	else
	{
		s.g_choice_selected_index += 1;
		if ((uint32_t)count <= (uint32_t)s.g_choice_selected_index) s.g_choice_selected_index = 0;   // :182-183
	}
	ROTH_REPAINT_SELECTED();
}
#undef ROTH_REPAINT_SELECTED

// choice_accept_selected -- dialogue_ui.c:215-228.
void DialogueUi::ChoiceAcceptSelected()
{
	Dbase100InterpState &s = St();
	if (s.g_choice_selected_index == -1) return;
	if (s.g_active_dialogue_context == 0 && g_choice_interaction_mode == 1)
		g_pending_choice_accept_index = s.g_choice_selected_index + 1;      // :219-220: an item topic
	s.g_active_dialogue_context = 0;
	s.g_move_freeze_gate = 0;
	s.g_dialogue_busy_flag = 0;
	const int32_t saved = s.g_choice_saved_index;
	s.g_choice_selected_index = -1;
	mInterp->ExecuteDialogueBranch((uint32_t)saved);
}

// use_enter_key_handler -- input.c:718-737, the dialogue half.
void DialogueUi::UseEnterKey()
{
	Dbase100InterpState &s = St();
	if (s.g_dialogue_busy_flag == 0) return;
	if (s.g_move_freeze_gate == GATE_MENU) { ChoiceAcceptSelected(); return; }
	if (s.g_move_freeze_gate != 0) DialogueVoiceForceEnd(0, 0);
}

// update_dialogue_choice_highlight -- dialogue_ui.c:231-271. The original
// tests the mouse against each choice's rectangle; here the presentation
// says which row the pointer is on, and the rest is the original's.
uint32_t DialogueUi::UpdateChoiceHighlight(int pos, int row)
{
	Dbase100InterpState &s = St();
	if (s.g_dialogue_busy_flag == 0 || s.g_move_freeze_gate != GATE_MENU) return 0;   // :233-234
	if (pos != g_choice_last_pos) s.g_choice_selected_index = -1;           // :238-239: the pointer moved
	g_choice_last_pos = pos;
	if (s.g_choice_selected_index != -1) return 2;                          // :242-243: the keyboard's choice stands
	StampMarker(mSegments, s.g_active_dialogue_context, (uint8_t)g_dialogue_panel_color, -1);   // :247-253
	if (row >= 0 && row < s.g_active_dialogue_context)                      // :257-266
	{
		StampMarker(mSegments, s.g_active_dialogue_context, (uint8_t)g_dialogue_panel_color, row);
		s.g_choice_selected_index = -1;
		s.g_choice_saved_index = row;
		return 1;
	}
	return 2;
}

// dialogue_voice_force_end -- audio.c:1334-1350. The interpreter calls it
// with (0, 0), a mouse position that is on no row.
void DialogueUi::DialogueVoiceForceEnd(uint32_t eax, uint32_t edx)
{
	(void)eax; (void)edx;
	PointerClick(0, -1);
}

void DialogueUi::PointerClick(int pos, int row)
{
	Dbase100InterpState &s = St();
	if (s.g_voice_stream_state == 1)                                        // :1336-1342
	{
		VoiceStopped();
		s.g_voice_stream_state = 2;
		if (s.g_move_freeze_gate < GATE_MENU) s.g_active_dialogue_context = 0;
		s.g_move_freeze_gate = 0;
		return;
	}
	if (s.g_move_freeze_gate == GATE_MENU)                                  // :1343-1348
	{
		if (UpdateChoiceHighlight(pos, row) == 1) mInterp->CloseDialogueAndRunBranch();
		return;
	}
	s.g_move_freeze_gate = 0;                                               // :1349
}

// try_interrupt_dialogue_voice -- audio.c:1371-1378. The original passes the
// live mouse position, so the skip key over a menu row is a click on it.
uint32_t DialogueUi::TryInterruptDialogueVoice()
{
	Dbase100InterpState &s = St();
	if (s.g_dialogue_action_queue_count != 0
		|| (s.g_active_dialogue_context != 0 && s.g_move_freeze_gate == GATE_WAIT))
	{
		PointerClick(mPointerPos, mPointerRow);
		return 1;
	}
	return 0;
}

// dialogue_voice_stop_all -- audio.c:1355-1366.
void DialogueUi::DialogueVoiceStopAll()
{
	Dbase100InterpState &s = St();
	s.g_dialogue_action_queue_count = 0;
	DialogueVoiceForceEnd(0, 0);
	VoiceStreamPump();
	mInterp->FinishDialogueRecordEval();
	DialogueVoiceForceEnd(0, 0);
	s.g_dialogue_busy_flag = 0;
	s.g_dialogue_action_queue_count = 0;
	s.g_active_dialogue_context = 0;
	s.g_move_freeze_gate = 0;
}

//==========================================================================
//
// Timed messages.
//
//==========================================================================

// queue_timed_message_color -- renderer.c:494-506.
void DialogueUi::QueueTimedMessageColor(const std::string &msg, uint8_t color)
{
	g_timed_message_color = color;
	const std::vector<uint8_t> src = Terminated((const uint8_t *)msg.data(), (uint32_t)msg.size());
	const int lines = LayoutTimedMessageText(mTimedLines, src.data(), screenPitch, 0xa, color);
	g_timed_message_timer = TIMED_MESSAGE_TICKS;
	g_timed_message_line_count = lines;
}

// The use / combine messages (inventory.c:704-727, 746-757, 812-860): the
// names first, then the format, then sprintf, then the queue.
void DialogueUi::ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB)
{
	const std::string a = LineText(nameA, 0x62);
	const std::string b = nameB != 0 ? LineText(nameB, 0x62) : std::string();
	const std::string fmt = ResolveDbase100Text(interfaceIndex, 0);
	std::string msg;
	int used = 0;
	for (size_t i = 0; i < fmt.size(); i++)
	{
		if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == 's')
		{
			msg += used == 0 ? a : (used == 1 ? b : std::string());
			used++;
			i++;
		}
		else msg += fmt[i];
	}
	if (colorIndex < 0 || colorIndex >= 7) colorIndex = 0;
	QueueTimedMessageColor(msg, defaultMessageColor[colorIndex]);
}

//==========================================================================
//
// An item's topic menu.
//
//==========================================================================

// build_available_choice_menu -- dialogue_ui.c:1059-1120.
uint32_t DialogueUi::BuildAvailableChoiceMenu(int32_t itemId)
{
	mChoiceLines.clear();
	mChoiceOptionBlocks.clear();
	g_choice_line_count = 0;                                                // :1064
	if (mDb == nullptr) return 0;
	const DbItem *rec = mDb->Item(itemId);
	if (rec == nullptr || !rec->ok) return 0;
	if (!(rec->flags & DBI_HAS_INFORMATION)) return 0;                      // :1067

	std::vector<uint8_t> buf(1, 0);     // [0] is the throwaway byte before the text (:1061)
	uint32_t count = 0;
	for (const DbTriggerBlock &b : rec->blocks)
	{
		if (b.trigger != DBTRIG_INFORMATION) continue;                      // :1072
		if (mInterp->EvalDialogueRecordConditionWithCleanup(b.offset, 2) == 0) continue;   // :1074-1075
		const uint32_t voiceOff = mDb->U32At(b.offset + 4) & 0xffffffu;     // :1073, 1076
		uint8_t line[200 + 2] = {};
		const uint32_t n = ReadNextDialogueLine(line, 200, voiceOff, 0);    // :1081
		if (n == 0) continue;
		buf.back() = 0x0a;                                                  // :1083: '\n' over the byte before
		buf.insert(buf.end(), line, line + n);
		count++;
		mChoiceOptionBlocks.push_back(b.offset);                            // :1086
	}
	if (buf.size() == 1) buf.push_back(0);
	buf.push_back(0); buf.push_back(0);

	const int lc = LayoutTimedMessageText(mChoiceLines, buf.data() + 1, screenPitch - 0x50, 0x10, g_timed_message_color);   // :1094-1096
	g_choice_line_count = lc;
	// :1098-1099: the topics' normal colour is the first line's.
	if (lc != 0 && !mChoiceLines.empty()) mTopicColor = mChoiceLines[0].Color();
	return count;
}

// activate_selected_choice_record -- dialogue_ui.c:191-203.
void DialogueUi::ActivateSelectedChoiceRecord(uint32_t index)
{
	if (index == 0) return;
	if (index - 1 >= mChoiceOptionBlocks.size()) return;
	const uint32_t rec = mChoiceOptionBlocks[index - 1];
	if (rec == 0) return;
	g_choice_line_count = 0;
	mInterp->EvalDialogueRecordConditionWithCleanup(rec, 0);
	g_choice_interaction_mode = 2;
}

//==========================================================================
//
// What to draw.
//
//==========================================================================

// game_core.c:556-560 (hover) and :607-632 (click): the dialogue takes the
// pointer when it is busy AND (the gate is at or above the menu value -- a
// menu, or a line waiting for a click -- OR something is queued behind it).
// A timed line with nothing queued leaves the pointer to the world.
bool DialogueUi::PointerBelongsToDialogue() const
{
	const Dbase100InterpState &s = St();
	if (s.g_dialogue_busy_flag == 0) return false;
	return s.g_move_freeze_gate >= GATE_MENU || s.g_dialogue_action_queue_count != 0;
}

bool DialogueUi::MenuUp() const
{
	const Dbase100InterpState &s = St();
	return s.g_active_dialogue_context != 0 && s.g_move_freeze_gate == GATE_MENU;
}

bool DialogueUi::LineUp() const
{
	const Dbase100InterpState &s = St();
	return s.g_active_dialogue_context != 0 && s.g_move_freeze_gate != GATE_MENU;
}

DialogueView DialogueUi::View() const
{
	DialogueView v;
	const Dbase100InterpState &s = St();
	const int ctx = s.g_active_dialogue_context;
	if (ctx == 0) return v;
	if (s.g_move_freeze_gate == GATE_MENU)
	{
		v.kind = DialogueView::CHOICES;
		for (int i = 0; i < ctx && i < (int)mSegments.size(); i++)
		{
			std::string row;
			for (const std::string &r : mSegments[i].Rows())
			{
				if (!row.empty()) row += "\n";
				row += r;
			}
			v.rows.push_back(row);
		}
		v.color = (uint8_t)g_dialogue_panel_color;
		v.highlightColor = markerSelected;
		// The marker is on the keyboard's selection, or on the row the
		// pointer last hit (dialogue_ui.c:131-132, 261-263).
		v.highlighted = mMarkerRow;
		return v;
	}
	v.kind = DialogueView::LINE;
	v.pages = ctx;
	v.page = g_dialogue_segment_index;
	if (g_dialogue_segment_index >= 0 && g_dialogue_segment_index < (int)mSegments.size())
	{
		v.rows = mSegments[(size_t)g_dialogue_segment_index].Rows();
		v.color = mSegments[(size_t)g_dialogue_segment_index].Color();
	}
	v.waitsForClick = s.g_move_freeze_gate == GATE_WAIT;
	return v;
}

} // namespace roth
