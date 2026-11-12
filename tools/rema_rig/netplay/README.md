# tools/netplay: the N-player desync rig

Runs 2-4 REMA instances on one Linux machine as one netgame, headless, each driving its hands
from a replay file, and diffs what every machine believed each tic.

```
bash tools/netplay/run.sh <name> <players> <tics>        # e.g. run.sh four 4 700
NOMONSTERS="" bash tools/netplay/run.sh four 4 700        # monsters on
```
Output in `tools/netplay/run/<name>/`: `p<N>.sync` (the per-tic log), `p<N>.stdout`. The last
lines are `syncdiff.py`'s verdict: `IN SYNC`, or the first tic and FIELD on which two machines
disagree. `BIN=<dir>` picks the engine (default: a fresh build at /home/claude/rig/build, else
`tools/linux_rig/bin`).

- `mkreplay.py <out> <player>` writes the replay: hands at rest, the off hand yawing/pitching/
  rolling, the main hand sweeping and rolling, grip then trigger squeeze, a fire, a crouch (head
  low), an off-hand trigger. Each player gets a yaw offset so every machine receives hands it did
  not make.
- `vr_synclog <file>` (engine cvar, p_vrsynclog.cpp) writes every player's raw command and the
  sim fields built from it, plus a hash of all actors. `vr_synclog_actors 1` adds one line per
  actor for finding WHICH actor diverged.
- The instances run flat (`vr_mode 0`): the multiplayer path reads no headset, by design, so the
  test needs none. `vr_mode 15` against Monado is `tools/linux_rig/`.

## Results (2026-10-30, main + netplay commits)
| run | players | tics | result |
|---|---|---|---|
| baseline, before fixes | 2 | 435 | in sync, but hand ANGLES ignored by every machine (off hand mirrored, main roll 0) |
| after angle fix | 2 | 435 | in sync; wire 90/-30/45 lands as 0/330/45 (yaw-90 convention) on both |
| four players, monsters | 4 | 713 | in sync |
| + walking, strafing, weapon switch, jump, fight (exec script) | 2 | 734 | in sync, both took damage |
| BD22 VR set (brutal22test6 + VoxelPack + VR-Reloaded 2.0.3h + hotfix) | 2 | 713 | in sync |
| per-player VR gates: VR + flat, VR player's hands stop mid-run | 2 | 705 | in sync |
| BD22 set, 2 VR + 2 flat (`FLAT="1 3"`), monsters | 4 | 735 | in sync |

`FLAT="1 3"` makes those players flatscreen (no replay). `TIMEOUT=<s>` for heavy mod sets.

## Resync and late join (2026-10-30, netplay branch)
| run | players | result |
|---|---|---|
| `netplay_desynctest 1` on one guest (forced drift) | 2 | desync seen by the guest, host snapshots, both reload, back in sync 2.3 s later |
| `LATE="1" LATEDELAY=15`: host alone, second player joins at tic 136 | 2 | in sync for the 1404 tics after the join |
| `LATE="2"`, monsters on: one guest in, third player joins at tic 91 | 3 | in sync for the 1449 tics after the join |
| `LATE="2 3"`, monsters on: two joins, at tic 58 and tic 142 (admitted one at a time) | 4 | in sync for the 1398 tics after, until the scripted quits |
| BD22 set, `LATE="2"`: join at tic 166 | 3 | in sync for the 243 tics after, then the box ran out of memory (BD22 is 2.3 GB an instance; this rig holds 6.3 GB, so 4 x BD22 cannot run here) |

How late join works (d_net.cpp `NetState_*`, i_net.cpp `Host_LateJoin*`, `Net_LateJoinLoad`):
the host keeps its socket open after the game starts (`net_latejoin 1`, default); a stranger's
knock goes through the normal handshake while the game runs; once the joiner is READY the host
snapshots the game (`netstate.zds`) and ships it to EVERY guest plus the joiner; then one
command adds the player and reloads the snapshot on every machine on the same tic. The joiner
cannot run that tic (its tic counter starts at 0), so GO carries the host's tic, consistency
count, lobby id and every player's userinfo as of that tic; the joiner loads the snapshot at
startup, runs the one load tic the host ran, and stands exactly where everyone else stands.
Everyone rewinds to the snapshot (about a second). Resync is the same transfer without the add.

## Level change and mid-game save/load (2026-11-09, netplay lane)

The net mod set from here on is the owner's 2026-11-09 list: `brutal22test6` + RSQ_BD_VR_Reloaded's
`pk3/` (`02_BD22_VoxelPack`, `03_BD22_VR-Reloaded_2.0.4c`, `04-07 XMVE monster packs`) + RS_Avatar
(trimmed for the rig), RS_Ballistics, RS_Radiance, RS_WorldHands. UBQ packs out for now.

