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
`addIndex` has token `0x06000062` and performs a linear integer scan.
It returns `void` and does not validate the sign or point range.
`setCurrentPoint` tests virtual key 17 before its forward range-selection path.
This key is Control. Shift is virtual key 16.

`onPickedPoint` has token `0x0600010A` and execution VA `0x1002FED0`.
A null active spline returns `-1.0`. Otherwise, the method finds the closest
interpolated point before it applies selection state.

Edit mode always adds the point without clearing the existing selection.
Outside edit mode, a click without Control clears the selection before add.
Control with an existing selection appends the shorter wrapped point range.
Control with an empty selection uses the clear-and-add path.
All additions use `addIndex`, so earlier entries retain their order.

The return reads the final selected vector entry. It divides this index by the
active spline point count. The method does not validate the input position,
the selected index, or a zero point count.

`onNodeRender` has method RVA `0x2F754` and execution VA `0x1002F760`.
It calls `showCurrentSplineIndexInfo` for each selected index. The first entry
does not have a special movement role. During mutable editing, the method calls
`editSplineManual` for each unique selected index. Thus, one input step moves
each selected point once. The movable edit point uses a separate edit-point
vector.

The CIL iterator loop spans offsets `0x02C1` through `0x0324`. It compares the
current pointer with the vector end, calls `editSplineManual`, advances four
bytes, and repeats. The method itself does not remove duplicates. Normal
uniqueness comes from `addIndex`.

`Spline::setPointAt` is at `0x1003408C`. It writes only the three position
components in the selected 20-byte point. It does not change the stored length
or tag. It does not recompute payload length or forward fields.

`onNodeRender` submits the primary spline and selected markers before the
movement loop. Thus, one movement becomes visible in these passes on the next
render callback. The movement does not clear the cached side splines.

The side-spline checks are at CIL offsets `0x01D9` through `0x01E8`.
`onNodeRender` calls `computeLeftRightSplines` only when one side pointer is
null. Direct point movement can therefore leave the cached native side splines
stale.

`refreshSplines` is at `0x1002E0A4`. It removes both side-spline objects and
calls `computeLeftRightSplines`. Then it calls `AISpline.buildGrid`.
`finishEditing` calls `refreshSplines` only when it applies five or more edit
points. The shorter path changes no point and does not refresh derived state.

`startEditing` has token `0x06000111` and execution VA `0x1002F2D8`.
It sets `isInEditMode`, at offset 72, and clears `selectedIndices`.
It does not change the backup, active points, cached forwards, movable point,
or temporary edit points.

`finishEditing` has token `0x06000112` and execution VA `0x1002F324`.
It clears `isInEditMode` but preserves `selectedIndices`.
It cleans the temporary visual indicators and clears the temporary edit points.
The method does not change the backup, cached forwards, or movable point.

`cancelEditing` has token `0x0600011A` and execution VA `0x1002F55C`.
It clears `isInEditMode`, `selectedIndices`, and the temporary edit points.
It cleans the temporary visual indicators but does not write active points.
It does not refresh the spline or change the backup, cached forwards, or
movable point.

The keyboard path supplies a local movement vector to `editSplineManual`.
The method converts this vector with the cached horizontal forward direction.
The C++ point-position API accepts an absolute position. This API is a
deterministic authoring boundary, not an emulation of the keyboard path.

`onNodeRender` builds the forward cache only when its vector is empty. For each
point, it normalizes the horizontal direction to the next wrapped point.
Direct movement does not clear this cache. `refreshSplines` also leaves it
unchanged. Only `resampleSpline` token `0x0600002F` clears the cache.

`mat44f.setFromHeadingUp` has token `0x06000058` and RVA `0x20B14`.
The global `transform` has token `0x06000101` and RVA `0x2D84C`.
`editSplineManual` calls both helpers with the cached heading and world up.
It passes `false` for homogeneous division. Neither input is normalized.
For horizontal heading `(hx, 0, hz)`, local movement `(dx, dy, dz)` produces:

```text
world.x = -hz * dx - hx * dz
world.y = dy
world.z =  hx * dx - hz * dz
```

Thus, Numpad 8 moves along the cached forward direction. Numpad 2 moves in the
opposite direction. Duplicate XZ points produce a zero cached heading.

`btEditSpline_Click` is at `ksEditor.exe` RVA `0x695C`. Entry calls
`ksGraphics.startEditingSpline` and clears the selected-index vector. Exit calls
`ksGraphics.finishEditingSpline`. Entry does not enable point movement.

`cbUnlockSplineEditClicked` is at RVA `0x68B5`. It calls
`ksGraphics.allowSplineEdit` and controls a separate movement flag. The render
callback tests this flag, but it does not test the edit-mode flag.
Finish preserves the selected indices. Cancel clears them.
Neither method changes the movement flag.

