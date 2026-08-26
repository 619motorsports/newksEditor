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

`indexed_shadow_alpha.frag` is the translated alpha-caster fixture. It uses
the recovered `ksShadowGenAT` resource locations: `t0`, `s3`, and `b4`.
Its identities are:

- Source SHA-256: `176fe3a0f177286217b3086b29a990c5f6a7a8a99faa33829b984b2bf20ced51`
- SPIR-V SHA-256: `773822db22976b4b20ecba311025e48407064460e596741d0b25a1f1e3e890ec`
- Compiler: glslang `16.4.0`
- Target: SPIR-V 1.0 for Vulkan 1.0

`render_backend_tests.cpp` contains the embedded SPIR-V bytes. Windows uses
equivalent HLSL and compiles it to DXBC with `D3DCompile` for the WARP test.

`indexed_ks_per_pixel.vert` and `indexed_ks_per_pixel.frag` are the bounded
directional-diffuse fixture. They use the existing draw-matrices push-constant
vertex ABI and the exact resource ABI requested by the native material path:
sampled image `set=0,binding=0`, sampler `set=0,binding=1`, material UBO
`set=0,binding=2`, and frame UBO `set=0,binding=3` (D3D12 `t0`, `s1`, `b2`,
and `b3`). The fragment equation is intentionally limited to ambient plus
directional diffuse, Blinn specular, and emissive:

```text
texel.rgb * (ambientColor * ksAmbient
  + sunColor * ksDiffuse * max(dot(normalize(worldNormal), normalize(sunDirection)), 0)
  + emissive)
  + sunColor * pow(max(dot(normalize(worldNormal), normalize(
      normalize(sunDirection) + normalize(cameraPosition - worldPosition))), 0),
      max(1, ksSpecularEXP + 1)) * ksSpecular
```

This is the exact no-map direct-light subset from `public/app.js:2080-2083`.
The fixture applies the production WebGL distance-fog equation after direct
lighting. Fog is disabled in the default frame record. Fresnel, reflection,
shadows, alpha test, normal map, detail map, CSP light, and overlay behavior
remain excluded. It is a source-evidenced
execution fixture for the explicitly bounded P3 slice, not a complete stock
`ksPerPixel` implementation.

`indexed_ks_skinned_mesh.vert` is the CPU-skinned transport fixture. It
declares all six attributes in the recovered 19-float stream. The CPU applies
the bone palette before the backend uploads the mutable vertex buffer. The
shader does not reproduce the recovered native `b13` GPU-skinning path.

The fixture keeps the direct-light output ABI from `indexed_ks_per_pixel.vert`.
It also carries tangent, weight, and index inputs as unused stage outputs. This
rule prevents the compiler from removing these test inputs.

- Source SHA-256: `1defab150ba22963fe37865bacbd48cc9db2222958e65bb497b586ee5511b8d1`
- SPIR-V SHA-256: `314e257b70764b142e1a41b0e46c37eef12fa6958282e53a3edb2b1a2163121a`
- Compiler: glslang `16.4.0`
- Target: SPIR-V 1.0 for Vulkan 1.0

Use these commands from the repository root:

```sh
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S vert \
  -o /tmp/apex_indexed_ks_skinned_mesh.spv \
  native/tests/shaders/indexed_ks_skinned_mesh.vert
spirv-val --target-env vulkan1.0 \
  /tmp/apex_indexed_ks_skinned_mesh.spv
```

Fixture identities:

- Vertex source SHA-256: `1df262a410660234a6918333e3ef0ef10773b2e9d47b277d4ed03a4b13101def`
- Vertex SPIR-V SHA-256: `4ef3c3282d34fb12dbb3945d61a1554edef91c419f4b787071ccc9b50bbb7837`
- Fragment source SHA-256: `eac3ed89b15cea0aa9acebfb3d06977308159852dfd8a6e34f300edae6fa6ba8`
- Fragment SPIR-V SHA-256: `0b7fec164119988f02ab455554fc04de9fbc355d42dcd96e9851392c29235491`
- Vertex compiler: glslc `2026.3`
- Fragment compiler: glslang `16.4.0`
- Validator: SPIRV-Tools `2026.3` (`vulkan-sdk-1.4.357.0-0-g9a49b0883`)
- Target: SPIR-V 1.0 for Vulkan 1.0

