# C++ / Vulkan / DirectX port roadmap

This document defines the migration target for Apex Editor. The target is a
maintainable C++ application with a backend-neutral renderer and Vulkan and
DirectX implementations. It is a port of the current editor, not a new
viewer: existing file formats, authoring behavior, diagnostics, and preview
fidelity remain part of the contract.

The current JavaScript/WebGL implementation is the reference implementation
while the port is in progress. Its tests, fixtures, serialized outputs, and
production browser captures are compatibility evidence. A C++ implementation
may use different internal data structures, but it must preserve the
observable behavior described below.

## Non-negotiable invariants

### Input safety

KN5, KSANIM, KNH, AI splines, ACD, CSP INI, VAO patches, FBX, DDS, and mod files are
untrusted input. Every reader must:

- use an explicit byte span/reader with checked integer arithmetic;
- validate magic, version, enum, count, size, offset, string length, and
  recursion depth before allocation or traversal;
- reject truncated data, integer overflow, impossible sizes, unsafe archive
  paths, duplicate entries where the current editor diagnoses them, and
  unsupported layouts with a useful source/offset error;
- have a malformed and truncated fixture test for every parser change;
- avoid treating a diagnostic fallback as valid game data.

Protected CSP KN5 payloads remain inspectable but must not be rewritten unless
the original encrypted bytes are preserved. Packed ACD input remains
read-only; standalone exports must be explicit.

### Rendering fidelity

Recovered native or CSP behavior must not be replaced with an unlabeled
approximation. Claims of exact behavior require source, fixture, or
disassembly evidence in the relevant test or design note. Any visible
rendering change requires a production WebGL check during the transition and
golden-image or numeric comparison coverage for the C++ backend.

### Desktop and process boundary

The renderer remains isolated from Node.js and filesystem capabilities. The
application server binds to loopback only. Runtime paths use portable path
operations and do not assume a target-specific working directory. Preserve
the current Electron security properties while the web UI is present:

- sandboxed renderer, context isolation, no Node integration;
- denied external navigation, window creation, and permission requests;
- allowlisted static server routes and restrictive CSP/security headers;
- no exposed permissions or arbitrary file/network access from scene code.

The native shell may eventually replace Electron, but these invariants do not
change.

## Target architecture

The port should be organized as independently testable libraries:

1. `apex-core`: spans, checked readers/writers, math, diagnostics, paths,
   cancellation, and deterministic random/noise utilities.
2. `apex-formats`: KN5, DDS, FBX, KSANIM, KNH, AI splines, ACD, INI/CSP, surfaces,
   cameras, VAO, and asset-folder indexing. No renderer or window dependency.
3. `apex-scene`: scene graph, materials, meshes, skins, visibility, workspace
   assembly, validation, authoring transactions, undo/recovery, and project
   serialization.
4. `apex-render`: backend-neutral resource, shader, frame-graph, camera,
   lighting, shadow, reflection, post-process, and effect contracts.
5. `apex-gfx-vulkan` and `apex-gfx-directx`: implementations of the same
   `RenderDevice` contract. Vulkan is the portable baseline; DirectX should
   initially target D3D12 on Windows. Backend-specific code must not leak into
   format parsing or authoring logic.
6. `apex-ui`: native UI or a transitional web UI adapter. UI commands operate
   on `apex-scene` transactions instead of mutating GPU objects directly.
7. `apex-app`: window, file grants, local server/bridge (if still needed),
   project lifecycle, logging, and packaging.

The scene model is the single source of truth. GPU resources are rebuildable
from it, and project/recovery/KN5/CSP exports consume the same transaction
state.

## Current implementation status

As of 2026-08-24, the native port is additive and the JavaScript/WebGL editor
remains unchanged and feature-complete.

