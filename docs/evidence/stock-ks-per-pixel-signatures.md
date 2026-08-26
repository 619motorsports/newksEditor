# Stock `ksPerPixel` stage signatures

This note records the legacy DXBC signatures in the installed Assetto Corsa editor.
The source hashes are in [the RDEF evidence](stock-ks-per-pixel-rdef.md).

## Legacy signature layout

The installed stages use `ISGN` and `OSGN` chunks.
They do not use the later `ISG1` or `OSG1` layouts.

Each signature payload starts with two 32-bit little-endian fields.
The first field contains the parameter count.
The second field contains the first parameter offset.
The installed first parameter offset is 8.

Each parameter record is 24 bytes:

| Offset | Field |
|---:|---|
| 0 | Semantic-name offset |
| 4 | Semantic index |
| 8 | System-value type |
| 12 | Component type |
| 16 | Shader register |
| 20 | Component mask |
| 21 | Read/write mask |
| 22 | Reserved 16-bit field |

The semantic-name offset starts at the signature payload.
Each installed reserved field is zero.

## Vertex input

The vertex `ISGN` payload has four records and 140 bytes.

| Semantic | Index | System | Type | Register | Mask | Read/write |
|---|---:|---:|---:|---:|---:|---:|
| `POSITION` | 0 | 0 | 3 | 0 | `0x0f` | `0x0f` |
| `NORMAL` | 0 | 0 | 3 | 1 | `0x07` | `0x07` |
| `TEXCOORD` | 0 | 0 | 3 | 2 | `0x03` | `0x03` |
| `TANGENT` | 0 | 0 | 3 | 3 | `0x07` | `0x00` |

The zero read mask shows that this shader declares the tangent but does not read it.
The declaration still matches the recovered 44-byte native vertex stream.

## Vertex output

The vertex `OSGN` payload has eight records and 224 bytes.

| Semantic | Index | System | Type | Register | Mask | Read/write |
|---|---:|---:|---:|---:|---:|---:|
| `SV_POSITION` | 0 | 1 | 3 | 0 | `0x0f` | `0x00` |
| `TEXCOORD` | 0 | 0 | 3 | 1 | `0x07` | `0x08` |
| `TEXCOORD` | 1 | 0 | 3 | 2 | `0x07` | `0x08` |
| `TEXCOORD` | 2 | 0 | 3 | 3 | `0x03` | `0x0c` |
| `TEXCOORD` | 6 | 0 | 3 | 3 | `0x04` | `0x0b` |
| `TEXCOORD` | 3 | 0 | 3 | 4 | `0x0f` | `0x00` |
| `TEXCOORD` | 4 | 0 | 3 | 5 | `0x0f` | `0x00` |
| `TEXCOORD` | 5 | 0 | 3 | 6 | `0x0f` | `0x00` |

`TEXCOORD2` uses register 3 components `xy`.
`TEXCOORD6` uses register 3 component `z`.

## Pixel signatures

The pixel `ISGN` payload has the same eight semantic, register, and component-mask entries.
Its read/write-mask bytes are `00, 07, 07, 03, 04, 07, 07, 07`.

The pixel `OSGN` payload has one `SV_TARGET0` record and 44 bytes.
The record uses register 0, mask `0x0f`, component type 3, and system-value type 0.
The semantic name, not the system-value field, identifies this output as `SV_TARGET`.

The base and alpha-to-coverage pixel stages have equal signature payloads.

## Native creation flow

Ghidra identifies `KGLShader::createVertexShader` at `0x1000fa30` in the installed `ksNet.dll`.
This function reflects the shader and creates the D3D11 vertex shader.
It then calls `getInputLayout` at `0x1000eea0`.

Input-layout type 0 contains the four fields in the vertex `ISGN` chunk.
Their byte offsets are 0, 12, 24, and 32.
Their formats are float3, float3, float2, and float3.
The complete stream stride is 44 bytes.

`KGLShader::loadShaderBinary` at `0x1000f5c0` selects this layout for a normal mesh.
The `SKINNED` and `PARTICLE` metadata fields select other recovered layouts.

`KGLShader::reflectVars` at `0x1000fab0` uses `D3DReflect` for each stage.
Its material texture list excludes texture registers 6 through 19.
Thus, the material API does not expose the three shadow maps as ordinary material textures.
The complete RDEF validator still includes those shader resources.

## Validation boundary

The validator uses the shared bounded DXBC reader.
It requires one legacy executable chunk and one requested signature chunk per stage.

The validator checks counts before record-size multiplication.
It also checks table bounds, name offsets, NUL terminators, reserved fields, and each exact parameter field.

The linkage contract compares vertex outputs with pixel inputs without their read/write masks.
Those raw masks have different meanings for input and output signatures.

The complete native-program gate requires the package shape, RDEF contract, and all four signatures.
This gate does not execute DXBC on Vulkan or create the native D3D12 root signature.
