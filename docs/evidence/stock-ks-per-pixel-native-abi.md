# Stock `ksPerPixel` native ABI evidence

## Provenance

The installed SDK editor supplies `ksPerPixel.shader` and
`ksPerPixelAT.shader`. The repository does not redistribute these files.

The native host evidence below comes from the installed 32-bit
`sdk/editor/ksNet.dll` (17,211,904 bytes), whose SHA-256 is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The function addresses are loaded-image addresses from the matching Ghidra
analysis.

| Item | Bytes | SHA-256 |
|---|---:|---|
| `ksPerPixel.shader` | 11,254 | `255d0228faa70d5b8454a2abe618447bef16ef29612ffab4a0d684dbedcfdb0b` |
| `ksPerPixelAT.shader` | 11,330 | `3e3ea224b80fadacb8070fe026e29626f7c60e991e6290902fb2341be27ba379` |
| Shared vertex stage | 3,728 | `55451102bc40c0b7cca57bdc4e6e51d12456c85e18b59eb69ef79196f82700a5` |
| Base pixel stage | 7,508 | `2f3a71060f92a69e1da5a4ee5e6597afaebb92b293e66672e549a77566138e2c` |
| AT pixel stage | 7,584 | `06f8436bba0f3b3b5714e02622cf0332504f3077775c7f5c02e3d9471c1efa15` |

The strict package parser identifies both stages as Shader Model 4.0 DXBC.
The installed-fixture test checks both package shapes when
`ASSETTO_CORSA_ROOT` is available.

## Register contract

DXBC reflection gives the complete register map for this family.

| Register | Name | Bytes | Stages |
|---|---|---:|---|
| `b0` | `cbCamera` | 224 | vertex |
| `b1` | `cbPerObject` | 64 | vertex |
| `b2` | `cbLighting` | 160 | vertex and pixel |
| `b3` | `cbShadowMaps` | 208 | vertex and pixel |
| `b4` | `cbMaterial` | 32 | pixel |
| `t0` | `txDiffuse` | — | pixel |
| `t6` through `t8` | `txShadow0` through `txShadow2` | — | pixel |
| `s0` | `samLinear` | — | pixel |
| `s1` | `samShadow` comparison sampler | — | pixel |

The backend projection keeps each native register number. Vulkan uses three
descriptor sets because Vulkan has one binding namespace in each set.

| Native class | Vulkan set | Binding rule | D3D12 space |
|---|---:|---|---:|
| `bN` constant buffer | 0 | `N` | 0 |
| `tN` sampled texture | 1 | `N` | 0 |
| `sN` sampler | 2 | `N` | 0 |

This projection keeps the unused texture bindings from `t1` through `t5`.
It also keeps `b0`, `t0`, and `s0` in separate Vulkan sets.

The D3D12 root plan uses five descriptor tables:

- Vertex constant buffers `b0` through `b3`
- Pixel constant buffers `b2` through `b4`
- Pixel texture `t0`
- Pixel textures `t6` through `t8`
- Pixel samplers `s0` through `s1`

The two constant-buffer tables can point to the same `b2` and `b3` heap
descriptors. This method keeps the recovered stage visibility exact.

`cbCamera` stores three matrices at offsets 0, 64, and 128. The camera
position starts at 192. The four trailing scalar values start at 208.

`cbPerObject` contains the 64-byte world matrix at offset 0.

`cbMaterial` stores four lighting scalars at offsets 0 through 12. The
three-component emissive color starts at 16. `ksAlphaRef` is at 28.

`cbShadowMaps` stores three matrices at offsets 0, 64, and 128. Its three
bias values start at 192. The reciprocal texture width is at 204.

The host transposes all matrices before it uploads them. This rule applies to
the view, projection, inverse-view-projection, world, and shadow matrices.

## Vertex and draw contract

The standard `MeshVertex` record has a stride of 44 bytes.

| Offset | Input | Format |
|---:|---|---|
| 0 | `POSITION0` | `R32G32B32_FLOAT` |
| 12 | `NORMAL0` | `R32G32B32_FLOAT` |
| 24 | `TEXCOORD0` | `R32G32_FLOAT` |
| 32 | `TANGENT0` | `R32G32B32_FLOAT` |

`MeshVertex::MeshVertex` at `0x10039ad3` initializes this record.
`VertexBuffer<MeshVertex>::VertexBuffer` at `0x10047245` supplies the 44-byte
stride. `getInputLayout` at `0x1000eea0` supplies the four input elements.

The index buffer uses `DXGI_FORMAT_R16_UINT`. The default primitive topology
is `D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST`.

## Sampler contract

`GraphicsManager::initSamplerStates` at `0x10045639` creates the stock
samplers. `GraphicsManager::setSamplerState` at `0x100466f6` binds them.

The `samLinear` sampler at `s0` uses these values:

