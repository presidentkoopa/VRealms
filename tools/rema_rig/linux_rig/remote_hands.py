#!/usr/bin/env python3
"""Drive Monado's remote driver (mndrmt3, TCP 4242) with a scripted headset and controllers.

Wire struct r_remote_data (376 bytes, checked against Monado 21.0's headers):
  u64 header "mndrmt3\0"
  r_head_data (128): views[2]{fov 4f, pose quat4f+pos3f, u32 pad}, center pose (7f), bool per_view_valid + 3 pad
  r_remote_controller_data left (120), right (120):
    pose 7f, lin_vel 3f, ang_vel 3f, hand_curl 5f, trigger f, squeeze f, squeeze_force f,
    thumbstick 2f, trackpad_force f, trackpad 2f, 15 bools, 1 pad byte
Coordinates are OpenXR: x right, y up, z backward, metres. Quaternion x,y,z,w.

  remote_hands.py idle      hold a standing pose until killed
  remote_hands.py wave      right hand sweeps, trigger pulses
  remote_hands.py hold [yaw pitch roll [x y z]]   [GRASP] the right hand held still in front of the eye,
                            grip squeezed (a gun held): turn it (degrees) to look at the hand from the side
  remote_hands.py proof     fixed analogs: right trigger 0.6, left squeeze 0.8, right stick (0.5,-0.25),
                            left thumb touch -- to prove the values reach the tic command unchanged
"""
import math, socket, struct, sys, time

HOST, PORT = "127.0.0.1", 4242
HEADER = struct.unpack("<Q", b"mndrmt3\0")[0]
RATE_HZ = 90.0
FOV = (-0.785, 0.785, 0.785, -0.785)
IDENT = (0.0, 0.0, 0.0, 1.0)

def pose(pos, quat):
    return struct.pack("<7f", quat[0], quat[1], quat[2], quat[3], pos[0], pos[1], pos[2])

def head(pos, quat):
    view = struct.pack("<4f", *FOV) + pose(pos, quat) + struct.pack("<I", 0)
    return view + view + pose(pos, quat) + struct.pack("<4B", 1, 0, 0, 0)

def ctrl(active, pos, quat, trig=0.0, sq=0.0, thumb=(0.0, 0.0), a=False, b=False, thumbtouch=None):
    if thumbtouch is None: thumbtouch = abs(thumb[0]) + abs(thumb[1]) > 0.05
    bools = [False, bool(active), False, False, bool(a), bool(a), bool(b), bool(b),
             trig > 0.9, trig > 0.05, False, thumbtouch, False, False, False, False]
    return (pose(pos, quat) + struct.pack("<3f", 0, 0, 0) + struct.pack("<3f", 0, 0, 0)
            + struct.pack("<5f", trig, trig, trig, trig, trig)
            + struct.pack("<3f", trig, sq, sq) + struct.pack("<2f", *thumb)
            + struct.pack("<f", 0.0) + struct.pack("<2f", 0.0, 0.0)
            + struct.pack("<16B", *[1 if x else 0 for x in bools]))

def frame(h, l, r):
    data = struct.pack("<Q", HEADER) + h + l + r
    assert len(data) == 376, len(data)
    return data

def quat_from_euler(yaw_deg, pitch_deg, roll_deg):
    cy, sy = math.cos(math.radians(yaw_deg) / 2), math.sin(math.radians(yaw_deg) / 2)
    cp, sp = math.cos(math.radians(pitch_deg) / 2), math.sin(math.radians(pitch_deg) / 2)
    cr, sr = math.cos(math.radians(roll_deg) / 2), math.sin(math.radians(roll_deg) / 2)
    w = cy * cp * cr + sy * sp * sr
    x = cy * sp * cr + sy * cp * sr
    y = sy * cp * cr - cy * sp * sr
    z = cy * cp * sr - sy * sp * cr
    return (x, y, z, w)

IDLE = dict(h=((0.0, 1.70, 0.0), IDENT), l=((-0.25, 1.30, -0.35), IDENT), r=((0.25, 1.30, -0.35), IDENT))

def connect():
    s = socket.create_connection((HOST, PORT))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    got = b""
    while len(got) < 2 * 376:
        chunk = s.recv(2 * 376 - len(got))
        if not chunk: raise RuntimeError("hub closed during handshake")
        got += chunk
    return s

def run(gen):
    s = connect()
    t0 = time.monotonic(); n = 0
    try:
        for (hp, hq), (lp, lq, lkw), (rp, rq, rkw) in gen:
            s.sendall(frame(head(hp, hq), ctrl(True, lp, lq, **lkw), ctrl(True, rp, rq, **rkw)))
            n += 1
            d = t0 + n / RATE_HZ - time.monotonic()
            if d > 0: time.sleep(d)
    finally:
        s.close()

def gen_idle():
    while True: yield IDLE["h"], (*IDLE["l"], {}), (*IDLE["r"], {})

def gen_wave():
    t0 = time.monotonic()
    while True:
        t = time.monotonic() - t0
        yield (IDLE["h"], (*IDLE["l"], {}),
               ((0.25 + 0.15 * math.sin(t * 1.5), 1.30 + 0.10 * math.sin(t * 2.3), -0.35),
                quat_from_euler(0, -30 * math.sin(t), 0), dict(trig=max(0.0, math.sin(t * 2.0)))))

def gen_proof():
    while True:
        yield (IDLE["h"], (*IDLE["l"], dict(sq=0.8, thumbtouch=True)),
               (*IDLE["r"], dict(trig=0.6, thumb=(0.5, -0.25))))

def gen_hold(yaw=0.0, pitch=0.0, roll=0.0, x=0.10, y=1.45, z=-0.32, headpitch=-35.0):
    q = quat_from_euler(yaw, pitch, roll)
    hq = quat_from_euler(0.0, headpitch, 0.0)   # look down at the hand
    while True:
        yield ((IDLE["h"][0], hq), (*IDLE["l"], {}), ((x, y, z), q, dict(sq=1.0)))

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "idle"
    gens = {"idle": gen_idle, "wave": gen_wave, "proof": gen_proof}
    if mode == "hold":
        run(gen_hold(*[float(a) for a in sys.argv[2:9]]))
        sys.exit(0)
    if mode not in gens: sys.exit(__doc__)
    run(gens[mode]())
