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

`indexed_ks_per_pixel.vert` and `indexed_ks_per_pixel.frag` are the bounded
directional-diffuse fixture. They use the existing draw-matrices push-constant
vertex ABI and the exact resource ABI requested by the native material path:
sampled image `set=0,binding=0`, sampler `set=0,binding=1`, material UBO
`set=0,binding=2`, and frame UBO `set=0,binding=3` (D3D12 `t0`, `s1`, `b2`,
and `b3`). The fragment equation is intentionally limited to ambient plus
directional diffuse and emissive:

```text
texel.rgb * (ambientColor * ksAmbient
  + sunColor * ksDiffuse * max(dot(normalize(worldNormal), normalize(sunDirection)), 0)
  + emissive)
```

No specular, Fresnel, reflection, fog, shadows, alpha test, normal map, detail
map, CSP light, or overlay behavior is implied by this fixture. It is a
source-evidenced execution fixture for the explicitly bounded P3 slice, not a
complete stock `ksPerPixel` implementation.

Fixture identities:

- Vertex source SHA-256: `71d50209737d8caa1b88261125bd59e2e417214772684f96b98fe98f9212d408`
- Vertex SPIR-V SHA-256: `98b395551eaa3f269962a338645fad99c1e80df6bd2352d9bfab41d6c6a0765b`
- Fragment source SHA-256: `defffb89e94d8149d3662c469621f3d7aed6f1bc82693a54dac1163e7bcd676f`
- Fragment SPIR-V SHA-256: `c44686b795e26512d7a623c37a60ed646ddf1914e946959aebf8c8798a3c55eb`
- Compiler: glslang `16.4.0`
- Validator: SPIRV-Tools `2026.3` (`vulkan-sdk-1.4.357.0-0-g9a49b0883`)
- Target: SPIR-V 1.0 for Vulkan 1.0

`indexed_ks_per_pixel_nm.vert` and `indexed_ks_per_pixel_nm.frag` are the
bounded tangent-space normal-map extension of that fixture. They retain the
same draw-matrices push constant and material/frame UBOs, and add a normal
image at `set=0,binding=4` plus a normal sampler at `set=0,binding=5`.
D3D12 maps those resources to `t4` and `s5`. The vertex shader follows the
production reconstruction in `public/app.js:2048`: world position, world
normal, world tangent, and world `cross(tangent,normal)` bitangent. The
fragment shader follows `public/app.js:2075-2076` for tangent-space normal
decoding and `public/app.js:2080-2083` for the no-maps direct-light path.

This fixture explicitly excludes maps/detail, object-space normals, Fresnel,
reflection, shadows, AO/VAO, rain, seasons, local/CSP lights, damage,
transparency, alpha test, and overlays. `maps=vec3(1)` is intentional, so the
specular multiplier is `ksSpecular` and the exponent is
`max(1,ksSpecularEXP+1)`. It is not a complete stock `ksPerPixelNM` shader.

The NM fixture identities are recorded after compiling with glslang 16.4.0
and validating with SPIRV-Tools 2026.3:

- Vertex source SHA-256: `44a4366343584418a39008707ea0c629454b716ef43b1783ccb20b0ddae257a7`
- Vertex SPIR-V SHA-256: `1d8fa2d0a866c374d42f55e9e92f4a39bbbf8abd6c9beceb8bb9f3de5945d34c`
- Fragment source SHA-256: `59ec129cf2199a6363866bf57faa41be65fd37bff429885fdc4395074aef6bad`
- Fragment SPIR-V SHA-256: `43a0c4b229eebc5153852d97c74f6acee67b4c252f627cd25ea349e68b9500d5`
