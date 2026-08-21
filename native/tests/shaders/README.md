# Indexed shader fixtures

`indexed_static_mesh.vert` is the source for the embedded Vulkan test shader.
The shader uses the `DrawMatrices` ABI from `device.hpp`.

`indexed_static_mesh_blue.frag` is the blue fragment shader used to prove
persistent color and depth behavior. Its identities are:

- Source SHA-256: `6b3ae3948ecab07330849819ce22f015f015c44ad614e6dee89bbca1e167f821`
- SPIR-V SHA-256: `b250bd205a32b206f37fdf169a37aa449b75a381376d46c56e08dfba21e3ce52`

`indexed_static_mesh_sampled.vert` and
`indexed_static_mesh_sampled.frag` define the portable diffuse-resource test
ABI. The vertex shader passes the KN5 UV at location 2 without a transform.
The fragment shader uses a sampled image at set 0, binding 0, and a sampler at
set 0, binding 1. These shaders test descriptor execution. They do not recover
or replace a stock KN5 or CSP shader.

The sampled shader identities are:

- Vertex source SHA-256: `1295be863feabdaf6c9639740ade404b0f487a36c89308d3a5bef3cc01b144e5`
- Vertex SPIR-V SHA-256: `7f0c96a8570de487658735be12f94c5f07d72f149c8f454f28548c853075281d`
- Fragment source SHA-256: `5bf9aeae9ce5e8b48475608f0274cf43fc8d57789746044353b308ec63f731a0`
- Fragment SPIR-V SHA-256: `9d8bcb09b04c7b583b83fee222b6c8d6efc71a28d7d62babca6b520201cf9cdd`
- Compiler: glslang `16.4.0`
- Target: SPIR-V 1.0 for Vulkan 1.0

`indexed_static_mesh_material.frag` extends the portable diffuse ABI with one
uniform buffer at set 0, binding 2. D3D12 maps the same record to `b2`. The
fixture reads the port-packed `KsPerPixelMaterialConstants` values. Its
small output equation proves constant transport and per-draw selection. It is
not the complete production lighting equation.

The material fixture identities are:

- Fragment source SHA-256: `0b39c1820e2a1176ca78fd4c3e070076dec79b6e356cd72d83d4f4f1ab65e2b7`
- Fragment SPIR-V SHA-256: `4686b1f9145bf11e2f018280d091a97c9a34064d6f868f551859168732fdc355`
- Compiler: glslang `16.4.0`
- Target: SPIR-V 1.0 for Vulkan 1.0

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

glslangValidator -V --target-env vulkan1.0 -Os -g0 -S vert \
  -o /tmp/apex_indexed_sampled_vert.spv \
  native/tests/shaders/indexed_static_mesh_sampled.vert
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S frag \
  -o /tmp/apex_indexed_sampled_frag.spv \
  native/tests/shaders/indexed_static_mesh_sampled.frag
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_sampled_vert.spv
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_sampled_frag.spv
sha256sum native/tests/shaders/indexed_static_mesh_sampled.vert \
  /tmp/apex_indexed_sampled_vert.spv \
  native/tests/shaders/indexed_static_mesh_sampled.frag \
  /tmp/apex_indexed_sampled_frag.spv

glslangValidator -V --target-env vulkan1.0 -Os -g0 -S frag \
  -o /tmp/apex_indexed_material_frag.spv \
  native/tests/shaders/indexed_static_mesh_material.frag
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_material_frag.spv
sha256sum native/tests/shaders/indexed_static_mesh_material.frag \
  /tmp/apex_indexed_material_frag.spv
```

`render_backend_tests.cpp` contains the embedded SPIR-V bytes. Windows uses
equivalent HLSL and compiles it to DXBC with `D3DCompile` for the WARP test.
