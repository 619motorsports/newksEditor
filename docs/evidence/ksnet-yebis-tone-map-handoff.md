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

## Evidence boundary

This evidence proves the resource handoff, function order, and variant-control
categories. It does not prove the math in each Yebis shader variant.

The C++ port must keep the first implementation labeled as an approximation.
An exact claim requires the selected shader bytecode and its constant-buffer
layout. A production rendering change also requires a production WebGL check.
