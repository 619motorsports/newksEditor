# ksNet Yebis tone-map handoff evidence

## Installed source

This note uses the installed Assetto Corsa SDK editor at this path:

`/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor`

The inspected `ksNet.dll` file has this SHA-256 value:

`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`

The file size is 17,211,904 bytes. The addresses are preferred-image
addresses from `ksNet.dll`. Ghidra decompilation and instruction inspection
supplied the evidence.

## Frame handoff

`CameraForwardYebis::render` at `0x1001aa70` controls the Yebis frame path.
It calls `CameraForwardYebis::renderApplyEffect` at `0x1001ad80`.

`renderApplyEffect` renders the scene into an RGBA16F multisample target.
It resolves the color and depth resources when multisampling is active. Then
it calls `CameraForwardYebis::applyPostProcessing` at `0x100159a0`.

`applyPostProcessing` gives these inputs to the Yebis effect:

| Input | Native operation |
|---|---|
| Scene color | `SetEffectSource(source SRV, width, height)` |
| Scene depth | `SetDepthFactorSource(depth SRV, width, height, format, 0, 1)` |

The function calls `ApplyEffects` after it supplies both resources.

## Frame parameters

`CameraForwardYebis::setPostProcessing` at `0x10014d00` supplies the frame
parameters. The inputs include the camera matrices, near plane, far plane,
field of view, and frame time.

The function enables the automatic-exposure path. It also supplies the
exposure, gamma, HDR tone-map function, mapping factor, and viewport scale.

## Final tone-map path

The final handoff uses this call chain:

```text
PPFX::CPostEffect::ApplyEffects_TonemapTemporaly  0x10082e60
  -> PPFX::CRenderGlare::TonemapToSurface         0x100e2f00
     -> PPFX::CTextureUtil::DrawRectGPU_TonemapHDR 0x100b9310
```

`ApplyEffects_TonemapTemporaly` binds the temporal tone-map framebuffer. Then
it calls `TonemapToSurface`.

`TonemapToSurface` selects a shader variant from these controls:

- Tone-map function
- Dither state
- Color-matrix state
- Vignette state
- Gamma state
- Resample state

The function binds the scene source, luminance textures, and tone-map
constants. Then it sends the work to `DrawRectGPU_TonemapHDR`.

`DrawRectGPU_TonemapHDR` selects the final shader variant. It draws one
full-screen rectangle into the selected output surface.

## Default shader variant

The default `FUNCTION=-1` setting resolves to `EHDRTONEMAP=10`.
`TEXSHADERSET::GetHandle_TonemapCombiniation_EHDRTONEMAP` is at `0x100ae060`.

The function builds this variant mask:

| Bit | Control |
|---:|---|
| `0x01` | Color matrix |
| `0x02` | Dither |
| `0x04` | Vignette |
| `0x08` | Simulated vignette |
| `0x10` | Exposure |
| `0x20` | Pre-color matrix |
| `0x40` | Gamma |

The default mask is `0x52`. It selects dither, exposure, and gamma.
The selected technique is `tech_TonemapHDR_Dither_Exposure_Gamma`.

The technique uses vertex program 5 and pixel program 2095. The pixel DXBC
starts at file offset `0xdd8d00` and has 4,088 bytes. Its SHA-256 value is:

`295e4f592f08c5b6cadc9652b7394d703e7b2f139e63e28b55167014c938a64`

The pixel shader has these texture bindings:

| Binding | Selected default input |
|---|---|
| `t0` | Resolved scene color |
| `t1` | Glare, or a 1×1 zero texture |
| `t2` | Gray, or film grain when enabled |
| `t3` | 4×4 full-color dither matrix |

The selected pixel shader has no depth or luminance texture binding.
Earlier effects consume depth. `CTextureLuminanceManage` performs automatic
exposure outside this shader.

The selected pixel shader uses `$Globals` at `b0`. The buffer has 4,096 bytes.
These reflected fields are active:

| Field | Byte offset |
|---|---:|
| `afRGBA_Modulate` | 2464 |
| `afRGBA_Offset` | 2976 |
| `fParam_GammaCorrection` | 3232 |
| `fParam_DitherOffsetScale` | 3236 |
| `fParam_TonemapMaxMappingLuminance` | 3328 |

`DrawRectGPU_TonemapHDR` writes reciprocal gamma. It writes dither scale and
negative half-scale beside gamma. `SetTonemapEffectParameters` writes the
mapping vector.

The scene and temporary tone-map targets use RGBA16F by default. The schedule
is scene render, MSAA resolve, Yebis effects, temporary tone-map, then optional
FXAA. This path does not prove the final screen format.

## Production WebGL check

The production Electron path loaded the Nissan 370Z LOD D model. It rendered
three meshes and 2,343 triangles with a four-sample RGBA16F target.

The run used Light Clouds, manual exposure 0.35, sun heading 40, and sun height
55. The displayed frame hash was `5efc6a1802de083a`.

The comparison at sun height 10 produced `ed5d01a32247b89b`. Both states
reported WebGL error zero. The browser smoke report contained no exceptions.

This check proves the production WebGL path remains operational. It does not
prove pixel equality between WebGL, the new native pass, and Yebis.

## Evidence boundary

This evidence proves the resource handoff, function order, default variant,
resource bindings, and active constant fields. Some curve semantics remain
unknown.

The C++ port must keep the first implementation labeled as an approximation.
The current curve-only pass omits default glare and dither inputs. An exact
claim requires the recovered runtime constants and controlled pixel checks.
A production rendering change also requires a production WebGL check.
