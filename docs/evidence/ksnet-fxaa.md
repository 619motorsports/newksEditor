# ksNet FXAA evidence

## Scope

This note records the installed editor FXAA path.
It separates exact disassembly and DXBC facts from inferred names and portable approximations.

## Installed source

The source binary is `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`.
Its SHA-256 is `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The file size is 17,211,904 bytes.
The addresses below use the preferred image base from Ghidra.

The shader files are in `system/shaders/win` under the installed editor path.

## Exact frame path

`CameraForwardYebis::render` is at `0x1001aa70`.
It calls `renderApplyEffect` at `0x1001ad80`.
That function renders the scene.
It resolves MSAA color for multisample targets.
It then calls `applyPostProcessing` at `0x100159a0`.

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

`getYebisDestinationRenderTarget` is at `0x1001ac50`.
If `fxaaTempRT` exists, `0x1001ac50` returns `fxaaTempRT->kidColor`.
If it does not exist, the function returns `screenRenderTarget`.
This routing places FXAA after the Yebis tone map.

## Exact resource conditions

The constructor at `0x10015ad0` reads `ppHDREnabled` and `ppFXAA`.
Both HDR and FXAA must be enabled for the constructor to create FXAA resources.

The constructor creates `fxaaTempRT` with `eR8G8B8A8`, the full viewport width
and height, one sample, and no depth surface.
It loads the `ksFXAA` shader by name.
These conditions and values are exact decompilation evidence.

`resetYebisStates` at `0x1001ac70` binds the screen render target and removes
the depth target before the full-screen FXAA draw.
This state transition is exact disassembly evidence.

## Exact defaults and gates

`VideoSettings::VideoSettings` at `0x10044a19` defaults `ppHDREnabled=false`.
It defaults `ppFXAA=false` and `ppGlare=5`.
The installed `cfg/video.ini` contains `DISABLE_HDR=0` and `FXAA=0`.
The `DISABLE_HDR` inversion remains inferred because `loadVideoSettings` did not decompile completely.

When the HDR flag at `GraphicsManager+0x40` is false, `0x1001aa70` calls the base camera render.
That branch skips Yebis and the FXAA draw.
The HDR and FXAA resource gate is exact disassembly evidence.

## Exact target formats

`kglCreateRenderTarget` at `0x1000ce30` maps `eR8G8B8A8` to
`DXGI_FORMAT_R8G8B8A8_UNORM`.
This mapping identifies the FXAA temporary target as linear RGBA8 UNORM.

`createDeviceAndSwapChain` at `0x1000d670` writes `0x1c` to the swap-chain
format field.
`0x1c` is `DXGI_FORMAT_R8G8B8A8_UNORM`.
`initDX11` at `0x1000d9f0` wraps the swap-chain back buffer as `screenRenderTarget`.
The temporary and presentation targets therefore use RGBA8 UNORM.

No inspected path selects `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`.
No inspected path enables hardware sRGB conversion.
These format facts come from exact disassembly and DirectX format values.

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
It copies position input `v0.xyzw` to the output position.
It copies `v2.xyxy` to output texture coordinates.
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
filtering and repeat addressing on U, V, and W.
The maximum anisotropy and mip bias come from video and render settings.
The FXAA shader uses explicit LOD zero for every texture sample.

The shader reflection names slot `s0` `samLinear`.
The runtime state assigns `samplerAniso` to that slot.
This slot assignment is exact.
No capture proves the driver's final filter interpretation for explicit LOD zero.

The runtime sampler state is exact.
The final filtering result remains untested because the installed evidence has no runtime GPU capture.

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
Each search stops after the sampled luma reaches the local gradient threshold.
The final instructions blend the nine-sample neighborhood and the selected
edge sample with the computed factor.
The shader writes RGB, alpha `1.0`, and depth `1.0`.

The sample pattern, coefficients, early exit, loop limit, and output values
come from exact `ksFXAA_ps.fxo` disassembly.
The semantic names for edge direction and subpixel factor are inferred.

## Portable implementation boundary

The native frame graph now has an optional full-resolution FXAA source.
The source uses RGBA8 UNORM with one sample.
If FXAA is enabled, tone mapping writes the FXAA source.
The FXAA pass reads this source and writes a separate presentation-format texture.
The presentation step copies that output to the platform target.

The native shell enables this path only with `--hdr --fxaa`.
The default LDR path and the default HDR path remain unchanged.
The portable sampler uses linear filtering with repeat addressing.
This sampler differs from the exact runtime sampler and remains an approximation.
Cross-backend sRGB compensation also remains an approximation.
The slice must not claim exact pixel parity.

The presentation format remains a backend input.
The implementation must label these portable format and sampler choices as approximations.

The checked-in portable shader identities are:

| Artifact | SHA-256 |
|---|---|
| `fxaa.frag` | `ff624dc722eef33dbbf11ba09144ce5013efe3b164b99181cf60cdba30730dda` |
| FXAA fragment SPIR-V | `1b199a87148f094ea8afcccef1c301f5a897ba640986bfaf8725ec79e0368559` |
| `d3d12_fxaa.hlsl` | `7028fb4fa978b21cca51ea2e85be73a046807ce8a09899038634f42d33283058` |
| FXAA vertex DXBC | `1226553b9f78efddaa9f9ab82cdb50b158893d78582e88d0a47c1a6e7660cb75` |
| FXAA pixel DXBC | `f5250d3707f0bbe5f3f6faddaec4a3994d6cd47f8cd0b7c7c62dd1eed9466bb7` |

`glslc` 2026.3 generated the SPIR-V for Vulkan 1.0.
`spirv-val` 2026.3 validated the result.
Wine 11.16 provided `D3DCompile` for the Shader Model 5 DXBC artifacts.
The drift test checks every source and generated artifact identity.

## Unresolved facts

The installed path proves RGBA8 UNORM for the screen target.
The installed path does not prove an sRGB target or hardware sRGB conversion.
No runtime capture proves the driver's filter result for the anisotropic sampler
with explicit LOD zero.

These unknowns affect final presentation and border samples.
They do not change the recovered pass order or shader sample pattern.

## Evidence boundary

The disassembly proves the frame order, resource condition, target formats,
texture slot, viewport constants, sampler assignment, and output routing.
The DXBC proves the sample pattern, arithmetic constants, early exit, and loop limit.
The semantic names remain inferred from register use.
The portable implementation remains an approximation until Vulkan and D3D12 readback tests cover these states.

## Portable slice verification

The Linux GCC build completed with all configured warning checks.
CTest passed all 104 tests with the Electron SwiftShader ICD.
Two Windows-only D3D12 tests reported their expected skip status.

The FXAA runtime test covered all four supported presentation formats.
It covered flat input, a diagonal stair-step, repeat execution, and source preservation.
It also covered HDR tone mapping before FXAA.

The native shell completed one validated automatic-exposure frame with FXAA.
It completed one validated manual-exposure frame with FXAA.
Both frames used SwiftShader and the checked-in LOD-B car.

The D3D12 file passed the strict Windows-target `clang-cl` syntax check.
This Linux host cannot run D3D12 WARP.

The JavaScript suite passed 382 tests and skipped 34 optional tests.
The production WebGL smoke produced hashes `35eb0e1d44047615` and
`defad6d31b8db34a`.
Both WebGL states returned error zero and no browser exception.
