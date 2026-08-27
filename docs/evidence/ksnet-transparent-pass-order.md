# ksNet transparent-pass order evidence

## Scope

This note records the transparent color-pass order in the installed `ksNet.dll`.
It also records the separate WebGL compatibility policy in the port.

The inspected binary is in the installed Assetto Corsa editor directory. Its
SHA-256 value is in `ksnet-shadow-pass-execution.md`.

## Recovered pass schedule

`CameraShadowMapped::renderPass` starts at `0x1005E681`. The function runs the
opaque pass first. It then renders the world axis and the transparent pass.

The transparent pass uses this sequence:

1. It creates `CameraMeshFilter` with transparent pass ID `1` at `0x1005E84E`.
2. It selects the no-write depth mode at `0x1005E866`.
3. It starts `PvsRenderMode::Classic` at `0x1005E88C`.
4. It calls the root render function at `0x1005E89A`.
5. It ends the processor pass at `0x1005E8A9`.

`PvsProcessor::end` at `0x10065F2F` sends Classic mode to
`PvsProcessor::doRenderClassic` at `0x10065E44`. This path does not sort draw
calls.

## Traversal order

`Node::render` starts at `0x1003F5DC`. It sets the world matrix and reads the
child vector from its first entry to its last entry.

The function skips an inactive child. It calls each remaining child in vector
order.

`Mesh::render` starts at `0x100494FF`. It applies the mesh and material filters,
binds resources, and sends the draw immediately.

`SkinnedMesh::render` starts at `0x1004A87E`. It updates the bone buffer and
sends the draw immediately. It renders its children after its own mesh.

These functions do not calculate a camera-distance key. Thus, the transparent
pass keeps the current scene traversal order.

## Other sort functions

`PvsProcessor::compareDrawCalls` starts at `0x1006587B`. It compares these
fields in this order:

1. Shader pointer.
2. Material pointer.
3. Blend mode.
4. Cull mode.
5. Depth mode.
6. Vertex-buffer pointer.
7. Index-buffer pointer.
8. Matrix-presence flag.

`PvsProcessor::doRenderCalls` starts at `0x10065B85`. It uses this comparator
for Default mode. The transparent pass uses Classic mode, so this sort does not
control transparent geometry.

`SceneGraphOptimizer::compareNodes` starts at `0x1006061A`.
`Node::sortNodes` at `0x1003F64F` can apply this comparator during scene
optimization. This one-time operation does not use the frame camera.

## Port policy

The retained WebGL viewport sorts transparent geometry for every frame. It uses
the current camera and the transformed center of each vertex AABB.

The native viewport keeps this feature through an explicit compatibility mode.
It uses a stable color permutation for the shared Vulkan and D3D12 path.

This compatibility mode is not recovered original-editor behavior. Directional
shadow passes use a separate prepared-index permutation. This permutation keeps
the recovered depth-first traversal for all three cascades. It preserves the
current bounded scene order, not the original one-time optimizer order.
