# ksNet FBX geometric mesh transform

Status: exact bounded mesh-node behavior recovered and implemented.

Evidence source:

- Installed binary: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`
- SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- Matching PDB: `ksNet.pdb`
- `FBXImporter::loadNode`: `0x10005880`
- `FBXImporter::fillMatrix`: `0x100057f0`

## Recovered behavior

The native geometry branch evaluates the node's local or global transform,
depending on the importer mode, and separately reads
`GetGeometricTranslation`, `GetGeometricRotation`, and `GetGeometricScaling`.
The calls occur in `loadNode` at the decompiled block corresponding to
`0x10005d67`–`0x10005dd5`. The three vectors are passed to
`FbxMatrix::SetTRS` at `0x10005e13`; the resulting geometric matrix is composed
with the node matrix before the mesh is handed to the mesh builder.

`fillMatrix` at `0x100057f0` copies the FBX matrix rows into the native node
matrix in row order. The existing importer uses the same column-major scene
matrix convention as the bridge. The recovered composition is therefore

```
meshWorld = nodeWorld * TRS(GeometricTranslation,
                            GeometricRotation,
                            GeometricScaling)
```

with XYZ Euler rotations and translation/rotation/scale multiplication in the
same order as the existing local-transform conversion. Geometric values affect
the mesh instance, not the parent/child node transform.

## Port and boundaries

`native/src/formats/fbx_conversion.cpp` now recognizes the three geometric
Properties70/Properties60 fields, builds their bounded TRS matrix, and uses it
for mesh-node rendering transforms and world-space bounds. The node's stored
local/world transform remains the ordinary FBX local hierarchy transform, so
children are not displaced by a parent's geometric offset. The current
`FbxStaticMesh` representation has no normal/tangent arrays; this change only
applies the recovered matrix to the mesh instance and its bounds.

`native/tests/fbx_conversion_tests.cpp` covers geometric translation plus scale,
composition after an existing node rotation, bounds, and a truncated geometric
property. Non-finite and malformed transform values remain rejected through the
same bounded conversion diagnostics.
