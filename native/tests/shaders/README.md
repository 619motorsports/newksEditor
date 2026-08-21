# Indexed transform shader fixture

`indexed_static_mesh.vert` is the source for the embedded Vulkan test shader.
The shader uses the `DrawMatrices` ABI from `device.hpp`.

The embedded artifact has these identities:

- Source SHA-256: `0e97d9331b5f3f84b07018c48919c88a3f53e7efede1e07830c649678f3f65c1`
- SPIR-V SHA-256: `ab80168ede57671bb84cf9a678e12dd961f200e151b69c24722486e7497fca96`
- Compiler: glslang `16.4.0`
- Target: SPIR-V 1.0 for Vulkan 1.0

Use these commands from the repository root:

```sh
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S vert \
  -o /tmp/apex_indexed_static_mesh.spv \
  native/tests/shaders/indexed_static_mesh.vert
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_static_mesh.spv
sha256sum native/tests/shaders/indexed_static_mesh.vert \
  /tmp/apex_indexed_static_mesh.spv
```

`render_backend_tests.cpp` contains the embedded SPIR-V bytes. Windows uses
equivalent HLSL and compiles it to DXBC with `D3DCompile` for the WARP test.
