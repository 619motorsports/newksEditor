# Native port seed

This directory contains the incremental C++20 port of Apex Editor. The
JavaScript/WebGL application remains the feature-complete reference while the
native implementation works through the parity gates in
[`docs/CPP_PORT.md`](../docs/CPP_PORT.md).

The native port has shared bounded input utilities. It reads KN5 v4/v5/v6,
DDS, bounded PNG, ACD, VAO, ordered INI/CSP, KSANIM v1/v2, and KNH. PNG
support covers non-interlaced 8-bit grayscale, RGB, indexed,
grayscale-alpha, and RGBA images. It writes KN5 files
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
colliders, and initial car validation. Project persistence includes material,
node, mesh, geometry, collider, damage, and bottom-collider edits. It also
retains the source identity for each secondary asset. Missing or mismatched
identities do not match when related edits exist. A bounded adapter sends KN5
edits to the bake path. The adapter retains diagnostics for CSP-only material
values. Material number strings use JavaScript number syntax. Empty strings and
decimal underflow become zero. Overflow and boolean material properties are not
stored.

Collider geometry keys are stable hierarchy paths. Examples are `root`, `0`,
and `0/1`. Bottom-collider keys are positions in the parsed collider list.
Secondary-asset adapters apply collider, damage, and bottom-collider edits from
immutable baselines. The adapters return no candidate asset after an identity
or edit error. Export uses the bounded KN5 and INI writers. A bounded
`colliders.ini` parser retains sparse source section numbers. It records
rejected sections, and export rejects this incomplete source data.

The render library includes DDS upload, recovered lighting math, shadow math,
reflection math, camera matrices, and frame-pass plans. The camera code uses
the `perspective`, `lookAt`, and `multiply` formulas from `public/app.js`.
It defines the Vulkan and D3D12 clip-space conversions separately. Its
backend-neutral contract supports headless Vulkan and Windows D3D12. Both
backends implement devices, buffers, 2D textures, samplers, shader modules,
and bounded color and D32 readback. Portable validation protects the desktop
boundary. Backend API types stay inside `src/render`. Format and authoring
libraries do not depend on a graphics API.

