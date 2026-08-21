# Native port seed

This directory contains the incremental C++20 port of Apex Editor. The
JavaScript/WebGL application remains the feature-complete reference while the
native implementation works through the parity gates in
[`docs/CPP_PORT.md`](../docs/CPP_PORT.md).

The native port has shared bounded input utilities. It reads KN5 v4/v5/v6,
DDS, ACD, VAO, ordered INI/CSP, KSANIM v1/v2, and KNH. It writes KN5 files
without byte changes when the model is unchanged. DDS includes bounded BC7 CPU
decode. Its upload planner supports DX10 2D arrays, cubemaps, and exact RGB24
to RGBA8 conversion. FBX support includes a bounded binary/ASCII DOM and a
static-geometry conversion subset.

The port supports surfaces, cameras, splines, workspaces, asset folders, skin
indexes, and skin metadata. It includes staged CSP evaluation and KN5 scene
conversion. The render model includes material profiles, bindings, and
validated draw packets. Driver rigs include CPU reference skinning. The
authoring model includes transactions, bounded project/CSP serialization,
geometry edits, KN5 baking, car damage data, bottom colliders, and initial car
validation.

The render library includes DDS upload, recovered lighting math, shadow math,
reflection math, and frame-pass plans. Its backend-neutral contract supports
headless Vulkan and Windows D3D12. Both backends implement devices, buffers,
2D textures, samplers, shader modules, and clear/readback operations. Portable
validation protects the desktop boundary. Backend API types stay inside
`src/render`. Format and authoring libraries do not depend on a graphics API.

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
The indexed path uses immutable vertex and index buffers. It accepts only
resource-free, no-depth packets with identity transforms. Draw-packet texture
resources resolve by canonical name. The serialized KN5 resource ID remains a
shader bind point, not a texture-table index. This is not evidence of complete
scene pixels. Window surfaces, swapchains, descriptor binding, complete scene
draws, and production pixel comparisons remain roadmap work. BC7 has an exact,
bounded CPU fallback. BC6H still requires a capable GPU path. The upload
planner supports DX10 2D arrays, cubemaps, and RGB24 conversion. It rejects
1D/3D textures and legacy D3D9 float textures.
The CPU decoder can still reject arrays and cubemaps. The FBX converter handles
static positions, triangulation, hierarchy, a bounded transform subset, and
first material assignment. It explicitly diagnoses unsupported images,
skinning, animation, layer mappings, and advanced transform semantics.

The Vulkan SDK is detected by the strict Linux builds. This machine has no
usable Vulkan physical device, so the local runtime test reports a skip. CI can
run the same test with a software Vulkan device. The production WebGL visual
check is still required before any visible-rendering parity claim.

## Contribution rules

- Put reusable byte, error, limit, and math primitives in `apex-core`.
- Keep parsers in `apex-formats` independent of windows and GPU APIs.
- Keep all Vulkan and D3D12 handles out of public backend-neutral headers.
- Apply count, size, offset, recursion, and aggregate limits before allocation.
- Add malformed input and every-boundary truncation tests for parser changes.
- Do not remove or weaken the WebGL reference path until the complete parity
  and rendering-evidence gates in `docs/CPP_PORT.md` are satisfied.