- Anisotropic filtering
- Wrap mode for the U, V, and W axes
- The video setting for maximum anisotropy
- The render flag for the mip LOD bias
- The maximum finite float for the maximum LOD

The `samShadow` sampler at `s1` uses these values:

- Comparison min-mag linear and mip point filtering
- Clamp mode for the U, V, and W axes
- `LESS` comparison

`kglCreateSampler` at `0x1000c560` maps the recovered filter and address
selectors to these D3D11 values. The instruction at `0x1000c5b5` writes
filter value `0x94`. The same function writes comparison value `2`.

## Recovered pixel equation

The base pixel stage uses the interpolated normal directly. The AT stage
normalizes the normal first. The remaining equations are identical.

```text
L = -ksLightDirection
direct = saturate(dot(N, L))
hemisphere = saturate(0.25 * N.y + 0.75)

baseLight =
    ksAmbientColor * ksAmbient * hemisphere
  + ksEmissive
  + ksLightColor * ksDiffuse * direct * shadow

H = normalize(normalize(-cameraToSurface) + L)
specular =
    ksLightColor
  * pow(saturate(dot(N, H)), max(1, ksSpecularEXP))
  * ksSpecular
  * shadow

lit = sampledDiffuse.rgb * baseLight + specular
rgb = lerp(lit, ksFogColor, fog)
alpha = sampledDiffuse.a
```

Neither main pixel stage discards a fragment. The AT package flag selects
alpha-to-coverage pipeline state. `ksAlphaRef` belongs to the separate shadow
cutout path.

## Vertex instruction evidence

The base and AT packages contain the same 3,728-byte vertex stage. Its
SHA-256 is
`55451102bc40c0b7cca57bdc4e6e51d12456c85e18b59eb69ef79196f82700a5`.

The vertex stage computes the world position with four `dp4` instructions:

```text
worldPos.x = dot(position, cbPerObject[0])
worldPos.y = dot(position, cbPerObject[1])
worldPos.z = dot(position, cbPerObject[2])
worldPos.w = dot(position, cbPerObject[3])
```

It then applies the view and projection matrices with eight more `dp4`
instructions. The host uploads transposed matrices. The native column-vector
equation is:

```text
clip = projection * view * world * position
```

The vertex stage transforms the normal with the upper three rows of the world
matrix. It does not normalize the result or apply an inverse-transpose matrix.

```text
normalOut.x = dot(normal, cbPerObject[0].xyz)
normalOut.y = dot(normal, cbPerObject[1].xyz)
normalOut.z = dot(normal, cbPerObject[2].xyz)
```

The camera-to-surface output uses the camera position at `cbCamera[12].xyz`:

```text
cameraToSurface = worldPos.xyz - cbCamera[12].xyz
```

The stage copies `texcoord0` without a change. It computes the fog output from
the view-space depth and two lighting values:

```text
q = (-viewPos.z / cbLighting[5].w) * 5.77078009
fogShape = saturate((exp2(q) - exp2(-q)) / (exp2(-q) + exp2(q)))
fogOut = fogShape * cbLighting[6].x
```

The Shader Model 4 `exp` instruction is a base-2 exponential. The Vulkan
source equivalent uses GLSL `exp2`; GLSL `exp` would change this equation.

`cbLighting[5].w` is the linear fog value at byte `0x5c`.
`cbLighting[6].x` is the fog blend at byte `0x60`.

The stage computes three four-component cascade coordinates. Each coordinate
uses one 4-by-4 shadow matrix and four `dp4` instructions.

| Output | `cbShadowMaps` registers | Byte offset |
|---|---|---:|
| `o4` | `0` through `3` | `0x00` |
| `o5` | `4` through `7` | `0x40` |
| `o6` | `8` through `11` | `0x80` |

The vertex stage does not divide these coordinates by `w`. It does not read
the bias and reciprocal-width record at `cbShadowMaps[12]`.

## Pixel instruction evidence

The recovered equations come from the `SHDR` instructions in the two pixel
stages. The base stage SHA-256 is
`2f3a71060f92a69e1da5a4ee5e6597afaebb92b293e66672e549a77566138e2c`.
The AT stage SHA-256 is
`06f8436bba0f3b3b5714e02622cf0332504f3077775c7f5c02e3d9471c1efa15`.

The main pixel stage reads only these `cbLighting` fields:

| Register value | Use |
|---|---|
| `b2[0].xyz` | Light direction |
| `b2[1].rgb` | Ambient color |
| `b2[2].rgb` | Sun color |
| `b2[6].yzw` | Fog color |

The main pixel stage does not read the other declared lighting fields.
It reads the fog value from `v3.z` and saturates it before the final color
interpolation.

The shader selects one shadow cascade. It selects cascade 1 only after
cascade 0 fails its bounds test. The same rule applies to cascade 2.