- P0 is partial. CMake, strict warnings, sanitizers, cross-platform CI,
  portable security checks, and a native inspection CLI are implemented. The
  port has a backend-neutral device API. Vulkan and D3D12 implement headless
  devices, buffers, 2D textures, samplers, and shader modules. Both backends
  report presentation prerequisites through the neutral API. An optional
  Vulkan mode creates a headless surface and swapchain. It acquires, clears,
  submits, and presents swapchain images. It also presents a completed,
  same-format offscreen color attachment through a backend-neutral API.
  An optional SDL3 platform layer now owns bounded native windows and events.
  It supplies borrowed, type-erased Vulkan surface callbacks and instance
  extensions without leaking Vulkan types into the public platform contract.
  Vulkan can create the same presentation target for this native surface.
  D3D12 reports the DXGI factory, device, queue, and borrowed Win32 window
  prerequisite. The Windows-only path creates a bounded DXGI flip-model swapchain,
  clears and presents synchronously, and copies completed same-format color
  attachments into the swapchain. Resize and device-removal diagnostics remain
  explicit; Linux builds retain the unavailable D3D12 path. A Windows SDK build
  and WARP runtime check remain required before this path is considered verified.
  Both backends implement bounded synchronous RGBA8/BGRA8 texture
  clear/readback. Each
  backend validates a pipeline and executes fixed and indexed R16 mesh draws
  with readback. Static draws use immutable vertex and index buffers. Skinned
  draws use mutable vertex buffers and immutable index buffers. A bounded
  adapter validates and uploads 11-float KN5 static geometry. A second adapter
  owns 19-float KN5 bind vertices and indices. It rejects malformed geometry,
  unsafe indices, invalid influences, and invalid packet ranges before
  allocation. A 128-byte draw-matrix contract binds world and
  camera transforms. Vulkan uses vertex push constants. D3D12 uses root
  constants at `b0`. Backend-specific camera frames make the clip-space
  conversion explicit. Both backends create persistent 1x or 4x D32
  attachments. Explicit load and clear controls retain color and depth across
  indexed draws. Both backends read initialized, single-sample D32 attachments
  through a bounded synchronous API. This readback is execution evidence. It
  is not a sampled shadow-map or receiver path. Both backends also implement a
  resource-free, depth-only indexed pass. The opaque pass requires a
  vertex-only backend shader, an 11-float static mesh, draw matrices, and one
  single-sample D32 target. It rejects material resources, fragment shaders,
  blending, skinning, and multisample depth. A Vulkan SwiftShader test clears
  D32 and draws indexed geometry without a color target. It reads back changed
  depth inside the triangle and the clear value outside it. The D3D12 implementation uses a
  zero-render-target pipeline and is part of the Windows WARP test path. This
  is the bounded opaque-caster execution seam. A backend-neutral owner now
  retains exactly three single-sample D32 maps. It also retains the three
  cascade matrices and converts them from WebGL clip space for each backend.
  The static-scene adapter selects `cast_shadows` packets from its retained
  geometry. It executes opaque and alpha-tested static casters, in packet
  order, for all three cascades. The shadow traversal gives each caster the
  recovered pass-local opaque blend and depth-write state. Thus, a static
  material with a non-opaque main pass can use `ksShadowGenAT`. Alpha-tested
  execution requires an explicit caller shader and the recovered bindings.
  The pixel ABI uses diffuse texture `t0`, sampler `s3`, and a 32-byte material
  record at `b4`.
  The record stores `ksAlphaRef` at byte offset 28. The adapter owns one
  aligned record for each used alpha-tested material. The stock-scene facade
  resolves and forwards the complete material-order record table. It rejects
  malformed resource tables, material values, and buffer budgets before drawing.
  The recovered skinned branch takes precedence over non-opaque material
  state. It uses the explicit skinned pipeline when present and otherwise
  remains staged. The adapter applies the recovered `doubleFaceShadow` cull
  rule. A real SwiftShader
  test reads all three maps and finds caster depth and clear depth in each map.
  The SwiftShader fixture also executes the portable receiver ABI and samples
  all three retained maps. Bounded reference tests cover the source-evidenced
  2/12/50 cascade boundaries and explicit 3x3 PCF. Both backends bind the three
  sampled-depth views, the nearest sampler, and the receiver constants. Both
  backends restore each retained map to its prior tracked state after the draw.
  The workspace viewport now owns the same fixed three-map set for its lifetime.
  It refreshes camera-bound matrices without reallocating the attachments.
  Each frame runs the three caster passes before the receiver color pass.
  Presentation occurs only after both operations succeed. The native shell
  accepts explicit programs for opaque static, alpha-tested static, and
  skinned casters. The alpha program uses a vertex and fragment pair with the
  recovered `t0`, `s3`, and `b4` resource contract. The skinned program uses
  the retained 19-float CPU-skinned stream. This is a labeled translation of
  the recovered `b13` bone-buffer path. Missing role programs keep only their
  caster classes staged. The viewport validates all role contracts before map
  allocation.
  The D3D12 pixel fixture requires a Windows WARP verification build. Vulkan
  integrates the receiver with the bounded stock `ksPerPixel` facade. Extended
  material variants and recovered DXBC register packing remain staged. The
  executable main-pass subset uses the source-evidenced
  `LESS` depth comparison. It also executes explicitly blended packets. The
  ordinary alpha, multiply, and transparent-as-black factors match
  `applyItemRenderState` in `public/app.js`. The indexed path supports 4x
  multisample targets and alpha-to-coverage. Each backend resolves the final
  color to one sample before readback. A batch can also resolve into a retained
  single-sample texture without a CPU copy. Indexed wireframe uses
  line-list topology over the source index stream. This matches the production
  `GL_LINES` selection in `public/app.js`. It does not use polygon-line
  rasterization. Both native backends execute this topology in single draws and
  ordered batches. A bounded batch preflights
  every request, preserves packet order, and records all draws in one pass with
  one final readback. The batch also accepts bounded position-and-color
  line overlays. These overlays use non-indexed line lists and no material
  resources. Each overlay has a bounded scene position. Vulkan and D3D12 use
  the same backend-neutral order and resolve only after all overlays. The
  portable frame order is opaque scene, selected mesh, AI spline,
  world-origin view axis, transparent scene, then the late grid and
  selected-node axis. The view-axis
  boundary is recovered. The single selected draw is a labeled portable
  mapping because native transparent-pass participation remains unresolved. The
  fixed grid contains 22 magenta segments on the
  XZ plane. It covers 10 m at 1 m intervals. The grid starts hidden and remains
  a viewport state. The axis helper matches the recovered normalized X, Y, and
  negative-Z directions. Each axis segment is one meter long. A frame can update
  the grid state and selected world transform before the draw. Callers must
  supply the explicit SPIR-V or DXIL shader pair. A validated SwiftShader test
  covers 1x and 4x output. The 4x output proves that both overlays reach the
  resolved image. The world-origin axis contains three one-meter segments in
  the positive X, Y, and Z directions. It uses an identity world matrix,
  disabled depth, and the recovered RGB intensities of three.
  The same batch accepts one recovered selected-mesh draw. Opaque geometry
  draws first. The selected mesh, AI spline, and view axis draw next.
  Transparent geometry and late editor overlays follow in that order.
  The optional primary AI spline uses the fixed line ABI with normal depth.
  Raw mode uses version-7 points or the recovered retained version-2 points.
  The line-list conversion keeps all source segments across bounded chunks.
  This conversion is a labeled portable translation of the OpenGL line strip.
  Explicit interpolated mode recomputes the installed editor's Catmull-Rom
  arc-length table. It ignores serialized point lengths and emits 5,001
  samples in three bounded draws. Raw mode remains the default.
  An optional normalized interval adds the recovered blue interpolated pass.
  This pass follows the primary spline and disables depth tests and writes.
  Independent version-7 options add the recovered cyan left and right side
  passes. These raw passes follow the interval and use normal depth.
  Repeated version-7 index options add recovered yellow center markers and
  optional cyan width markers. The port keeps insertion order and ignores
  duplicate indices. The port records the last unique index for UI adapters.
  This pass follows the side passes and uses normal depth.
  The format library writes the exact `AISpline::save` version-7 record
  layout. The writer emits zero reserved words and preserves a valid grid.
  It rejects version 2 because the port has not recovered the conversion of
  legacy records into version-7 payloads. A separate authoring adapter ports
  the recovered six-field waypoint edit for one selected point. It resolves
  the payload through the point tag. It applies each nonzero replacement, then
  each additive value. Camber values use degrees at this boundary and radians
  in the file. The adapter returns a validated candidate and owned bytes.
  A separate spline session owns an immutable load-time backup and the current
  state. It provides bounded undo and redo history. The byte limit counts each
  canonical serialization. Separate limits bound the logical size of each model
  and the models in history. A separate limit bounds raw selection entries.
  The session restores selected records from the backup. Camber inversion uses
  raw selection order in the library.
  The CLI ignores duplicate indices, as the native selection method does.
  The edit CLI commands write validated bytes without replacing an output.
  Payload edits preserve the valid grid. A bounded format-layer builder ports
  the recovered grid algorithm. It reproduces the checked-in native pit-lane
  fixture byte for byte. The session can set one absolute point position.
  It preserves the point length, tag, and all payload fields. It rebuilds the
  grid and serializes the candidate before one atomic commit. The point and grid
  participate in the same bounded undo or redo revision. Selected reset also
  rebuilds the grid when it restores a point position. The
  `--set-ai-spline-point` command exposes this boundary without replacing an
  existing output file. A batch operation accepts ordered point-position
  records. It validates all records before it changes one candidate. Identical
  duplicate records apply once in first-seen order. Conflicting duplicates are
  rejected. The batch rebuilds and serializes the grid once, then creates one
  history revision. The `--set-ai-spline-points` command exposes this operation.
  One overlay builder rebuilds all enabled passes from one model candidate.
  A transactional controller owns the version-7 session and visible overlays.
  The viewport allocates all replacement buffers before it changes a pass.
  A successful call replaces primary, interval, side, selection, and camber
  resources as one generation. An error keeps the complete prior generation.
  Replacement uses the device that prepared the viewport. The operation stays
  on the render thread between synchronous frames. It uses the shared Vulkan
  and D3D12 device API. A change to pass presence still recreates the viewport.
  The selection pass is the explicit exception. A controller viewport retains
  its validated pipeline while the selected-index vector is empty.
  The controller edits a copied session and builds a complete next state.
  It publishes the model only after the viewport accepts all candidate buffers.
  An error keeps the prior model revision and the prior visible generation.
  The viewport binds each controller update to an owner, model revision, and
  publication number.
  A controller cannot use a viewport that belongs to another controller.
  Each replacement keeps its owner and uses the next publication number.
  The model revision stays current or advances by one.
  Thus, selection-only publications do not create model history.
  Transactional undo, redo, and baseline reset use the same publication path.
  Dirty state compares canonical current bytes with canonical baseline bytes.
  The controller also ports start, finish, and cancel edit-mode state.
  Start clears the selected indices. Finish preserves them. Cancel clears them.
  Start and cancel do not change model bytes, revision, history, or dirty state.
  The controller also accepts one validated spline point index at runtime.
  The request includes one controller input snapshot.
  This snapshot contains the generation, input epoch, and edit mode.
  Each selection or lifecycle change advances the input epoch.
  This rejects queued input after selection and lifecycle ABA changes.
  Manual movement also requires the snapshot that captured its selection.
  Thus, a delayed movement cannot act on replacement selected points.
  Each visible overlay replacement also advances the publication number.
  Thus, the controller rejects a stale viewport at the same model revision.
  Edit mode appends the point and ignores Control for range selection.
  View mode without Control replaces the selection with the point.
  Control appends the shorter cyclic range from the final stored index.
  Equal cyclic distances use the clicked-to-anchor route.
  Each addition keeps first-seen order and ignores an existing index.
  Each result returns the current input snapshot for the next queued command.
  A selection result also returns the final index and normalized position.
  Stale input and stale viewport bindings have different status values.
  A selection update keeps model bytes, revision, history, and dirty state.
  It publishes the complete staged overlay set with the next publication.
  Invalid indices and publication errors keep the old selection and buffers.
  A right-button release now runs the recovered screen-to-mesh picker.
  It traverses active static KN5 meshes and skips skinned meshes.
  It preserves first-triangle order and strict nearest-node ordering.
  It also preserves the native mesh-local callback-position quirk.
  The grid-aware resolver scans raw spline points with 3D squared distance.
  Control extends the recovered cyclic selection through the existing controller.
  Direct malformed geometry and grid ranges fail before selection publication.
  Shift plus right mouse adds bounded temporary points after two endpoints exist.
  The temporary record keeps click order, duplicate points, and cached forward.
  It uses picked X/Z and copies Y from the closest raw spline point.
  A strict distance below one selects the first temporary point for movement.
  The selected point displays the recovered red and green three-unit axes.
  Five points enable the recovered green 5,001-sample interpolation pass.
  Finish samples the first-to-final endpoint range and writes `[first, last)`.
  One successful finish creates one model revision and clears temporary state.
  Reverse endpoint order clears temporary state without point writes.
  Temporary overlays use two latent backend-neutral line passes.
  The native sphere marker has no line ABI. The port uses a labeled green line.
  The native window option `--ai-spline-edit-point` applies one scripted batch
  after viewport setup.
  The first frame uses the accepted model and all derived passes.
  Unsafe authoring baselines retain the read-only viewer path.
  The original editor polls Numpad 8, 2, 4, 6, 9, and 3 each frame.
  It uses a fixed `0.16` delta for each render callback.
  Thus, a key moves `0.016` units, or `0.16` units with Control.
  It transforms local movement with a cached horizontal forward vector.
  `--ai-spline-unlock-edit` enables this held-key path for version 7.
  The configured `--ai-spline-index` values select the moved points.
  The platform layer maps SDL scan codes to portable keypad and Control keys.
  One frame combines all held directions into one controller transaction.
  The update occurs after the current frame, so the next frame shows it.
  The portable window clears held input after focus loss.
  It also clears held input while the presentation surface has no size.
  This focus rule differs from the original global Windows key polling.
  The controller rejects an overflowed cached-forward normalization.
  The window retries transient allocation and viewport publication errors.
  Open splines clamp the final cached forward. Closed splines wrap it.
  `--save-ai-spline` accepts version 7, rebuilds the grid, and atomically
  replaces its output. Legacy version-2 conversion is not yet recovered.
  `--ai-spline-save-on-exit` saves the live controller after a clean exit.
  Save does not change the revision, history, baseline, selection, or dirty state.
  A save error keeps the prior destination and the complete session state.
  This atomic replace differs from the original direct file truncation.
  Replacement can also change destination permissions or access-control metadata.
  Enter maps to the native edit/finish button actions. Escape maps to cancel.
  These keys are portable controls. The installed editor uses UI buttons.
  Another version-7 option adds the recovered vertical camber lines after the
  side passes. Positive values are green. Other values are red.
  The selected pass uses solid fill, front-face culling, opaque blend state,
  and disabled depth. It writes magenta RGB and the recovered fading alpha.
  The viewport submits the draw at 2000 ms with zero alpha. It stops the draw
  after 2000 ms. Vulkan uses a 256-byte fragment uniform view. D3D12 binds the
  same view as `b5`. SwiftShader pixel tests cover 1x and 4x output.
  The backend also executes one explicitly authorized portable diffuse pair.
  The pair uses a sampled image at set 0, binding 0,
  and a sampler at set 0, binding 1. An optional uniform at binding 2 carries
  one aligned material record. The 80-byte record uses a port-defined
  std140/HLSL-compatible layout. Its last 16 bytes contain `damageZones`.
  Vulkan maps it to a uniform descriptor.
  D3D12 maps it to `b2`. The executor also accepts one optional frame record at
  binding 3. D3D12 maps this record to `b3`.
  The first 64 bytes contain the sun direction, sun color, ambient color, and
  camera position. Static scenes derive the camera position from the active
  `CameraFrame`. The next 64 bytes contain horizon, sky, and fog values.
  A flag keeps fog disabled for older callers. Static scenes own one bounded
  mutable record and update it before an ordered batch.
  Both backends execute the source-evidenced ambient, directional diffuse,
  direct Blinn specular, and emissive equation. Known-pixel tests cover
  specular enable and removal, light reversal, frame updates, and per-draw
  frame selection. A Vulkan receiver variant appends three sampled D32 maps,
  one nearest sampler, and one 256-byte record at bindings 16-20. It follows
  `public/app.js:2067-2068` for cascade selection and explicit 3x3 PCF. It
  follows `public/app.js:2082` by applying shadow only to direct diffuse and
  specular light. A real stock-scene fixture produces `(3,40,18,255)` with
  zero-depth maps and `(16,116,34,255)` with one-depth maps. The fixture does
  not include Fresnel, reflections, alpha tests, normal maps, detail maps, CSP
  lights, or overlays. All portable material variants apply the production
  WebGL distance-fog equation. A known-pixel test covers enabled fog. The
  D3D12 receiver path is implemented. A Windows SDK and WARP run must still
  verify it. This remains a bounded test ABI, not a complete stock KN5 or CSP
  shader.
  A bounded resolver reads parsed KN5 values and typed CSP
  overrides. It preserves override precedence and WebGL emissive conversion.
  A second bounded ABI executes tangent-space `ksPerPixelNM`. It adds a normal
  image at binding 4 and a normal sampler at binding 5. D3D12 maps these
  resources to `t4` and `s5`. Both backends reconstruct the tangent-space
  normal and calculate the direct sun specular term. This behavior follows the
  production no-maps fallback. The resolver rejects object-space normals and
  nonzero Fresnel levels. A third bounded ABI adds a linear `txMaps` image at
  binding 6 and its sampler at binding 7. D3D12 maps these resources to `t6`
  and `s7`. The shader multiplies specular strength by `maps.r`. It calculates
  the exponent as `max(1, ksSpecularEXP * maps.g + 1)`. Known-pixel tests cover
  zero strength, low exponent, and full exponent. They also cover independent
  samplers and mixed six-binding and eight-binding batches. This ABI requires
  zero Fresnel, so it does not consume `maps.b`. The material handoff uses
  this ABI for `ksPerPixelMultiMap` and `ksPerPixelMultiMap_AT`. It rejects
  active generic detail and object-space normals. The AT profile requires a
  4x target and retains alpha-to-coverage. A fourth bounded ABI executes
  the `ksPerPixelMultiMap_NMDetail` family. This family includes
  `ksPerPixelMultiMap_AT_NMDetail`. It adds `txDetail` at bindings 8 and 9.
  It adds `txNormalDetail` at bindings 10 and 11. The legacy `txDetailNM` name
  is an alias for `txNormalDetail`. The material record includes the detail UV
  multiplier, normal-detail strength, and detail enable value. The AT profile
  retains the production alpha-to-coverage state. Tests cover detail alpha,
  repeated UVs, normal blending, descriptor selection, and mixed resource
  layouts. Fresnel, reflections, shadow cutouts, and executable receiver
  variants for these extended profiles remain staged.
  The resolver rejects oversized CSP shader, blend, depth, and cull strings
  before profile selection.
  A bounded static-scene adapter validates the complete packet set before
  allocation. It uploads each static node once and retains duplicate ordered
  draws. It owns one mutable skinned upload for each skinned packet.
  A frame can supply new world matrices and bone palettes. The adapter computes
  all skinned vertices before it updates the first buffer. It restores exact
  bind-pose bytes when animation is not active. Backend uploads are sequential:
  a failed upload prevents batch submission but can leave earlier successful
  mutable uploads committed. Retrying the complete frame restores consistency.
  A frame can also supply a byte visibility mask in stable packet order. An
  empty mask keeps all packets visible. Other masks must match the prepared
  packet count and contain only zero or one. Hidden packets do not draw or
  update skinned geometry. Shadow submission uses the same mask contract.
  An all-hidden color frame runs a clear-only batch and keeps the viewport.
  It owns one 256-byte material buffer per used material. Duplicate packets
  reuse the same buffer. Count and byte limits bound these allocations. Static
  scenes accept 1x or 4x pipelines. An alpha-to-coverage pipeline requires 4x
  color and matching depth samples. A bounded material handoff derives the
  supported resource layouts, constants, and profile state from KN5 materials.
  The caller must supply explicit SPIR-V or DXIL modules. The handoff does not
  translate stock shader containers. Production packets remain marked as
  staged. This work proves an explicit production-material boundary. It does
  not prove complete scene-rendering parity.
  A bounded stock-scene facade composes the render plan, draw packets, material
  handoff, and static-scene preparation. Its preflight uses linear topology
  traversal. It rejects malformed edges, cycles, and over-budget plans before
  backend allocation. A real SwiftShader pixel test executes the complete
  facade with an embedded DDS texture and owned material and frame records.
  The facade can opt into receiver-capable modules. Its static scene owns the
  receiver sampler and constants. A frame accepts only retained maps from the
  same device, backend, and camera before any mutable update or batch submit.
  A second facade test executes the three-texture MultiMap path. It checks the
  exact bounded `maps.r` and `maps.g` result. It also executes the AT family on
  a 4x target and checks partial resolved coverage. The same tests run on the
  D3D12/WARP CI path. A bounded CSP adapter resolves the exact per-node
  `IS_TRANSPARENT`, `LAYER`, `LOD_IN`, `LOD_OUT`, and `CAST_SHADOWS` fields.
  Later matching sections replace earlier fields in source order. The adapter
  maps KN5 preorder identities to dense scene IDs. It rejects mismatched and
  over-budget input. The facade rejects unknown, duplicate, and non-finite
  overrides before backend allocation.
  The render plan applies CSP LOD, order, pass, and shadow state per node.
  Static scenes can select a pipeline per packet. Nodes that share one material
  can therefore retain different transparent and depth-write states. CSP
  shader, property, and resource changes remain staged. A bounded damage
  adapter resolves the five exact F4 prefix sequences. It uses the first
  pre-order duplicate and stops each sequence at the first missing suffix.
  The adapter creates activity overrides without changing the scene. It also
  creates a complete material override table in final material order. F4
  `damageZones` writes replace CSP values. The one-way `glassDamage` write
  applies to shared material identities after each F4 edge. The adapter rejects
  mismatched identities, invalid material references, and over-budget output.
  It identifies the recovered dirt-zero material branch. The material handoff
  executes this branch with explicit SPIR-V or DXIL modules. It binds
  `txDamage` at 12/13 and `txDamageMask` at 14/15. The handoff rejects nonzero
  dirt. A low-level ABI binds `txDust` at the mutually exclusive bindings 8/9.
  Pixel tests prove that its alpha changes direct diffuse and specular light.
  The stock material handoff selects this ABI for exact six-resource damage
  packets. The caller must label the matching module set as `damage_dust`.
  Legacy five-resource damage packets retain the 12-resource ABI.
  The handoff also rejects
  active detail, sun-specular, Fresnel, and reflection branches. The
  stock-scene facade can resolve this F4 state before allocation.
  It merges node activity and the complete material table into the handoff.
  A real backend test executes broken and intact F4 states through this facade.
  The test executes `txDust` alpha and the recovered normal-alpha attenuation.
  The caller must still resolve surface overlays. A
  bounded workspace adapter maps metadata to merged scene roots. It attaches
  file and auxiliary labels without partial mutation. KN5 conversion computes
  the exact transformed AABB for authored preview-visible vertices. A bounded
  resolver implements half-open workspace LOD ranges and the production FOV
  formula. The viewport prepares one stable packet catalog for all car LODs.
  Each frame derives a packet mask from the live camera. A validated
  `--lod-index` supplies a fixed selection. Camera movement does not replace
  graphics resources at a range boundary. This stable catalog is a port
  optimization for the current live-distance feature. It is not exact stock
  editor behavior. The recovered editor loads and compiles one FBX for each
  LOD menu selection.
  A second bounded resolver
  implements cockpit F3, rim F1, and driver cockpit-hidden state. It does not
  change authored node flags. Show-hidden bypasses authored and preview state.
  It keeps driver suppression, the workspace LOD mask, and mesh LOD. Exact
  mesh isolation bypasses visibility, subtree filters, and the workspace LOD
  mask. It keeps the selected mesh LOD range. These workspace-file rules follow
  `itemPreviewVisible()` and the draw filter in `public/app.js`. They do not claim parity with recovered
  ksNet per-mesh culling. The cockpit pair follows `src/cockpit-preview.js`.
  A separate bounded predicate implements the recovered ksNet per-mesh rule.
  It includes the FOV scale, radius floor, inclusive limits, and zero-limit
  bypass. It also keeps PVS and explicit `NO_CULL` inputs. Render-plan
  integration remains staged until the loader and submission paths are recovered.
  Shadows, reflections, sky, CSP lights, and post-processing remain staged
  with explicit evidence.