| run | players | result |
|---|---|---|
| net set, stress (god, 12 summons) | 2 | in sync 705 tics |
| net set, host dies at 390, forced respawn at 426, shot again | 2 | in sync 836 tics |
| plain, `changemap map02`, both `netplay_ready` | 2 | MAP02 at tic 199, in sync through 534 |
| net set, `changemap map02`, both `netplay_ready` | 2 | MAP02 at tic 534, in sync through the change; holsters recalibrate on MAP02 |
| plain, `save netsave1` at 305, `netplay_load netsave1` at 500 | 2 | both back at the saved spot and weapon at 516, in sync 835 tics, `slot` switches work after |
| net set, same | 2 | loaded together at gametic 770 (the 120 KB transfer is slow on lavapipe), in sync |

What was needed, all in the engine or the rig:
- **`netplay_ready`** (d_net.cpp): UZDoom 5's intermission is a ready vote on Space / Mouse1 / Pad A
  inside the screen job. Without it a netgame sits in the intermission forever (`wi_autoadvance` does
  not vote). The rig has no keys; a headset has no keyboard. Bind it or type it.
- **`netplay_load <save>`** (d_net.cpp): `save` works in a netgame (DEM_SAVEGAME, every machine
  writes the file) but stock UZDoom can only load locally, which breaks the game. The host's copy of
  the save is staged as the resync snapshot and goes through the resync path: chunks to every guest,
  one DEM_NETLOADSTATE, everyone loads it on the same tic.
- **`+snd_backend null`** in `run.sh`: the intermission re-inits OpenAL; with no audio device
  `al::backend_exception` goes uncaught and the engine exits 0 silently. `-nosound` does not cover it.
- The quit timer (`+wait N; quit`) counts intermission tics too; give level-change runs headroom.

Still not exercised: a player leaving mid-game (the host quitting at the end of a run makes the
joiner's consistency check report "out of sync" against the departed player -- harmless at quit,
to be looked at for a real leave), internet conditions (latency, loss, NAT), four players on the
net set.

## Roomscale travel in the command (2026-11-09, netplay lane)

Until today walking in the room never moved a multiplayer pawn: the OpenXR device kept the room
delta in a local accumulator, `G_BuildTiccmd` teleported the LOCAL pawn once it reached 0.35 m and
then sent a `DEM_WARPCHEAT` -- a sim write on one machine, tics before the others. In the owner's
05:24 headset round his pawn sat on the start spot for 46 s, and the grab he made from where he
really stood split the game. Fixed by making the travel part of the command:

