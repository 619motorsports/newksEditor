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

KN5, KSANIM, KNH, ACD, CSP INI, VAO patches, FBX, DDS, and mod files are
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
2. `apex-formats`: KN5, DDS, FBX, KSANIM, KNH, ACD, INI/CSP, surfaces,
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

As of 2026-08-21, the native port is additive and the JavaScript/WebGL editor
remains unchanged and feature-complete.

- P0 is partial. CMake, strict warnings, sanitizers, cross-platform CI,
  portable security checks, and a native inspection CLI are implemented. The
  port has a backend-neutral device API. Vulkan and D3D12 implement headless
  devices, buffers, 2D textures, samplers, and shader modules. Both backends
  also implement bounded synchronous RGBA8/BGRA8 texture clear/readback. Each
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
  indexed draws. The executable main-pass subset uses the source-evidenced
  `LESS` depth comparison. It also executes explicitly blended packets. The
  ordinary alpha, multiply, and transparent-as-black factors match
  `applyItemRenderState` in `public/app.js`. The indexed path supports 4x
  multisample targets and alpha-to-coverage. Each backend resolves the final
  color to one sample before readback. Indexed wireframe uses
  line-list topology over the source index stream. This matches the production
  `GL_LINES` selection in `public/app.js`. It does not use polygon-line
  rasterization. Both native backends execute this topology in single draws and
  ordered batches. A bounded batch preflights
  every request, preserves packet order, and records all draws in one pass with
  one final readback. The backend also executes one explicitly authorized
  portable diffuse pair. The pair uses a sampled image at set 0, binding 0,
  and a sampler at set 0, binding 1. An optional uniform at binding 2 carries
  one aligned material record. Its first 64 bytes use the current WebGL
  `ksPerPixel` defaults in a port-defined std140/HLSL-compatible layout.
  Vulkan maps it to a uniform descriptor.
  D3D12 maps it to `b2`. The executor also accepts one optional frame record at
  binding 3. D3D12 maps this record to `b3`.
  The first 64 bytes contain the sun direction, sun color, ambient color, and
  camera position. Static scenes derive the camera position from the active
  `CameraFrame`. They own one bounded mutable record and update it before an
  ordered batch.
  Both backends execute the source-evidenced ambient, directional diffuse,
  direct Blinn specular, and emissive equation. Known-pixel tests cover
  specular enable and removal, light reversal, frame updates, and per-draw
  frame selection. This fixture does not include Fresnel, reflections, fog,
  shadows, alpha tests, normal maps, detail maps, CSP lights, or overlays.
  This remains a bounded test ABI, not a complete stock KN5 or CSP shader.
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
  zero Fresnel, so it does not consume `maps.b`. A fourth bounded ABI executes
  the `ksPerPixelMultiMap_NMDetail` family. This family includes
  `ksPerPixelMultiMap_AT_NMDetail`. It adds `txDetail` at bindings 8 and 9.
  It adds `txNormalDetail` at bindings 10 and 11. The legacy `txDetailNM` name
  is an alias for `txNormalDetail`. The material record includes the detail UV
  multiplier, normal-detail strength, and detail enable value. The AT profile
  retains the production alpha-to-coverage state. Tests cover detail alpha,
  repeated UVs, normal blending, descriptor selection, and mixed resource
  layouts. Fresnel, reflections, shadow cutouts, and shadows remain staged.
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
  The same test runs on the D3D12/WARP CI path. The caller must first resolve
  workspace LOD/FOV, preview modes, per-node CSP overrides, and surface
  overlays. Shadows, reflections, sky, CSP lights, and post-processing remain
  staged with explicit evidence.