- P1 is partial. Bounded readers support KN5 v4/v5/v6, DDS, ACD, INI/CSP,
  KSANIM v1/v2, and KNH. The port also supports byte-stable KN5 writing, VAO
  ZIP decoding, and track surfaces, cameras, and splines. KN5 object creation
  has a configurable aggregate byte budget. Count-driven containers, strings,
  texture payloads, and encryption records consume it before allocation.
  Bounded asset support includes directory/ACD resolution, asset-folder/skin
  indexing, and skin metadata JSON. A CPU bridge resolves external DDS, PNG,
  JPEG, and BMP files
  through explicit `AssetSource` grants. It rejects unsafe, missing, ambiguous,
  and over-budget input. It retains source identity and returns no partial
  table after an error. It creates an owned effective model with opaque image
  names. It rewrites exact material slots and preserves serialized bind points.
  The KN5 baker requires an explicit bind point before it adds a new resource
  slot. It reports and skips a new slot when that value is not known.
  Real backend tests execute ACD DDS and PNG payloads through the stock-scene
  facade. Workspace viewport preparation resolves selected external files
  before GPU allocation. It supplies the owned effective model to Vulkan or
  D3D12 through the shared embedded-texture path. The renderer receives no
  `AssetSource`, grant, or external path. Raw external-file overrides remain
  invalid at the renderer boundary. A staged CSP
  configuration model is implemented. A bounded
  binary/ASCII FBX DOM parser is implemented. ASCII numeric arrays become typed
  FBX arrays after declared-count, finite-value, and allocation-budget checks.
  A bounded FBX conversion subset
  supports static positions, polygon triangulation, hierarchy, local/world
  transforms, first material assignment, and one
  `ByPolygonVertex`/`IndexToDirect` UV layer. The UV path expands seams and
  negates V like `src/fbx-import.js`. It rejects malformed or over-budget UV
  data and diagnoses skinning, animation, embedded images, unsupported layer
  mappings, and advanced transform semantics.
  The conversion budget includes temporary vectors, maps, sets, child lists,
  flattened property views, copied strings, and output containers.
  A bounded VAO binder matches decoded records to mesh views. It uses exact
  names, vertex counts, and first positions. It binds primary, secondary, and
  eligible normal channels. The VAO reader also limits aggregate native
  objects, ZIP metadata, extracted payloads, and record arrays. Alternate
  records and split-AO application remain staged with explicit diagnostics.
  KN5 baking is available. Bounded PNG decoding supports non-interlaced,
  8-bit grayscale, RGB, indexed, grayscale-alpha, and RGBA images. An optional
  bounded decoder supports baseline 8-bit grayscale and RGB JPEG images.
  Builds without libjpeg report this format as unsupported. A bounded BMP
  decoder supports the observed uncompressed 24-bit Windows font layout.
  WebP, progressive JPEG, unsupported PNG variants, and remaining image
  formats are not ported.
