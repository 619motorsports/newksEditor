# ksNet FXAA evidence

## Scope

This note records the installed editor FXAA path.
It separates exact disassembly and DXBC facts from unresolved facts and future portable work.

## Installed source

The source binary is `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`.
Its SHA-256 is `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The file size is 17,211,904 bytes.
The addresses below use the preferred image base from Ghidra.

## Exact frame path

`CameraForwardYebis::render` is at `0x1001aa70`.
It calls `renderApplyEffect` at `0x1001ad80`.
That function renders the scene, resolves MSAA color when needed, and calls
`applyPostProcessing` at `0x100159a0`.

The exact postprocess order is:

```text
0x1001aa70 CameraForwardYebis::render
  -> 0x1001ad80 CameraForwardYebis::renderApplyEffect
     -> MSAA color resolve
     -> 0x100159a0 CameraForwardYebis::applyPostProcessing
        -> Yebis tone map
  -> 0x1001ac70 CameraForwardYebis::resetYebisStates
  -> 0x10046967 GraphicsManager::setTextureRT(slot 5, fxaaTempRT)
  -> 0x100475e7 GLRenderer::fullScreenQuad(fxaaShader)
```

The call to `resetYebisStates` uses the virtual slot at `CameraForwardYebis`
vtable offset `0x28`.
The function address is exact disassembly evidence.

`getYebisDestinationRenderTarget` at `0x1001ac50` returns
`fxaaTempRT->kidColor` when that resource exists.
It returns `screenRenderTarget` when the resource does not exist.
This routing places FXAA after the Yebis tone map.

## Exact resource conditions

The constructor at `0x10015ad0` reads `ppHDREnabled` and `ppFXAA`.
It creates FXAA resources only when HDR and FXAA are both enabled.

The constructor creates `fxaaTempRT` with `eR8G8B8A8`, the full viewport width
and height, one sample, and no depth surface.
It loads the `ksFXAA` shader by name.
These conditions and values are exact decompilation evidence.

`resetYebisStates` at `0x1001ac70` binds the screen render target and removes
the depth target before the full-screen FXAA draw.
This state transition is exact disassembly evidence.

## Exact shader assets

The installed shader files have these hashes and sizes:

| File | Size | SHA-256 |
|---|---:|---|
| `ksFXAA_ps.fxo` | 6,424 bytes | `5a86d2f885f93e67487d98e97c680ba558690bd27283ed7a7af5c91f04f8674d` |
| `ksFXAA_vs.fxo` | 648 bytes | `1f8b3d4116484182878d9abcae0a4768e87f71d3736a62f6250819b97f22da599` |
| `ksFXAA_meta.ini` | 54 bytes | `a0c5eb4eb8b26fcff3ad8bf9d1c1a492d098079ea4a73511432427435a91cf53` |

The metadata sets `ALPHATEST=0`, `SKINNED=0`, `PARTICLE=0`, and `2D=0`.
The pixel program is Shader Model 4.0.
It binds `cbLighting` at `b2`, `txCurrent` at `t5`, and sampler `s0`.
The constant buffer size is 160 bytes.

The vertex program has no constant buffer or texture binding.
It copies position input `v0` to the output position.
It copies UV input `v2` to output texture coordinates.
These bindings and operations come from DXBC reflection and disassembly.

## Exact viewport and sampler state

`GraphicsManager::setViewport` at `0x10046ad3` calls `kglSetViewport`.
It stores `1 / viewportWidth` in `cbLighting +0x54`.
It stores `1 / viewportHeight` in `cbLighting +0x58`.
The stores call `CBuffer::set` at `0x1004abeb`.

`GraphicsManager::setSamplerState` at `0x100466f6` binds these pixel samplers:

| Slot | Runtime sampler |
|---:|---|
| `s0` | `samplerAniso` |
| `s1` | `samplerShadow` |
| `s2` | `samplerPoint` |
| `s4` | `samplerPointClamp` |
| `s3` | `samplerLinearShadow` |
| `s5` | `samplerLinearSimple` |

`kglCreateSampler` at `0x1000c560` creates `samplerAniso` with anisotropic
filtering and wrap addressing on U, V, and W.
The maximum anisotropy and mip bias come from video and render settings.
The FXAA shader uses explicit LOD zero for every texture sample.

The shader reflection names slot `s0` `samLinear`.
The runtime state assigns `samplerAniso` to that slot.
This slot assignment is exact.
No capture proves the driver's final filter interpretation for explicit LOD zero.

## Exact pixel algorithm

The shader samples the center, four cardinal neighbors, and four diagonal
neighbors from `t5` with `sample_l` and LOD zero.
It computes luminance as:

```text
luma = red + green * 1.96321070
```

It computes the minimum and maximum luminance across the center and cardinal
samples.
It computes the range between those values.
The early-exit threshold is:

```text
threshold = max(maxLuma * 0.125, 0.041666667)
```

If `range < threshold`, the shader returns the center RGB value.
It writes alpha `1.0` and depth `1.0` on this path.

The shader computes a subpixel factor from cardinal luminance values.
The factor subtracts `0.25`, multiplies by `1.33333337`, and clamps to `0.75`.
The shader then samples the four diagonal neighbors.

The shader selects an edge direction from luminance differences.
It searches both directions with a loop limit of 32 iterations.
Each search stops when the sampled luma reaches the local gradient threshold.
The final instructions blend the nine-sample neighborhood and the selected
edge sample with the computed factor.
The shader writes RGB, alpha `1.0`, and depth `1.0`.

The sample pattern, coefficients, early exit, loop limit, and output values
come from exact `ksFXAA_ps.fxo` disassembly.
The semantic names for edge direction and subpixel factor are inferred.

## Portable implementation boundary

The current native frame graph has no FXAA resource or execution pass.
Its postprocess pass writes directly to `backbuffer`.

A bounded Vulkan and D3D12 slice can add an optional full-resolution
`fxaa_temp` resource with RGBA8 and one sample.
Tone mapping can write this resource when FXAA is enabled.
The FXAA pass can read this resource and write the presentation target.
The pass can use the recovered nine-sample pattern and constants.

This portable slice must keep the presentation format as a backend input.
It must not claim native presentation parity.
It must label any sampler change from the recovered runtime slot as an approximation.

## Unresolved facts

The screen target's DXGI format remains unresolved.
The installed path does not prove an sRGB target or hardware sRGB conversion.
No runtime capture proves the driver's filter result for the anisotropic sampler
with explicit LOD zero.

These unknowns affect final presentation and border samples.
They do not change the recovered pass order or shader sample pattern.

## Evidence boundary

The disassembly proves the frame order, resource condition, resource format,
texture slot, viewport constants, sampler assignment, and output routing.
The DXBC proves the sample pattern, arithmetic constants, early exit, and loop limit.
The portable implementation remains a source-evidenced approximation until
Vulkan and D3D12 readback tests cover these states.
