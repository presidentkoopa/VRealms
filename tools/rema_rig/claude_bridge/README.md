# claude_bridge -- the desk test kit

A snapshot, so a new lane does not have to rebuild any of this. The LIVE copies run from
the owner's PC at the paths below; this folder is the record of exactly what they were.

## Current state (2026-11-05): read this first

- **This is how REMA is tested from a cloud session.** The Linux rig (`tools/linux_rig/`) cannot
  install its packages from a cloud session: the Ubuntu mirrors are blocked. So builds and desk
  tests run on the owner's PC through this bridge. The owner starts it by double-clicking
  `START_CLAUDE_BRIDGE.cmd`; if requests sit in `requests\` with no `.out`, it is not running:
  ask him, do not wait silently.
- A cloud session reaches `E:\DOOMWork` through the desktop app's device tools (list, stage,
  commit files). It writes `.req` files there and reads `.out` files back.
- **The bridge runs requests oldest file first (LastWriteTime), not by name.** A request written
  later runs later, even if its name sorts earlier (`r337a` after `r339`). Write a queue in the
  order it must run; to move one back, rewrite it.
- **The `build` action is dead:** `E:\DOOMWork\_buildtemp\run_build.ps1` no longer exists. Build with
  `python scripts\build_named.py <new exe name>` instead. It runs `tools\windows_build\build.py` and
  copies `doomxr.exe` to the new name only if the build succeeded. It refuses a name that exists.
- Bringing the owner's copies up to GitHub, fast-forward only, refusing on local changes:
  `scripts\pull_main.py` (UZDXREMA from `mine/main`) and `scripts\rsa_to_main.py` (RS_Avatar onto
  `main` from `origin/main`).
- Packing: `scripts\pack_as.py <mod dir> <new pk3>`, which refuses an existing name.
- Pushing to `main`: a cloud session pushes to GitHub itself, then the owner's copy is brought up
  with `pull_main.py`. `push_branches.py` refuses main on purpose.
- The current baseline load order is `test_avatar1101.zdl`, and the owner's launcher for it is
  `desktop\VR BODY TEST 1101 MAIN.bat` (live on his Desktop). The checklist is still
  `desktop\REMA TEST CHECKLIST 1027.txt`.
- Holster readout in a boot test: `-ExtraArgs ... +set bootnetevent rs-holster-debug` (the boot
  test's helper sends it at tic 230).
- The current state of the work and the queued requests: `HANDOFF_2026-11-05.md` at the repo root.

The sections below are the original 2026-10-26 record. Its branch names are retired: both
repos work on `main` now.

| here | live on the owner's PC |
|---|---|
| `CLAUDE_BRIDGE.ps1`, `START_CLAUDE_BRIDGE.cmd` | `E:\DOOMWork\_buildtemp\claude_bridge\` |
| `scripts\*.py` | `E:\DOOMWork\_buildtemp\claude_bridge\scripts\` |
| `run_build.ps1` | `E:\DOOMWork\_buildtemp\run_build.ps1` |
| `main_boottest\` | `E:\DOOMWork\tools\main_boottest\` |
| `test_avatar1026.zdl` | `E:\DOOMWork\_buildtemp\claude_bridge\` |

## How a lane uses it

The owner double-clicks `START_CLAUDE_BRIDGE.cmd`. A session writes
`requests\rNNN.req` -- line 1 the action, then one argument per line -- and reads
`responses\rNNN.out`, whose last line is `EXIT=<code>`. Actions and what each refuses are
listed at the top of `CLAUDE_BRIDGE.ps1`. Rules a lane keeps:

- Desk tests only through `boottest` (windowed, muted, hidden desktop). Never launch the
  game otherwise, never fullscreen. `VR BODY TEST.bat` is the owner's only launcher.
- Every build or pk3 the owner may test gets a NEW name (`copy_new.py`, `pack_as.py`
  refuse to overwrite). Never write over a file he may be testing.
- Commit by explicit path. Never commit pk3s.
- The owner's ini is read-only (the boot test copies it and guards it).

## Git through python scripts

The bridge's own `git` action refuses push and merge. On 2026-10-26 the owner asked for the
work to be put online, so three guarded scripts do what the git action will not:

- `push_branches.py <repo> [remote=<name>] <branch>...` -- push named branches with upstream.
  Refuses main/master, never forces or deletes. RS_Avatar's remote is `origin`, UZDXREMA's
  is `mine`.
- `git_merge.py <repo> <branch>` -- `merge --no-ff` into the checked-out branch; refuses
  when that is main/master; stops on a conflict.
- `git_remotes.py <repo>` -- read-only listing.

## The state these were used to test (2026-10-26)

- Engine: UZDXREMA `avatar-ik-work` at 857bbec089, built to
  `build-dxr\RelWithDebInfo\doomxr_armsolver1026.exe` (sha256 f077662d...).
- Mod: RS_Avatar `avatar-test-1026`, packed with `pack_as.py` to
  `E:\DOOMWork\RS_Avatar\RS_Avatar_test_1026.pk3`.
- Load order: `test_avatar1026.zdl` (the Desktop test.zdl with that pk3 in place of
  RS_Avatar.pk3). Loading both RS_Avatar pk3s at once is a fatal error: every class is
  defined twice.
- Result: boots green, `vr_rig_selftest` 46 passed, 0 failed. (The boot test ran
  `doomxr.exe`; the `_armsolver1026` copy is byte-identical to it.)

`iqm_export.py` and `iqm_export_rs.py` are Lee Salzman's Blender IQM exporter (public domain);
the `_rs` copy carries this project's changes.