- P1 is partial. Bounded readers support KN5 v4/v5/v6, DDS, ACD, INI/CSP,
  KSANIM v1/v2, and KNH. The port also supports byte-stable KN5 writing, VAO
  ZIP decoding, and track surfaces, cameras, and splines. KN5 object creation
  has a configurable aggregate byte budget. Count-driven containers, strings,
  texture payloads, and encryption records consume it before allocation.
  Bounded asset support includes directory/ACD resolution, asset-folder/skin
  indexing, and skin metadata JSON. A staged CSP configuration model is implemented. A bounded
  binary/ASCII FBX DOM parser is implemented. A bounded FBX conversion subset
  supports static positions, polygon triangulation, hierarchy, local/world
  transforms, and first material assignment. It rejects or diagnoses skinning,
  animation, embedded images, layer mappings, and advanced transform semantics.
  The conversion budget includes temporary vectors, maps, sets, child lists,
  flattened property views, copied strings, and output containers.
  A bounded VAO binder matches decoded records to mesh views. It uses exact
  names, vertex counts, and first positions. It binds primary, secondary, and
  eligible normal channels. The VAO reader also limits aggregate native
  objects, ZIP metadata, extracted payloads, and record arrays. Alternate
  records and split-AO application remain staged with explicit diagnostics.
  KN5 baking is available. The remaining image formats are not ported.
- P2 is partial. KN5 conversion feeds the neutral scene snapshot. Material
  binding is explicit. KN5 scene conversion has aggregate limits for native
  records, copied strings, child links, geometry metadata, and path state.
  It validates the complete source tree before it allocates the destination
  snapshot. Track/car workspace manifests assemble
  deterministically. Driver rigs assemble. Bounded project transactions,
  undo/redo/recovery, geometry authoring, KN5 baking, and render/frame-plan
  selection are implemented. Bounded `.apex.json` persistence is implemented.
  The port deterministically exports modeled material edits to CSP. It reports
  unsupported project categories instead of approximating them. Car damage and
  bottom-collider authoring have bounded schemas and deterministic output. A
  single-hierarchy car validator checks required nodes, wheels, pivots,
  colliders, and visual-model bounds. Multi-LOD validation is still staged.
  Full validation and the remaining project/export categories are not ported.
- P3–P7 are not complete. The native backends create real resources. DDS data
  has a checked backend upload plan. Validated draw packets include resolved
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
  authority validates every DDS payload before backend allocation. It decodes
  supported 2D mip chains to RGBA8 and retains explicit sRGB metadata. Stock
  shader translation and complete material-resource resolution remain staged.
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
  device API directly uploads immutable, one-layer BC1 and BC3 sampled
  textures. The upload path validates compressed block rows before allocation.
  Vulkan and D3D12 query format support before image creation. Direct BC5
  normal uploads remain staged because the bounded shader reads three normal
  channels. The embedded static-scene path still uses its exact CPU decode.
  The static-scene path requires explicit backend shader bytecode. The bounded
  material handoff selects modules by material or shader family. It derives
  the 12-binding layout, material constants, A2C state, culling, and depth
  state. The stock-scene facade composes this handoff with visibility, LOD,
  stable draw ordering, and packet validation for a pre-resolved snapshot.
  It does not execute stock KN5 shader containers. The port does not create
  windows or swapchains. It does not provide full golden-image parity.

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
an exact source fixture for the bounded maps equation. Production packets and
stock shader translation remain staged. The gate does not prove native pixel
parity for `ksPerPixelMultiMap` or `ksPerPixelMultiMap_NMDetail`.

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
separately prove direct BC1 and BC3 upload and sampling.

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
remains outside the bounded stock-scene facade.

DDS BC7 has a bounded CPU decoder with differential fixtures for all eight
modes. BC6H remains explicit and requires a GPU path. The checked upload
planner supports DX10 2D arrays and cubemaps. It also converts legacy RGB24 to
RGBA8 without a color change. The embedded static-scene path uses the bounded
CPU decoder for supported 2D mip chains. Each decode receives the remaining
aggregate byte budget before it allocates output. Upload plans also reject
subresource-entry floods before iteration. This path retains the DDS sRGB flag.
It rejects arrays, cubemaps, BC6H, and legacy D3D9 float data. This portable
decode is still the embedded static-scene path. The separate device path
uploads BC1 and BC3 blocks directly. It does not prove stock-shader parity.

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
| Asset I/O | KN5 v4/v5/v6, DDS, FBX, KSANIM, KNH, ACD, VAO, CSP/INI and safe external assets | Fixture round trips; malformed/truncated tests; diagnostics compare |
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
