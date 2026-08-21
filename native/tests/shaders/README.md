# Indexed shader fixtures

`indexed_static_mesh.vert` is the source for the embedded Vulkan test shader.
The shader uses the `DrawMatrices` ABI from `device.hpp`.

`indexed_static_mesh_blue.frag` is the blue fragment shader used to prove
persistent color and depth behavior. Its identities are:

- Source SHA-256: `6b3ae3948ecab07330849819ce22f015f015c44ad614e6dee89bbca1e167f821`
- SPIR-V SHA-256: `b250bd205a32b206f37fdf169a37aa449b75a381376d46c56e08dfba21e3ce52`

The embedded transform artifact has these identities:

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

glslangValidator -V --target-env vulkan1.0 -Os -g0 -S frag \
  -o /tmp/apex_indexed_static_mesh_blue.spv \
  native/tests/shaders/indexed_static_mesh_blue.frag
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_static_mesh_blue.spv
sha256sum native/tests/shaders/indexed_static_mesh_blue.frag \
  /tmp/apex_indexed_static_mesh_blue.spv
```

`render_backend_tests.cpp` contains the embedded SPIR-V bytes. Windows uses
equivalent HLSL and compiles it to DXBC with `D3DCompile` for the WARP test.
