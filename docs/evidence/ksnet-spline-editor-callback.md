# ksEditor spline callback evidence

This note records recovered behavior from the installed `ksNet.dll` and its
matching PDB. The DLL SHA-256 is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The native viewport implements the raw primary-spline part of this pass.
The implementation uses a labeled line-list translation for Vulkan and D3D12.

`SplineEditor.onNodeRender` has managed token `0x06000107` and RVA `0x2F748`.
It reads `RenderContext.meshFilter.passID` and returns unless the pass is
`RenderPassID::Opaque`. It also returns when the primary spline is null.

The constructor has token `0x06000106` and RVA `0x2EB3C`. It stores the spline,
graphics object, screen message, and frame delta. It registers the callback on
the `SCENE_FINISHED` node through `NodeCallback.addListener` (`0x060002A8`).

The callback sets an identity world matrix for the primary raw spline.
It draws this spline in `(3, 0, 3, 1)`. The raw path does not change the depth
mode. If interpolated display is active, it calls `renderSplineInterpolated`
(`0x06000108`) instead.
That helper samples from zero through one with an increment of
`0.0001999999949975`. It sends the samples to `GLRenderer::spline`.

If the input and output interval values are valid, the callback draws that
interval in `(0, 0, 3, 1)`. The interval helper (`0x06000109`) uses the same
increment. It selects depth mode 2 before the draw and restores depth mode 0.

The callback creates a wrapped horizontal direction cache. It creates the left
and right splines only when the left spline exists and the right spline is
null. Enabled side splines use `(0, 3, 3, 1)` and the raw line-strip helper.

The callback then processes the selected indices and the current movable edit
point. If more than four edit points exist, it draws a temporary interpolating
spline in `(0, 3, 0, 1)`. It draws camber data only when that mode is active.

`GLRenderer::spline` is at `0x100479E8` and PDB location `0001:289256`.
It returns when the point count is two or less. Otherwise, it emits open line
strips that do not exceed the renderer vertex limit. A new chunk starts at the
next point. The original helper does not repeat the last point of a chunk.

`NodeCallback::render` is at `0x1006096E`. It calls listeners in insertion
order before it renders the callback node. The scene order puts the model and
selected mesh before `SCENE_FINISHED`. The grid and axis children follow it.

The C++ adapter uses version-7 points in source order. For version 2, it uses
the points that the recovered native loader retains at source indices 0, 3,
6, and so on. The adapter keeps the two-point early return and open shape.

Vulkan and D3D12 use line-list topology. The adapter converts each adjacent
point pair to one line-list segment. It keeps every segment at a portable
chunk boundary. Thus, this behavior is not an exact copy of the boundary gap
in the original OpenGL helper.

The viewport stores the converted data in an immutable buffer. It uses normal
less-or-equal depth tests and writes. It draws the spline after the selected
mesh, before the view axis and transparent geometry. The grid and the selected
node axis remain in the late overlay phase.

The CLI option is `--ai-spline <file>`. It accepts bounded version-2 and
version-7 files. It requires a workspace model and the authoring-overlay shader
pair. Interpolation, intervals, side splines, camber, and edit controls remain
staged.

The production WebGL source has no AI-spline load or render path. A source
search found no AI-spline or `fast_lane.ai` identifiers. The in-app browser had
no available connection during this verification. Thus, a production WebGL
visual comparison was not possible for this native-only feature.

SwiftShader executes the native line path at 1x and 4x MSAA. The pixel test
checks magenta color clamping, normal-depth rejection, and the final resolve.
The D3D12 code uses the same batch contract. A Windows WARP test remains
necessary for D3D12 execution evidence.