`movePointByKeyboard` has token `0x06000110` and execution VA `0x1002DFF4`.
It polls these numeric-keypad virtual keys through `GetAsyncKeyState`:

- Numpad 8 and 2 change local Z.
- Numpad 4 and 6 change local X.
- Numpad 9 and 3 change local Y.
- Control changes the speed from `0.1 * deltaT` to `1.0 * deltaT`.

Each direction uses an independent test. Thus, concurrent keys combine their
movement. The nonzero return test makes held-key movement level-triggered.

The `SplineEditor` constructor has token `0x06000106` and RVA `0x2EB3C`.
`ksGraphics.initSceneGraph` has token `0x060003F1` and RVA `0x25490`.
This caller passes the literal `0.16F` as `deltaT`. No later method changes it.
Thus, normal movement is `0.016` units for each callback. Control movement is
`0.16` units for each callback. The movement rate depends on callback rate.

The original path does not test window focus. It also accepts the low-order
transition bit from `GetAsyncKeyState`. The portable port tracks key-down and
key-up events instead. It clears all held keys after SDL reports focus loss.
It ignores new key-down events until SDL reports focus gain.
This focus behavior is an explicit cross-platform safety boundary.

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

The spline session keeps a separate load-time backup. Save and refresh do not
replace this backup. The session supplies bounded undo and redo history.
The history byte limit counts canonical writer bytes. Separate limits count the
logical model size of one snapshot and all retained history snapshots. Writer
limits bound the serialized records. A separate limit bounds hostile raw
selection input.

The selected reset copies complete points and tagged payloads from the backup.
If it restores a point position, the session rebuilds the grid before commit.
Payload-only resets preserve the parsed grid.

The recovered camber command negates stored radians in raw selection order.
The low-level API keeps duplicate entries. The CLI removes duplicates in
insertion order, as `SplineEditor.addIndex` does.

`--ai-spline-show-camber` enables the independent camber pass. This option
also requires version-7 payloads and starts off. The viewport rebuilds this
geometry from the current session state after an edit.

The managed `saveAISpline` wrapper has token `0x060003CC` and RVA `0x25C54`.
If a spline exists, it calls `AISpline.buildGrid` and then `AISpline.save`.
`AISpline.buildGrid` has token `0x06000228` and native VA `0x10068A86`.
`AISpline.save` has token `0x06000227` and RVA `0x6A3B9`. It always writes
version 7. It writes zero for the header and payload reserved words.

`Form1.saveToolStripMenuItem1_Click` has RVA `0x5DA0`. It opens a save dialog.
The handler continues only when the dialog result is one.
It calls `ksGraphics.saveAISpline` through MemberRef token `0x0A00016C`.
Then it shows a success message.
The wrapper returns silently when no spline exists.

The native save method truncates the destination directly. It does not use a
temporary file or inspect stream errors. The UI reports success after the
call. Save does not change the backup, selection, edit mode, or dirty state.
The C++ format writer implements the recovered byte layout.
The C++ save boundary rebuilds the grid before serialization.
It writes a temporary file, then atomically replaces the destination.
An error keeps the old destination and the complete session state.
Save does not change the revision, history, baseline, or dirty state.
This file-safety behavior is an explicit portable difference.
Replacement can change destination permissions or access-control metadata.
The POSIX path does not sync the parent directory after replacement.
It does not promise persistence after a power loss.
The current save boundary accepts version 7 only.
The native version-2-to-version-7 payload conversion is not yet recovered.
Other edit commands do not replace an existing destination.

The format layer now includes a bounded port of `AISpline.buildGrid`. The port
reproduces the checked-in native pit-lane grid byte for byte. The session changes
only the selected point position before it rebuilds this grid. It serializes the
candidate and commits the point and grid as one undoable revision.

The `--set-ai-spline-point` command exposes the absolute-position session API.
The command rejects non-finite values and invalid indices. It writes only a
fully rebuilt and validated candidate. An identical position is a byte-stable
no-op and preserves the parsed grid.

The session also accepts an ordered batch of absolute point positions. This
operation is a deterministic authoring boundary, not a keyboard or mouse-drag
emulation. It validates all records before it changes a candidate. Then it
rebuilds the grid once and creates one history revision.

The batch keeps first-seen selection order. Identical duplicate point records
apply once, which follows the native `addIndex` uniqueness rule. Conflicting
duplicate positions are an invalid safe-port input. The native selection vector
cannot contain that state through `addIndex`.

The `--set-ai-spline-points` command exposes this batch boundary. Current
primary, interval, side, selection, and camber builders consume one committed
session model. This behavior is an explicit safe-port refresh boundary. It does
not reproduce stale native side caches during a direct movement tick.

