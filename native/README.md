# Native port seed

This directory contains the incremental C++20 port of Apex Editor. The
JavaScript/WebGL application remains the feature-complete reference while the
native implementation works through the parity gates in
[`docs/CPP_PORT.md`](../docs/CPP_PORT.md).

The native port has shared bounded input utilities. It reads KN5 v4/v5/v6,
DDS, ACD, VAO, ordered INI/CSP, KSANIM v1/v2, and KNH. It writes KN5 files
without byte changes when the model is unchanged. KN5 parsing also has an
aggregate native-object budget. Counts, strings, texture payloads, and
encryption records consume this budget before allocation. DDS includes bounded
BC7 CPU decode. Its upload planner supports DX10 2D arrays, cubemaps, and exact
RGB24 to RGBA8 conversion. FBX support includes a bounded binary/ASCII DOM and
a static-geometry conversion subset.

The VAO core binds decoded records to non-owning mesh views. It uses the exact
name, vertex count, and first-position gate from `src/vao-patch.js`. It binds
primary, secondary, and eligible normal channels. Explicit limits protect all
validation and value copies. Alternate records and split-AO application remain
staged and produce diagnostics.

The port supports surfaces, cameras, splines, workspaces, asset folders, skin
indexes, and skin metadata. It includes staged CSP evaluation and KN5 scene
conversion. Scene conversion validates the complete source tree before
snapshot allocation. Aggregate limits cover records, copied strings, child
links, geometry metadata, and path state. The render model includes material
profiles, bindings, and validated draw packets. Driver rigs include CPU
reference skinning. The authoring model includes transactions, bounded
project/CSP serialization, geometry edits, KN5 baking, car damage data, bottom
colliders, and initial car validation.

The render library includes DDS upload, recovered lighting math, shadow math,
reflection math, camera matrices, and frame-pass plans. The camera code uses
the `perspective`, `lookAt`, and `multiply` formulas from `public/app.js`.
It defines the Vulkan and D3D12 clip-space conversions separately. Its
backend-neutral contract supports headless Vulkan and Windows D3D12. Both
backends implement devices, buffers, 2D textures, samplers, shader modules,
and clear/readback operations. Portable validation protects the desktop
boundary. Backend API types stay inside `src/render`. Format and authoring
libraries do not depend on a graphics API.

## Build and test

CMake 3.25 or newer and a C++20 compiler are required. Vulkan is enabled when
the SDK headers and loader are available. D3D12 is enabled only on Windows.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Probe an available backend or inspect a KSANIM file:

```sh
out/native/dev/native/apex-native --backend vulkan
out/native/dev/native/apex-native --inspect-kn5 model.kn5
out/native/dev/native/apex-native --inspect-dds texture.dds
out/native/dev/native/apex-native --inspect-acd car_directory data.acd
out/native/dev/native/apex-native --inspect-ini ext_config.ini
out/native/dev/native/apex-native --inspect-vao car.vao-patch
out/native/dev/native/apex-native --inspect-ksanim animation.ksanim
```

