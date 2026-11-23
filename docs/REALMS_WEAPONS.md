# Realms weapons: what runs, and how to put a mesh on one

The weapon logic is the original's, ported from ROTH.C and checked tick by tick
against the running original (`tools/actors_test`, replay mode `weapon`). It
covers the trigger, the shot cycle and its rate, ammo, reload, recharge,
damage and timing, and it draws nothing. Every tic it tells one actor in the
world what the weapon is doing. That actor is the only thing drawn, so a
modder dresses it with ordinary MODELDEF.

## The actor

`RealmsHeld` (wadsrc/static/zscript/actors/realmsheld.zs). There is one
subclass per Realms weapon, named by the weapon's DBASE100 index:
`RealmsWeapon9`, `RealmsWeapon12`, ... `RealmsWeapon222`. These are the 15
items that carry WeaponAction blocks. The engine spawns the subclass for the
weapon in hand. If no such class exists, it spawns `RealmsHeld`.

- **VR:** `FollowHandMode` is 1, so the model rides the main-hand controller at
  draw rate (the fork's own hand-held model path, `actor.h`).
- **Flat:** the actor stands in front of the eye, facing the view. Its offset
  follows the original's own draw point on its 320 x 200 screen: bob, sway and
  the lowering.

## The fields (written by the engine every tic; read them, do not write them)

| field | meaning |
|---|---|
| `RealmsItem` | DBASE100 index held (Adam's own item for bare hands) |
| `RealmsState` | `RHS_AWAY`, `RHS_RAISE`, `RHS_READY`, `RHS_FIRE`, `RHS_RELOAD`, `RHS_LOWER`; `RealmsStateName()` gives the name |
| `RealmsFrame` | the original's weapon picture frame (0 when not firing; while firing, the shot cycle's frame) |
| `RealmsFrameCount` | frames in that picture |
| `RealmsStateTics`, `RealmsStateLength` | Realms ticks (70 a second) into the state, and the state's length in the original (0 = open-ended) |
| `RealmsProgress` | 0..1 through the state |
| `RealmsCharge`, `RealmsChargeFull` | a recharging weapon's charge (-1 if it is not one) |
| `RealmsPullTic`, `RealmsShotTic`, `RealmsAmmoTic`, `RealmsReloadTic`, `RealmsNoAmmoTic`, `RealmsEquipTic`, `RealmsCycleEndTic` | `Level.maptime` of the last trigger pull, shot leaving, ammo used, ammo item consumed, out-of-ammo, equip and end of cycle; -1 means never |

These are the original's states, named after its variables (player.c:463-501):

- **Away**, **Raise**, **Ready** and **Lower** are the four values of its
  weapon-anim word.
  - Raise and Lower each take 25 Realms ticks.
  - Lower starts after 0x578 ticks (20 s) without firing, and the weapon then
    stays Away until the next pull.
- **Fire** is the fire lock. Frame N lasts `rate` ticks, and the shot leaves on
  the record's own frame.
- **Reload** is a cycle drawn with the record's third picture, after the
  trigger consumed an ammo item.

## Option 1: frame for frame

The subclass's sprite is `R` plus the index in three digits (`R009`, `R012`,
...). The frame letter is the original's frame: A = 0, B = 1, and so on, up
to 28. A model per letter therefore keeps the original's timing exactly:

```
Model RealmsWeapon9
{
    Path "models/realms/pistol"
    Model 0 "pistol.iqm"
    Scale 1 1 1
    FrameIndex R009 A 0 0     // ready
    FrameIndex R009 B 0 1     // fire, frame 1
    FrameIndex R009 C 0 2
    FrameIndex R009 D 0 3
    // ... one line per frame of the original's picture
}
```

(Look up the frame count with `roth_weapon` on the console.)

## Option 2: named states and progress

Leave the frames alone and play the mesh's own animations from script,
stretched to the original's length:

```
class MyPistol : RealmsWeapon9
{
    override void Tick()
    {
        Super.Tick();
        // e.g. SetAnimation('fire', ...) when RealmsState == RHS_FIRE,
        // with the frame position RealmsProgress * the clip's length
    }
}
```

For a subclass to be used it must be the class named `RealmsWeapon<index>`.
Replace that class (or edit realmsheld.zs) rather than adding a new name.

## No model at all

A weapon class with no MODELDEF is drawn as the original's own picture (its
DBASE200 frames, decoded with the original's rules) on a flat card:

- **VR:** in the hand, at about 0.06 units a texel.
- **Flat:** in front of the eye, laid out where the original draws it on its
  320 x 200 screen.

## Console (all queued; the optional last argument is a level tic)

- `roth_give <index> [tic]`: give_item, as the scripts do (a weapon equips itself).
- `roth_trigger [tic]`: pull the trigger once (the fire button does it every tic it is held).
- `roth_weapon [tic]`: state, frame, timing and the held actor.

## The Colt (i9) as a real M1911 in VR (2026-11-16)

