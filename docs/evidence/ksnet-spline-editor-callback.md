# ksEditor spline callback evidence

This note records recovered behavior from the installed `ksNet.dll` and its
matching PDB. The DLL SHA-256 is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The native viewport implements the primary spline and interpolated interval.
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
`0.00019999999494757503F`. Float accumulation emits 5,001 samples.
The final sample is approximately `0.99994123`, not exactly one.
It sends the samples to `GLRenderer::spline`.

`AISpline::loadFast` is at `0x1006959B`. It selects the legacy loader at
`0x1006968F` or the version-7 loader at `0x10069AEC`. It then calls
`Spline::computeSplineLength` at `0x10069663` for both versions. Thus, stored
version-7 point lengths do not control interpolation.

`Spline::computeSplineLength` is at `0x100365C3`. It samples each Catmull-Rom
segment with float increment `0.001F`. The constant bytes at `0x1011072C` are
`6f 12 83 3a`. A closed spline also samples the wrapped final segment.
`Spline::length` at `0x10033A1A` adds the straight endpoint chord to that curve
length. Closing-segment lookup divides by the same endpoint chord. The safe
adapter rejects a zero endpoint chord instead of reproducing native division
by zero.

The constructor sets `inPoint` and `outPoint` to `-1.0F` at offsets 12 and 16.
The callback skips the interval when either value equals this sentinel.
The Track Cameras tab supplies these values through its `set IN` and `set OUT`
controls. The AI tab has no interval controls.
The interval helper has token `0x06000109` and native RVA `0x1002DA90`.
It samples from `inPoint` through `outPoint` with the same float increment.
The helper uses `(0, 0, 3, 1)` and depth mode 2 (`eDepthOff`).
It restores depth mode 0 (`eDepthNormal`) after the draw.

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

The viewport stores each converted pass in an immutable buffer. The primary
pass uses normal less-or-equal depth tests and writes. The blue interval uses
no depth test or write. It follows the primary pass at the same callback phase.
The grid and selected-node axis remain in the late overlay phase.

The CLI option is `--ai-spline <file>`. It accepts bounded version-2 and
version-7 files. It requires a workspace model and the authoring-overlay shader
pair. Raw mode remains the default. `--ai-spline-mode interpolated` enables the
recovered interpolated primary path. `--ai-spline-interval <in> <out>` adds the
recovered blue interval. The safe adapter requires a finite, ordered range from
zero to one. Side splines, camber, and edit controls remain staged.

The production WebGL source has no AI-spline load or render path. A source
search found no AI-spline or `fast_lane.ai` identifiers. Thus, a direct WebGL
visual comparison is not possible for this native-only feature. The complete
production WebGL suite passed 380 tests. It skipped 34 installed-fixture tests.

SwiftShader executes both native line passes at 1x and 4x MSAA. The pixel test
checks magenta depth rejection and blue depth-off rendering through an occluder.
The sanitizer-enabled native suite passed all 75 tests with SwiftShader.
The D3D12 code uses the same batch contract. A Windows WARP test remains
necessary for D3D12 execution evidence.
