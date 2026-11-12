# Netplay-safe copies of third-party mods

Loaded in place of the original, which stays untouched.

- `Gameplay_Damnums_1.0.1_REMA.pk3` (Damnums by Xaser Acheron, 1.0.1): the damage numbers and
  their font actors were playsim actors turned toward each machine's own camera (`+CLIENTSIDEONLY`
  is a no-op flag), so two machines disagreed the first time anything took damage (owner's PC,
  2026-10-31 07:41, tic 44). This copy spawns them with `Actor.SpawnClientSide` (`+CLIENTSIDE`),
  rolls no playsim RNG for a look, and is `version "4.15"` for that call (one `return self.Destroy()`
  became `Destroy(); return;`). 50-file roster with it: IN SYNC 439 tics under stress.

Found with `tools/windows_build/bisect.py`. Also learned on that roster, same night: RSQ_Wheel
clashes with RS_WeaponSelectionSystem (cvar `wr_wheel_mode`), RSQ_Saber + RSQ_TacticalRig together
desync at tic 0, and BD22_VR-Reloaded 2.0.3h owns the class name `RSA_Handler` (RS_Avatar's is
`RSAV_Handler` since fd15119).
