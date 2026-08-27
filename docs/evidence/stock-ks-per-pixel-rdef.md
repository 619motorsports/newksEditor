# Stock `ksPerPixel` RDEF evidence

This note records the resource definitions in the installed Assetto Corsa editor.
The source files are under `sdk/editor/system/shaders/win`.

## Source files

| File | SHA-256 |
|---|---|
| `ksPerPixel_vs.fxo` | `4a446d5c1fd0325ae5b4882d8105796ef1e72e18740a8e35e1280216fe30c7b1` |
| `ksPerPixel_ps.fxo` | `cd75bdaa71b68536a6f0879bebbd8c79234e374e2bdd24a268a8663d1a0732ad` |
| `ksPerPixelAT_ps.fxo` | `37a7841b6f47c774c50c3e83f30b9ac7b77a32f902c48e29912c4002b1c841f` |

The base and alpha-to-coverage pixel shaders have equal RDEF data.
Their instruction chunks have different sizes.

## RDEF layout

The Shader Model 4 RDEF header is 28 bytes.
All table and string offsets start at the first RDEF payload byte.

The header contains these fields:

| Offset | Field |
|---:|---|
| 0 | Constant-buffer count |
| 4 | Constant-buffer table offset |
| 8 | Resource count |
| 12 | Resource table offset |
| 16 | RDEF target token |
| 20 | Compiler flags |
| 24 | Creator string offset |

Each resource record is 32 bytes.
Each constant-buffer record is 24 bytes.
The parser reads each field as an explicit little-endian integer.

The vertex RDEF target is `0xfffe0400`.
The pixel RDEF target is `0xffff0400`.
These values use the RDEF stage encoding, not the SHDR program encoding.

## Vertex resources

| Name | Class | Register | Bytes |
|---|---|---:|---:|
| `cbCamera` | Constant buffer | `b0` | 224 |
| `cbPerObject` | Constant buffer | `b1` | 64 |
| `cbLighting` | Constant buffer | `b2` | 160 |
| `cbShadowMaps` | Constant buffer | `b3` | 208 |

## Pixel resources

| Name | Class | Register | Bytes |
|---|---|---:|---:|
| `samLinear` | Sampler | `s0` | 0 |
| `samShadow` | Comparison sampler flag | `s1` | 0 |
| `txDiffuse` | `Texture2D<float>` | `t0` | 0 |
| `txShadow0` | `Texture2D<float>` | `t6` | 0 |
| `txShadow1` | `Texture2D<float>` | `t7` | 0 |
| `txShadow2` | `Texture2D<float>` | `t8` | 0 |
| `cbLighting` | Constant buffer | `b2` | 160 |
| `cbShadowMaps` | Constant buffer | `b3` | 208 |
| `cbMaterial` | Constant buffer | `b4` | 32 |

The texture register gap from `t1` through `t5` is present in the installed bytecode.
The RDEF sampler flag identifies `samShadow` as a comparison sampler.
RDEF does not contain the complete filter, address, or comparison function state.

## Parser boundary

The internal DXBC reader validates the header, size, chunk table, chunk bounds, and chunk overlap.
The material parser, pipeline validator, and device validator use this reader.

The stock RDEF validator also limits counts and table ranges.
It requires NUL-terminated names inside the RDEF payload.
It rejects missing RDEF chunks, duplicate RDEF chunks, and wrong target tokens.
It also rejects resource, register, type, dimension, flag, and constant-buffer size differences.

Synthetic tests cover truncated RDEF payloads and invalid offsets.
They also cover oversized counts, unterminated strings, duplicate resources, and incorrect bindings.
Installed-fixture tests validate both supported packages against this contract.