The overlay-set builder publishes no partial set when one enabled pass fails.
The prepared viewport then creates one immutable buffer for each enabled pass.
It changes the retained generation only after all buffers pass validation and
upload. An error leaves every prior buffer and draw chunk active.

The update accepts only the device that prepared the viewport. It also keeps
the prepared pass set and the shared authoring-overlay budget. The shared
device contract contains no Vulkan or D3D12 types at this application boundary.
The caller uses this synchronous operation on the render thread between draws.
D3D12 can wait for idle resources when the old generation leaves ownership.
The native port now owns version-7 models in a transactional controller. The
controller copies the current session and applies the edit to this copy. It
then builds all enabled overlays and allocates the complete next state.

The viewport receives the candidate buffers before the controller changes its
state. After a successful upload, one no-throw pointer swap publishes the model
and overlay set. An upload error keeps the model revision and visible buffers.
The viewport also compares the expected visible revision before replacement.

The native window option `--ai-spline-edit-point` exposes an absolute startup
batch on this render-thread path.
Repeated options form one bounded batch after viewport preparation. The first
frame uses the accepted generation.

The `--ai-spline-unlock-edit` option exposes the separate movement flag.
It starts off, like the original checkbox and constructor field.
The `--ai-spline-index` options supply the ordered selected-index vector.
The window maps Numpad and Control scan codes to portable semantic keys.
It combines all held directions into one point batch for each frame.
The controller publishes that batch after the current draw completes.
Thus, the next frame shows the new generation, as in `onNodeRender`.

The controller retains the load-time horizontal forward cache across movement.
A zero XZ forward suppresses horizontal movement but keeps vertical movement.
The port rejects a forward length that overflows during normalization.
Open splines clamp the final cached forward. Closed splines wrap it.
The port rebuilds the grid and all enabled passes for each accepted frame.
This transactional refresh is safer than the original stale derived caches.
Each accepted frame also creates one bounded undo revision.
The unlock option stays off by default. D3D12 buffer retirement can wait for
idle for each pass, so a Windows WARP cadence test remains necessary.
The window retries transient allocation and viewport publication errors.
It also clears held input while the presentation surface has no size.

The C++ controller ports the exact mode and selection changes for start,
finish, and cancel. These calls do not change model history or the revision.
The viewport retains a validated selection pipeline when the selection is
empty. Thus, start and cancel can clear markers without viewport recreation.
The temporary edit-point producer and five-point finish operation remain
pending in the C++ port.
The window cannot change the selected AI indices at runtime. It also has no
control for edit mode, cancel, or refresh.
The `--ai-spline-save-on-exit` option saves after a clean window exit.
The `--save-ai-spline` command exposes the same rebuilt-grid save boundary.

Dirty state compares the current canonical bytes with the baseline bytes.
Transactional undo, redo, and baseline reset use the same publication path.
An unsafe version-7 authoring baseline stays on the read-only viewer path.

The viewport test replaces all six passes after one committed batch edit. It
releases the input set, then compares each owned buffer with the rebuilt bytes.
It also rejects a forged overrun chunk and an over-limit aggregate. An injected
third buffer upload error keeps the controller and viewport at the prior
revision. The test also covers stale revisions, no-op edits, undo, redo, and
baseline reset. Temporary buffers leave no retained allocation.
A D3D12 fake checks one manual transaction and the clip-space contract.
It does not run a D3D12 queue, fence, or resource destructor.

The manual-input test covers repeated key-down events and simultaneous keys.
It covers both Control keys, opposite-key cancellation, and focus-state clear.
The viewport test covers the exact local transform for two selected points.
It also covers cached forwards, zero forwards, upload failure, and retry.
The save tests cover rebuilt grids, reload, limit errors, and in-place output.
They also prove that a failed save preserves the existing destination.
The file-output test covers temporary-name collisions and promotion failure.
Headless tests cover controller bytes and direct command output.
They do not run the successful window-exit save path.

The production WebGL source has no AI-spline load or render path. A source
search found no AI-spline or `fast_lane.ai` identifiers. Thus, a direct WebGL
visual comparison is not possible for this native-only feature. The complete
production WebGL suite passed 380 tests. It skipped 34 installed-fixture tests.

SwiftShader executes the native line passes at 1x and 4x MSAA. The pixel test
checks magenta and cyan depth rejection. It also checks blue depth-off output.
The test checks red and green camber lines with normal depth.
The current native suite discovered 79 tests. It passed 77 tests and skipped
two unavailable fixture targets. The affected controller test passed with GCC
and Clang sanitizers.
The complete sanitizer run reported one 183-byte NVIDIA driver leak after the
platform-window test. The D3D12 code uses the same batch contract. A Windows
WARP test remains necessary for D3D12 execution evidence.
