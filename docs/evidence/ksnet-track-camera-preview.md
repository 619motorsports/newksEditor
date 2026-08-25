# ksEditor track-camera preview evidence

This note records behavior from the installed editor. The inspected hashes are:

- `ksNet.dll`: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- `ksEditor.exe`: `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`

PDB segment-1 addresses map to image addresses by adding `0x10001000`.

## Camera file and static preview

`CameraManager::load` is at `0x10038270`. `CameraManager::save` is at
`0x10038D21`. The loader reads the serialized position, forward vector, up
vector, FOV range, interval, clip planes, exposure, DOF, spline, and fixed flag.

`mat44f::setFromHeadingUp` is at `0x10038E4C`. It stores `-FORWARD` in the
third basis row and `UP` in the second basis row. It derives the first row from
their cross product. It does not normalize either serialized vector.

`ksGraphics::moveToTrackCamera` has managed RVA `0x21E24`. It copies the saved
64-byte matrix to the live camera. Then it applies `MIN_FOV`, the three shadow
splits, the near and far planes, exposure, and DOF values. It does not read
`MAX_FOV`, `FOV_GAMMA`, `SPLINE_ROTATION`, or `SPLINE_ANIMATION_LENGTH`.

Thus the installed editor's static preview uses `MIN_FOV`. This differs from
the production WebGL editor, which uses the midpoint FOV for a static camera.

## Spline preview

`ksGraphics::setCurrentCameraSplinePosition` has managed RVA `0x21C08`.
It multiplies the normalized input by `0.9999899864196777` and clamps the
result from zero to one. It calculates a spline position and look-ahead target.

`mat44f.createTarget` has managed RVA `0x21A84`. It normalizes the target
direction and builds a right vector against world up `(0, 1, 0)`. It calculates
the up vector from the direction and right vector. If the result points down,
it reverses that up vector. The helper then writes a target-facing matrix.

The native `InterpolatingSpline` constructor at `0x10034848` selects Catmull-Rom
by default. `getLastIndexFromNorm` at `0x10036CCA` maps normalized position to
arc length. `calculateCatmullRom` at `0x1003514E` uses four wrapped points and
the standard cubic basis with a factor of one-half.

The installed editor spline preview is not the game runtime linear sampler.
The recovered preview methods do not apply `SPLINE_ROTATION` or use the saved
animation length. No current native implementation claims this exact behavior.