- P2 is partial. KN5 conversion feeds the neutral scene snapshot. Material
  binding is explicit. KN5 scene conversion has aggregate limits for native
  records, copied strings, child links, geometry metadata, and path state.
  It validates the complete source tree before it allocates the destination
  snapshot. Track/car workspace manifests assemble
  deterministically. Driver rigs assemble. Bounded project transactions,
  undo/redo/recovery, geometry authoring, KN5 baking, and render/frame-plan
  selection are implemented. Bounded `.apex.json` persistence includes
  material, node, mesh, geometry, collider, damage, and bottom-collider edits.
  It retains bounded identities for the three secondary asset types. Legacy
  edits without an identity remain unbound. The native identity gate rejects a
  missing or mismatched identity when related edits exist. Collider geometry
  edits now use the stable hierarchy paths from the JavaScript project model.
  Separate bounded adapters apply and export collider, damage, bottom-collider,
  and surface edits from immutable baselines. They return no candidate
  asset after a stale identity or malformed edit. Bottom-collider export keeps
  sparse source section numbers and rejects a baseline with rejected sections.
  A bounded native parser supplies this bottom-collider source metadata.
  An in-memory application service now owns the primary KN5, the project
  session, and all secondary baselines. It calculates SHA-256 identities and
  returns owned output without giving filesystem access to the renderer.
  Project load, transactions, undo, redo, and export use this service. A
  one-shot native command exports KN5, CSP, collider KN5, `damage.ini`,
  `colliders.ini`, or `surfaces.ini` output. The command refuses to replace an
  existing file.
  The service accepts current JavaScript projects that omit the primary hash.
  This compatibility requires matching normalized file name, stored nonzero
  size and KN5 version, and observed primary KN5 bytes. Explicit hashes and
  secondary identities stay strict.
  A bounded adapter
  sends KN5 edits to the bake path. It retains diagnostics for CSP-only
  material modes, properties, and resources. Material number strings follow
  JavaScript syntax. Decimal underflow becomes signed zero. Overflow and
  boolean material properties remain invalid.
  The port deterministically exports modeled material and mesh edits to CSP.
  It reports unsupported project categories instead of approximating them.
  Bounded serializers now write `models.ini`, `lods.ini`, and `surfaces.ini`.
  They retain sparse source identities and reject unsafe or unbounded output.
  A bounded application workspace session now accepts caller-granted model and
  manifest bytes, or an existing `AssetSource`, and atomically assembles track
  and car-LOD workspaces into backend-neutral scene snapshots and workspace
  bindings. Missing/ambiguous references, truncated models, and aggregate
  input or scene limits fail closed. GPU and window ownership remain separate.
  A bounded native analog-instrument adapter parses the first
  `RPM_INDICATOR` section. It applies the recovered local positive-Z transform
  to every exact-name match before scene conversion. The native window command
  accepts an explicit configuration file and a finite RPM value. It does not
  clamp the input. LUT instruments remain unsupported and produce an error.
  A bounded native animation adapter samples KSANIM at a finite normalized
  position. It applies tracks only to exact-name KN5 null nodes. Later animated
  duplicate tracks replace earlier tracks. The window command applies animation
  after the RPM transform, so animation wins when both target one node. The
  recovered editor path does not advance or loop this value. It sends the
  slider position on each frame and clamps it to `[0, 1]`. The command uses the
  same fixed-position contract. The original player uses one shared frame
  count. The adapter rejects animated tracks with different frame counts
  instead of sampling them approximately. It exposes a bounded pure pose
  sampler for future interactive controls. When a matched animation changes a
  model with skinned geometry, the window enables CPU skinning for each color
  frame. When a skinned shadow pipeline is available, both passes use the same
  prepared pose. An interactive slider remains staged.
  A bounded hierarchy service validates scene topology before it searches or
  selects nodes. Search uses deterministic preorder and ASCII case-insensitive
  matching. Duplicate names remain distinct. The window command maps selection,
  isolation, hidden-node visibility, and wireframe state to the existing
  backend-neutral viewport request.
  The native INI parser rejects a dangling continuation marker. The reference
  JavaScript parser only reports missing fields for this malformed input.
  A separate renderer-facing workspace viewport bridge now prepares this
  document through the bounded stock-scene facade. It owns one-sample or
  four-sample color and D32 targets. A four-sample viewport also owns one
  single-sample resolve texture. Vulkan and D3D12 resolve into this texture in
  the color submission. The viewport skips the unused CPU readback and presents
  only the resolved texture. It accepts caller-supplied SPIR-V or DXIL modules
  and embedded KN5 textures. The bridge prepares all car LOD packets once. It
  selects them with a frame mask and does not change the session. The bridge
  resolves cockpit and rim preview state during preparation. It never presents
  after a failed draw. The native shell maps bounded SDL mouse gestures and
  portable WASD/QE keycodes to an application-owned camera controller. Motion
  translates the orbit target without exposing platform or backend types.
  The shell can also select a bounded track-camera record and its relative
  spline. Fixed positions and one-shot playback use the production WebGL
  saved-basis mapping. Spline motion changes the eye position and uses
  `MIN_FOV`. This mapping does not claim the game's focused-car target.
  An explicit installed-editor mode uses recovered Catmull-Rom interpolation.
  It uses raw spline points, arc-length mapping, and a target-facing camera.
  It does not apply the production spline rotation or saved-position offset.
  The default mode remains the production WebGL mapping.
  The camera and interpolated AI-spline paths share one source-evidenced
  Catmull-Rom and arc-length implementation.
  Pointer, wheel, and WASD/QE input return control to the orbit camera.
  The shell also evaluates one of the seven stock weather presets with bounded
  sun angles. It uploads sun, sky, horizon, and fog values through the 128-byte
  portable frame record. Its first 64 bytes keep the earlier lighting layout.
  The portable material modules apply the production WebGL distance-fog
  equation after direct lighting. This behavior does not claim recovered stock
  shader bytecode. Directional shadow refresh uses the same sun direction.
  The default preset and angles match the production renderer. Native sky and
  post-processing remain staged.
  Recovered `ksNet.dll` evidence supports the lighting record order.
  `GraphicsManager::updateLightingSetttings` at `0x10046c2c` writes sun
  direction at offset `0x00`, sun RGB at `0x30`, and ambient RGB at `0x40`.
  The installed editor `race.ini` selects `5_light_clouds`. Managed editor UI
  startup controls use values `50`, `180`, and `90`. Therefore, the shell's
  40-degree and 55-degree angles are labeled production WebGL defaults. They
  are not claimed as recovered original-editor startup angles.
  Car damage and bottom-collider authoring have bounded schemas and deterministic output. A
  single-hierarchy car validator checks required nodes, wheels, pivots,
  colliders, and visual-model bounds. Multi-LOD validation is still staged.
  Full validation and the remaining project/export categories are not ported.
