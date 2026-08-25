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

The callback creates a wrapped horizontal direction cache. It rebuilds both
side splines if either cached side spline is null.

`SplineEditor.computeLeftRightSplines` has token `0x0600010E` and RVA
`0x2DDB8`. For each point, it gets the next point through `Spline.wrapIndex`.
An open spline clamps the final index. A closed spline wraps the final index to
zero. `Spline.isClosed` uses the recovered 75 m endpoint threshold.

The helper gets each payload through `AISpline.payloadAtSplineIndex`. This
method uses the point tag as the payload index. The safe adapter rejects a tag
that is outside the payload array.

The helper forces the point delta onto the XZ plane and normalizes it. It then
uses `cross(delta, (0, 1, 0))` as the side basis. The left point subtracts the
basis times `payload.sides[0]`. The right point adds the basis times
`payload.sides[1]`.

If `payload.sides[0]` is zero, the helper skips both generated points for that
index. A zero right width does not skip the right point. The generated splines
use tag zero and remain open. Enabled side splines use `(0, 3, 3, 1)`, an
identity matrix, normal depth, and the raw line-strip helper.

The callback then processes the selected indices and the current movable edit
point. If more than four edit points exist, it draws a temporary interpolating
spline in `(0, 3, 0, 1)`. It draws camber data only when that mode is active.

`SplineEditor.showCurrentSplineIndexInfo` has token `0x0600010D` and RVA
`0x2DB38`. Despite its name, the method draws line geometry. It does not draw
text. It draws a yellow `(3, 3, 0)` line from the current point to 40 units
above that point. It uses the horizontal direction to the next wrapped point.
The cross product of that direction and world up gives the side direction.
If the left payload width is nonzero, the method draws two cyan `(0, 3, 3)`
vertical lines. Each line extends from 20 units below to 20 units above its
side point. A zero left width skips both side lines. The point tag selects the
payload. The method inherits the identity world matrix and normal depth.

The `selectedIndices` field is a `std::vector<int>` at offset 52.
`SplineEditor.addIndex` is at `0x10024BFC`. It ignores an existing index and
appends each new index. Thus, the vector keeps insertion order without
duplicates. `getSelectedIndices` is at `0x1001F6A8` and returns a copy.

`onNodeRender` has method RVA `0x2F754` and execution VA `0x1002F760`.
It calls `showCurrentSplineIndexInfo` for each selected index. The first entry
drives `editSplineManual` during mutable editing. The movable edit point uses
a separate edit-point vector.

`SplineEditor.renderCamberOnSpline` has token `0x0600010C` and RVA `0x2E8C0`.
It draws one vertical line for each point. The line height is
`abs(payload.camber) * 1000`.

Positive camber uses `(0, 3, 0, 1)`. Zero and negative camber use
`(3, 0, 0, 1)`. The helper changes neither the matrix nor the depth mode.
It gets each payload through the point-tag mapping.

`onPickedPoint` is at `0x1002FED0`. It returns the final selected index divided
by the point count. This value is the normalized pick/UI position. This return
does not make the final entry the mutable edit point.

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
zero to one.

`--ai-spline-show-left` and `--ai-spline-show-right` enable independent side
passes. Both options require version-7 payloads. The default state is off,
which matches the two installed-editor checkboxes.

Each `--ai-spline-index <index>` adds one recovered selected-point marker.
Repeated options model `addIndex`: they keep insertion order and ignore
duplicate indices. The CLI reports the last selected index. The native
normalized UI calculation uses that last index. Marker selection does not edit
payloads.

`getCurrentWaypointInfo` and `setWaypointInfo` require exactly one selected
point. The edit resolves the payload through the point tag. It exposes radius,
two side distances, camber, length, and grade. Camber uses degrees in the UI
and radians in the payload.

The first edit object uses zero as an unchanged sentinel. Each nonzero field
replaces its payload field. The second object adds all fields after replacement.
The C++ `--edit-ai-spline` command ports this order. It adds strict bounds,
finite-value validation, complete writer validation, and exclusive output.

`--ai-spline-show-camber` enables the independent camber pass. This option
also requires version-7 payloads and starts off. Spline edit controls remain
staged.

The managed `saveAISpline` wrapper has token `0x060003CC` and RVA `0x25C54`.
If a spline exists, it calls `AISpline.buildGrid` and then `AISpline.save`.
`AISpline.save` has token `0x06000227` and RVA `0x6A3B9`. It always writes
version 7. It writes zero for the header and payload reserved words.

The native save method truncates the destination directly. It does not use a
temporary file or inspect stream errors. The UI reports success after the
call. The C++ format writer implements the recovered byte layout. The CLI uses
a temporary file and does not replace an existing destination. Payload-only
edits preserve the parsed grid. Point edits still require a grid rebuild.

The production WebGL source has no AI-spline load or render path. A source
search found no AI-spline or `fast_lane.ai` identifiers. Thus, a direct WebGL
visual comparison is not possible for this native-only feature. The complete
production WebGL suite passed 380 tests. It skipped 34 installed-fixture tests.

SwiftShader executes the native line passes at 1x and 4x MSAA. The pixel test
checks magenta and cyan depth rejection. It also checks blue depth-off output.
The test checks red and green camber lines with normal depth.
The previous sanitizer-enabled suite passed 75 tests with SwiftShader. This
increment passed 76 sanitizer tests. The explicit Vulkan target skipped because
this preset found no physical device.
The D3D12 code uses the same batch contract. A Windows WARP test remains
necessary for D3D12 execution evidence.
