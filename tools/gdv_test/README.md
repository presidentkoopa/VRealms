# gdv_test -- the GDV movie player against the original

`src/roth/roth_gdv` decodes Realms' GDV movies (picture, palette, DAC, DPCM audio, subtitle
timing). This folder proves it against ROTH.C's own code, record by record.

- `gdvtool.cpp` (port side, `build.sh`): decodes a movie with roth_gdv and prints one `F` line per
  record (CRC-32 of the decode buffer, of the movie palette, of the DAC as the screen shows it, the
  chunk flags, the CRC of the decoded audio block), `S` lines for the subtitle machine (record,
  show/clear, text CRC, colour index, layout x0/x1/height), and a `T` total.
  `GDV_GAMEPAL=<hex file>` gives it the game palette the subtitle colour byte indexes;
  `GDV_ORDER=imagefree` runs a frame's settle before its subtitle call, as ROTH.C's image-free
  host does (see below); `GDV_LOOK=<frame>:<file.ppm>` writes one frame for a person to look at
  (put it outside the repository).
- `gdvprobe.c` (original side, a ROTH.C mod, `run_probe.sh`): takes over the boot's first
  `play_gdv_cutscene`, plays each movie in `$GDV_MOVIES` through the original `play_gdv_cutscene`
  with only the timed loop (`gdv_decode_frame`) replaced by a synchronous drive of the original's
  own pieces, and prints the same lines. The DAC is read from the original's VGA port writes.
- `compare.py <original.trace> <port files...> [--col]`: per movie, the match counts.

Run (writes only under /tmp):

    tools/gdv_test/build.sh /tmp/gdvprobe
    tools/gdv_test/run_probe.sh /tmp/gdvprobe/orig.trace
    grep '^P' /tmp/gdvprobe/orig.trace | cut -c3- > /tmp/gdvprobe/gamepal.hex
    for m in GREMLOGO CHPTR1 TUNNWARP BELEXTRA REFLECTA RAYSEL1F EPILOGUE INTRO; do
      GDV_GAMEPAL=/tmp/gdvprobe/gamepal.hex /tmp/gdvprobe/gdvtool "$D/GDV/$m.GDV" "$D/DBASE100.DAT" "$D/DBASE400.DAT" > /tmp/gdvprobe/$m.port
    done
    tools/gdv_test/compare.py /tmp/gdvprobe/orig.trace /tmp/gdvprobe/*.port --col

Result on the 8 staged movies (2026-11-12): 7882 of 7882 records match in decode buffer, movie
palette, DAC, chunk flags and audio block; 27,751,560 of 27,751,560 audio samples (CRC per movie
equal); 163 subtitle events: 162 match in record, text, colour and layout in the original's order,
and all 163 with `GDV_ORDER=imagefree`. The one difference (INTRO, record 2688 vs 2689) is the
image-free host's reordering: it runs the blitter's palette settle in the frame publish, before
the subtitle callback (traps.c host_gdv_publish_frame), where the original blitter runs it as its
tail, after the callback (gdv_cutscene.c:1550-1571). roth_gdv follows the original.

Nothing here writes movie data into the repository: the traces are CRCs and subtitle text from
the player's own files, kept in /tmp.
