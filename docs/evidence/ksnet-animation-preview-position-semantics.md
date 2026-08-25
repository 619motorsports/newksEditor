# ksNet animation preview position semantics

Status: exact native position-update path recovered; no playback clock or
looping behavior is evidenced in the inspected editor path. No code change is
made by this note.

## Binary evidence

- `ksNet.dll`: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`
- `ksNet.dll` SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- `ksEditor.exe`: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksEditor.exe`
- `ksEditor.exe` SHA-256: `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`
- Matching PDBs are installed beside both binaries.

The native addresses below use the loaded ksNet image base `0x10000000`.

## Exact position path

The mixed-mode metadata for `ksNet.ksGraphics` identifies these methods:

- `render` method row 905, metadata RVA `0x27388`. Its IL body is 3,736
  bytes. At method offset `0x0898`, it loads `sceneGraph + 0x10`; when that
  pointer is non-null, offsets `0x08a7`–`0x08b0` load `this.animationSpeed`,
  push integer zero, and call native `AnimationPlayer.setCurrentPos`.
- `setAnimationSpeed` method row 976, metadata RVA `0x1e9f8`. Its complete
  body is `ldarg.0; ldarg.1; stfld animationSpeed; ret`; it performs no scale,
  integration, wrapping, or reset.
- The `ksGraphics` constructor (row 904, RVA `0x26824`) stores the exact
  float `0.10000000149011612` into `animationSpeed` at method offset `0x0364`.

The managed `ksEditor.Form1.txtAnimationSpeed_Scroll` handler (method row 137,
RVA `0x5731`) computes `TrackBar.Value * 0.001f` and calls
`ksGraphics.setAnimationSpeed`. There is no delta-time read or increment in
that handler. `loadFBXAnimation` (row 933, RVA `0x25e1c`) replaces the native
scene graph but has no store to `animationSpeed`; the inspected method does not
reset the position field.

Therefore, in the inspected preview path, each render submits the current
normalized slider value as a position. The literal zero passed as the second
argument is the native `force` argument. This means unchanged positions are
normally skipped by the native cache; it is not evidence of playback.

## Native sampling and reset boundary

The matching PDB names `AnimationPlayer::setCurrentPos` at `0x1006001a`, with
`float position, bool force`. Its recovered body has these exact rules:

1. An empty player-set vector returns without applying a frame.
2. The position is clamped to `[0, 1]` by helper `0x10013e20`.
3. If the clamped position equals the cached value at `this + 0x14` and
   `force` is false, no frame is applied. The constructor at `0x1005f503`
   initializes this cache to `-1.0f`, so the first render applies.
4. For `N` frames, it computes `x = position * N`, converts `x` to the lower
   integer frame index, and blends with the next frame using the fractional
   part. At or beyond the final interval it selects frame `N - 1` for both
   samples and uses blend `0`. Position zero explicitly selects frames `0` and
   `1` with blend `0` (the one-frame case remains a native edge case to verify).
5. The body never wraps the position. Any loop or progression would have to
   occur in an uninspected caller that writes a new position; no such write is
   present in `ksGraphics::render` or `loadFBXAnimation`.

The PDB-named helper `AnimationPlayer::isAnimatingNode` is at `0x1005fe6b`.
The native application loop at `0x1005f3c8` processes only player-set records
whose animated byte is set, then `quatpos::lerp` at `0x1005fe91` produces the
sampled transform. These are selection/application details, not a playback
clock.

## Integration recommendation

Do not add an automatic `deltaTime * speed` clock to the current fixed-position
adapter: that would contradict the recovered editor path. The maintainable
next slice is to keep the caller-supplied normalized position, add the native
clamp/cache/force semantics only where the API can represent player state, and
test endpoint sampling and unchanged-position no-op behavior. A separate
clock/loop API should remain explicitly opt-in until a caller that writes
positions over time is recovered.

## Explicit unknowns

- The managed `Form1.onIdle`/`Editor.onIdle` callback (Form1 RVA `0x3fd2`,
  Editor RVA `0x2fdc`) calls `ksGraphics.render`, but its two metadata
  parameters are a focus flag and a native-object parameter, not a proven
  elapsed-time value. This callback therefore does not establish a clock.
- The native `AnimationPlayer` constructor's `AnimationAddingMode` filtering
  and the target-node byte tested by `0x1005f3c8` are not fully named by the
  available PDB. Do not infer reset or selection policy from the managed
  `loadFBXAnimation` wrapper alone.
- NaN/out-of-range conversion behavior outside the normal finite normalized
  slider domain is not a supported editor input contract. The current adapter
  may continue to reject non-finite positions until a native malformed-input
  test justifies another policy.
