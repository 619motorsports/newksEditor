# Portable Grass Preview Evidence

## Scope

The portable renderer now accepts expanded grass blades from a caller-owned vertex buffer.
Vulkan and Direct3D 12 use the same vertex data, constants, atlas, sampler, and draw order.

This pass is a labeled preview approximation. It does not claim exact CSP or native behavior.

## Source basis

The CPU generator follows the triangle-stratified path in `src/grass-fx.js`.
It uses source-triangle area, upward-facing tests, deterministic placement, and one atlas tile.

The fragment shaders follow these public preview rules:

- The alpha cutoff is `0.5`.
- Ambient light uses a factor of `0.35`.
- Diffuse sun light uses a factor of `0.8`.
- Wet albedo uses `pow(0.65, 2.2)`.
- Distance fog uses the WebGL fog equation.

The shaders use a small sinusoidal wind offset. This offset is a portable fallback.

## Deferred behavior

This pass does not include these CSP functions:

- CSP configuration parsing and evaluation
- Native compute-density generation
- Surface ownership and occlusion targets
- Deformation maps
- Local lights and local shadows
- The CSP wind field

Callers must evaluate source meshes and settings before they call this renderer contract.

## Input limits

The generator rejects nonfinite vertices, invalid normals, invalid atlas grids, and truncated declared spans.
It limits source triangles, candidates, blades, vertices, coordinates, normals, and allocation sizes.

Each blade contains six vertices. Each vertex contains 14 `float32` values.

The batch contract requires a depth attachment for drawable grass. It also rejects attachment aliasing and clip-space mismatches.

## Backend behavior

Both backends draw grass after the opaque scene prefix and before the first transparent draw.
Both backends use depth testing, depth writes, no culling, and alpha testing.
Both backends enable alpha-to-coverage for a four-sample target.

The Vulkan path uses separate uniform-buffer, texture, and sampler descriptors.
The Direct3D 12 path uses `b0`, `t0`, and `s0` for the same resources.

## Verification

The generator tests cover malformed spans, nonfinite data, resource limits, deterministic output, and nondegenerate geometry.
The contract tests cover missing resources, missing depth, invalid counts, target aliasing, and clip-space mismatches.

Shader-drift tests bind the GLSL and HLSL sources to the embedded SPIR-V and DXBC artifacts.
The Direct3D 12 source passes a strict `clang-cl` syntax check with Windows SDK headers.

The production WebGL smoke test used the installed Drift track and CSP configuration.
The assembled test generated 9,009 grass instances with shadows and scene-cubemap rendering.
The test reported WebGL error zero and no browser exceptions.

An isolated `06GRASS001` test generated 816 instances.
The enabled capture hash was `e8fcb216edc657c9`.
The disabled capture hash was `764772262c98c808`.