- `vrheadpose_t` has `roomdx roomdy` (map units x16, this tic's room travel) and `turn`
  (accumulated controller turn, BAM16, for the avatar lane's torso). `DEMOGAMEVERSION` 0x222.
- `VKOpenXRDeviceMode::WriteHandCommand` moves the frame accumulator into the command
  (`VR_TakeMultiplayerRoomscaleForCommand`); `P_PlayerThink` applies it to the pawn on every
  machine, for every player, on the tic the command runs (`VR_ApplySimRoomMove`, same body as
  the single-player move). Prediction runs it too. Both local teleports in `G_BuildTiccmd` are
  gone; the explicit teleport is the `DEM_WARPCHEAT` alone.
- The view still leads the pawn by the frames since the last command (`r_utility.cpp`), and by
  the in-flight commands only when prediction did not run them (dead, paused).
- Synclog P line: `head.roomdx head.roomdy head.turn` after `head.roll`; `syncdiff.py` follows.

The rig has no headset, so `netplay_roomtest N` (dev cvar, `g_game.cpp`) writes a 1-unit step
into the next |N| commands (east if positive, west if negative), and `STILL="2"` in `run.sh`
makes a player that does nothing else. Shown working:

    STILL="2" P2CMD="wait 400; netplay_roomtest -60" bash tools/netplay/run.sh room11 3 700
    -> p2 pawn x -189.40, -190.40 (tic 404), -191.40 (405), ... identical on all three logs, IN SYNC
    MODS="<net set>" STRESS=1 STILL="1" P1CMD="wait 400; netplay_roomtest 60" run.sh room12 2 700 +gl_precache 0
    -> p1 x -192 -> -132 over tics 455..514, both machines, IN SYNC

A console `wait` before `netplay_roomtest` counts lobby tics too, so use a generous one.
`usercmd_selftest`: ALL PASSED (head group 15 bytes).

**Headset-confirmed (owner, 2026-11-09 11:03-11:07, `nethost3_20261109-105715` / `run_20261109-110343`):**
the owner hosted in VR with `doomxr_room1109a.exe` (main a873fe15) on the net set, a flat joiner
through the bridge. 4175 shared tics IN SYNC, no resync, no consistency failure; his commands carried
room travel on 1247 tics (about 1.6 m of walking per axis in total) and 25 controller turns, and the
joiner's copy of his pawn moved with every step. The only differences are on the tic he quit on (the
host's last command never reaches the guest; not a desync). The 05:24 split is closed.

## Where a shot starts: the wall clamp (2026-11-10, netplay lane)

A tracked hand pushed through a wall used to fire from behind it. `P_ShotOrigin(mo, offhand)`
(`p_map.cpp`) now gives every engine fire path its origin (P_LineAttack, P_RailAttack, P_LineTrace
with TRF_USEWEAPON, P_SpawnPlayerMissile and the two missile spawners in `p_mobj.cpp`): QuakeVR's
rule, a trace from the replicated head (`P_PlaysimHeadPos`) to the replicated hand through solid
geometry only; if a wall, floor or ceiling comes first the shot starts 2 units short of it. Actors
are not traced. Flat players (hand == view) are unchanged. `sv_vr_shotclamp` (serverinfo, default on)
switches it; `vr_shotclamp_debug 1` prints every clamp. The draw path may read `P_ShotOrigin` to
pull the gun back to the same point (avatar lane); it must never write it.

Proof on the rig: `netplay_handtest D` makes a machine send a fake tracked main hand D units ahead
at eye height and pull the trigger every 20th tic (it also makes that player hand-aimed).

    STILL="2" P2CMD="vr_shotclamp_debug 1; wait 400; netplay_handtest 1500" P0CMD="vr_shotclamp_debug 1" \
      P1CMD="vr_shotclamp_debug 1" bash tools/netplay/run.sh clamp3 3 700
    -> all three machines print the same 16 lines for p2 ("hand 1500.0 from head, hit wall at 768.9
       -> origin pulled back 733.1"), plus 4 identical lines for p0, the replay player, whose real
       hand went 8 units into a wall; md5 of the three lists equal; IN SYNC.
    net set + STRESS=1 + the same handtest: IN SYNC (no clamp lines there: the standing BD22 player
    holds Melee_Attacks, whose punch does not go through the engine's hand fire path).

Headset: not yet confirmed. Needs a PC build (`a873fe15` + this).

## THE SUITE (frozen 2026-11-10): `bash tools/netplay/suite.sh`

One command, thirteen scenarios, GREEN/RED each, exit 1 on any red. This is what ROADMAP step 3 calls
the regression suite: every engine change -- the renderer rebuild above all -- must leave it green
on the rig before it goes anywhere near the owner's headset. Plain freedoom2 only (`~12 min`);
`NETSET=1` adds the two BD22 net-set scenarios (`/home/claude/netmods`, ~6 GB, +8 min);
`ONLY="room clamp"` picks scenarios.

| scenario | what must hold |
|---|---|
| plain | 2 players, replays, IN SYNC |
| stress | monsters + 12 summons, god, IN SYNC |
| room | standing 3rd player takes 60 fake room steps: moved >= 10 units, same on every machine |
| clamp | standing player fires with a hand 1500 units through the level: identical `shotclamp:` lines on all machines |
| netem | 80 ms +-20, 2 % loss through the proxy, IN SYNC |
| level | `changemap map02` + ready votes: both machines reach MAP02, IN SYNC through it |
| load | `save` at 305, `netplay_load` at 500: save written, IN SYNC through the reload |
| leave | guest quits at 300: the host needs no resync |
| latejoin | a 3rd player joins the running game (snapshot + DEM_NETADDCLIENT), in sync after |
| hostleave | host quits at 300: the two survivors agree from tic 400 on (`syncdiff --from`); found the ghost-pawn bug 2026-11-10 |
| four | 4 players, IN SYNC |
| netset / netset_room | stress on the BD22 net set; room steps on it (god, no monsters) |

"IN SYNC" accepts the end-of-run quit (the host's last tics never reach the guests; syncdiff's
"clean through tic N" with N past the run length). The checks are about the contract, not about
reference logs: the replay drifts against gametic with the lobby length, so a byte-for-byte
reference would go red on nothing. First full run, engine `9c3f951a`: ALL GREEN, net set included. Second (2026-11-10, with latejoin/hostleave/four, engine `baeaaad5`+): ALL GREEN.
