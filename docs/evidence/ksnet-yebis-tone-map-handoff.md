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

## Recovered default shader math

The pixel shader samples the auxiliary texture twice through two color-grade
records. It combines the second result with glare before tone mapping.

```text
A = grade(sample(t2, v3), cb186, cb187)
B = grade(sample(t2, v3), cb188, cb189)
G = sample(t1, v1).rgb
glareComposite = saturate((A + G * B) * cb154.y)

S = sample(t0, v0)
alpha = saturate(dot(S.rgba, cb155))
x = max(B * S.rgb * cb154.x, 2^-14)

e = exp2(-x * cb208.x)
u = saturate((1 - e) * (1 - e * cb208.y)^2)
x = u + (1 - u) * glareComposite
x = min(x + 2^-22, 1)
x = exp2(log(x) * cb202.x)
x += sample(t3, v4).r * cb202.y
output.rgb = x + cb202.z
```

The reflected default for `cb202.x` is `0.454545468`. The reflected default
for `cb208` is `{1, 1, 1.015625, 1}`.

The shader applies dither after its gamma operation. It has no final saturate
instruction after dither. The render-target conversion can clamp the result.

The exact color grades remain unknown. Runtime values for `cb154`, `cb155`,
and `cb186..189` are necessary for an exact implementation.

## Recovered dither texture

`CTextureUtil::InitializeDevice` at `0x100afae0` creates the 4×4 RGBA8 dither
texture. The code uses this Bayer order:

```text
0,  8,  2, 10
12, 4, 14,  6
3, 11,  1,  9
15, 7, 13, 5
```

The initialization transforms each value with `(value * 0.0625) + 0.03125`.
It multiplies the result by `255.999893` and truncates it to one byte.

```text
7, 135, 39, 167
199, 71, 231, 103
55, 183, 23, 151
247, 119, 215, 87
```

Each texel repeats its value in all four channels. This byte table comes from
decompiled arithmetic. A runtime upload capture can show the driver interpretation.

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

The viewport integration run repeated this scene through Electron and
software WebGL. The initial frame hash was `405c7cc1dc38cc9d`.

The comparison frame hash was `81830fb2a5c97af9`. Both captures reported
WebGL error zero, and the browser report contained no exceptions.

## Evidence boundary

This evidence proves the resource handoff, function order, default variant,
resource bindings, and active constant fields. Some curve semantics remain
unknown.

The C++ port must keep the first implementation labeled as an approximation.
The current curve-only pass omits default glare and dither inputs. An exact
claim requires the recovered runtime constants and controlled pixel checks.
A production rendering change also requires a production WebGL check.
