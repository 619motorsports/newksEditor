# ksNet FBX animation model selection

Status: recovered behavior implemented as a bounded native bridge slice.

Evidence source:

- Installed binary: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`
- SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- Matching PDB: `ksNet.pdb`
- Ghidra functions: `FBXImporter::loadAnimation` at `0x100071a0` and
  `FBXImporter::loadAnimationNode` at `0x10007550`

## Recovered rule

`loadAnimation` obtains the selected stack time range and calls
`loadAnimationNode` on the FBX root. `loadAnimationNode` first calls
`GetNodeAttribute()->GetAttributeType()` and emits a sampled frame only when
the attribute type is 3, 4, or 1. FBX SDK 2014 names these attributes
`eSkeleton`, `eMesh`, and `eNull`; the corresponding FBX Model type strings are
`LimbNode`, `Mesh`, and `Null`. Camera Models are therefore excluded.

The function then calls `GetChildCount` and `GetChild(i)` and recursively visits
every child in increasing index order (`0x10007550` disassembly after the frame
emission call). This makes the output order a depth-first, ordered hierarchy
walk. Children are visited even when their parent is not an eligible attribute.
The frame call itself uses `EvaluateLocalTransform`; pivot reconstruction is
not part of this slice.

At `0x10007602`–`0x10007631`, the native loop starts at the selected start time,
stops before the end time, and advances by `(end - start) * 0.01`. The constant
at `0x1101d740` is the IEEE-754 double `0.01`. The implementation keeps this
100-sample schedule and the existing explicit-linear interpolation restriction.

The loader divides each FBX start and stop tick by `46,186,158`. It truncates
the result before sampling. `loadAnimationNode` multiplies each sample value by
the same integer before it creates `FbxTime`. This pair is consistent with an
integer-millisecond conversion. The unit interpretation is an inference from
the paired operations and the FBX tick scale.

`Animation::getAnimationSet` compares exact wide-string names. A duplicate name
reuses the first set and appends more frames. The first occurrence controls the
set order. `Animation::addFrame` marks a set as animated when a matrix float
changes. `loadAnimation` later marks every imported set as animated, including
static sets.

`AnimationPlayer` calls recursive `Node::findChildrenByName` at `0x1003f343`.
This method returns every exact-name scene node in depth-first hierarchy order.
Therefore, one merged animation set controls all matching scene nodes.

## Port boundary and tests

`native/src/formats/fbx_conversion.cpp` builds the ordered Model hierarchy from
bounded `OO` links, emits only the three recovered Model types, and still emits
base-transform frames for eligible Models with no curve binding. A stack with a
bounded `LocalStart`/`LocalStop` and no curve records is also accepted for this
static-track case. Every hierarchy container and selected-ID set is charged to
the conversion budget; depth and `max_animation_tracks` remain enforced.

`native/tests/fbx_conversion_tests.cpp` covers ordered Mesh/Null/LimbNode output,
Camera exclusion, static tracks alongside a curve-bound Model, a bounded
curve-free stack, and the track-count limit. Malformed curve and connection
coverage remains in the existing focused test group.

The bounded bridge merges duplicate Model names in first-seen order. Its tests
cover merged frame order and aggregate frame limits. Exact native pivot
evaluation and non-linear interpolation remain separate recovery targets. This
change does not claim to reproduce pivot transforms.