Each bounds test uses strict normalized limits for both texture coordinates.
The valid range is more than `-1` and less than `1`.

The shader reads the three biases from `b3[12].xyz`. It reads the reciprocal
shadow-map width from `b3[12].w`. Each cascade depth uses this adjustment:

```text
adjustedDepth = cascadeDepth - bias * (2048 * reciprocalWidth)
```

The selected cascade uses nine `sample_c_lz` instructions. The shader averages
the nine comparison results. An invalid cascade selection produces a shadow
factor of `1`.

Cascade 2 also uses this fade term:

```text
fade = 2 * max(-(cascade2.y + 0.5), 0)
shadow = saturate(nineTapAverage + fade)
```

The comparison sampler uses `LESS`. This value comes from the host sampler
creation flow and the `sample_c_lz` shader instructions.

The base stage uses the interpolated normal without normalization. The AT
stage applies `dp3`, `rsq`, and `mul` before all lighting calculations.

Both stages copy the sampled diffuse alpha to `o0.w`. Neither stage reads
`ksAlphaRef`, and neither stage contains a discard instruction.

## Host and shadow evidence

`Material::initShaderVars` at `0x10040420` selects alpha-to-coverage for an AT
package. `MaterialFilterSM::apply` at `0x100650c7` selects the separate
alpha-tested shadow generator for non-opaque materials.

The shadow receiver uses a comparison sampler and nine PCF taps. The host
writes the shadow state through these functions:

- `GraphicsManager::setShadowMapBias` at `0x10046833`
- `GraphicsManager::setShadowMapMatrix` at `0x10046873`
- `GraphicsManager::setShadowMapTexture` at `0x100468c7`
- `CameraShadowMapped::endShadowMapPass` at `0x1005e57f`

`GraphicsManager::initCBuffers` at `0x10045138` creates the four shared system
buffers (`b0` through `b3`), registers them by the names `cbCamera`,
`cbPerObject`, `cbLighting`, and `cbShadowMaps`, and marks them as system
buffers. `CBuffer::CBuffer` at `0x1004ab2d` clears every buffer byte.
`GraphicsManager::commitShaderChanges` at `0x10044dc6` commits the updated
camera, lighting, object, and shadow buffers.

`KGLShader::reflectVars` at `0x1000fab0` obtains the low-level reflected
constant-buffer, variable, and texture descriptions from `D3DReflect`.
`Shader::reflectVars` at `0x1004b218` then applies the ownership boundary:
for each reflected constant buffer it calls `GraphicsManager::getCBuffer`
with the reflected name. A matching shared buffer is not appended to the
shader's `cBuffers` vector. The same name lookup is performed for each
reflected variable; variables in a shared system buffer are not added to the
shader's material-variable list. Only buffers with no matching system record
are created as shader-owned buffers.

`Mesh::render` at `0x100494ff` applies the material before it binds the mesh.
It then binds the vertex buffer and the index buffer.
Next, it commits shader changes and calls `GraphicsManager::drawPrimitive` at
`0x10044ffc`.

`Shader::apply` at `0x1004b11b` binds the input layout, vertex shader, and
pixel shader. `kglSetIndexBuffer` at `0x1000d610` uses `DXGI_FORMAT_R16_UINT`.
`GraphicsManager::commitShaderChanges` commits the system buffers in this
order: camera, lighting, object, and shadow.

The matrix setters provide the upload convention:

- `GraphicsManager::setProjectionMatrix` at `0x10046632`
- `GraphicsManager::setViewMatrix` at `0x1004698f`
- `GraphicsManager::setWorldMatrix` at `0x10046b7e`
- `GraphicsManager::updateLightingSetttings` at `0x10046c2c`

`GraphicsManager::initCBuffers` initializes the shared records before the
first scene update. The light direction starts as normalized
`(0.7071068, -0.7071068, 0)`. Ambient starts as `(0.4, 0.4, 0.4, 1)`.
Exposure starts at `2`, the exposure limits at `0` and `10000`, DOF focus and
range at `400` and `500`, and saturation at `1`. The constructors clear the
remaining bytes. New material matrices are therefore zero matrices, not
identity matrices.

`Material::setShader` at `0x100407fc` calls `Material::createCBuffers` at
`0x1004026a` after a shader change. That function allocates only the buffers
already present in `Shader::cBuffers`. For stock `ksPerPixel` and
`ksPerPixelAT`, system-buffer filtering leaves only `cbMaterial` at `b4`, so
the material owns and commits only that 32-byte buffer. It does not own or
overwrite `b0` through `b3`.