The owner's mesh: "Colt 1911" by AlexChuchvaga, Sketchfab, CC BY 4.0
(https://sketchfab.com/3d-models/colt-1911-561fccb5b3314feda06da315b8d36553), in
`vrealms_iwad/models/weapons/colt1911/` with its `CREDITS.txt` (the FBX byte for byte; the diffuse reduced to 2048²).
Code: `src/roth/roth_colt.h` (engine-free rules and state machine), `src/roth/roth_coltvr.{h,cpp}` (the engine side);
tests `tools/colt_test/run.sh`; PC run `tools/claude_bridge/cfg/colt_flat.cfg`.

- **Flat** is unchanged: the original's picture and its automatic reload on the pull.
- **VR** (a headset or the controller-pose test): `RealmsColt1911` replaces the card. The mesh rides the main hand's
  GRIP frame (FollowHandMode 3) at a seat measured by `tools/colt_test/colt_fit` on the authored `grip_pistol` hand
  (the owner's three points: beavertail on the web, index pad toward the trigger face, the grip in the wrapped fingers;
  the residuals are printed by the test). `roth_colt fit dx dy dz rx ry rz` nudges it live, never archived.
- The shot leaves the drawn muzzle along the drawn bore (`ColtBoreAim`, read by WeaponTick).
- The parts are posed from the mesh's own take on the original's fire events (`roth_colt.h PartsFor`): trigger and
  hammer before the shot frame, the slide's recoil from the shot to the cycle's end, locked back on the last round.
- The spent case (`?part=Gils`) and a dropped magazine (`?part=Magaz`) are world actors with gravity and collision;
  presentation only. The loader's `part=<joint>` option keeps one joint's triangles (fbx_skinned.h).
- **The reload's rules** (each the original's, the hands only choosing when; full text in `roth_colt.h`): the release
  drops the magazine only when the word is 0 (the original has no partial magazines); a magazine is taken from the
  belt pouch only when one is carried; let go before it seats, it goes back; **the seat is the original's reload**: the
  engine pulls `trigger_weapon_fire` once (WC:843-866: remove_item(i51), the word 0 + 12, the reload picture's lock,
  no shot); a pull on the empty word with i51 carried is a dry click (the automatic reload replaced); with none
  carried it is the original's (message 6, the next weapon); the slide must be racked or released before a shot.
- `roth_colt release` / `roth_colt sliderelease` are the reload inputs (bindable). The Colt lying in the world
  (ADEMO[99]) is the same mesh (roth_img3d.cpp), the picture kept for picking.
- **The shooting hand's button (R8, 2026-11-23).** The main hand's lower face button (A on a right controller, X on a
  left) is the 1911's magazine catch and slide stop, read as the trigger and grip are (VRFingerInput::faceLower), no
  binding written. One press, by the gun's state:

  | the gun | the button |
  |---|---|
  | slide forward, a magazine in | the magazine release: R1 decides (word 0 and no shot cycling: dropped; else refused, a double buzz on that hand, nothing moves) |
  | slide locked back on the empty magazine (after the last shot) | the magazine release (dropped) |
  | slide locked back on a magazine with rounds (a fresh one seated) | the slide stop: forward, chambered |
  | no magazine in | nothing |

  The off hand's fingertip on the catch plus its trigger still releases too. While the Colt is drawn in the VR hands the
  button's own `+use` key does not use the world (p_map.cpp P_UseLines asks roth_coltvr.h ColtTakesUseKey).
- **Two hands (R9, owner 2026-11-23: "does our hands allow for 'two handing' the pistol").** `tools/colt_test/support_fit`
  fits the other hand (the owner's fbx_twohand_support) to the gun and to the posed shooting hand; the seat is in
  `support_fit.txt` and baked in roth_coltvr.cpp. The off hand at that place (6 cm / 50 deg to take, 10 cm / 75 deg to
  keep) takes the gun with its grip, in `vr_support_mode`'s own modes (3, hold, by default); it is then drawn there
  (VRHandPose_SetHeldBy: the shooting hand's frame times the seat) and L2 counts it SUPPORTING. It lets go when it
  leaves, when the grip opens, and at once on a reload action (the catch with its trigger, the pouch, the slide). Aim
  stays the shooting hand's bore. A pulse on the off hand when it takes and lets go; recoil on both while it holds.
  NOT met by the fit: the support palm stands 25 mm (right shooting) / 33 mm (left) out from the off grip panel; the
  shooting fingers come round onto that panel (renders docs/images/colt/support_fit_*.png).
- **Feel:** recoil on each shot (the main hand; the supporting hand too), a click on a dry pull, a pulse when the
  magazine seats and when the slide closes, a double buzz on a refusal. One output: roth_coltvr.cpp Pulse
  (VR_ScriptHaptic, as L2's feedback).
- **Away:** the original's weapon goes Away after 20 s without a shot (lowered off its screen). In VR the Colt's mesh
  stays drawn in the hand; the original's state and its Raise on the next pull run unchanged.
