# ksEditor FBX skinning influences

Status: recovered behavior. The C++ skinning port is not implemented.

The evidence comes from the installed `ksNet.dll`. Its SHA-256 value is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

## Import path

`FBXImporter::loadNode` is at `0x10005880`. It reads deformer index zero and
treats that object as an `FbxSkin`.

The function imports every cluster. For each cluster, it reads the link name,
the link-transform matrix, all control-point indices, and all weights.

The cluster ordinal becomes the bone index. The relevant code is from
`0x10006065` through `0x10006112`, and from `0x10006376` through `0x100063c3`.

## Influence limit

`MeshBuilder::setWeight` is at `0x10042CB9`. It scans exactly four influence
slots for each control point.

Each record contains four float weights and four integer bone indices. The
function uses the first slot with bone index `-1`.

The function does not sort or normalize weights. It prints a warning and does
not store a fifth influence.

The native function does not validate the control-point index. The safe port
must reject an index that is outside the control-point table.

## Output layout

`MeshBuilder::buildSkinnedMesh` is at `0x10042538`. It emits 19 floats for each
vertex in this order:

```text
position[3], normal[3], uv[2], tangent[3], weights[4], boneIndices[4]
```

The function changes unused bone indices from `-1` to zero. It converts all
bone indices to float vertex attributes.

Related functions are:

- `MeshBuilder::addBone(std::wstring, mat44f)` at `0x10042353`.
- `MeshBuilder::addBone(Node*, mat44f)` at `0x1004230A`.
- `SkinnedMeshBuilderBatch::emitVertex` at `0x1004286C`.
- `SkinnedMesh::addBone` at `0x10049F82` and `0x10049FCE`.

No bone-palette limit was found in this import path. A future port must add
explicit count and index limits for untrusted FBX data.