- P3–P7 are not complete. The native backends create real resources. DDS and
  bounded PNG data have checked decode and backend upload paths. Validated draw packets include resolved
  resource references, render state, and bone palettes. KN5 material resource
  IDs remain shader bind points. The draw-packet builder resolves textures by
  canonical resource name and rejects missing or ambiguous names. A stricter
  CPU skinning matches `src/skinning.js`. It transforms positions, normals,
  and tangents, then uploads the complete 19-float vertex stream. This path
  also preserves the production bind-pose behavior when animation is not
  active. CSP
  selectors and recovered lighting, shadow, and reflection math feed
  deterministic plans. Source-based camera math matches the `perspective`,
  `lookAt`, and `multiply` functions in `public/app.js`. Separate projections
  define the Vulkan and D3D12 clip-space conversions. The indexed backend
  shader contract uses these camera matrices. The static-scene adapter can
  dispatch bounded resource-free packets and the portable `txDiffuse` pair.
  It can dispatch the bounded `txDiffuse` and `txNormal` pair for
  `ksPerPixelNM`. It can also dispatch the bounded `txDiffuse`, `txNormal`, and
  `txMaps` set. It can dispatch `txDetail` and `txNormalDetail` for the bounded
  `ksPerPixelMultiMap_NMDetail` family. This includes the AT variant. Caller
  tables or embedded KN5 ownership can supply these textures. Preparation
  rejects incomplete or duplicated packet resources before backend allocation.
  It also rejects sRGB maps and malformed maps before allocation. The texture
  budgets include all five source payloads and their decoded pixels. A separate
  aggregate limit bounds host-side preparation tables and retained copies.
  It can also bind a source-valued material record for explicitly authorized
  pipelines. An explicitly authorized pipeline can also bind one source-valued
  frame-light record. Vulkan uses descriptor bindings 2 and 3 for these
  records. D3D12 uses `b2` and `b3`.
  The frame record drives the exact bounded WebGL equation for ambient,
  directional diffuse, direct Blinn specular, and emissive output. The record
  table uses final material order. Preparation copies
  only used records and rejects non-finite values before backend allocation.
  One authority resolves the textures through caller-owned tables in the final KN5
  texture order. A second authority owns the used embedded KN5 textures. This
  authority validates every supported DDS and PNG payload before backend allocation. It decodes
  supported DDS 2D mip chains and bounded PNG images to RGBA8. DDS retains
  explicit sRGB metadata; PNG follows the WebGL no-conversion RGBA8 UNORM contract. Stock
  shader translation and complete material-resource resolution remain staged.
  The external-texture bridge is a separate target above Assets and Render.
  It materializes owned DDS or PNG bytes before the stock-scene handoff. Thus,
  filesystem authority does not enter the renderer library.
  Vulkan and D3D12 create a basic graphics pipeline and execute fixed and
  indexed R16 static and CPU-skinned mesh draws. A real Vulkan pixel test
  proves that a one-bone update moves the triangle. The same test runs on the
  D3D12/WARP CI path. A production WebGL gate uses the same one-bone behavior.
  It reports distinct captures, a `1.0` displacement, and no WebGL errors. The
  indexed path executes only a deliberately
  restricted draw-packet subset. It supports as many as five portable sampled
  images and five samplers. A request-local material record and frame record
  supply the bounded lighting constants. It executes
  the three source-evidenced WebGL blend
  modes. It supports 4x alpha-to-coverage with explicit color and depth sample
  counts. It does not support complete scene resources. It also maps
  the production `GL_LINES` wireframe behavior to native line-list topology.
  An odd final source index does not form a line primitive. It executes
  finite non-identity world and camera
  transforms through an explicit shader contract. It preserves 1x or 4x D32
  depth across synchronous draws and supports explicit test/write state. The
  ordered multi-draw path clears or loads attachments once and submits one
  pass. Each backend resolves a 4x color target once after the final draw. The
  same batch boundary can submit resource-free vertex-only draws to a
  persistent single-sample D32 target without a color allocation or readback.
  The retained static-scene path filters the equivalent
  `DrawPacket::flags.cast_shadows` state. It submits the supported opaque and
  non-opaque static subset to exactly three owned shader-readable D32 maps.
  Vulkan binds these maps through the portable receiver ABI. Callers can
  instead select the recovered 208-byte stock `cbShadowMaps` layout for
  matching shader modules.
  This choice is explicit and does not infer an ABI from shader bytecode.
  D3D12 creates typeless depth resources and sampled views. Its descriptor
  execution is implemented. The executable fixture still needs a Windows WARP
  verification build. The
  device API directly uploads immutable, one-layer BC1, BC2, BC3, BC4 UNORM,
  BC5 UNORM, BC5 SNORM, BC6H, and BC7 sampled textures. The upload path
  validates compressed block rows before allocation.
  It maps legacy D3DFMT_R32F DDS data to R32_SFLOAT and keeps the source bits.
  This exact path is limited to immutable, one-layer sampled resources. It
  does not add an RGB material binding or a CPU RGBA8 approximation.
  Vulkan and D3D12 query format support before image creation. The generic
  device API accepts BC4 UNORM, BC5 UNORM, and BC5 SNORM uploads. BC4 is scalar
  and remains outside the RGB material bindings. The normal-map ABI remains
  RGBA8/BGRA8 because its shader reads RGB and does not reconstruct BC5 Z.
  The embedded static-scene path still uses its exact CPU decode.
  The static-scene path requires explicit backend shader bytecode. The bounded
  material handoff selects modules by material or shader family. It derives
  the eight-binding or 12-binding layout, material constants, A2C state,
  culling, and depth state. The stock-scene facade composes this handoff with visibility, LOD,
  stable draw ordering, CSP mesh state, and packet validation for a
  pre-resolved snapshot.
  It does not execute stock KN5 shader containers. Vulkan can create a
  headless or SDL3-backed native surface and swapchain. Vulkan can present a
  completed offscreen color attachment with the same size and format. The
  port contains a bounded Windows D3D12 swapchain path pending its Windows/WARP
  verification gate, but does not provide full stock-shader golden-image parity.

