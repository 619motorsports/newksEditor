# Native port seed

This directory contains the incremental C++20 port of Apex Editor. The
JavaScript/WebGL application remains the feature-complete reference while the
native implementation works through the parity gates in
[`docs/CPP_PORT.md`](../docs/CPP_PORT.md).

The first slice provides shared bounded input utilities, KSANIM v1/v2 and KNH
parsers, a backend-neutral device contract, headless Vulkan and Windows D3D12
implementations, and portable desktop-boundary validation. Backend API types
stay inside `src/render`; format and authoring libraries do not depend on a
graphics API.

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
out/native/dev/native/apex-native --inspect-ksanim animation.ksanim
```

An unavailable SDK, driver, validation layer, or adapter is reported
explicitly. It is never presented as a successful backend initialization.

## Contribution rules

- Put reusable byte, error, limit, and math primitives in `apex-core`.
- Keep parsers in `apex-formats` independent of windows and GPU APIs.
- Keep all Vulkan and D3D12 handles out of public backend-neutral headers.
- Apply count, size, offset, recursion, and aggregate limits before allocation.
- Add malformed input and every-boundary truncation tests for parser changes.
- Do not remove or weaken the WebGL reference path until the complete parity
  and rendering-evidence gates in `docs/CPP_PORT.md` are satisfied.