`indexed_ks_per_pixel_shadow.frag` is the optional Vulkan receiver variant of
the bounded `ksPerPixel` fixture. It retains bindings 0-3 and adds three
sampled D32 maps at bindings 16-18, one nearest clamp-to-edge sampler at
binding 19, and a 256-byte receiver record at binding 20. The record contains
the three native-clip-space matrices, 2/12/50 splits, source biases, main
camera position, and camera direction.

The shader follows `public/app.js:2067-2068` for hard cascade selection and
explicit 3x3 PCF. Because native matrices already convert depth to Vulkan's
`[0,1]` convention, the receiver remaps only projected x and y. It follows
`public/app.js:2082` by multiplying direct diffuse and Blinn specular by the
shadow factor while leaving ambient and emissive unchanged. It is a
source-evidenced portable fixture, not recovered stock bytecode. D3D12
receiver execution remains staged pending a Windows WARP verification.
This variant applies the same fog stage after shadowed direct lighting.

Compile and verify it with:

```sh
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S frag \
  -o /tmp/apex_indexed_ks_per_pixel_shadow_frag.spv \
  native/tests/shaders/indexed_ks_per_pixel_shadow.frag
spirv-val --target-env vulkan1.0 \
  /tmp/apex_indexed_ks_per_pixel_shadow_frag.spv
```

- Fragment source SHA-256: `901d53a3e005dfcf75f656d3eeaf0d8aae5045c4184fe402caa4057b1d42772d`
- Fragment SPIR-V SHA-256: `584315bed0313e5ff39db8974a684a295cc9ac38295addacb2231f3d13e4765a`
- Compiler: glslang `16.4.0`
- Validator: SPIRV-Tools `2026.3` (`vulkan-sdk-1.4.357.0-0-g9a49b0883`)
- Target: SPIR-V 1.0 for Vulkan 1.0

SwiftShader Vulkan pixel evidence with validation enabled uses the complete
stock-scene facade and uniform retained maps:

- Zero-depth maps: `(3,40,18,255)`; direct sun is fully attenuated.
- One-depth maps: `(16,116,34,255)`; the original direct-light pixel remains.

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
The fixture applies the shared fog stage after this bounded lighting equation.

The NM fixture identities are recorded after compiling with glslang 16.4.0
and validating with SPIRV-Tools 2026.3:

- Vertex source SHA-256: `44a4366343584418a39008707ea0c629454b716ef43b1783ccb20b0ddae257a7`
- Vertex SPIR-V SHA-256: `1d8fa2d0a866c374d42f55e9e92f4a39bbbf8abd6c9beceb8bb9f3de5945d34c`
- Fragment source SHA-256: `fcf96fa5618c8c7c592bf42d4d85ab039bc6ce33e443132d2ab53405bd90ab9a`
- Fragment SPIR-V SHA-256: `eab2ebc8d37b968f635b83e085cd15727abb4c04004d1db4c2b0ab79b00b995c`

`indexed_ks_per_pixel_nm_maps.vert` and
`indexed_ks_per_pixel_nm_maps.frag` extend the bounded tangent-space normal
fixture with the exact `txMaps` channels used by `public/app.js:2080`.
The maps image and sampler are set 0 bindings 6 and 7 (D3D12 `t6` and `s7`),
after diffuse 0/1, material/frame UBOs 2/3, and normal 4/5. The fragment
equation is:

```text
maps = texture(txMaps, vUv).rgb       // absent-source fallback is vec3(1)
mappedSpecular = ksSpecular * maps.r
mappedPower = max(1, ksSpecularEXP * maps.g + 1)
```

The resulting lighting remains the bounded tangent-space direct-light path:
ambient plus directional diffuse, sun Blinn specular, and emissive. The
fixture excludes `txDetail`/`txNormalDetail`, multilayer world-XZ resources,
Fresnel/reflection (`maps.b` is sampled but has no consumer in this bounded
fixture), shadows, AO/VAO, rain, seasons, local/CSP lights, damage, alpha
testing, transparency, and overlays. This is an execution fixture for the
maps channel semantics. The stock facade uses it for the bounded
`ksPerPixelMultiMap` and `ksPerPixelMultiMap_AT` families. Material preflight
rejects active detail and Fresnel. This is not a complete stock shader.