The production WebGL material gate uses the synthetic `BC7_PLANE` scene. The
baseline screenshot SHA-256 is
`4b744f36ad118f58b1a13ce4607a48f88d1501f275c705cd33849a842369ec38`.
A CSP override changed `ksAmbient`, `ksDiffuse`, and `ksEmissive`. The WebGL
state reported the exact override values and no GL error. Its screenshot
SHA-256 is
`bb82fef5bbca2d9e0911c34f1e0251c2bcb664f59e073a3a8162f329fe2c49db`.
This gate proves override consumption. Source inspection remains the authority
for absent-property defaults and emissive normalization.

The production WebGL lighting gate uses `GEO_Fabric1` from the repository car.
All 63 embedded textures were ready, and WebGL reported no errors. The capture
at a sun height of 55 degrees had hash `9eb6de45a8e0c07f`. The capture at 10
degrees had hash `cb993d752a7af5db`. This difference proves that the production
lighting path responds to the sun direction. It does not prove native pixel
parity for the complete `ksPerPixelMultiMap_NMDetail` shader.

The production WebGL maps gate uses `GEO_STEER.002` and material `Swheel` from
the repository car. This material uses `ksPerPixelMultiMap`. The active
resources are `txDiffuse`, `txNormal`, and `txMaps`. Its detail and Fresnel
controls are zero. The capture at a sun height of 55 degrees had hash
`f0c4a1d3753fa220`. The capture at 10 degrees had hash `cea4a0300ce8c9d4`.
WebGL reported no errors, and all 63 textures were ready. This gate identifies
an exact source fixture for the bounded maps equation. The native stock-scene
facade now executes this bounded three-texture family. Stock shader translation
and full `ksPerPixelMultiMap` parity remain staged. The gate does not prove
parity for Fresnel, reflection, detail, or `ksPerPixelMultiMap_NMDetail`.

The production WebGL detail-stack gate uses `GEO_Fabric1`, material `Fabric1`,
and shader `ksPerPixelMultiMap_NMDetail`. Its active resources include
`txDetail` and `txNormalDetail`. Its detail controls are `1`, `50`, and `1`.
All 63 textures were ready. WebGL reported no errors. Sun heights of 55 and 10
degrees produced hashes `7f8a40c621fa8c79` and `162274111c068324`.

