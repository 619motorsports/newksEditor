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

## Stock material scene target

`CameraForwardYebis::initRenderTargets` at `0x1001a140` creates the main
scene target. Its color format is `eR16G16B16A16_MS`.

`RenderTarget::RenderTarget` at `0x10064b05` applies the configured sample
count to this target. A one-sample target uses `eR16G16B16A16`.

`kglCreateRenderTarget` at `0x1000ce30` maps both color types to
`DXGI_FORMAT_R16G16B16A16_FLOAT`. The multisample type also uses the configured
sample count.

`CameraForwardYebis::renderApplyEffect` binds the float color and depth
targets. Then it starts the main pass and calls the scene-render callback.

`Node::render` at `0x1003f5dc` traverses the active scene nodes. `Mesh::render`
at `0x100494ff` binds the geometry and commits the selected material shader.

`MaterialFilter::apply` at `0x10064fa2` calls `Material::apply` at
`0x1004016e`. This function does not replace the bound scene target.

`KN5IO::loadMaterialsBinary` at `0x1003b9de` reads each serialized shader
name. It gives that name to `Material::setShader`.

Thus, a `ksPerPixel` material draws with the stock shader into the active
RGBA16F scene target. The material path does not select an RGBA8 target.

The installed `ksPerPixel_ps.fxo` declares one `float4` output at `o0`. The
shader does not declare a render-target format or an output conversion.

When multisampling is active, `renderApplyEffect` resolves the scene color at
`0x1001ae68`. The resolve format is `DXGI_FORMAT_R16G16B16A16_FLOAT`.

When the sample count is one, the resolved color aliases the scene color.
`applyPostProcessing` then gives the one-sample float image to Yebis.

## Frame parameters

`CameraForwardYebis::setPostProcessing` at `0x10014d00` supplies the frame
parameters. The inputs include the camera matrices, near plane, far plane,
field of view, and frame time.

The function enables the automatic-exposure path. It also supplies the
exposure, gamma, HDR tone-map function, mapping factor, and viewport scale.

## Final tone-map path

The final handoff uses this call chain:

```text
PPFX::CPostEffect::EndPostEffectScene_InternalProcess 0x100846c0
  -> PPFX::CRenderGlare::TonemapToSurface              0x100e2f00
     -> PPFX::CTextureUtil::DrawRectGPU_TonemapHDR     0x100b9310
```

`ApplyEffects_TonemapTemporaly` at `0x10082e60` writes a separate temporal HDR
target before this final handoff. The preferred intermediate format is
`DXGI_FORMAT_R16G16B16A16_FLOAT`.

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

## Final output and FXAA handoff

`EndPostEffectScene_InternalProcess` passes the surface at `this+0xac0` to
`TonemapToSurface`. This surface is the effective final tone-map destination.

`UpdateParameters_TonemapDestination` at `0x10088520` sets that field from an
explicit override. Without an override, it uses the surface at `this+0x14`.

`CameraForwardYebis::initRenderTargets` at `0x1001a140` asks a virtual method
at vtable offset `0x2c` for the destination. That method is
`getYebisDestinationRenderTarget` at `0x1001ac50`.

When FXAA is enabled, the method returns `fxaaTempRT->kidColor`. The constructor
at `0x10015ad0` creates `fxaaTempRT` with the `eR8G8B8A8` format.

`kglCreateRenderTarget` at `0x1000ce30` maps that format to
`DXGI_FORMAT_R8G8B8A8_UNORM`. Therefore, the proven FXAA input is an RGBA8
UNORM surface.

`CameraForwardYebis::render` then runs the full-screen `ksFXAA` pass. It binds
the temporary color surface at texture slot 5 and draws to the screen target.

When FXAA is disabled, Yebis writes directly to `screenRenderTarget`.
`kglGetScreenRenderTarget` at `0x1000c820` only returns its pointer. The
screen target's DXGI format remains unresolved.

No inspected path selects `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` or enables a
proven hardware sRGB conversion. The tone-map shader performs its gamma math
with `fParam_GammaCorrection`. This math does not prove an sRGB target state.

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

## Recovered default upload vectors

`TEXSHADERSET::CreateEffect` at `0x100aa830` resolves the handles for
`afRGBA_Modulate` and `afRGBA_Offset`. `CTextureUtil` embeds that shader set at
offset `0x20`.

`DrawRectGPU_TonemapHDR` builds two modulate vectors and four offset vectors.
It uploads them on every draw. The default temporal path produces these
register values:

| Register | Proven default upload |
|---|---|
| `cb154` | `{1, 0, 1, 1}` |
| `cb155` | `{1, 0, 0, 1}` |
| `cb186` | `{2, -1, 0, 0}` |
| `cb187` | `{0.5, 0, 0, 0}` |
| `cb188` | `{2, -1, 0, 1}` |
| `cb189` | `{0.5, 0, 0, 0}` |

The vector construction occurs at `0x100b9784..0x100b980d` and
`0x100b9dc5..0x100b9ea3`. `CPostEffect::Initialize` at `0x1008a080` supplies
the default tone-map state.

The default `cb154.y` value is zero. It disables the glare-composite result in
pixel program 2095. The default `cb154.x` value keeps scene modulation at one.

The register values and native upload order are proven. The `COLORT4` channel
names still need a packing check before the alpha dot product receives an
RGBA semantic label.

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

The recovered defaults reduce both auxiliary color grades to constant values.
Non-default glare modes can change the offset vectors at runtime.

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

## Recovered dither sampling

`DrawRectGPU_TonemapHDR` binds the dither texture at `t3`. Pixel program 2095
samples it through `s3` with the fourth pixel-shader texture coordinate.

Sampler mode `0xd` selects point filtering and wrap addressing for U and V.
The W address mode is clamp, but this setting does not affect the 2D texture.

`DrawRectGPU_GetVerticesAndTexCoords` sends the supplied coordinate endpoints
to the rectangle vertices. It does not add a half-texel offset.

The dither coordinates cover one texture period per four output pixels. This
gives an integer pixel lookup modulo four when the offset and span signs are
fixed.

Yebis also maintains four dither-coordinate seeds. The first two can offset
the sample phase. The other two can reverse each coordinate span.

The portable shaders use the recovered table, scale, placement, and four-pixel
period. They currently use a fixed phase and positive spans.

This procedural lookup is a labeled approximation of the native sampled
texture path. It does not claim frame-by-frame pixel identity.

The scene and temporary tone-map targets use RGBA16F by default. The schedule
is scene render, MSAA resolve, Yebis effects, final RGBA8 UNORM tone-map, then
FXAA when enabled. The screen target format remains unresolved.

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

The fixed-phase 4×4 dither update used the same production scene and controls.
Its initial frame hash was `b0782090f3b83e5e`.

The sun-height 10 comparison hash was `09177c61d521e938`. Both captures
reported WebGL error zero, and the browser report contained no exceptions.

## Evidence boundary

This evidence proves the resource handoff, function order, default variant,
resource bindings, and active constant fields. Some curve semantics remain
unknown.

The C++ port must keep the current implementation labeled as an approximation.
The current pass includes fixed-phase dither but omits automatic exposure.
The default glare composite scale is zero, so the missing glare texture does
not affect the proven default pixel path.

An exact claim still requires vector packing confirmation and native dither
phase sequencing. A production rendering change also requires a production
WebGL check.
