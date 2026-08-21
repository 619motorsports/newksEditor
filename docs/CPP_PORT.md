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
  backend validates a pipeline and executes fixed and indexed R16 static-mesh
  draws with readback. The indexed path uses immutable vertex and index
  buffers. A bounded adapter validates and uploads 11-float KN5 static
  geometry. It rejects malformed geometry, unsafe indices, and invalid packet
  ranges before allocation. A 128-byte draw-matrix contract binds world and
  camera transforms. Vulkan uses vertex push constants. D3D12 uses root
  constants at `b0`. Backend-specific camera frames make the clip-space
  conversion explicit. Both backends also create persistent single-sample D32
  attachments. Explicit load and clear controls retain color and depth across
  indexed draws. The executable main-pass subset uses the source-evidenced
  `LESS` depth comparison. A bounded batch preflights every request, preserves
  packet order, and records all draws in one pass with one final readback. The
  backend still accepts only resource-free opaque packets. A bounded
  static-scene adapter validates the complete packet set before allocation.
  It uploads each referenced node once and retains duplicate ordered draws.
  Caller-supplied SPIR-V or DXIL pipelines authorize only their local requests.
  Production packets remain marked as staged. This work proves basic pipeline
  creation, geometry binding, ordered submission, and rasterization. It does
  not prove scene-rendering parity.
- P1 is partial. Bounded readers support KN5 v4/v5/v6, DDS, ACD, INI/CSP,
  KSANIM v1/v2, and KNH. The port also supports byte-stable KN5 writing, VAO
  ZIP decoding, and track surfaces, cameras, and splines. Bounded asset support
  includes directory/ACD resolution, asset-folder/skin indexing, and skin
  metadata JSON. A staged CSP configuration model is implemented. A bounded
  binary/ASCII FBX DOM parser is implemented. A bounded FBX conversion subset
  supports static positions, polygon triangulation, hierarchy, local/world
  transforms, and first material assignment. It rejects or diagnoses skinning,
  animation, embedded images, layer mappings, and advanced transform semantics.
  KN5 baking is available. The remaining image formats are not ported.
- P2 is partial. KN5 conversion feeds the neutral scene snapshot. Material
  binding is explicit. Track/car workspace manifests assemble
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
  CPU reference skinning path bridges KN5 scenes to the render contract. CSP
  selectors and recovered lighting, shadow, and reflection math feed
  deterministic plans. Source-based camera math matches the `perspective`,
  `lookAt`, and `multiply` functions in `public/app.js`. Separate projections
  define the Vulkan and D3D12 clip-space conversions. The indexed backend
  shader contract uses these camera matrices. The static-scene adapter can
  dispatch a bounded resource-free packet set. Stock shader translation and
  material-resource binding remain staged.
  Vulkan and D3D12 create a basic graphics pipeline and execute fixed and
  indexed R16 static-mesh draws. The indexed path executes only a deliberately
  restricted draw-packet subset. It has no descriptors, scene resources,
  blending, or alpha-to-coverage. It executes finite non-identity world and
  camera transforms through an explicit shader contract. It preserves D32
  depth across synchronous draws and supports explicit test/write state. The
  ordered multi-draw path clears or loads attachments once and submits one
  pass. The static-scene path requires explicit backend shader bytecode. It
  does not execute stock KN5 shader packages. The port does not create windows
  or swapchains, bind complete materials, or provide golden-image parity
  evidence.

DDS BC7 now has a bounded CPU decoder covered by differential fixtures for all
eight modes. BC6H remains recognized and explicitly GPU-required. The port
does not substitute an approximate CPU decoder. The checked backend upload
planner supports DX10 2D arrays and cubemaps. It also converts legacy RGB24 to
RGBA8 without changing color values. The CPU decoder can still reject arrays
and cubemaps. The upload planner rejects 1D/3D textures, BC6H, and legacy D3D9
float data. These limits must stay visible until their roadmap gates are met.

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
wireframe, isolation, and selection overlays.

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
