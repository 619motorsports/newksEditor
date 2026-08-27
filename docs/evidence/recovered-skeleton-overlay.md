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

## Recovered behavior

The renderer uses white line vertices for each marker.
It emits three marker lines around the world translation of a node.

Each marker line has a half-extent of `0.03`:

- `x - 0.03` to `x + 0.03`
- `y - 0.03` to `y + 0.03`
- `z - 0.03` to `z + 0.03`

The renderer emits a marker only for a node that has children.
It also emits parent-to-child connector lines in source order.

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

The builder validates all input before it allocates output geometry.
It rejects truncated spans, invalid child indices, nonfinite translations, cycles, and duplicate references.
It also limits nodes, edges, depth, and output vertices.

The tests cover exact marker coordinates, connector filtering, source order, malformed spans, and all resource limits.
The tests also run with AddressSanitizer and UndefinedBehaviorSanitizer.