The production alpha-to-coverage gate selects `GEO_SEAT_SUB1` and material
`Seat`. The shader is `ksPerPixelMultiMap_AT_NMDetail`. The scene reported
five alpha-to-coverage profiles and 63 ready textures. WebGL reported no
errors. The capture hash was `3c76ed8c4207647b`. The PNG SHA-256 was
`7145342632b9adee7ad61948c15c5d8ba0026795dd9c1e4405d47f29689065db`.
The final WebGL state is false because the post-process pass disables
alpha-to-coverage. Thus, this gate proves profile selection and an error-free
production capture. The diffuse texture has zero alpha in this capture. Thus,
the screenshot is not a useful color-parity reference. A controlled native
test proves partial 4x coverage with all five detail-stack textures. This test
runs on Vulkan and the D3D12/WARP CI path. It uses source-equivalent modules,
not recovered stock bytecode.

The production compressed-texture gate selects `ford13_body_SUB2` and material
`ford13_skin`. Its color resources use BC1 and BC3 mip chains. All 63 textures
were ready. WebGL reported no errors. The capture hash was
`02f639e082e1e5b2`. The PNG SHA-256 was
`c64caf51986b652c5cd71e6093e7311a281bdf10924cf7eb05afbe591692497f`.
This gate proves the production fixture and WebGL path. Native pixel tests
cover direct BC1, BC3, and BC7 upload and sampling, plus the BC5 upload and
capability boundary. This host had no usable Vulkan device, and the D3D12 test
still requires a Windows run.

The production LOD gate uses `data/lods.ini` from the repository car. LOD0
selected index 0 and produced state hash `6029214d5cdb79d0`. Its screenshot
SHA-256 was
`2080d4b7a777ddaeb7d968ae24f91c5f90f50b9d395f8f129448fa663a6571ae`.
LOD1 selected index 1 and produced state hash `fac54063a7647c2d`. Its
screenshot SHA-256 was
`5ccf52cd2e49cddd524d96f09c7c35a2e3f437c755101fca4f78d33617a00af2`.
Both captures loaded all 63 textures and all 158 stock profiles. WebGL
reported no errors. The selected wheel changed from 1,164 to 212 triangles.
This gate proves production LOD selection. Native workspace LOD/FOV selection
uses the same half-open ranges and FOV formula. Native tests cover automatic,
forced, overlapping, gapped, auxiliary, and track-camera cases. The native
viewport retains all prepared car LOD packets. It selects packets with a
frame-time byte mask and keeps their order stable. KN5 conversion supplies the
production preview AABB from transformed, preview-visible vertices. Real LFS
tests also reject the lower LOD with a non-finite vertex.
The failed open returns no partial workspace and reports the exact source
offset.

The production preview-state gate uses the repository car and Sepang track.
The car loaded all 63 textures. Its one HR root contains 48 meshes. Its one LR
root contains three meshes. Low and high cockpit states produced distinct
hashes `b31f311aa1602378` and `0a5057759735f01e`. The four regular and four
blurred rim roots also produced distinct states. Their hashes were
`24775ec55e0532cf` and `ba812a90bc832fcf`. Sepang loaded all 130 textures.
Show-hidden increased its visible mesh count from 877 to 1,070. The off and on
hashes were `768c5cb43a93cebc` and `b44c2b261a48c1dd`. Every capture reported
WebGL error zero. The repository has no shared driver KN5 fixture. Bounded
native tests therefore prove driver-name matching and subtree suppression.

DDS BC7 has a bounded CPU decoder with differential fixtures for all eight
modes. Both native backends can upload BC7 blocks when the adapter reports
format support. BC5 is also available through the generic device path. The
normal-map resource ABI remains unchanged. The generic device path uploads
BC6H UF16 and SF16 blocks on a capable adapter. The checked upload
planner supports DX10 2D arrays and cubemaps. It also converts legacy RGB24 to
RGBA8 without a color change. The generic device path uploads legacy
D3DFMT_R32F as a scalar R32_SFLOAT texture. Other D3D9 float layouts remain
unsupported. The embedded static-scene path uses the bounded
CPU decoder for supported 2D mip chains. Each decode receives the remaining
aggregate byte budget before it allocates output. Upload plans also reject
subresource-entry floods before iteration. This path retains the DDS sRGB flag.
It rejects arrays, cubemaps, BC6H, and legacy D3D9 float data. No portable
BC6H CPU decoder is available. This portable
decode is still the embedded static-scene path. The separate device path
uploads BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 blocks directly. It does not prove
stock-shader parity.

## Source-module mapping

| Current module | Port boundary and retained responsibility |
| --- | --- |
| `src/kn5.js`, `src/kn5-write.js`, `src/kn5-bake.js` | Checked KN5 v4/v5/v6 read, write, visibility, CSP KN5ENC inspection, project baking |
| `src/dds.js` | DDS/DXGI/D3D9 headers, raw formats, BC1/2/3/4/5 decode, BC6H/BC7 capability/fallback |
| `src/fbx-import.js` | Binary/ASCII FBX geometry, materials, UVs, skinning, images, clips, KN5 conversion |
| `src/ksanim.js`, `src/knh.js` | KSANIM v1/v2 parse/sample/serialize and recursive driver-base hierarchy |
| `src/acd.js`, `src/asset-files.js` | ACD browsing/decryption, virtual files, safe paths, skins, animations, external resources |
| `src/kn5-workspace.js`, `src/workspace-authoring.js` | Track models, dynamic objects, car LODs, placement, manifest editing/export |
| `src/track-validation.js`, `src/surface-authoring.js` | Surfaces parsing, runtime binding/audits, 14-field editing and export |
| `src/track-cameras.js` | Camera v3 validation, spline CSV rotation/sampling/playback |
| `src/car-validation.js` | Collider/bottom-box checks, geometry budgets, 14 SDK hierarchy and wheel/pivot audits |
| `src/driver-workspace.js`, `src/skinning.js` | Driver3D/KNH pose and hiding, shared driver resolution, CPU skinning math |
| `src/node-authoring.js`, `src/geometry-authoring.js` | Stable node paths, transforms, topology repair, normals/tangents/bounds |
| `src/editor-project.js` | Versioned `.apex.json`, recovery state, CSP/project serialization and edit counts |
| `src/csp-config.js`, `src/custom-emissive.js` | CSP INI selectors/LUTs/templates, shader/material overrides, emissive atlas operations and vehicle inputs |
| `src/vao-patch.js` | VAO ZIP versions, bounds/CRC checks, binding and AO conversion |
| `src/shader-profiles.js` | Stock shader profiles, render-state defaults, material diagnostics |
| `src/lighting.js`, `src/shadows.js` | Weather, sun, exposure/tone-map/bloom, directional/local shadow math and policies |
| `src/reflections.js` | Cubemap capture selection, Fresnel, blur/mip heuristic, procedural fallback |
| `src/seasons.js`, `src/rain-fx.js` | Seasonal LUT/color behavior and RainFX classification/wet response |
| `src/grass-fx.js`, `src/csp-wind.js`, `src/csp-noise.js`, `src/csp-occlusion.js` | Grass targets/instances/material response, wind map, deterministic noise, light occlusion |
| `public/app.js` | Transitional command/controller layer and WebGL reference renderer; split into UI, scene service, and render graph rather than porting as one file |
| `public/index.html`, `public/app.css` | Transitional UI surface; every command/control needs a native or retained-web equivalent |
| `src/server.js`, `desktop/main.cjs` | Loopback server, route allowlist, CSP headers, sandboxed desktop shell and lifecycle |

## Phased parity roadmap

Each phase has an exit gate. Later phases must not silently drop behavior from
an earlier phase.

### P0 — Build, diagnostics, and platform seam

