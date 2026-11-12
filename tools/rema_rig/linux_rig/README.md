# REMA Linux rig (headless test engine)

**Read this before building anything.** REMA runs headless on Linux in its real VR path:
`vr_mode 15` (OpenXR) + `vid_preferbackend 1` (Vulkan), against Monado with a simulated
headset and two controllers. Software Vulkan (lavapipe), Xvfb display. No GPU, no headset.

```
bash tools/linux_rig/setup.sh          # downloads the prebuilt engine for this commit (seconds)
bash tools/linux_rig/monado_start.sh   # OpenXR runtime, leave running
SHOT=50 bash tools/linux_rig/boot.sh smoke 75    # -> run/smoke.stdout, run/smoke.png
```

`bin/` IS the prebuilt engine, committed in the repo (doomxr + pk3s, built from the engine source
named in `CURRENT` and `bin/SOURCE`; `openxr/include` holds the OpenXR 1.1 headers a rebuild
needs). `setup.sh` compares the checkout's engine code to that commit by content and refuses on
any difference: then `bash setup.sh build` (~90 min on 2 cores), then `publish.sh` commits the new
`bin/` and `CURRENT` so nobody builds it twice. No tags, no releases: a session can push a commit.

## Proven (2026-10-31, engine source b0a1c9f5: netplay with resync and late join)
- `tools/netplay/run.sh` (2-4 instances, one netgame, replayed hands, exec scripts, late joiners)
  runs on this bin/; see tools/netplay/README.md for the results table.

## Proven (2026-10-30, engine source 94bf04be)
- Fresh shallow clone -> `setup.sh` -> all six files pass their checksums -> boots to MAP01.
- Boots, loads Freedoom2 MAP01 in OpenXR mode on Vulkan, two eye views, HUD and weapon drawn;
  zero script errors; exits cleanly on the timer.
- Monado accepts every interaction profile REMA suggests (Index fix in 94bf04be).

## Traps (each cost a run)
- `REMA_RIG_RELAX_XR_EXT=1` is required: lavapipe lacks `VK_KHR_external_fence_fd` /
  `_semaphore_fd`, which Monado asks for. Rig only; unset, the engine is unchanged.
- OpenXR **1.1** headers are needed to compile; Ubuntu 24.04's are 1.0.20 and fail. The release
  tarball carries them (`openxr/include`). The loader at run time is the system one.
- `pkill -f monado-service` kills the shell that runs it; the scripts use `pkill -x`.
- A runtime frame takes ~1.5 s on lavapipe; Monado logs "Frame late". Expected.
- **First boot on a fresh machine is slow** (~2-3 min to MAP01): lavapipe compiles every Vulkan
  pipeline once. Give the first run `boot.sh name 180`; later runs reuse the cache in `run/home`.
- The engine can ignore SIGTERM while starting; `boot.sh` uses `timeout -k 10`, so exit 137 is
  the same normal end as 124.
- Killing the engine mid-frame can segfault in `xrDestroySwapchain` on the way out. Harmless.
- Old notes say "GL"/"exits 57 at D_CheckNetGame": both are obsolete. REMA is Vulkan-only.

## Driving the controllers
`python3 tools/linux_rig/remote_hands.py proof &` streams a scripted headset and two controllers
into Monado (modes: `idle`, `wave`, `proof`). With REMA booted on top (`boot.sh ... +vr_replay_record
<file>`), the recorded rows show exactly what reached the tic command: `proof` gives trigger 153,
grip 204, stick 64/-32 (2026-10-30). Monado's remote driver reports no touch inputs, so the
thumb/index touch bits stay 0 here; that is the rig, not the engine.
`BIN=<dir>` makes `boot.sh` run a different engine (a fresh build) instead of `bin/`.
