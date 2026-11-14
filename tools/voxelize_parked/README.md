# Parked: item models from the close-up films (owner, 2026-11-13: "sprites will work for now")

Kept so the work is not lost. Nothing here is built into the engine.

- `roth_voxelize.*` + `voxelize_test/`: the first voxelizer (turntable fit of each close-up film, silhouette carving,
  KVX writer checked through the engine's own KVX parser). Owner's verdict on its output: not good enough.
- `../closeup_mesh/`: the second attempt, by the lead: frames upscaled 4x (de-dithered in the item's mask, cubic,
  sharpened), the hull carved at 1/4 film pixel, the unseen underside clipped, each surface coloured from the frames
  that face it, then a marching-cubes MESH with vertex colours (GLB). Six test items were sent to the owner (snake
  statuette, lamp, key, Aelf's dagger, matchbox, book): key/dagger/snake good; book lines, matchbox label, lamp glass
  not yet.
- Rule: everything derived from retail art is built at load from the install; nothing derived is committed.
