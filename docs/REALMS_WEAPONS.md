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
