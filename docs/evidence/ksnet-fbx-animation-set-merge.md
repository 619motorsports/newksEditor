# ksNet FBX animation set name merge

Status: exact native behavior recovered and implemented in the bounded FBX bridge.

Evidence source:

- Installed binary: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`
- SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- Matching PDB: `ksNet.pdb`
- `Animation::addFrame`: `0x10043793`
- `Animation::getAnimationSet`: `0x10043823`
- UTF-16/name comparison helper called by the set lookup: `0x1000aa10`

## Recovered behavior

`Animation::getAnimationSet` walks the animation-set vector in `0x28`-byte
record steps. The vector starts at `this + 0x0` and ends at `this + 0x4`.
At `0x10043838`, the function passes each record and the requested name to
`0x1000aa10`. A comparison result of zero returns that record immediately
(`0x10043842`–`0x10043846`).

When no name matches, the function constructs a set record from the requested
name (`0x1004384e`–`0x1004387b`). Then it appends the record and returns the new
last record (`end - 0x28`, `0x10043887`–`0x1004389f`). Thus, the vector preserves
first-seen name order. It does not create a second set for a duplicate name.

`Animation::addFrame` calls `getAnimationSet` before appending each sampled
frame (`0x1004379d`–`0x100437a2`, then `0x100437dc`–`0x100437ed`). The frame
append helper at `0x10043ccf` inserts at the requested position and grows the
frame vector in `0x28`-byte elements. Duplicate node names consequently merge
their frames into the one name-selected animation set rather than producing
duplicate output tracks.

The same function sets the animated flag at record offset `+0x24` when the
incoming frame differs from an existing frame. The comparison loop at
`0x100437be`–`0x100437d8` reads 16 dwords. The append stride is `0x28` bytes.
This 64-byte comparison and 40-byte element need a separate layout audit.

## Bounded port and remaining boundary

After the recovered FBX hierarchy walk selects eligible Models, the bridge keys
output animation sets by the native track name. It preserves first insertion
order and appends frames to an existing name entry. It does not use only the
Model ID. This rule is important for FBX null and mesh pairs that share a node
name. The main research record documents the SDK `sphere.kn5` `Sphere001` case.

Focused coverage places two eligible Models with the same name at different
hierarchy positions and asserts one output track in first-seen order with both
frame contributions. The track limit counts unique native animation-set names.
The bounded bridge also limits the total frames in each merged output track.
The animated-flag comparison remains staged until the `0x28`/16-dword layout
discrepancy is resolved. This change does not claim that rule is safe to
reproduce.
