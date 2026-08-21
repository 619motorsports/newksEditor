# Native port seed

This directory contains the incremental C++20 port of Apex Editor. The
JavaScript/WebGL application remains the feature-complete reference while the
native implementation works through the parity gates in
[`docs/CPP_PORT.md`](../docs/CPP_PORT.md).

The native port has shared bounded input utilities. It reads KN5 v4/v5/v6,
DDS, ACD, VAO, ordered INI/CSP, KSANIM v1/v2, and KNH. It writes KN5 files
without byte changes when the model is unchanged. DDS includes bounded BC7 CPU
decode. FBX support is currently a bounded binary/ASCII DOM foundation.

The port supports surfaces, cameras, splines, workspaces, asset folders, skin
indexes, and skin metadata. It includes staged CSP evaluation and KN5 scene
conversion. The render model includes material profiles, bindings, and
validated draw packets. Driver rigs include CPU reference skinning. The
authoring model includes transactions, bounded project/CSP serialization,
geometry edits, and KN5 baking.

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
2D textures, samplers, and immutable shader modules. They can execute bounded
RGBA8/BGRA8 texture clears and canonical RGBA8 readback for backend validation.
This is not evidence of complete scene pixels. Window surfaces, swapchains,
descriptor binding, pipeline states, shader execution, drawing, and production
pixel comparisons remain roadmap work. BC7 has an exact, bounded CPU fallback.
BC6H still requires a capable GPU path. The native CPU decode/upload paths also
reject DX10 arrays, cubemaps, 1D/3D textures, raw 24-bit uploads, and legacy
D3D9 float textures. Those paths remain available in the unchanged WebGL
reference. The FBX parser currently stops at a bounded DOM and does not claim
geometry, material, image, skin, or animation conversion parity.

## Contribution rules

- Put reusable byte, error, limit, and math primitives in `apex-core`.
- Keep parsers in `apex-formats` independent of windows and GPU APIs.
- Keep all Vulkan and D3D12 handles out of public backend-neutral headers.
- Apply count, size, offset, recursion, and aggregate limits before allocation.
- Add malformed input and every-boundary truncation tests for parser changes.
- Do not remove or weaken the WebGL reference path until the complete parity
  and rendering-evidence gates in `docs/CPP_PORT.md` are satisfied.
