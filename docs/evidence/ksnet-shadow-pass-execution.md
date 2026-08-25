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

`setShadowMapTexture(level, nullptr)` clears texture stage `level + 6`. It does
not write the texture size. A completed level binds its depth resource. It also
updates the shared reciprocal width. The caller must bind all three completed
levels before a shadow-enabled receiver draw.

## Caster material contract

`MaterialFilterSM::apply` is at `0x100650c7`. It selects the skinned shadow
shader when the material shader variable at native offset `+0x50` is `1`.
Otherwise, it selects the normal shadow shader. It selects the alpha-tested
shadow shader when the material field at `+0x5c` is nonzero.

For alpha-tested materials, it binds the first texture record whose texture
slot is zero. It commits the material constant buffer whose slot is `4`. It
always forces opaque blending. It sets back-face culling unless the material
byte at `+0x64` is nonzero. If this byte is nonzero, it uses no culling.

This rule is the exact bounded caster-side behavior for the current staged
`doubleFaceShadow` and alpha-tested casters. It does not prove that the port
can translate arbitrary unknown shaders. The selected native shader and its
material resources remain a separate backend contract.

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
resource-state barriers. It does not establish a complete translation of
`ksShadowGenAT` or `ksShadowGenSKIN`. These items remain backend and shader
translation gates. The evidence does not authorize opaque approximations for
alpha-tested or skinned casters.