Set up CMake, compiler warnings, sanitizers, unit-test runners, reproducible
build metadata, portable paths, structured diagnostics, and a backend-neutral
`RenderDevice`/resource lifetime interface. Add a native window/input seam and
keep the existing browser build runnable.

Exit evidence: clean builds on Linux and Windows, tests run without the UI,
no renderer-to-Node dependency, and a minimal Vulkan plus DirectX device
probe reports capabilities without creating application-specific resources.

### P1 — Safe format and asset core

Port the source-module mapping for KN5, DDS, ACD, KSANIM, KNH, FBX, VAO, INI,
surfaces, cameras, and asset indexing. Use immutable byte spans and explicit
limits. Port representative fixtures and all malformed/truncated tests before
adding authoring or rendering behavior.

Exit evidence: parser round trips match current outputs; every parser has
valid, truncated, malformed-count/offset, and unsupported-version tests;
protected KN5 and packed ACD policies are enforced.

### P2 — Scene, workspaces, and authoring

Implement scene graph ownership, recursive visibility, static/skinned meshes,
materials/resources, track/car assembly, LOD selection, physics/camera/driver
audits, stable node paths, geometry repair, transaction history, recovery, and
project/KN5/CSP/INI export.

Exit evidence: existing authoring smoke scenarios pass against the C++ model;
edit, undo, redo, recovery, reload, and export produce equivalent state and
diagnostics. Hierarchy-only edits remain excluded from CSP export as today.

### P3 — Baseline rendering on both backends

Implement geometry upload, texture/mipmap resources, stock shader profiles,
material state ordering, camera controls, visibility/LOD, skinning, normal and
detail maps, multilayers, transparency, alpha-to-coverage, culling, depth,
wireframe, isolation, and selection overlays. CPU skinning must keep the
production 19-float layout and bind-pose behavior.

Exit evidence: Vulkan and DirectX render the KN5/FBX fixture set with no GPU
validation errors; production WebGL checks still pass; capture hashes and
numeric material/visibility diagnostics are within approved tolerances.

### P4 — Native lighting and scene composition

Port weather presets, sun, fog, HDR/MSAA, auto/manual exposure, tone mapping,
bloom/glare/dither, directional cascades, CSP local shadows, cubemap capture,
Fresnel/reflections, HDR scene resolve and refraction.

Exit evidence: per-effect golden images and deterministic metadata for cars,
tracks, showroom/reflection environments, transparent materials, and shadow
casters on both native backends plus the reference WebGL path.

### P5 — CSP authoring effects

Port CSP selectors, templates, condition LUTs, material replacements,
CustomEmissive/Multi, SelfLight/point/spot/line lights, popup headlights,
occlusion/fades/receiver modes, VAO, seasons, RainFX, and vehicle input
bindings.

Exit evidence: fixture configs produce matching target selection, diagnostics,
light lists, material overrides, atlas coordinates, and rendered captures;
unsupported operations are explicit and attributed to their source.

### P6 — GrassFX, wind, and high-cost effects

Port deterministic GrassFX parsing, target generation, atlas selection,
multilayer/material responses, topology/normal handling, shadows, occlusion,
wind/noise, wetness, deformation targets, and bounded authoring fallback.

Exit evidence: deterministic instance/target hashes, resource-size limits,
out-of-bounds fallback tests, and backend golden captures. GPU-density or
performance improvements must not change the reference result without an
approved fidelity note.

### P7 — Native UI, packaging, and release proof

Move the command surface to `apex-ui` or retain the browser UI over a narrow
local bridge. Add project recents only if implemented with the same security
and persistence contract. Package Linux, Windows, and macOS targets; DirectX
is an additional Windows backend, not a portability requirement for other
systems.

Exit evidence: desktop smoke tests, parser corpus, round trips, golden images,
WebGL production checks, backend validation-layer checks, and target-native
package launches all pass.

## Feature-parity matrix and acceptance evidence

| Area | Must retain | Minimum proof before declaring parity |
| --- | --- | --- |
| Asset I/O | KN5 v4/v5/v6, DDS, bounded PNG, FBX, KSANIM, KNH, ACD, VAO, CSP/INI and safe external assets | Fixture round trips; malformed/truncated PNG and parser tests; diagnostics compare |
| Scene model | Hierarchy, active/visible/renderable rules, static/skinned meshes, materials, resources, bounds | Structural snapshots and visibility tests |
| Track workflow | `models*.ini`, placement/dynamic objects, surfaces, cameras/splines, physics overlay | Manifest/surface export equivalence and assembled-layout audits |
| Car workflow | ACD/LODs, skin overrides, collider checks, SDK node audit, driver pose/cockpit mode | LOD/skin/driver/collider fixtures and inspector diagnostics |
| Authoring | Node/material/geometry/workspace/surface edits, undo/redo, recovery, project persistence | Real UI or command-level edit/undo/reload/export smoke test |
| Output | KN5 copy, CSP, `models.ini`, `lods.ini`, `surfaces.ini`, `.apex.json`, KSANIM | Byte/semantic comparison and protected-input policy tests |
| Stock rendering | Shader defaults/overrides, maps, blend/depth/cull, transparency, LOD, skinning | WebGL/Vulkan/DirectX golden images and GPU validation |
| Lighting | Weather, sun/fog, exposure, tone map, bloom, directional/local shadows | Numeric lighting fixtures plus captures at fixed camera/state |
| CSP rendering | Templates, emissives, lights, occlusion/fades, VAO, seasons, RainFX | Config fixture evaluation and per-effect capture/hash evidence |
| GrassFX/wind | Deterministic generation, targets, atlas/material response, wind/noise, shadows | Instance/target hashes, bounds tests, and captures |
| Platform | Loopback-only server, sandboxed renderer, no external navigation/permissions | Desktop smoke test and route/security-header assertions |

## Parser-security gate for every change

Before merging a parser or serializer change, add or update:

1. a valid representative fixture;
2. a truncated fixture at each variable-length boundary touched;
3. an invalid count/offset/length fixture that would have caused an unsafe
   allocation or read;
4. an unsupported version/type fixture;
5. a test proving source attribution and offset/context in the error;
6. a test proving no partial mutation is exposed after failure.

Archive parsers additionally need traversal/path, duplicate, CRC, compressed
size, and decompressed-size limits. Texture decoders need per-mip bounds and
dimension/format limits. Scene readers need child-count and recursion limits.

## Rendering-evidence gate

For each visible rendering change, record the behavior source (stock shader
rule, CSP config behavior, fixture, or disassembly), add deterministic inputs,
and run:

- the existing production WebGL browser smoke check;
- Vulkan validation and capture checks;
- DirectX debug-layer/capture checks on Windows;
- a golden image or numeric comparison at a fixed camera, weather, animation,
  and asset set.

If a backend cannot implement a path, report the limitation in diagnostics and
the parity matrix. Do not silently substitute a visually different path.

## Criteria before WebGL removal

WebGL remains the reference and compatibility fallback until all of the
following are true:

- P1–P7 exit evidence is complete for every row in the parity matrix;
- all current parser, authoring, server, and browser smoke tests have C++ or
  retained-UI equivalents;
- representative official/mod fixtures round-trip through the C++ pipeline;
- Vulkan and DirectX pass production validation and approved golden-image
  tolerances on supported targets;
- malformed/truncated input coverage exists for every port-side parser;
- projects and exports created by the current editor load with equivalent
  state and diagnostics in the C++ editor;
- the native desktop shell preserves loopback, sandbox, path, navigation, and
  permission invariants;
- unsupported CSP behavior is documented and intentionally accepted by the
  release decision;
- at least one release candidate has been packaged and smoke-tested on each
  supported OS/backend combination.

Until then, keep a feature flag or separate executable for WebGL reference
comparison. Removal is a release decision backed by the evidence above, not a
consequence of the C++ renderer merely launching.
