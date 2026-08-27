# Recovered Skeleton Overlay Evidence

## Binary identity

The evidence comes from the installed Assetto Corsa SDK editor.
The analyzed file is `ksNet.dll`.
Its SHA-256 hash is `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The matching PDB SHA-256 is `0009d617e6d6840dcbf92a962f7b05eecd84e2626852a101ac869ccf0275b093`.

The PDB symbols identify these functions:

- `SkeletonRenderer::render` at `0x1005f34d`
- `SkeletonRenderer::intRender` at `0x1005f138`

The analysis also identifies the OpenGL begin, color, end, and vertex calls.
The managed `renderSkeleton` thunk passes the scene-graph root first.
It passes the selected node second.

## Frame state and order

The managed `ksGraphics.render` method has IL RVA `0x27388`.
It disables depth before the F2 skeleton branch.
The branch tests virtual key `0x71`.

The skeleton renderer uses a line list, opaque blending, and no culling.
The grid and other world overlays follow the skeleton.
The method restores normal depth before screen overlays.

The portable viewport keeps the skeleton in this late world-overlay order.
Portable reflection captures clear this overlay list before drawing each face.

## Animated frame order

`ksGraphics.render` updates the animation position before it traverses the world matrices.
It calls `renderSkeleton` after both operations.

`SkeletonRenderer::intRender` reads the current node world matrix through virtual slot `0x20` near `0x1005f170`.
The recursive helper at `0x1005f001` calls `intRender` for each child.
Thus, the native renderer reads the current world matrix for each visited node during each skeleton render.

The native renderer emits immediate-mode vertices during this traversal.
It does not retain a skeleton vertex buffer.

## Recovered behavior

The renderer uses white line vertices for each marker.
It emits three marker lines around the world translation of a node.

Each marker line has a half-extent of `0.03`:

- `x - 0.03` to `x + 0.03`
- `y - 0.03` to `y + 0.03`
- `z - 0.03` to `z + 0.03`

The renderer emits a marker only for a node that has children.
It also emits parent-to-child connector lines in source order.
It emits all connectors for the current node before it enters the first child.

`SkeletonRenderer::intRender` emits the marker near `0x1005f170`.
It calls the connector helper at `0x1005f103` before the recursive helper at `0x1005f001`.

The current node controls the color of its connectors.
Connectors from the selected current node are yellow `(1, 1, 0)`.
All other connectors are magenta `(1, 0, 1)`.
Markers stay white `(1, 1, 1)`.

The connector helper excludes mesh and skinned-mesh children through recovered RTTI checks.
The recursive walk still enters those children.

`intRender` returns when its input has no parent.
Therefore, the installed scene-graph entry must be parented to emit output.

The portable adapter uses the snapshot model root as this traversal entry.
This mapping is a portability inference because the snapshot omits engine-owned graph objects.
The adapter does not add that inferred parent to the rendered hierarchy.

## Selected-node evidence

`SkeletonRenderer::render` stores the selected-node pointer at class offset `0x4`.
`SkeletonRenderer::intRender` compares the current node with this pointer at `0x1005f2e0`.
The connector color branch starts at `0x1005f308`.

The selected-node branch changes connector colors only.
The branch does not change marker colors.

## Portable contract

The portable builder accepts caller-owned nodes, child-index spans, node kinds, and world translations.
It emits backend-neutral `OverlayLineVertex` data for the existing overlay pass.

The selected-node overload applies the recovered yellow and magenta connector colors.

The portable viewport retains the validated topology and color data.
A frame can supply a dense span of current world matrices in scene-node order.
The viewport then refreshes only the retained vertex positions before any draw or reflection capture.

This retained-buffer design is a portable adaptation of the native immediate-mode traversal.
It does not claim identical resource ownership.

The builder validates all input before it allocates output geometry.
It rejects truncated spans, invalid child indices, nonfinite translations, cycles, and duplicate references.
It also limits nodes, edges, depth, and output vertices.

The frame refresh validates every complete matrix and the exact node count before it changes a vertex.
A preallocated staging copy keeps the active CPU pose coherent with the GPU buffer after an upload error.

The tests cover exact marker coordinates, connector filtering, source order, malformed spans, and all resource limits.
They also cover Vulkan and D3D12 frame refresh, buffer reuse, upload errors, and recovery.
The tests also run with AddressSanitizer and UndefinedBehaviorSanitizer.
