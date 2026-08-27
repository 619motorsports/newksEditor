# Native editor constant-buffer lifecycle

This note records D3D11 constant-buffer ownership and update cadence from the installed editor. It does not claim D3D12 or Vulkan object parity.

## Shared buffer creation

`GraphicsManager::initCBuffers` at `0x10045138` creates four shared system buffers. Each buffer belongs to one `GraphicsManager`.

The constructor clears every byte and marks each buffer as dirty.

| Name | Slot | Native size | Ownership |
| --- | ---: | ---: | --- |
| `cbCamera` | `b0` | 224 bytes | Shared camera state |
| `cbPerObject` | `b1` | 64 bytes | Shared object state |
| `cbLighting` | `b2` | 160 bytes | Shared scene state |
| `cbShadowMaps` | `b3` | 208 bytes | Shared shadow state |

`Material::createCBuffers` at `0x1004026a` creates separate material buffers from shader reflection. Stock `cbMaterial` uses `b4` and 32 bytes.

## Buffer operations

`CBuffer::CBuffer` at `0x1004ab2d` allocates the requested byte count. It clears the host data and creates the native buffer.

`CBuffer::set` at `0x1004abeb` copies bytes to the specified offset. It then marks the buffer as dirty.

`CBuffer::commit` at `0x1004ab88` maps data only when the buffer is dirty. It binds the recorded slot and clears the dirty flag.

`GraphicsManager::commitShaderChanges` at `0x10044dc6` commits system buffers in this order:

1. `cbCamera`.
2. `cbLighting`.
3. `cbPerObject`.
4. `cbShadowMaps`.

## Camera buffer

`GraphicsManager::setProjectionMatrix` at `0x10046632` transposes the projection matrix. It writes 64 bytes at offset `0x40`.

`GraphicsManager::setViewMatrix` at `0x1004698f` transposes the view matrix. It writes 64 bytes at offset `0x00`.

The same function computes `inverse(view * projection)` and transposes the result. It writes 64 bytes at offset `0x80`.

If the caller supplies a camera, the function writes these values:

| Offset | Size | Value |
| ---: | ---: | --- |
| `0xc0` | 12 bytes | Camera position |
| `0xd0` | 4 bytes | Near plane |
| `0xd4` | 4 bytes | Far plane |
| `0xd8` | 4 bytes | Field of view |

`Camera::renderCamera` at `0x100649ab` updates projection and view for each camera render.

Shadow passes supply no camera pointer. They update matrices but retain the previous position and lens values.

The main draw loop marks `cbCamera` dirty and commits it before draw traversal.

## Per-object buffer

`GraphicsManager::setWorldMatrix` at `0x10046b7e` transposes the world matrix. It writes 64 bytes at offset `0x00`.

`PvsProcessor::doRenderClassic` at `0x10065e44` updates this buffer immediately before each classic mesh draw.

`PvsProcessor::prepareDrawCallsDefault` at `0x10065fa8` stores the matrix in each 100-byte draw record. The record stores `hasMatrix` at offset `0x20`.

`PvsProcessor::doRenderCalls` at `0x10065b85` updates `cbPerObject` only when `hasMatrix` is true.

If `hasMatrix` is false, the previous GPU value stays bound. The port must not insert an implicit identity matrix.

## Lighting buffer

`GraphicsManager::beginScene` at `0x10044cfd` calls `updateLightingSetttings` at `0x10046c2c` for each scene.

The function writes these recovered fields:

| Offset | Size | Value |
| ---: | ---: | --- |
| `0x00` | 16 bytes | Light direction record |
| `0x10` | 12 bytes | Ambient color |
| `0x20` | 16 bytes | Sun color record |
| `0x30` | 12 bytes | Horizon color |
| `0x40` | 12 bytes | Sky color |
| `0x5c` | 4 bytes | Linear fog value |
| `0x60` | 4 bytes | Fog blend |
| `0x64` | 12 bytes | Fog color |
| `0x70` | 4 bytes | Cloud cover |
| `0x74` | 4 bytes | Cloud cutoff |
| `0x78` | 4 bytes | Cloud color |
| `0x7c` | 4 bytes | Cloud offset |
| `0x90` | 4 bytes | Saturation |
| `0x94` | 4 bytes | Game time |

The main draw loop commits `cbLighting` before traversal. This buffer is shared scene state, not per-material state.

## Shadow buffer

`GraphicsManager::setShadowMapBias` at `0x10046833` writes three bias values at offset `0xc0`.

`GraphicsManager::setShadowMapMatrix` at `0x10046873` writes each transposed cascade matrix to its 64-byte slot.

| Cascade | Matrix offset | Texture slot |
| ---: | ---: | ---: |
| `0` | `0x00` | `t6` |
| `1` | `0x40` | `t7` |
| `2` | `0x80` | `t8` |

`GraphicsManager::setShadowMapTexture` at `0x100468c7` binds the completed depth texture. It writes reciprocal texture width at offset `0xcc`.

A null shadow target clears the selected shader resource view. It does not change the reciprocal texture width.

`CameraShadowMapped::shadowMapPass` at `0x1005eb1f` runs exactly three cascade passes.

The main draw loop marks `cbShadowMaps` dirty and commits it before traversal.

## Material buffer

`Material::setShader` at `0x100407fc` recreates reflected material buffers when the shader pointer changes.

`Material::apply` at `0x1004016e` commits each material buffer after shader and texture binding.

The default draw loop commits material buffers only when the material pointer changes. Stock `cbMaterial` remains bound at `b4` between matching materials.

## Port requirements

The retained scene can share camera, lighting, and shadow records across native draws. Each draw record must retain its required object state.

Native batch execution must preserve the dirty-update cadence. It must not replace retained data with default values.

The D3D12 port can use aligned 256-byte views. The visible record bytes must preserve the recovered native sizes and offsets.
