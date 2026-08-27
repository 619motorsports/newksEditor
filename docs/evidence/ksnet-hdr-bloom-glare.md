# ksNet HDR bloom and glare evidence

## Scope

This note records the production WebGL bloom and glare path. It separates
source evidence from the portable native implementation boundary.

## Installed binary

Exact native evidence comes from `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`.
The file size is 17,211,904 bytes. Its SHA-256 is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The addresses below use the preferred image base from the Ghidra import.

## Production WebGL topology

The production path resolves the HDR MSAA color into `hdrTexture` and builds
its mip chain (`public/app.js:2709`, `public/app.js:2712`). It reads the last
mip at 1×1 for luminance, then selects manual or immediate automatic exposure
(`public/app.js:2712`).

The `generateBloom` pass uses this order (`public/app.js:2711`):

1. Run the bright pass at `KS_EDITOR_GLARE.sourceScale`.
2. Downsample each level by four taps.
3. Blur each level in horizontal and vertical passes.
4. Bind five bloom levels to the final postprocess shader.

The final shader adds the bloom result before gamma conversion and dither
(`public/app.js:2210-2211`, `public/app.js:2712`). The pass stays disabled for
LDR output and surface overlays (`public/app.js:2711`).

## Constants and formulas

`src/lighting.js:11-32` defines the production controls:

- `sourceScale = 0.25`
- `levels = 5`
- `threshold = 5`
- `brightPassRemap = 1`
- `bloomFilterThreshold = 0.002`
- `bloomGaussianRadiusScale = 0.95`
- `bloomSourceLevel = 2`
- `bloomRadiusDisplayScale = 2.2`
- `bloomKernelSamples = 15`
- `generationRangeScale = 1`
- `shapeLuminance = 5`
- `shapeBloomLuminance = 0.038`
- `compositeBase = 0.035`

The bright-pass formula is `max(source * exposure - threshold, 0) * remap`
(`src/lighting.js:73-78`). The portable kernel calculation uses four-channel
dispersion, 15 taps, and a bounded density cutoff (`src/lighting.js:84-115`).
The composite scale uses the recovered luminance controls
(`src/lighting.js:80-81`).

## Recovered native evidence

The installed Yebis path binds a resolved scene texture and a glare texture to
the final tone-map shader (`docs/evidence/ksnet-yebis-tone-map-handoff.md:164-175`).
The default variant uses the glare input as a 1×1 zero texture when glare is
not active (`docs/evidence/ksnet-yebis-tone-map-handoff.md:156-170`).

The exact native call order is:

```text
0x100846c0 EndPostEffectScene_InternalProcess
  -> 0x100e2c10 CRenderGlare::GenerateGlare
    -> 0x100db370 GenerateGlare_Source
      -> 0x100dad80 GenerateGlare_Source_BrightPass
        -> 0x100c80b0 CTextureUtil::BrightPass
          -> 0x100b7000 CTextureUtil::DrawRectGPU_BrightPass
    -> 0x100db560 GenerateGlare_Bloom
      -> GaussianFilterMax61x61_2Pass
        -> 0x100cfde0 GaussianFilterMax29x29_2Pass
      -> 0x100dbdd0 GenerateGlare_Bloom_CompositeSubLevels
    -> 0x100dc490 GenerateGlare_Star
    -> 0x100dc7c0 GenerateGlare_AnamorphicFlare
    -> 0x100dd9f0 GenerateGlare_Ghost
    -> 0x100df820 GenerateGlare_Afterimage (conditional)
    -> 0x100dfd50 GenerateGlare_LightShaft
    -> 0x100e08e0 GenerateGlare_Composite
  -> 0x10080d30 ApplyEffects_UpdateAutoExposure
  -> 0x10082e60 ApplyEffects_TonemapTemporaly
  -> 0x100e2f00 CRenderGlare::TonemapToSurface
    -> 0x100b9310 CTextureUtil::DrawRectGPU_TonemapHDR
```

The call order is exact disassembly evidence from
`PPFX::CPostEffect::EndPostEffectScene_InternalProcess` at `0x100846c0`.
Glare runs before automatic exposure updates and temporal tone mapping.

`CPostEffect::SetTonemapParameters` at `0x100933a0` stores tone-map exposure
at `CPostEffect +0x600`. `GenerateGlare` stores its exposure arguments at
`CRenderGlare +0x11c`. `CTextureUtil::BrightPass` consumes that value.
This ordering makes the original bright pass use the stored exposure for the
current frame.

The exact embedded bright-pass equation is
`clamp(max(source * (tonemapExposure / sceneLuminanceScale) - threshold, 0) * remap, 0, 64000)`.
The native instruction flow proves the operations and the clamp bounds.
The field names are inferred from setters and shader constants.

`CPostEffect::Initialize` at `0x1008a080` first sets generic glare values to
`1.0`, `0.0`, `PFXGBP_DEFAULT`, and `1.0`. Camera runtime configuration then
loads `[GLARE]` values through `setPostProcessing` at `0x10014d00`.
The `[GLARE]` section initializes shape and ghost to `1`, ghost distortion to
`0.5`, afterimage to `0`, and anamorphic glare to `0`.
These defaults are exact parser and setter evidence.