Maps fixture identities, compiled with glslang `16.4.0` and validated with
SPIRV-Tools `2026.3` (`vulkan-sdk-1.4.357.0-0-g9a49b0883`), target SPIR-V 1.0
for Vulkan 1.0:

```sh
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S vert \
  -o /tmp/apex_indexed_ks_per_pixel_nm_maps_vert.spv \
  native/tests/shaders/indexed_ks_per_pixel_nm_maps.vert
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S frag \
  -o /tmp/apex_indexed_ks_per_pixel_nm_maps_frag.spv \
  native/tests/shaders/indexed_ks_per_pixel_nm_maps.frag
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_ks_per_pixel_nm_maps_vert.spv
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_ks_per_pixel_nm_maps_frag.spv
```

- Vertex source SHA-256: `54e299f32bf8ec6b73f5feaefd84c1c8e67c07438635a27e6f2d5715cd68d20f`
- Vertex SPIR-V SHA-256: `1d8fa2d0a866c374d42f55e9e92f4a39bbbf8abd6c9beceb8bb9f3de5945d34c`
- Fragment source SHA-256: `566264a053d684d5f66afc2be4244c1b023861ccd0b9bae7761a432c33098cf5`
- Fragment SPIR-V SHA-256: `cd4e079ce2fff6e34a8babf3cebbf1a2ee43d4eab9e9ed06c247f9046d23651f`

SwiftShader Vulkan pixel evidence (`VK_ICD_FILENAMES=/opt/google/chrome/vk_swiftshader_icd.json`,
`APEX_RENDER_VALIDATION=1`) at the center of the fixed triangle is:

- `maps=(0,0,1)`: `(7,88,37,255)`; specular scale is zero.
- `maps=(1,0,1)`: `(99,180,128,255)`; exponent is independently reduced to one.
- `maps=(1,1,1)`: `(74,155,103,255)`; full maps scale and exponent restore the NM fixture.

`indexed_ks_per_pixel_nm_detail_stack.vert` and
`indexed_ks_per_pixel_nm_detail_stack.frag` are the bounded 12-binding generic
detail stack. They retain diffuse/normal/maps at bindings 0/1, 4/5, and 6/7,
and add `txDetail` at bindings 8/9 and `txNormalDetail` (or `txDetailNM`) at
bindings 10/11. D3D12 uses the corresponding `t0/s1`, `b2/b3`, `t4/s5`,
`t6/s7`, `t8/s9`, and `t10/s11` registers.

The material record is the 80-byte portable layout. The detail fixture reads
`lighting`, `fresnel`, `emissive`, and `detail`. The final `damageZones` value
is unused. The `detail` value is `(useDetail, detailUVMultiplier,
detailNormalBlend, reserved)`. The fragment source follows
`public/app.js:2074` for the detail color/mask, `:2077` for tangent-space
normal detail, and `:2080` for detail-alpha specular modulation and maps.r/g.
It excludes Fresnel/reflection, multilayer world-XZ resources, damage, rain,
seasons, AO, shadows, local/CSP lights, custom emissions, shadow cutouts,
transparency, and overlays. A pipeline can pair this main-color fixture with
4x A2C because the production main pass uses output alpha without a discard.
This is an exact bounded execution fixture, not complete stock shader parity.

Detail-stack identities, compiled with glslang `16.4.0` and validated with
SPIRV-Tools `2026.3` (`vulkan-sdk-1.4.357.0-0-g9a49b0883`), target SPIR-V 1.0
for Vulkan 1.0:

```sh
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S vert \
  -o /tmp/apex_indexed_ks_per_pixel_nm_detail_stack_vert.spv \
  native/tests/shaders/indexed_ks_per_pixel_nm_detail_stack.vert
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S frag \
  -o /tmp/apex_indexed_ks_per_pixel_nm_detail_stack_frag.spv \
  native/tests/shaders/indexed_ks_per_pixel_nm_detail_stack.frag
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_ks_per_pixel_nm_detail_stack_vert.spv
spirv-val --target-env vulkan1.0 /tmp/apex_indexed_ks_per_pixel_nm_detail_stack_frag.spv
```

