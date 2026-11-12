#!/bin/bash
# Xvfb :77 + Monado OpenXR runtime with the REMOTE driver: a simulated headset and two
# controllers, driven over TCP 4242. Leave it running; boot.sh connects to it.
R=$(cd "$(dirname "$0")" && pwd)/run; mkdir -p "$R"
export DISPLAY=:77
pgrep -f "Xvfb :77" >/dev/null || (setsid Xvfb :77 -screen 0 1920x1080x24 >/dev/null 2>&1 < /dev/null &)
sleep 1
export P_OVERRIDE_ACTIVE_CONFIG=remote XRT_COMPOSITOR_FORCE_XCB=1 XRT_COMPOSITOR_XCB_FULLSCREEN=0
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json XRT_LOG=info
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
pkill -x monado-service; rm -f $XDG_RUNTIME_DIR/monado_comp_ipc "$R/monado.stdin"
mkfifo "$R/monado.stdin"; (setsid sleep 100000000 > "$R/monado.stdin" &)   # monado-service dies without stdin
setsid monado-service > "$R/monado.log" 2>&1 < "$R/monado.stdin" &
sleep 4
grep -q "Listening on port" "$R/monado.log" && echo "monado up (remote driver, port 4242)" || { tail -20 "$R/monado.log"; exit 1; }
