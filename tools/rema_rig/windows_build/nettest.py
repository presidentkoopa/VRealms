#!/usr/bin/env python3
"""Two-player netplay test on the owner's Windows PC: one host, one joiner, same machine, the
owner's real load order (Desktop\\test.zdl), flat windows, muted. Each writes a vr_synclog and
tools/netplay/syncdiff.py says whether the two machines agreed on every tic.

Through the bridge:
    python
    E:\\DOOMWork\\UZDXREMA\\tools\\windows_build\\nettest.py
    -Tics 700            (how long to play once both are in; default 700 = 20 s)
    -Zdl <path>          (default %USERPROFILE%\\Desktop\\test.zdl; "none" = engine only)
    -Late 1              (the joiner starts 15 s AFTER the host has begun playing: late join)
    -Map e1m1
    -Actors 1            (vr_synclog_actors: one line per actor per tic, to name WHICH actor differs)
    -Files a.pk3;b.pk3   (an explicit load order instead of a ZDL: one mod at a time)
    -HostCmd wait 200; netevent rsm_toggle      (a console command string run by the host only)
    -GuestCmd ...        (same for the joiner)
    -JoinOnly 1          the owner already hosts (his Desktop bat, -host 2 -port 5029): launch
                         only the flat joiner against 127.0.0.1:5029 and return. Same -Files as
                         the bat. -GuestCmd applies. Logs under work/nettest/run_<stamp>/.
    -Live 1              THE OWNER PLAYS: the host runs in VR (+vr_mode 15) on his own config and
                         bindings, the joiner is a flat window on a scratch config, neither quits
                         and nothing is killed -- this script returns at once. Sync logs as always.
    -Stress 1            the owner's stress run: god mode for everyone, the host summons a dozen mixed
                         monsters at the start (sv_cheats on); map monsters stay (there is no -nomonsters here)
Output: one summary; engine logs and sync logs under E:\\DOOMWork\\_buildtemp\\claude_bridge\\work\\nettest\\run_<stamp>
(where a session can read them back), else %TEMP%\\doomxr_nettest.
"""
import os, re, shutil, subprocess, sys, time, zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(REPO, 'build-dxr', 'RelWithDebInfo')
EXE = os.path.join(ENG, 'doomxr.exe')
IWAD = os.path.join(ENG, 'doom.wad')
INI = os.path.join(os.environ.get('USERPROFILE', ''), 'Documents', 'My Games', 'DoomXR', 'doomxr.ini')
SYNCDIFF = os.path.join(REPO, 'tools', 'netplay', 'syncdiff.py')
PORT = 5029


def parse_args(argv):
    opts = {'Tics': 700, 'Actors': 0, 'Files': '', 'HostCmd': '', 'GuestCmd': '', 'Zdl': os.path.join(os.environ.get('USERPROFILE', ''), 'Desktop', 'test.zdl'), 'Late': 0, 'Map': 'e1m1', 'Skill': 3, 'Stress': 0, 'Live': 0, 'JoinOnly': 0}
    for a in argv:
        a = a.strip()
        if not a.startswith('-') or ' ' not in a:
            continue
        k, v = a[1:].split(' ', 1)
        opts[k] = int(v) if k in ('Tics', 'Late', 'Skill', 'Actors', 'Stress', 'Live', 'JoinOnly') else v.strip()
    return opts


def load_order(zdl, explicit=''):
    if explicit:
        files = [f.strip() for f in explicit.split(';') if f.strip()]
        for f in files:
            if not os.path.exists(f):
                raise SystemExit('load order file missing: ' + f)
        return files
    if zdl.lower() == 'none':
        return []
    pairs = []
    for line in open(zdl, errors='replace'):
        m = re.match(r'^file(\d+)=(.+)$', line.strip())
        if m:
            pairs.append((int(m.group(1)), m.group(2).strip()))
    files = [p for _, p in sorted(pairs)]
    for f in files:
        if not os.path.exists(f):
            raise SystemExit('load order file missing: ' + f)
    return files


def scratch_ini(dst):
    txt = open(INI, errors='replace').read() if os.path.exists(INI) else '[GlobalSettings]\n'
    for k, v in (('vr_mode', '0'), ('vid_fullscreen', 'false'), ('i_pauseinbackground', 'false'),
                 ('snd_mastervolume', '0'), ('snd_musicvolume', '0'), ('vid_maxfps', '35'), ('cl_capfps', 'true')):
        if re.search(r'(?m)^%s=' % k, txt):
            txt = re.sub(r'(?m)^%s=.*$' % k, '%s=%s' % (k, v), txt)
        else:
            txt = re.sub(r'(?m)^\[GlobalSettings\]\s*$', '[GlobalSettings]\n%s=%s' % (k, v), txt, count=1)
    open(dst, 'w', encoding='utf-8').write(txt)