`Material::apply` at `0x1004016e` binds material resources and the shader,
then commits the material buffer vector. `PvsProcessor::doRenderCalls` at
`0x10065b85` sorts its 100-byte draw records, commits shared camera, lighting,
and shadow state, binds shadow maps at `t6` through `t8` (and the cube map at
`t10` when present), and commits the material's `b4` buffer when the material
changes. If a draw record has a world matrix, it writes and commits `b1`; if
not, the previous `b1` value remains bound. It then issues the indexed draw.

`PvsProcessor::doRenderClassic` at `0x10065e44` is the non-sorted traversal:
it applies the material, sets the world matrix when present, binds geometry,
calls `GraphicsManager::commitShaderChanges`, and draws. Neither PVS path
replaces the shared `b0` through `b3` records with material-owned copies.

## Vulkan source-equivalent modules

The checked-in GLSL implements the recovered vertex, base pixel, and AT pixel
equations. It preserves the native three-set descriptor projection and does
not use the portable draw-matrix push-constant ABI. The sources and generated
SPIR-V have these identities:

| Module | Source SHA-256 | SPIR-V SHA-256 |
|---|---|---|
| Shared vertex | `be94c0a01e37a32d744a1521c220a57508acec1c42c2cdcf7bd32c1fbcde84c0` | `8d325332d2d8e3a38499845f108f701d23758e1d454d9e2e349aa2fdf4dacf58` |
| Base fragment | `e7f5e6e45b7a2241662e0dd1f34705f87b5c74318db4dea7893330e6464848d9` | `c28c30946d76b1e856c2bfcb1dfe44e08d7645d9505913a5bb54b3fa300ff1b6` |
| AT fragment | `0c628068a21eca27ad54fb6416506484bd90664dc777ce0f475b192d2cf72a64` | `189c37c74539603bb0100e87afffe4bd5e31ae754ff6655692201f8e7678f989` |

The modules were generated with glslang `16.4.0`:

```text
glslangValidator -V --target-env vulkan1.0 -Os -g0 -S <stage> -o <output> <source>
spirv-val --target-env vulkan1.0 <output>
```

The comparison samples use explicit LOD zero. SPIR-V disassembly contains
`OpImageSampleDrefExplicitLod` with a zero LOD, matching `sample_c_lz`.
Diffuse sampling remains implicit LOD, matching the native `sample`
instruction. The specular power uses an explicit GLSL `log2` and `exp2` pair,
matching the Shader Model 4 base-2 `log` and `exp` instructions.

The generated SPIR-V is a source equivalent. It is not a translation of the
installed DXBC and does not claim instruction-stream identity. The move-only
`ValidatedStockKsPerPixelVulkanSourceProgram` selects only these private
modules by variant and validates their exact bytes, descriptor contract,
vertex layout, render-target state, and provenance. Arbitrary caller SPIR-V
cannot enter this authority by using a `source_equivalent` label.

## Port boundary

[`stock_ks_per_pixel.hpp`](../../native/include/apex/render/stock_ks_per_pixel.hpp)
owns the exact buffer layouts and register manifest. It also contains the
separate base and AT variant contract.

`create_validated_stock_ks_per_pixel_native_program` takes ownership of a
parsed package. It returns a move-only program only after the complete gate
accepts the package. Thus, a caller cannot change the stage bytes after
validation.

`allocate_stock_ks_per_pixel_native_shaders` accepts only this move-only
program. D3D12 allocates the vertex and pixel module objects in stage order.
The function rejects Vulkan DXBC before it calls the device.

`allocate_stock_ks_per_pixel_native_constant_buffers` validates the five
native records before allocation. Vulkan and D3D12 each create one mutable
256-byte view for every reflected buffer. The allocator clears all bytes
after each record. It releases earlier buffers if a later allocation fails.

The Vulkan draw request uses
`explicit_stock_ks_per_pixel_vulkan_source_equivalent`. It requires the exact
move-only source owner and a separate native-ABI resource binding. Common
preflight rejects copied pipelines, probe/source authority overlap, portable
resource overlap, variant/sample-state drift, missing records, and descriptor
contract drift. The Vulkan backend reuses the three-set native-ABI descriptor
executor without relabeling the structural probe as source-equivalent.

The SwiftShader runtime test executes both variants. The base path uses a
single-sample target. The AT path uses a 4x target and resolves it for
readback. Both center pixels are checked against the recovered CPU equation.
This test executes the generated SPIR-V; it does not execute the installed
DXBC.

The CPU evaluator follows the recovered pixel equation. It rejects non-finite
records and degenerate normalization inputs before evaluation. This evaluator
does not execute the installed DXBC.

The Vulkan ABI probe manifest uses separate descriptor sets for constant
buffers, textures, and samplers. It validates structure only. The runtime does
not execute this three-set probe yet.

The D3D12 native draw path creates the root signature, pipeline state,
descriptors, and command list. Its bounded batch path supports opaque base
draws on a single-sample target. A Windows WARP pipeline and readback test must
pass before production selection becomes available.