`SetGlareParameters` at `0x10090590` stores luminance at `+0x578`, threshold
at `+0x574`, bright-pass type at `+0xa00`, and remap at `+0xa04`.
`SetGlareBloomNumLevels` at `0x10098a00` clamps levels to `[1,max]`.
`SetGlareBloomFilterThreshold` at `0x10098b80` stores `+0x29c`.
`SetGlareBloomGaussianRadiusScale` at `0x10098b50` stores `+0x2a0`.

The exact parsed controls include `LUMINANCE`, `PRECISION`, `BLUR`,
`BLOOM_FILTER_THRESHOLD`, `BLOOM_GAUSSIAN_RADIUS_SCALE`,
`BLOOM_LUMINANCE_GAMMA`, `BLOOM_NUM_LEVELS`, `GENERATION_RANGE_SCALE`,
`THRESHOLD`, and `BRIGHT_PASS`.
The quality-3 path uses a quarter-viewport source and five bloom levels.
This is exact decompilation evidence from `GenerateGlare_Source` and
`GenerateGlare_Bloom` at the addresses listed above.
The five levels halve the source dimensions across the chain.
The native scene target uses `DXGI_FORMAT_R16G16B16A16_FLOAT`.
`CRenderGlare::CreateSystemObjects` at `0x100d8c50` creates the glare level
targets and selects the high-precision format.

`CRenderGlare::GenerateGlare_Bloom` at `0x100db560` invokes the native
Gaussian path for each level. Unequal channel dispersion selects the
`GaussianFilterMax29x29_2Pass` fallback at `0x100cfde0`.
The exact source dispersion is red `1`, green
`5.399999736e-7/6.149999763e-7`, blue
`4.649999994e-7/6.149999763e-7`, and alpha `0`.
The portable sigma model is inferred as
`sigma(level) = 2^level * 2^(1-bloomSourceLevel) * radiusScale * displayScale`.
With the active controls, levels zero through four produce `1.045`, `2.09`,
`4.18`, `8.36`, and `16.72`.

`GenerateGlare_Bloom_CompositeSubLevels` at `0x100dbdd0` builds an
intermediate bloom result. The default level weights are equal.
`GetBloomCompositeLuminanceScale` at `0x100e14f0` yields
`1 * 0.035 * 5 * 1.6 * 0.038 = 0.01064` for the active default controls.
The default calculation is exact decompilation evidence. The general
gamma-dependent branch remains only partially recovered.

`GenerateGlare_Composite` at `0x100e08e0` uses
`CTextureUtil::DrawRectGPU_MadLumGammaTn`. The selected tone-map program
uses glare texture `t1` and applies `mapped + (1 - mapped) * saturate(glare)`
before reciprocal gamma and dither. The native intermediate target and final
blend are exact shader evidence from the tone-map handoff.

The current WebGL path has explicit fidelity differences. It measures
immediate exposure before `generateBloom`, while native glare runs before
`ApplyEffects_UpdateAutoExposure`. Its blur averages channel offsets and uses
red-channel weights, while native filtering uses channel-specific dispersion.
Its four-tap downsample approximates native `Resample` and `ConeFilter3x3`.
It sums five bloom textures directly in the final shader, which bypasses the
native intermediate composite target and its luminance scale.

## Native port boundary

The native port now implements optional five-level bloom on Vulkan and D3D12.
The pass uses quarter-resolution RGBA16F ping-pong targets. It runs a bright
pass, four-tap downsampling, and separable 15-tap filtering for each level.
The tone-map pass combines the five levels before reciprocal gamma and dither.

The public default remains disabled. Callers must set
`HdrToneMapParameters::bloom.enabled` to opt in. This preserves existing native
output until the application exposes a bloom policy.

The implementation targets the current production WebGL topology. It is a
portable approximation and does not claim exact Yebis parity. In particular:

- Bloom uses the current immediate exposure. It does not use the recovered
  native temporal exposure order.
- The filter uses the shared portable offsets and weights. It does not keep
  native per-channel dispersion.
- The downsample uses the WebGL four-tap approximation.
- The tone-map pass sums five levels. It does not create the native
  intermediate glare composite.

The native frame plan continues to expose bloom as a separate optional pass
after HDR resolve. Vulkan and D3D12 also validate the full transient-resource
budget before they allocate bloom targets.

The automatic exposure slice uses synchronous final-mip readback. It does not
implement the recovered Yebis temporal ring or six-frame analysis cadence
(`docs/evidence/ksnet-auto-exposure-adaptation.md:284-296`). Bloom must use the
same selected exposure without adding a second exposure policy.

## Completion evidence

The neutral tests cover bloom parameters, dimensions, and bounded transient
resources. Lighting tests cover the five kernel levels, tap limits, symmetry,
and normalized weights. Shader drift tests bind the checked-in GLSL, HLSL,
SPIR-V, and D3D12 inline shader source to fixed hashes.

The Vulkan runtime test uses SwiftShader readback. It compares disabled and
enabled output, checks spatial spread, repeats the pass, and verifies that the
HDR source is unchanged. The same test runs on D3D12 WARP on Windows. This
Linux verification host cannot execute WARP, so the D3D12 source is also
compiled with the Microsoft shader compiler and a Windows-target C++ parser.

The production WebGL smoke test produced hashes `b0782090f3b83e5e` and
`09177c61d521e938`. Both captures reported WebGL error zero. The browser report
contained no exceptions.