- Vertex source SHA-256: `27b9f93f0037d0ab7975d479a4746e42e7481fe094f0dad3da9db116bb456155`
- Vertex SPIR-V SHA-256: `1d8fa2d0a866c374d42f55e9e92f4a39bbbf8abd6c9beceb8bb9f3de5945d34c`
- Fragment source SHA-256: `4f34f4631323cf09218e96752911a5e21b852dc961154dc3f88b392b086280c9`
- Fragment SPIR-V SHA-256: `f38e6771eb2f3888299c12b25df62800f49c348b68f1de9937fd08b28a9e8972`

SwiftShader execution uses a diffuse alpha of zero. This value gives the detail stack a full mask.

- `useDetail=0`: `(35,70,42,0)`. The shader preserves the base diffuse result.
- `detailUVMultiplier=1`: `(35,18,21,0)`. The second detail texel changes the diffuse color.
- `detailUVMultiplier=2`: `(9,70,21,0)`. Repeated UVs select the first detail texel.
- `txDetail.a=0`: `(35,70,42,0)`. The detail alpha suppresses mapped specular.
- `txDetail.a=1`: `(102,137,109,0)`. The detail alpha restores mapped specular.
- `detailNormalBlend=1`: `(26,13,16,0)`. The normal-detail texture changes the tangent-space normal.

The same run passes descriptor-switch batches and a mixed 6/8/12-binding batch.

`indexed_ks_per_pixel_damage.frag` implements the recovered dirt-zero branch of
`ksPerPixelMultiMap_damage_dirt`. The installed shader container has SHA-256
`76d6a625c34e386641667a44a0433c6d1dc2af5be9e4a8ebbd6a886181f97dc8`.
Its pixel FXO has SHA-256
`ef7835b917eddf0b6e233d05ef8d66126e20bf4ca828b429b742dca3552d7743`.
The DXBC disassembly proves that the final alpha source is `txNormal.a`.

The portable ABI uses `txDamage` at bindings 12/13. It uses `txDamageMask` at
bindings 14/15. The 80-byte material record stores `damageZones` at byte 64.
The base fixture does not bind `txDust`. It isolates the recovered damage-mix
equation, where the dust RGB factor is one for zero dirt.
The fixture does not execute stock detail, sun-specular, Fresnel, or reflection
branches. It is a bounded damage stage, not a complete stock-material shader.
The source SHA-256 is
`9e9035e8db826a2a68ff983a32b7122627dedb28aefbc404216d13e74a6e0b0f`.
The SPIR-V SHA-256 is
`8807c4be81b7d067c8335691ebd33fd08b496bb9314850eb62094fd638c84b54`.

SwiftShader Vulkan pixel evidence is:

- `damageZones=(0,0,0,0)`: `(74,155,103,255)`.
- `damageZones=(1,0,0,0)`: `(81,81,60,255)`.
- `damageZones=(1,0,0,0)` and `txNormal.a=0`: `(64,65,43,255)`.

`indexed_ks_per_pixel_damage_dust.frag` extends the same damage fixture with
the source-evidenced `txDust` alpha sample. It uses bindings 8 and 9, which are
mutually exclusive with the generic detail stack. The damage resources remain
at bindings 12 through 15. This low-level ABI does not prove that the stock
material facade binds `txDust`.

The fixture identities are:

- Source SHA-256: `5bf97d8f30038d8e522018d2e5c8255d795580f7a5e48e13754d81fac5b57332`
- SPIR-V SHA-256: `bc9acb6ec974bf9d9211d3b04dc8ae0e69482b35eb117cfa6f1d6c8e47721276`
- Compiler: glslang `16.4.0`
- Target: SPIR-V 1.0 for Vulkan 1.0

SwiftShader Vulkan pixel evidence is:

- `txDust.a=1`: `(81,81,60,255)`, equal to the base damage fixture.
- `txDust.a=0`: `(4,4,2,255)`, with direct diffuse and specular light removed.

## Selected mesh

`selected_mesh.frag` reads one flat RGBA value from set zero, binding zero.
The Vulkan backend uses this uniform with the selected-mesh matrix constants.
D3D12 maps the same value to pixel register `b5`.

This shader is a portable ABI fixture. It is not recovered stock bytecode.
Its source SHA-256 is
`55721172b8484cfe279fbeb9442c319c4a792808e8153cc21975cf90c17231f8`.
Its SPIR-V SHA-256 is
`4343fe40bd1add7687d114c66dc9c7c419fa74b420e89629ffe07223576c6213`.
