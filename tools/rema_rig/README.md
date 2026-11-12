# rema_rig: the UZDXREMA test rig, copied here

Copied (not moved) from `presidentkoopa/UZDXREMA` at `39e4dad`, 2026-11-12, at the
owner's request. Nothing in UZDXREMA was changed. These are that repo's files as
they were; paths inside them still say UZDXREMA until each is adapted.

| folder | what it is | state for REMAROTH |
|---|---|---|
| `linux_rig/` | Headless Linux run of the real VR path: Xvfb + Monado (simulated headset and two controllers) + software Vulkan. `remote_hands.py` streams a scripted headset and hands. `bin/` (UZDXREMA's own prebuilt engine, 43 MB) was NOT copied: it cannot load Realms. | Needs a Linux build of REMAROTH and the engine-side pieces below. |
| `claude_bridge/main_boottest/` | The desk boot test: runs the game on a Win32 desktop that is never shown, windowed and muted, on a scratch copy of the ini, and guards the owner's ini, logs and saves. | Not adapted yet. This is what should replace the REMAROTH bridge's `run`, which puts a window on the owner's screen. |
| `claude_bridge/CLAUDE_BRIDGE.ps1`, `README.md` | UZDXREMA's bridge, for reference. The REMAROTH bridge is `tools/claude_bridge/`. | Reference only. |
| `windows_build/` | build, boot test, bisect, symbolize, kill helpers. | Not adapted. |
| `netplay/` | Multi-instance netgame runs with replayed hands. | Not needed yet. |
| `meshcheck/` | Mesh checker. | Not adapted. |
| `engine_side/p_vrreplay.{h,cpp}` | COPIES of the engine source that records and replays headset and hand input (`vr_replay_record`). REMAROTH does not have this file. | To be ported into `src/playsim/` (also touches `g_game.cpp`, `p_vrdemo.cpp`, `p_vrtelemetry.cpp`, `CMakeLists.txt`). |

Also engine-side in UZDXREMA and missing here: `REMA_RIG_RELAX_XR_EXT` and
`REMA_RIG_EYE` in `vk_openxrdevice.cpp` / `vk_renderdevice.cpp`.

## Where the rig can run

- **A cloud session cannot run `linux_rig`** as it stands: Monado and the SDL2,
  OpenAL and Vulkan development headers are not installed, and package installs
  are blocked. UZDXREMA's own `CLAUDE.md` says the same and sends cloud sessions
  through the owner's PC bridge.
- So for REMAROTH the working route is the hidden-desktop boot test on the
  owner's PC, plus hand replay once `p_vrreplay` is ported.