An unavailable SDK, driver, validation layer, or adapter is reported
explicitly. It is never presented as a successful backend initialization.
The current backends initialize devices and create/upload buffers and bounded
2D textures, samplers, and immutable shader modules. They execute bounded
RGBA8/BGRA8 texture clears and canonical RGBA8 readback. They also validate a
pipeline and execute fixed and indexed R16 static-mesh draws with readback.
The device API uploads immutable, one-layer BC1 and BC3 sampled textures.
It validates each block row before allocation. Vulkan and D3D12 query format
support before they create a compressed image. Other block formats remain
outside this direct-upload path.
The indexed path validates 11-float KN5 static geometry before allocation.
It uploads immutable vertex and R16 index buffers. The adapter rejects
malformed geometry, non-finite values, invalid packet ranges, and unsafe
indices. The execution path accepts finite world and camera transforms.
Vulkan uses a 128-byte vertex push-constant block. D3D12 uses an equivalent
root-constant block. Each camera must use the clip-space convention for its
backend. Both backends create persistent 1x or 4x D32 attachments.
Indexed requests use explicit color-load and depth-clear controls. The
source-evidenced main path uses `LESS` depth testing. The execution path accepts
opaque and explicitly blended packets. The packet and pipeline blend flags must
match. Ordinary alpha, multiply, and transparent-as-black factors match
`applyItemRenderState` in the production WebGL renderer. Vulkan and D3D12 apply
these factors in single draws and ordered batches. The indexed path supports
4x multisample targets and alpha-to-coverage. Each backend resolves the final
color to one sample before readback. Indexed wireframe
uses line-list topology over the source index stream. This behavior matches the
production `GL_LINES` selection in `public/app.js`. It does not use polygon-line
rasterization. Vulkan and D3D12 apply this topology in single draws and ordered
batches. A request-local
authority can enable one portable diffuse resource pair: a sampled image at set
0, binding 0, and a sampler at set 0, binding 1. Vulkan and D3D12 execute this
pair for single draws and ordered batches. The pair is a portable test ABI. It
is not a recovered stock KN5 or CSP shader contract. An optional uniform buffer
at set 0, binding 2 carries one aligned material record. D3D12 maps it to
`b2`. Its first 64 bytes use source-evidenced `ksPerPixel` defaults in this
port's std140/HLSL-compatible packing. A bounded resolver reads parsed KN5
values and typed CSP overrides. It preserves CSP precedence and the WebGL
emissive conversion. The plain `ksPerPixel` fixture executes ambient,
directional diffuse, direct Blinn specular, and emissive output. Pixel tests
prove specular enable and removal and per-draw record selection. Fresnel,
reflections, fog, shadows, and CSP lights remain staged. A bounded batch
preflights all requests. It clears or loads attachments once and returns one
final readback. Draw-packet texture resources resolve by canonical name.
An exact tangent-space extension adds `txNormal` at bindings 4 and 5. A second
extension adds linear `txMaps` at bindings 6 and 7. D3D12 uses `t4`, `s5`,
`t6`, and `s7`. The maps shader uses `maps.r` for specular strength. It uses
`maps.g` for the source exponent equation. The maps ABI requires zero Fresnel,
so `maps.b` remains staged. A fourth ABI supports the
`ksPerPixelMultiMap_NMDetail` family. This family includes
`ksPerPixelMultiMap_AT_NMDetail`. It adds `txDetail` at bindings 8 and 9. It
adds `txNormalDetail` at bindings 10 and 11. The legacy `txDetailNM` name is an
alias for `txNormalDetail`. The 64-byte material record includes the detail UV
multiplier, normal-detail strength, and detail enable value. The AT profile
retains the production A2C state. Reflections and shadows remain staged.
Runtime tests cover known pixels and mixed resource layouts.
The serialized KN5 resource ID remains a shader bind point, not a
texture-table index. A bounded static-scene adapter maps the final KN5 tree to
dense scene IDs. It validates all packets and pipelines before buffer creation.
The adapter uploads each referenced node once. It retains duplicate packets as
ordered draw instances. It submits one batch with caller-supplied SPIR-V or
DXIL pipelines. A constants-enabled pipeline requires an explicit table in
final material order. The adapter validates this table before allocation. It
owns one 256-byte buffer per used material and reuses it for duplicate packets.
Static scenes accept 1x and 4x pipelines. A2C requires 4x color and matching
depth samples. A bounded material handoff derives the supported resource
layouts, constants, and profile state from KN5 materials. The caller must
supply explicit SPIR-V or DXIL modules. Production packets remain marked as
staged. The handoff does not translate stock shader containers. Window
surfaces, swapchains, and complete stock execution remain roadmap work.
A bounded stock-scene facade composes the render plan, draw packets, material
handoff, and static-scene preparation. It uses linear topology preflight and
rejects malformed edges, cycles, and over-budget plans before backend
allocation. A real SwiftShader test executes the complete facade through
pixel readback. The same test runs through D3D12/WARP in CI. The input snapshot
must contain resolved workspace and CSP state. A bounded workspace adapter
maps files to merged scene roots in source order. It attaches file and
auxiliary labels without partial mutation. A bounded LOD resolver uses the
production half-open ranges and FOV formula. The caller must supply the exact
preview AABB center and camera position. The resolver gives excluded roots to
the stock-scene facade. Isolation bypasses these exclusions and authored
visibility. Preview modes, surface overlays, shadows, reflections, and
post-processing remain staged.
The static-scene adapter has two texture-authority modes. The first mode
uses caller-owned tables in the final KN5 texture order. The second mode owns
the used embedded KN5 textures and one linear-repeat sampler. It validates all
used DDS payloads before backend allocation. It decodes supported 2D mip chains
to RGBA8 and retains explicit sRGB metadata. This portable CPU decode is not a
direct block-compressed path. The separate device API supports direct BC1 and
BC3 uploads. BC7 has an exact, bounded CPU fallback.
BC6H still requires a capable GPU path. The upload planner supports DX10 2D
arrays, cubemaps, and RGB24 conversion. The embedded static-scene mode rejects
arrays, cubemaps, 1D/3D textures, BC6H, and legacy D3D9 float textures.
The maps path rejects sRGB payloads before backend allocation. Source, decoded,
and host-preparation budgets include the maps resource and its retained tables.
The FBX converter handles static positions, triangulation, hierarchy, a
bounded transform subset, and first material assignment. Its aggregate budget
includes temporary containers, copied strings, and output containers. It
explicitly diagnoses unsupported images, skinning, animation, layer mappings,
and advanced transform semantics.

The strict Linux builds detect the Vulkan SDK. Local runtime checks use the
SwiftShader ICD. CI runs the same checks with a software Vulkan device. The
production WebGL visual check remains mandatory for each rendering change.

## Contribution rules

- Put reusable byte, error, limit, and math primitives in `apex-core`.
- Keep parsers in `apex-formats` independent of windows and GPU APIs.
- Keep all Vulkan and D3D12 handles out of public backend-neutral headers.
- Apply count, size, offset, recursion, and aggregate limits before allocation.
- Add malformed input and every-boundary truncation tests for parser changes.
- Do not remove or weaken the WebGL reference path until the complete parity
  and rendering-evidence gates in `docs/CPP_PORT.md` are satisfied.
