#!/bin/bash
# Boot REMA headless: vr_mode 15 (OpenXR) + vid_preferbackend 1 (Vulkan), against Monado.
#   boot.sh <name> <seconds> [engine args...]     e.g. boot.sh smoke 75 -file mymod.pk3
# Logs: run/<name>.stdout, session log under run/home. Screenshot: set SHOT=<seconds>.
H=$(cd "$(dirname "$0")" && pwd); R=$H/run; N=${1:-boot}; T=${2:-60}; shift 2
export DISPLAY=:77 XDG_RUNTIME_DIR=/tmp/xdg SDL_AUDIODRIVER=dummy
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json
export REMA_RIG_RELAX_XR_EXT=1                  # lavapipe lacks Monado's fence/semaphore fd exts (rig only)
export REMA_RIG_EYE=${REMA_RIG_EYE:-640x720}    # small eyes: a software GPU is slow
mkdir -p "$R/home"; export HOME=$R/home
[ -n "$SHOT" ] && (sleep "$SHOT"; xwd -root -silent | convert xwd:- "$R/$N.png") &
cd "${BIN:-$H/bin}"
timeout -k 10 "$T" ./doomxr -iwad /usr/share/games/doom/freedoom2.wad -nosound -nomusic -nocrashdialog \
  +vr_mode 15 +vid_preferbackend 1 -warp 1 -skill 3 "$@" > "$R/$N.stdout" 2>&1
rc=$?; wait
grep -q "script error\|errors, 0 warnings" "$R/$N.stdout" && echo "SCRIPT ERRORS: see $R/$N.stdout"
grep -m1 -E "^MAP[0-9]+ - " "$R/$N.stdout" "$HOME"/.local/share/games/doomxr/logs/latest.txt 2>/dev/null | head -1
echo "exit $rc (124 = ran until the timer, which is the normal end)"
