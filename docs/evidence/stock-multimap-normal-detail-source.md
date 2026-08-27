# Stock MultiMap normal-detail source package

## Scope

This package implements a bounded portable detail stack for two stock material names:

- `ksPerPixelMultiMap_NMDetail`
- `ksPerPixelMultiMap_AT_NMDetail`

The package supports Vulkan SPIR-V and D3D12 DXBC. Both backends use the same portable material and resource contract.

The package does not claim installed shader-bytecode parity. It omits native reflections, shadows, weather, damage, overlays, and parts of the native lighting model.

## Installed package evidence

The installed editor is in `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/`. The inspected `ksNet.dll` SHA-256 value is:

`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`

The installed shader packages have these identities:

| Package | Bytes | Alpha flag | SHA-256 |
|---|---:|---:|---|
| `ksPerPixelMultiMap_NMDetail.shader` | 16,286 | 0 | `d4929337f453eb2d76c14cc5fd7cac4d8b3411524cb50346117389ef87cff99d` |
| `ksPerPixelMultiMap_AT_NMDetail.shader` | 16,346 | 1 | `a284bec37701e62b6edfb4dc1c259674594654cdb3c91669dc51e4ab2cab571c` |

Both packages use the same 4,072-byte vertex stage. Its SHA-256 value is `259c7201d763ac8d8f95c290b0daa0d35c6a4d6bee19e28eec70f8eddcb5ef21`.

The pixel-stage SHA-256 values are:

- NMDetail: `2e45a59c20284802cda77ae02488d0f44aaa8955492767e75dcd0c29b7631222`
- AT_NMDetail: `51fd6d6c1bc4f18c1d978ddde362d09391fd719fac80ca9b347fdb95e743e4d6`

The recovered pixel shaders use `txDiffuse`, `txNormal`, `txMaps`, `txDetail`, and `txNormalDetail`. The installed register numbers differ from the portable contract.

The disassembly shows that both detail textures use `detailUVMultiplier`. The inverse diffuse alpha supplies the detail mask.

The disassembly also shows that detail alpha changes the packed specular result. The AT pixel shader keeps the sampled diffuse alpha.

The main AT pixel shader has no `discard` or `clip` instruction. `Material::initShaderVars` at `0x10040420` selects blend mode 2 and alpha-to-coverage.

`KGLShader::loadShaderBinary` is at `0x1000f5c0`. `Material::apply` is at `0x1004016e`.

## Portable contract

The portable package uses these set-0 bindings:

| Binding | Resource |
|---:|---|
| 0 | `txDiffuse` |
| 1 | `txDiffuseSampler` |
| 2 | `ksPerPixelMaterial` |
| 3 | `ksPerPixelFrame` |
| 4 | `txNormal` |
| 5 | `txNormalSampler` |
| 6 | `txMaps` |
| 7 | `txMapsSampler` |
| 8 | `txDetail` |
| 9 | `txDetailSampler` |
| 10 | `txNormalDetail` |
| 11 | `txNormalDetailSampler` |

D3D12 maps these numbers to `t`, `s`, and `b` registers by resource type. Vulkan uses descriptor set 0 and the listed binding numbers.

The vertex record is 44 bytes. It contains position, normal, UV, and tangent data.

The program uses the `draw_matrices` transform contract. The AT variant requires a four-sample target and alpha-to-coverage.

## Source and artifact identities

| Item | SHA-256 |
|---|---|
| Vulkan vertex source | `3f5345267a174b150471dad34b958052655327f51c4495acaf2d52d7091ed40c` |
| Vulkan fragment source | `e53f946111c776e93029299797675dba9a8d3ffa82c70a149634f76005279011` |
| D3D12 HLSL source | `366e41b6b710ff00bf92b8ccc1e341256518ab1bce7db84715f0ba15d80639f4` |
| Vertex SPIR-V | `1f89e1213742d97a91a0aa6291ebc7c84d7035db6a12bf588e9c9f3a9f54b482` |
| Fragment SPIR-V | `ec0b7bc1826a7f77f6218fb5c3b156499b55d1120fbf2ee18bb00cb3659d7239` |
| Vertex DXBC | `411515e9ee41778c5a0e79566db664346319d5baa2100cd8d27da59f1cc01dc9` |
| Pixel DXBC | `8ba9faec47f9a6b91af7d70941b079616cd6065c16cb6b6b2724273680788a34` |

The source-drift test compares these values with the maintained files and embedded artifacts. Strict program validation rejects changed or truncated artifacts.

## Selection limits

The built-in selector accepts an exact material-family name and a static mesh. Caller shader modules take precedence.

The selector rejects reflection, directional-shadow receiver, damage, wireframe, skinned, and transparent paths. These paths require explicit shader modules.
