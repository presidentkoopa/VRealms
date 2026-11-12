#!/usr/bin/env python3
"""A bad network between the host and its guests, in user space (the rig's kernel has no netem).

    python3 netem_proxy.py --listen 5030 --host 127.0.0.1:5029 --delay 80 --jitter 20 --loss 2

Guests `-join 127.0.0.1:5030`; the proxy forwards to the host and back, holding every datagram
for delay +/- jitter ms (one way, so a round trip is twice that) and dropping loss% of them.
One socket per guest towards the host, so the host sees each guest as its own address, as on the
internet. Delay and loss apply in both directions. Prints a count every 10 s.
"""
import argparse, random, select, socket, time, heapq

ap = argparse.ArgumentParser()
ap.add_argument('--listen', type=int, default=5030)
ap.add_argument('--host', default='127.0.0.1:5029')
ap.add_argument('--delay', type=float, default=0.0, help='one-way delay, ms')
ap.add_argument('--jitter', type=float, default=0.0, help='+/- ms, uniform')
ap.add_argument('--loss', type=float, default=0.0, help='percent of datagrams dropped')
ap.add_argument('--seed', type=int, default=1)
a = ap.parse_args()
random.seed(a.seed)
hh, hp = a.host.rsplit(':', 1); HOST = (hh, int(hp))

front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
front.bind(('127.0.0.1', a.listen))
front.setblocking(False)
guests = {}       # guest addr -> socket towards the host
by_sock = {}      # socket -> guest addr
queue = []        # (due, seq, out_sock, data, dest)
seq = 0
stats = {'fwd': 0, 'back': 0, 'dropped': 0}
last = time.time()

def hold(sock, data, dest):
    global seq
    if a.loss > 0 and random.random() * 100.0 < a.loss:
        stats['dropped'] += 1; return
    d = a.delay + (random.uniform(-a.jitter, a.jitter) if a.jitter > 0 else 0.0)
    heapq.heappush(queue, (time.time() + max(0.0, d) / 1000.0, seq, sock, data, dest)); seq += 1

print(f'netem_proxy: :{a.listen} -> {a.host}, delay {a.delay} ms +/- {a.jitter}, loss {a.loss}%', flush=True)
while True:
    timeout = 0.05
    if queue: timeout = max(0.0, min(timeout, queue[0][0] - time.time()))
    socks = [front] + list(by_sock.keys())
    r, _, _ = select.select(socks, [], [], timeout)
    for s in r:
        try: data, addr = s.recvfrom(65535)
        except (BlockingIOError, ConnectionResetError): continue
        if s is front:
            if addr not in guests:
                up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); up.setblocking(False)
                up.bind(('127.0.0.1', 0)); guests[addr] = up; by_sock[up] = addr
                print(f'netem_proxy: guest {addr} -> host via {up.getsockname()}', flush=True)
            hold(guests[addr], data, HOST); stats['fwd'] += 1
        else:
            hold(front, data, by_sock[s]); stats['back'] += 1
    now = time.time()
    while queue and queue[0][0] <= now:
        _, _, s, data, dest = heapq.heappop(queue)
        try: s.sendto(data, dest)
        except OSError: pass
    if now - last > 10:
        last = now
        print(f"netem_proxy: fwd {stats['fwd']} back {stats['back']} dropped {stats['dropped']} queued {len(queue)}", flush=True)