The device API reports presentation prerequisites without creating a window.
An optional Vulkan mode creates a `VK_EXT_headless_surface` surface and a
swapchain. It can acquire, clear, submit, and present an image. The target owns
its image views, framebuffers, commands, and synchronization objects. It can
also copy a completed, same-format color attachment into a swapchain image.
D3D12
reports its DXGI factory, device, and queue prerequisites. D3D12 does not create
a swapchain because the application does not yet supply a native window handle.

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
The owned static-scene path uploads bounded PNG decodes as RGBA8 UNORM.
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
Single-sample D32 attachments can request shader-readable allocation. The
fixed receiver ABI uses three distinct maps at bindings 16-18, one nearest
clamp-to-edge sampler at binding 19, and one 256-byte constants record at
binding 20. Vulkan executes this ABI and transitions retained maps between
depth-write, shader-read, and transfer-read states. A SwiftShader test samples
all three maps and then runs the caster pass again. D3D12 creates a typeless
R32 resource with a D32 view and an R32_FLOAT sampled view. D3D12 receiver
descriptor execution stays staged until a Windows WARP build verifies it.
The native binding numbers and clip conversion are portable choices. They are
not claims about the recovered native register layout.
Static scenes that declare this receiver ABI own the sampler and 256-byte
record. Each frame must supply retained maps made for the same device, backend,
camera position, and camera direction. The adapter validates all maps and all
draws before it updates receiver, skin, or frame buffers. The stock-scene
facade exposes the same receiver opt-in and selects only matching shader
modules.
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
`b2`. The 80-byte record uses the port's std140/HLSL-compatible packing. Its
first 64 bytes contain the existing `ksPerPixel` values. Its last 16 bytes
contain `damageZones`. A bounded resolver reads parsed KN5
values and typed CSP overrides. It preserves CSP precedence and the WebGL
emissive conversion. The plain `ksPerPixel` fixture executes ambient,
directional diffuse, direct Blinn specular, and emissive output. Pixel tests
prove specular enable and removal and per-draw record selection. An optional
Vulkan `ksPerPixel` receiver fixture applies the source-evidenced three-cascade
selection and explicit 3x3 PCF to direct diffuse and specular light. A real
stock-scene test proves fully shadowed `(3,40,18,255)` and fully lit
`(16,116,34,255)` center pixels. Fresnel, reflections, fog, D3D12 receiver
execution, and CSP lights remain staged. A bounded batch
preflights all requests. It clears or loads attachments once and returns one
final readback. Draw-packet texture resources resolve by canonical name.
An exact tangent-space extension adds `txNormal` at bindings 4 and 5. A second
extension adds linear `txMaps` at bindings 6 and 7. D3D12 uses `t4`, `s5`,
`t6`, and `s7`. The maps shader uses `maps.r` for specular strength. It uses
`maps.g` for the source exponent equation. The maps ABI requires zero Fresnel,
so `maps.b` remains staged. The material handoff supports
`ksPerPixelMultiMap` and `ksPerPixelMultiMap_AT` through this eight-binding
ABI. It rejects active generic detail, object-space normals, and nonzero
Fresnel. The AT profile retains alpha-to-coverage on a 4x target. A fourth ABI supports the
`ksPerPixelMultiMap_NMDetail` family. This family includes
`ksPerPixelMultiMap_AT_NMDetail`. It adds `txDetail` at bindings 8 and 9. It
adds `txNormalDetail` at bindings 10 and 11. The legacy `txDetailNM` name is an
alias for `txNormalDetail`. The 80-byte material record includes the detail UV
multiplier, normal-detail strength, and detail enable value. The AT profile
retains the production A2C state. Reflections and executable receiver shader
variants for these extended profiles remain staged.
Runtime tests cover known pixels and mixed resource layouts.
The dirt-zero damage ABI uses the same diffuse, normal, and maps resources.
It adds `txDamage` at bindings 12 and 13. It adds `txDamageMask` at bindings
14 and 15. A low-level extension binds `txDust` at the mutually exclusive
bindings 8 and 9. Vulkan and D3D12 use its alpha for direct diffuse and
specular light. Pixel tests cover alpha values of zero and one. The stock
material handoff selects this extension for six-resource damage packets. It
requires the caller to label the matching shader module set as `damage_dust`.
It retains the 12-resource path for legacy packets without `txDust`. Stock detail,
sun-specular, Fresnel, and reflection remain staged.
The serialized KN5 resource ID remains a shader bind point, not a
texture-table index. A texture replacement preserves the source bind point.
A new resource slot requires an explicit bind point. The baker reports and
skips a new slot when the bind point is not known. A bounded static-scene
adapter maps the final KN5 tree to dense scene IDs. It validates all packets
and pipelines before buffer creation.
The adapter uploads each referenced node once. It retains duplicate packets as
ordered draw instances. It submits one batch with caller-supplied SPIR-V or
DXIL pipelines. A constants-enabled pipeline requires an explicit table in
final material order. The adapter validates this table before allocation. It
owns one 256-byte buffer per used material and reuses it for duplicate packets.
Static scenes accept 1x and 4x pipelines. A2C requires 4x color and matching
depth samples. A bounded material handoff derives the supported resource
layouts, constants, and profile state from KN5 materials. The caller must
supply explicit SPIR-V or DXIL modules. Production packets remain marked as
staged. The handoff does not translate stock shader containers. Vulkan can
create a headless surface and swapchain. It can present a completed offscreen
color attachment with the same size and format. Native window surfaces,
D3D12 swapchains, and complete stock execution remain roadmap work.
A bounded stock-scene facade composes the render plan, draw packets, material
handoff, and static-scene preparation. It uses linear topology preflight and
rejects malformed edges, cycles, and over-budget plans before backend
allocation. The facade can resolve F4 damage before it creates the plan. It
merges the activity writes and complete material table before allocation. A
real SwiftShader test executes the complete facade through
pixel readback. Its optional retained-shadow receiver uses the same validated
facade and static-scene ownership path. A second test executes the
three-texture MultiMap facade and
checks the `maps.r` and `maps.g` result. It also executes the AT family on a
4x target and checks partial resolved coverage. An F4 facade test executes
broken and intact states with six embedded textures. It checks `txDust` alpha
and the recovered normal-alpha attenuation. The same tests run through D3D12/WARP in
CI. The input snapshot must contain resolved workspace and CSP state. A
bounded workspace adapter
maps files to merged scene roots in source order. It attaches file and
auxiliary labels without partial mutation. A bounded LOD resolver uses the
production half-open ranges and FOV formula. The caller must supply the exact
preview AABB center and camera position. The resolver gives excluded roots to
the stock-scene facade. Isolation bypasses these exclusions and authored
visibility. A second bounded resolver supplies cockpit, rim, and driver preview
state without changing the scene. It uses exact cockpit and rim names. Driver
hidden names are trimmed, matched with ASCII case ignored, and limited to driver
subtrees. Show-hidden bypasses authored and preview state. It does not bypass
driver suppression, workspace LOD exclusions, or mesh LOD. Isolation bypasses
visibility and subtree filters. It still applies the selected mesh LOD range.
The contract follows `itemPreviewVisible()` and the draw filter in
`public/app.js`. The cockpit audit in `src/cockpit-preview.js` supplies the F3
pair. Surface overlays, extended-profile shadow shader variants, reflections,
and post-processing remain staged. A separate bounded damage adapter resolves the
five exact F4 node sequences. It creates scene activity and material overrides
without changing the parsed model. It applies `damageZones` after CSP values.
It also retains the native one-way `glassDamage` write for shared materials.
The adapter identifies the recovered dirt-zero branch. The bounded material
handoff executes this branch with caller-supplied SPIR-V or DXIL modules.
Nonzero dirt remains staged. Active detail, sun-specular, Fresnel, and
reflection branches also remain staged.
The static-scene adapter has two texture-authority modes. The first mode
uses caller-owned tables in the final KN5 texture order. The second mode owns
the used embedded KN5 textures and one linear-repeat sampler. It validates all
used supported DDS and PNG payloads before backend allocation. It decodes
supported DDS 2D mip chains and bounded PNG images to RGBA8. DDS retains
explicit sRGB metadata. PNG retains straight alpha and top-to-bottom rows and
uses RGBA8 UNORM without implicit color conversion. This portable CPU decode is not a
direct block-compressed path. The separate device API supports direct BC1 and
BC3 uploads. BC7 has an exact, bounded CPU fallback.
BC6H still requires a capable GPU path. The upload planner supports DX10 2D
arrays, cubemaps, and RGB24 conversion. The embedded static-scene mode rejects
arrays, cubemaps, 1D/3D textures, BC6H, and legacy D3D9 float textures.
The maps path rejects sRGB payloads before backend allocation. Source, decoded,
and host-preparation budgets include the maps resource and its retained tables.
A separate CPU bridge resolves external DDS and PNG files through explicit
`AssetSource` grants. It rejects unsafe, missing, ambiguous, and over-budget
requests. It retains source identity and returns no partial table after an
error. The bridge copies validated image bytes into opaque, synthetic KN5
textures. It rewrites only the requested material slots and preserves their
bind points. Then the stock-scene facade can own and execute the effective
model. A real backend pixel test proves the ACD-to-GPU path. The renderer does
not receive the `AssetSource`, the grant, or the external path.
The FBX converter handles static positions, triangulation, hierarchy, a
bounded transform subset, and first material assignment. Its aggregate budget
includes temporary containers, copied strings, and output containers. It
explicitly diagnoses unsupported images, skinning, animation, layer mappings,
and advanced transform semantics.

The strict Linux build detects the Vulkan SDK. The runtime test uses a software
Vulkan device when an ICD is available. CI defines the same Vulkan test. The
current GitHub account billing error prevents fresh CI evidence. The production
WebGL visual check remains mandatory for each rendering change.

## Contribution rules

- Put reusable byte, error, limit, and math primitives in `apex-core`.
- Keep parsers in `apex-formats` independent of windows and GPU APIs.
- Keep all Vulkan and D3D12 handles out of public backend-neutral headers.
- Apply count, size, offset, recursion, and aggregate limits before allocation.
- Add malformed input and every-boundary truncation tests for parser changes.
- Do not remove or weaken the WebGL reference path until the complete parity
  and rendering-evidence gates in `docs/CPP_PORT.md` are satisfied.
