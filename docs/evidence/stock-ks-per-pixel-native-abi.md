# Stock `ksPerPixel` native ABI evidence

## Provenance

The installed SDK editor supplies `ksPerPixel.shader` and
`ksPerPixelAT.shader`. The repository does not redistribute these files.

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

`cbCamera` stores three matrices at offsets 0, 64, and 128. The camera
position starts at 192. The four trailing scalar values start at 208.

`cbPerObject` contains the 64-byte world matrix at offset 0.

`cbMaterial` stores four lighting scalars at offsets 0 through 12. The
three-component emissive color starts at 16. `ksAlphaRef` is at 28.

`cbShadowMaps` stores three matrices at offsets 0, 64, and 128. Its three
bias values start at 192. The reciprocal texture width is at 204.

The host transposes all matrices before it uploads them. This rule applies to
the view, projection, inverse-view-projection, world, and shadow matrices.

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

`GraphicsManager::initCBuffers` at `0x10045138` creates the five reflected
buffer sizes. `CBuffer::CBuffer` at `0x1004ab2d` clears every buffer byte.
`GraphicsManager::commitShaderChanges` at `0x10044dc6` commits the updated
camera, lighting, object, and shadow buffers.

The matrix setters provide the upload convention:

- `GraphicsManager::setProjectionMatrix` at `0x10046632`
- `GraphicsManager::setViewMatrix` at `0x1004698f`
- `GraphicsManager::setWorldMatrix` at `0x10046b7e`
- `GraphicsManager::updateLightingSetttings` at `0x10046c2c`

## Port boundary

[`stock_ks_per_pixel.hpp`](../../native/include/apex/render/stock_ks_per_pixel.hpp)
owns the exact buffer layouts and register manifest. It also contains the
separate base and AT variant contract.

The CPU evaluator follows the recovered pixel equation. It rejects non-finite
records and degenerate normalization inputs before evaluation. This evaluator
does not execute the installed DXBC.

The current Vulkan path remains a labeled portable ABI. A future translated
path needs separate descriptor sets for constant buffers, textures, and
samplers because Vulkan uses one binding namespace.

The current D3D12 path still needs an opt-in native root signature. A Windows
WARP pipeline and readback test must pass before production selection becomes
available.
