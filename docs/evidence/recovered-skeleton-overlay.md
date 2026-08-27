# Recovered Skeleton Overlay Evidence

## Binary identity

The evidence comes from the installed Assetto Corsa SDK editor.
The analyzed file is `ksNet.dll`.
Its SHA-256 hash is `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

The PDB symbols identify these functions:

- `SkeletonRenderer::render` at `0x1005f34d`
- `SkeletonRenderer::intRender` at `0x1005f138`

The analysis also identifies the OpenGL begin, color, end, and vertex calls.

## Recovered behavior

The renderer uses white line vertices.
It emits three marker lines around the world translation of a node.

Each marker line has a half-extent of `0.03`:

- `x - 0.03` to `x + 0.03`
- `y - 0.03` to `y + 0.03`
- `z - 0.03` to `z + 0.03`

The renderer emits a marker only for a node that has children.
It also emits parent-to-child connector lines in source order.

The connector helper excludes mesh and skinned-mesh children through recovered RTTI checks.
The recursive walk still enters those children.

## Unknown behavior

The class stores a selected-node pointer.
The analyzed renderer functions do not prove that these functions read that pointer.

Therefore, the portable builder does not claim selected-node highlighting.
It also does not claim behavior outside the recovered traversal and line generation.

## Portable contract

The portable builder accepts caller-owned nodes, child-index spans, node kinds, and world translations.
It emits backend-neutral `OverlayLineVertex` data for the existing overlay pass.

The builder validates all input before it allocates output geometry.
It rejects truncated spans, invalid child indices, nonfinite translations, cycles, and duplicate references.
It also limits nodes, edges, depth, and output vertices.

The tests cover exact marker coordinates, connector filtering, source order, malformed spans, and all resource limits.
The tests also run with AddressSanitizer and UndefinedBehaviorSanitizer.