def launch(name, run, files, extra, opts, quit_after, pcmd=''):
    home = os.path.join(run, name)
    os.makedirs(home, exist_ok=True)
    cfg = os.path.join(home, 'doomxr.ini')
    sync = os.path.join(run, name + '.sync')
    liveHost = opts['Live'] and name == 'host'
    cmd = [EXE, '-iwad', IWAD, '-stdout', '-nocrashdialog']
    if liveHost:
        cmd += ['-savedir', home]           # his own config and bindings, the headset's
    else:
        scratch_ini(cfg)
        cmd += ['-config', cfg, '-savedir', home]
    if files:
        cmd += ['-file'] + files
    cmd += extra
    if liveHost:
        cmd += ['+vid_preferbackend', '1', '+vr_mode', '15']
    else:
        cmd += ['-width', '640', '-height', '480', '+vid_preferbackend', '1', '+vr_mode', '0']
    cmd += ['+vr_synclog', sync, '+vr_synclog_actors', str(opts['Actors']), '-skill', str(opts['Skill'])]
    if opts['Stress']:
        cmd += ['+sv_cheats', '1']
        pre = 'wait 10; god; '   # god FIRST: a player killed by the fresh monsters respawns without the test weapon
        if name == 'host':
            pre += ('wait 22; summon ZombieMan; summon ShotgunGuy; summon ChaingunGuy; summon DoomImp; summon DoomImp; '
                    'summon Demon; summon Spectre; summon LostSoul; summon Cacodemon; summon HellKnight; summon Revenant; '
                    'summon Arachnotron; ')
        pcmd = pre + (pcmd or 'echo')
    if opts['Live'] and not opts['Stress']:
        # 2026-10-31, run_094502: the host was launched with +god and no sv_cheats, so the command
        # ran before the map existed and would have been refused anyway -- he died to the monsters
        # with every hit a 200 ms pulse on both hands ("haptics go ballistic"). Cheats on, and the
        # command waits for the map.
        cmd += ['+sv_cheats', '1']
        if pcmd: pcmd = 'wait 35; ' + pcmd
    if pcmd:
        cmd.append('+' + pcmd)
    if not opts['Live']:
        cmd.append('+wait %d; quit' % quit_after)
    out = open(os.path.join(run, name + '.stdout'), 'w', errors='replace')
    print('%s: %s' % (name, ' '.join(c if ' ' not in c else '"%s"' % c for c in cmd[1:] if not c.startswith(('E:/', 'D:/', 'C:/')))), flush=True)
    return subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=ENG), sync


def main(argv):
    opts = parse_args(argv)
    files = load_order(opts['Zdl'], opts['Files'])
    stamp = time.strftime('%Y%m%d-%H%M%S')
    base = r'E:\DOOMWork\_buildtemp\claude_bridge\work'
    run = os.path.join(base if os.path.isdir(base) else os.path.join(os.environ.get('TEMP', '.'), 'doomxr_nettest'), 'nettest', 'run_' + stamp)
    os.makedirs(run, exist_ok=True)
    print('nettest %s: %d mod files, map %s, %d tics%s' % (stamp, len(files), opts['Map'], opts['Tics'], ', late join' if opts['Late'] else ''), flush=True)
    quit_after = opts['Tics'] + 40
    if opts['JoinOnly']:
        opts['Live'] = 1    # no quit timer, god waits for the map
        guest, gsync = launch('guest', run, files, ['-join', '127.0.0.1:%d' % PORT], opts, quit_after, opts['GuestCmd'])
        print('JOINER: flat joiner started against the owner\'s host; its sync log is ' + gsync)
        print('the host\'s sync log is wherever his bat put it (REMA NET HOST: E:\\DOOMWork\\_buildtemp\\claude_bridge\\work\\nethost\\)')
        return 0
    host_net = ['-host', '2', '-port', str(PORT), '+map', opts['Map']]
    if opts['Late']:
        host_net += ['-netautostart', '1']
    host, hsync = launch('host', run, files, host_net, opts, quit_after + (15 * 35 if opts['Late'] else 0), opts['HostCmd'])
    if opts['Late']:
        time.sleep(15)
        # the host is alone in the level by now (it loads in a few seconds); the joiner knocks
    guest, gsync = launch('guest', run, files, ['-join', '127.0.0.1:%d' % PORT], opts, quit_after, opts['GuestCmd'])
    if opts['Live']:
        print('LIVE: host in VR, joiner flat, both running; sync logs in ' + run)
        print('when done, quit both from the game; then: python tools\\netplay\\syncdiff.py <run>\\host.sync <run>\\guest.sync')
        return 0
    deadline = time.time() + 240 + opts['Tics'] / 35 * 3
    for p in (host, guest):
        while p.poll() is None and time.time() < deadline:
            time.sleep(1)
        if p.poll() is None:
            p.kill()
            print('killed (timeout)', flush=True)
    print('host exit %s, guest exit %s' % (host.returncode, guest.returncode))
    for name in ('host', 'guest'):
        log = open(os.path.join(run, name + '.stdout'), errors='replace').read().splitlines()
        keys = [l for l in log if re.search(r'Player \d of \d|NETPLAY|NetLobby|joined the game|out of sync|resync|left the game|new host|^MAP|^E\dM\d|VM execution|Script error|No player|I_Error|fatal|slow motion|worldclock|RSM', l)]
        print('--- %s (%d lines)' % (name, len(log)))
        for l in keys[:40]:
            print('  ' + l[:160])
    print('--- syncdiff')
    sys.stdout.flush()
    rc = subprocess.call([sys.executable, SYNCDIFF, hsync, gsync])
    print('logs: ' + run)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
