# ksNet KSANIM animated-track flag

Status: exact native v2 load behavior recovered and covered by the existing
bounded KSANIM parser.

Evidence source:

- Installed binary: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`
- SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- Matching PDB: `ksNet.pdb`
- PDB symbol: `Animation::load`, entry `0x10043a00`
- Related symbols: `Animation::addFrame` `0x10043793`, `AnimationSet` constructor
  `0x100431a5`

## Recovered v2 rule

`Animation::load` reads the file version and track count, then creates one
`AnimationSet` per serialized track. For version 2 (`0x10043b95`–`0x10043bab`),
it reads `frameCount * 0x28` bytes directly into the track frame vector. A v2
frame is therefore ten 32-bit words: four quaternion values, three position
values, and three scale values.

After the frame vector is populated, the function compares every frame with
the first frame. The loop starts at the first frame (`0x10043bb9`–`0x10043bc4`),
compares exactly ten dwords (`0x10043bc4`–`0x10043bd5`), advances by `0x28`
bytes, and repeats for the track frame count (`0x10043bd7`–`0x10043bdf`).
The first unequal dword writes `1` to the track's byte at `AnimationSet+0x24`
(`0x10043be3`). No epsilon is used, so signed zero and other bit-level float
changes mark the track animated. One or zero frames leave the byte clear.

The bounded C++ parser in `native/src/formats/ksanim.cpp` performs the same
comparison with `std::bit_cast<uint32_t>` for all 40 bytes. Its
`sampleKsAnimation` binding skips tracks whose recovered flag is clear. It
also retains input limits and finite-value checks before this comparison.

Focused coverage in `native/tests/ksanim_tests.cpp` verifies that identical
40-byte frames stay static, a scale-only word marks a track animated, and the
playback binding emits only the marked track. Existing signed-zero coverage
verifies the no-epsilon rule.

## Boundary

This evidence does not recover FBX curve interpolation, FBX pivot evaluation,
or the editor's UI playback speed. The native flag is derived after serialized
KSANIM frame conversion; it is not an animated-metadata flag from FBX curves.
