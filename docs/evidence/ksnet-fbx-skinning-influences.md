# ksEditor FBX skinning influences

Status: recovered behavior. The bounded C++ conversion path is implemented.

The evidence comes from the installed `ksNet.dll`. Its SHA-256 value is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

## Import path

`FBXImporter::loadNode` is at `0x10005880`. It reads deformer index zero and
treats that object as an `FbxSkin`.

The function imports every cluster. For each cluster, it reads the link name,
the link-transform matrix, all control-point indices, and all weights.

The cluster ordinal becomes the bone index. The relevant code is from
`0x10006065` through `0x10006112`, and from `0x10006376` through `0x100063c3`.

## Bind matrix and node lookup

`convertMatrix(FbxAMatrix&)` is at `0x10003B80`. It copies the 16 FBX matrix
values directly into `mat44f`. The cluster path does not transpose the matrix.
It does not change the axes, handedness, or units.

`FBXImporter::loadNode` reads `GetTransformLinkMatrix`. It then calls the
matrix inverse helper at `0x10002C20`. It does not read the mesh transform
matrix. It stores this value through one of these functions:

- `MeshBuilder::addBone(Node*, mat44f)` at `0x1004230A`.
- `MeshBuilder::addBone(std::wstring, mat44f)` at `0x10042353`.

Thus, the imported offset matrix is:

```text
offsetMatrix = inverse(cluster.TransformLinkMatrix)
```

The string overload keeps the FBX link name. After the hierarchy exists,
`MeshBuilder::solveSkinnedMeshBones` at `0x10042DDC` searches the root by exact
name. `Node::findChildByName` at `0x1003F2EB` uses recursive depth-first search.
It does not use FBX object identity, and it does not create a missing bone.

`SkinnedMesh::updateBonesBuffer` is at `0x1004A91B`. Its loop body is at
`0x100498C0`. The recovered runtime order is:

```text
boneBuffer[i] = offsetMatrix[i] * boneNode.matrixWS
```

The shared C++ `Matrix4` type uses column-major math. A direct copy of native
row-major values stores the transpose. The equivalent C++ product is therefore
`boneWorld * inverseBind`. It is the transpose of the native
`offsetMatrix * matrixWS` product. A non-commuting scale and translation test
protects this representation conversion.

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

After a skinned batch is built, the importer changes its material shader to
`ksSkinnedMesh`. The call is in the finalizer lambda at `0x10041E77`.

## Safe C++ translation

The C++ converter retains the first skin deformer and all its clusters in
source order. It reads `TransformLink` and intentionally ignores `Transform`,
as the recovered importer does. It rejects missing links, mismatched arrays,
out-of-range control points, duplicate bone names, negative or non-finite
weights, singular matrices, and limit violations. It keeps control-point
weights across UV and normal seam expansion.

The render adapter emits the same 19-float layout. It copies the material into
an isolated `ksSkinnedMesh` entry, so a static batch that uses the same source
material does not change. Vulkan and D3D12 use the existing shared CPU-skinned
scene path. The port adds explicit bone and influence limits because the native
import path does not validate those values.
