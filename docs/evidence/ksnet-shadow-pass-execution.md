# ksNet directional shadow pass execution

Status: This note records the exact native pass schedule and shadow-resource
ABI. It does not change production code.

## Binary evidence

- `ksNet.dll`: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`
- SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- Matching `ksNet.pdb` is installed beside the binary.
- Native virtual addresses below use the loaded image base and the PDB
  `.text` section base mapping.

## Cascade pass scheduling

PDB/Ghidra identifies these functions:

| Function | Address |
| --- | ---: |
| `CameraShadowMapped::beginShadowMapPass` | `0x1005dd2e` |
| `CameraShadowMapped::endShadowMapPass` | `0x1005e57f` |
| `CameraShadowMapped::shadowMapPass` | `0x1005eb1f` |
| `CameraShadowMapped::renderPass` | `0x1005e681` |
| `PvsProcessor::begin` | `0x10065859` |
| `PvsProcessor::end` | `0x10065f2f` |

`shadowMapPass(root, dt)` sets normal depth and opaque blending. It also sets a
`MaterialFilterSM` and a `CameraMeshFilter` with the `Shadowgen` pass ID. It
copies the camera matrix. Then it loops through `level = 0..2`.

The function creates the `Shadowgen` filter at `0x1005EB8F`. It passes the
main `CameraShadowMapped` camera to this filter. Each mesh supplies its current
world matrix. The filter does not use a cascade camera for visibility.

For each level, it does these operations:

1. calls `beginShadowMapPass(level, ...)`.
2. starts `PvsProcessor::begin(..., Shadows, nullptr, nullptr)`.
3. invokes the root virtual render traversal.
4. calls `PvsProcessor::end()`.
5. calls `endShadowMapPass(level)`.

`PvsProcessor::end` applies exclusion, distance/LOD, and frustum tests. Then it
dispatches `doRenderClassic` for `Shadows`. Thus, the native loop renders three
independent depth passes. It does not render all cascades in one pass. It does
not select a cascade from the receiver distance during caster traversal.

`beginShadowMapPass` sets back-face culling and normal depth. It computes the
level matrix from the camera matrix, near/far values, light direction, and
camera height. Then it sets the viewport from the level render target. It
clears the target depth to `1.0`.

`endShadowMapPass` unbinds the render targets. It binds the completed depth
target for the level and uploads the level matrix.

## Map allocation and lifetime

`CameraShadowMapped::CameraShadowMapped` is at `0x1005d872`. It resizes the
target and matrix vectors to exactly three entries. It then calls `0x1005e5c4`
once to allocate all three targets.

The allocation loop reads one width from `GraphicsManager + 0x2c`. It uses
that value for both target dimensions. The installed `cfg/video.ini` sets
`SHADOW_MAP_SIZE=2048`.

`CameraShadowMapped::~CameraShadowMapped` is at `0x1005da89`. It deletes each
target, then destroys both vectors. No other constructor or resize call site
was found. This evidence supports three reusable maps for the camera lifetime.
It does not prove a separate device-loss recovery path.

## Receiver resource ABI

PDB/Ghidra identifies the exact host setters:

| Function | Address | Effect |
| --- | ---: | --- |
| `GraphicsManager::setShadowMapBias` | `0x10046833` | three floats to `cbShadowMap + 0xc0` |
| `GraphicsManager::setShadowMapMatrix` | `0x10046873` | transpose one matrix to `+0x00`, `+0x40`, or `+0x80` |
| `GraphicsManager::setShadowMapTexture` | `0x100468c7` | bind the depth view at texture stage `level + 6` and write `1.0 / width` to `+0xcc` |

The resulting stock constant buffer is 208 bytes. It contains three 64-byte
matrices, three bias floats, and one texture-size float. The installed stock
`ksPerPixel_ps.fxo` declares this buffer at pixel register `b3`. It samples
depth maps at `t6`, `t7`, and `t8`. It uses comparison sampler `s1`.

The shader selects the cascades and does the 3x3 comparison PCF operations.
The host setters only bind maps and upload matrices, bias, and reciprocal
width.

The constant-buffer write helper at `0x1004abeb` changes CPU bytes and marks
the buffer dirty. `CBuffer::commit` at `0x1004ab88` performs the GPU update and
clears that flag. The graphics manager commits this buffer at its next shared
commit boundary. Therefore, each cascade updates receiver state before the
later color draw. The exact outer managed dispatch order remains an inference.

`setShadowMapTexture(level, nullptr)` clears texture stage `level + 6`. It does
not write the texture size. A completed level binds its depth resource. It also
updates the shared reciprocal width. The caller must bind all three completed
levels before a shadow-enabled receiver draw.

## Caster material contract

`MaterialFilterSM::MaterialFilterSM` is at `0x10064fc4`. It resolves the
`ksShadowGen`, `ksShadowGenAT`, and `ksShadowGenSKIN` shader identifiers
through `GraphicsManager::getShader`. It sets `useNullPS = true` on the normal
and skinned shader objects. It does not set this value on the alpha-tested
object. `MaterialFilterSM::apply` is at `0x100650c7`.

The PDB `Material` layout gives the relevant fields exactly:

| field | native offset | type/meaning |
| --- | ---: | --- |
| `blendMode` | `+0x5c` | `BlendMode`: `eOpaque=0`, `eAlphaBlend=1`, `eAlphaToCoverage=2` |
| `cBuffers` | `+0x3c` | vector of material constant buffers |
| `resources` | `+0x30` | vector of `{slot, texture}` records |
| `doubleFaceShadow` | `+0x64` | bool |

`apply` first selects `smSkinned` when the material shader variable at `+0x50`
is `1`. Otherwise, it selects `smAlphaTested` when `blendMode != eOpaque`.
It selects `smNormal` for the remaining materials.

The non-skinned, non-opaque branch scans the resource vector in order. It
finds the first record with texture slot `0`. Then it calls
`GraphicsManager::setTexture(0, texture)`.

Every non-opaque material scans the material constant buffers and commits each
buffer that uses slot `4`. This buffer rule also applies when the earlier
skinned branch selected `smSkinned`. The skinned branch does not scan or bind
the diffuse texture.

The common order is shader selection, opaque blend state, optional texture
binding, optional slot-4 buffer commit, and cull state.
`GraphicsManager::setTexture` is at
`0x10046927`. This function forwards the material resource slot to the
matching native texture stage. It also updates the cached texture pointer.
`CBuffer::commit` is at `0x1004ab88`. After it maps a modified buffer, it
binds the buffer to the recorded slot.

The ordinary opaque path uses the null-pixel path of the normal shader. It
does not enter the texture or slot-4 buffer branch. The skinned shader also
has `useNullPS = true`. A non-opaque skinned material still commits its slot-4
buffers, but it keeps the skinned shader and binds no diffuse texture.

The PDB names `Material::doubleFaceShadow`. Thus, the final cull test is not an
inferred alias for the ordinary `doubleFace` field at `+0x1c`.
`doubleFaceShadow == false` selects `eCullBack`. A true value selects
`eCullNone`.

When `isAlphaTested` is true, `Material::initShaderVars` sets
`blendMode = eAlphaToCoverage`. This function is at `0x10040420`. The
constructor at `0x1003fcc7` initializes new materials to `eOpaque` and
`doubleFaceShadow = false`. It does not write a default `ksAlphaRef` value.
Thus, an alpha-tested static material reaches the `ksShadowGenAT` branch. The
alpha threshold comes from the shader-variable data of the material.

The installed alpha-tested shader binaries provide the register and field
layout. `ksShadowGenAT_ps.fxo` SHA-256 is
`6782f08729c2dcd68254553f08ce6ad682e28f00fc9f493c48cd8c027ffc21f2`. Its
DXBC RDEF declares `cbMaterial` at `b4`, `txDiffuse` at `t0`, and
`samLinearShadow` at `s3`. `cbMaterial` is 32 bytes:
`ksAmbient` offset `0`, `ksDiffuse` `4`, `ksSpecular` `8`, `ksSpecularEXP`
`12`, `ksEmissive` `16` (three floats), and `ksAlphaRef` offset `28`
(`cb4[1].w`). The shader token stream contains a `discard` instruction after
the diffuse-texture sample and alpha comparison. `ksShadowGenAT_vs.fxo`
SHA-256 is
`8558a417dead16385fc9565c4882da6a8771f39b2ba4e1af47badb55284d1c85`. Its
RDEF binds `cbCamera` at `b0` and `cbPerObject` at `b1`. It places `ksWorld`
at byte offset `0` in `b1`. These shader binaries establish the exact texture,
sampler, buffer, and alpha-reference locations. They do not establish a
default `ksAlphaRef` value for a material that omits this variable.
`MaterialFilterSM::apply` does not call a sampler-state setter. Thus, the `s3`
sampler setup occurs outside this per-material binding function. The port must
not create a texture-slot alias for this sampler.

`ksShadowGenSKIN_ps.fxo` SHA-256 is
`78bc800223f4fc2a4c75616ee454a25e4b5c05a6e150d2514fb573b9d48873d4`.
It is byte-identical to the null `ksShadowGen` pixel shader.
`ksShadowGenSKIN_vs.fxo` SHA-256 is
`e2e7a6f2a18814832b2cc13271d4872c713fbbbbadf266d9cb24a5ffe4437fab`.
Its RDEF declares a 224-byte `cbCamera` at `b0` and a 3,520-byte `cbBones` at
`b13`. The bone buffer contains 55 matrices. The shader reads four weights and
four indices for each vertex. These bindings do not authorize the static
alpha-tested path for skinned materials.

This rule is the exact bounded caster-side behavior for `doubleFaceShadow`,
static alpha-tested casters, and skinned shader selection. It does not prove
that the port can translate arbitrary unknown shaders. The selected native
shader and its material resources remain a separate backend contract.

## Safe implementation contract and boundaries

A native backend integration can use this recovered boundary as follows:

- Retain three depth targets and three matrices.
- Run an ordered caster traversal once per cascade with `Shadows` mode.
- Bind each completed depth target at `t6 + level`.
- Transpose each matrix into the 64-byte cascade slot in a 208-byte `b3`
  record.
- Upload three bias values at byte offset `192` and reciprocal map width at
  byte offset `204`.
- Apply shadow attenuation only in the receiver shader's direct-light branch.

The native evidence does not establish the D3D12 descriptor-heap layout or
resource-state barriers. It does not provide backend translations of
`ksShadowGenAT` or `ksShadowGenSKIN`. Those translations remain explicit
backend contracts. The evidence does not authorize unlabeled approximations.

## Portable CLI mapping

The native shell accepts one explicit module set for each supported caster
role:

- `--directional-shadow-vertex` supplies the opaque static vertex program.
- `--directional-shadow-alpha-vertex` and
  `--directional-shadow-alpha-fragment` supply the alpha-tested static pair.
- `--directional-shadow-skinned-vertex` supplies the CPU-skinned vertex
  program.

The alpha-tested pipeline maps material resource slot zero to `txDiffuse` at
`t0`. It maps the shadow sampler to `s3` and the 32-byte material record to
`b4`. It keeps `ksAlphaRef` at byte offset 28.

The skinned CLI pipeline does not claim the recovered `cbBones b13` ABI. The
port updates a retained 19-float vertex stream on the CPU. The explicit vertex
program consumes that updated stream with the portable draw-matrices contract.
This is a labeled backend translation.

One shared validator checks the target, shader stages, vertex stride, resource
bindings, fill mode, blend state, and depth state. The viewport calls this
validator before it allocates the three maps. Missing role programs keep their
caster classes staged.
