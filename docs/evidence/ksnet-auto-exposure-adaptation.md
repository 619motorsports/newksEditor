# ksNet automatic exposure adaptation evidence

## Scope and binary

This note records automatic exposure behavior in the installed Assetto Corsa SDK editor.

The inspected binary is `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`.

The binary size is 17,211,904 bytes.

The SHA-256 value is `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

Address values use preferred-image addresses from the `ksNet.dll` image.

Ghidra decompilation and x86 disassembly supplied the exact evidence in this note.

Field names in this note are inferred unless the text marks them as exact.

## Exact frame call order

`CPostEffect::EndPostEffectScene_InternalProcess` starts at `0x100846c0`.

It calls `CPostEffect::ApplyEffects_UpdateAutoExposure` at `0x10080d30` from call site `0x10086a93`.

The call occurs before `CPostEffect::ApplyEffects_TonemapTemporaly` at `0x10082e60`.

`CameraForwardYebis::setPostProcessing` starts at `0x10014d00`.

It calls `IPfxBaseContext::GetAutoExposureAdjusted` at `0x1006bf90` from call site `0x10014dfb`.

It passes the returned exposure to `SetTonemapParameters`.

The final call chain uses `CRenderGlare::TonemapToSurface` at `0x100e2f00`.

That function calls `CTextureUtil::DrawRectGPU_TonemapHDR` at `0x100b9310`.

## Exact timing and luminance evidence

`CPostEffect::SetRenderSceneParameters` starts at `0x1008fd90`.

It stores the frame delta in `CPostEffect +0x568`.

`IPfxBaseContext::SetRenderSceneElapsedTime` stores the same field at `0x1006b5b0`.

Initialization sets the default delta to `1/60` at `0x1008e93b`.

`CTextureLuminanceManage::UpdateFrame` starts at `0x100f7460`.

It stores samples in a ring with 17 entries.

The ring index uses modulo `0x11`.

The history coefficient is exact:

```text
historyWeight = 1 - pow(0.05, dt * 6 / 0.1)
historyWeight = 1 - pow(0.05, dt * 60)
```

The `pow` call occurs at `0x10080e48`.

The binary stores `0.05` at `0x1011ed68`.

The binary stores `6.0` at `0x1011076c`.

The binary stores `0.1` at `0x1101d72c`.

`CTextureLuminanceManage::Analyze` starts at `0x100f73f0`.

`CTextureLuminance::Analyze` starts at `0x100f6c50`.

The analysis path resamples the source and then reads an asynchronous luminance result.

`ApplyEffects_UpdateAutoExposure` waits until the exposure frame counter reaches five.

It then starts analysis when the luminance manager reports a ready result.

The function resets the counter to `-1` after each analysis.

The counter increments at the end of each update.

This produces a six-frame analysis cadence after the warm-up period.

## Exact delay behavior

The delay mode enum has these exact PDB values:

```text
PFXAAD_PHYSICALTIME = 0
PFXAAD_FRAMECOUNT   = 1
PFXAAD_DEFAULT      = 0
```

`SetAutoExposureDelay` starts at `0x10095e00`.

The runtime uses mode `PFXAAD_DEFAULT` with delay `0.0`.

Physical-time mode computes `max(delay, 0.5) / dt` samples.

Frame-count mode computes `max(delay, 0.0)` samples.

Both modes round `(sampleCount - 6) / 6` and clamp the ring delay to `[0, 16]`.

The delay index update occurs at `0x10080da8` through `0x10080e1f`.

## Exact target and adaptation formula

`CPostEffect::ApplyEffects_UpdateAutoExposure` starts at `0x10080d30`.

It calls `SetInverseTonemapForPhysicalLuminance` at `0x100f7bd0`.

It obtains the current luminance value with `GetCurrentValue` at `0x100f7b50`.

It obtains the current luminance with `GetCurrentLuminance` at `0x100f7b60`.

It computes the physical middle-gray value with `GetDecodeTonemapPhysicalLuminance` at `0x100f7b80`.

The exact ratio calculation is:

```text
physicalTarget = DecodeTonemap(middleGray, maxMappingLuminance, 1.0)
ratio = physicalTarget / (currentValue + epsilon)
```

The middle-gray input is `PEAUTOEXPOSURE +0x04`.

The maximum mapping input is `CPostEffect +0x9fc`.

The ratio comparison selects a directional target weight.

```text
weight = ratio >= PEAUTOEXPOSURE +0x2c
           ? PEAUTOEXPOSURE +0x40
           : PEAUTOEXPOSURE +0x34
```

The code then adapts in log2 exposure space.

```text
baselineLog = log2(CPostEffect +0x600 + epsilon)
ratioLog = log2(ratio)
targetLog = baselineLog + weight * (ratioLog - baselineLog)
```

The log calls and interpolation occur at `0x10081202` through `0x10081276`.

When `targetLog <= baselineLog`, the exact disassembly computes:

```text
response = 1 - pow(1 - PEAUTOEXPOSURE +0x30, dt / 0.05)
rawDelta = response * (targetLog - baselineLog)
deltaCap = -(PEAUTOEXPOSURE +0x38) / (CPostEffect +0x9f8) * dt
targetAdjustment = max(rawDelta, deltaCap)
```

It then applies the second response and clamps the adjustment.

```text
adjustResponse = 1 - pow(1 - PEAUTOEXPOSURE +0x3c, dt / 0.05)
newAdjustment = oldAdjustment + adjustResponse * (targetAdjustment - oldAdjustment)
newAdjustment = clamp(newAdjustment, targetAdjustment, 0.0)
```

The decreasing-exposure branch runs from `0x10081287` through `0x10081378`.

When `targetLog > baselineLog`, the exact disassembly computes:

```text
response = 1 - pow(1 - PEAUTOEXPOSURE +0x3c, dt / 0.05)
rawDelta = response * (targetLog - baselineLog)
deltaCap = +(PEAUTOEXPOSURE +0x44) / (CPostEffect +0x9f8) * dt
targetAdjustment = min(rawDelta, deltaCap)
```

It then applies the second response and clamps the adjustment.

```text
adjustResponse = 1 - pow(1 - PEAUTOEXPOSURE +0x30, dt / 0.05)
newAdjustment = oldAdjustment + adjustResponse * (targetAdjustment - oldAdjustment)
newAdjustment = clamp(newAdjustment, 0.0, targetAdjustment)
```

The increasing-exposure branch runs from `0x1008138a` through `0x10081474`.

The code clears the adjustment when the adjusted log value reaches or passes the target.

The conversion to exposure is exact:

```text
outputExposure = pow(2.0, adjustedLog)
outputExposure = clamp(outputExposure,
                       PEAUTOEXPOSURE +0x24,
                       PEAUTOEXPOSURE +0x28)
```

The conversion and clamp occur at `0x10081488` through `0x100814b6`.

The function stores the result at `PEAUTOEXPOSURE +0x54`.

## Inferred field labels

The following labels come from setter argument positions and disassembly use.

| Offset | Inferred value |
|---|---|
| `+0x00` | Enable flag |
| `+0x04` | Middle gray |
| `+0x08` | Secondary middle-gray value |
| `+0x1c` | Delay mode |
| `+0x20` | Delay value |
| `+0x24` | Minimum exposure |
| `+0x28` | Maximum exposure |
| `+0x2c` | Direction threshold |
| `+0x30` | One directional response rate |
| `+0x34` | Directional target weight |
| `+0x38` | Decreasing exposure cap rate |
| `+0x3c` | Other directional response rate |
| `+0x40` | Other directional target weight |
| `+0x44` | Increasing exposure cap rate |
| `+0x48` | Analysis frame counter |
| `+0x4c` | Current log adjustment |
| `+0x54` | Adjusted exposure output |
| `+0x58` | Previous enable flag |
| `+0x5c` | Luminance manager pointer |

These labels are not native symbols from the binary.

`CPostEffect +0x600` stores manual tonemap exposure.

`CPostEffect +0x9f8` receives `1.0` from `SetAutoExposureAdjustment` at `0x10095be6`.

`CPostEffect +0x9fc` stores effective maximum mapping luminance.

`CPostEffect::UpdateParameters_ChangeFormat` calculates that field at `0x10087800`.

## Exact defaults and reset behavior

`CPostEffect::Initialize` starts at `0x1008a080`.

It calls `SetAutoExposureAdjustment` with `0.5, 0.85, 8.0, -1.0, -1.0, -1.0, 1.0`.

The setter starts at `0x10095ab0`.

The setter resolves negative fallback values to the earlier directional values.

Initialization sets the exposure range to `1.1754944e-38` through `3.4028235e+38`.

Runtime uses `0.0` through `32.0` when exposure limits are disabled.

Runtime uses the configured minimum and maximum when exposure limits are enabled.

Initialization sets middle gray to `0.18`.

Initialization sets the analysis delay to `-1.0` with the default delay mode.

Runtime sets the delay to `0.0` with the default delay mode.

Initialization sets the metering area to full-frame values.

Runtime also sets the metering area to full-frame values.

`CPostEffect::InitializeAutoExposure` starts at `0x10095730`.

It initializes the luminance manager to `0.18`.

It sets the output exposure to `1.0`.

It clears the frame counter and the current adjustment.

It clears the previous enable flag.

`CPostEffect::SetAutoExposureAdjustmentReset` starts at `0x100962c0`.

It clears only the current adjustment field at `+0x4c`.

It does not clear the 17-sample luminance ring.

`ApplyEffects_UpdateAutoExposure` clears `+0x4c` when the enable flag changes.

That edge detection compares `+0x00` with `+0x58` at `0x10080e76` through `0x10080e7d`.

Reinitialization recreates the luminance manager and clears its resources.

## Portable implementation boundary

The portable slice uses a synchronous readback from the final mip.

The slice applies `ksEditorAutoExposure` immediately after the readback.

This slice is portable and WebGL-aligned.

It does not implement the 17-sample ring or the six-frame analysis cadence.

It does not implement the log-space update, directional caps, or `DecodeTonemap`.

It is not exact Yebis parity because the original uses asynchronous luminance queries and proprietary tonemap decoding.

## Remaining unknowns

The exact `HDRTMAP_DecodeTonemap` curve remains unresolved for every enum variant.

The exact luminance query shader and GPU feedback latency remain unresolved.

The PEAUTOEXPOSURE field names remain inferred from setter offsets and use.

The secondary middle-gray field at `+0x08` is not read by `ApplyEffects_UpdateAutoExposure`.

Its other use remains unresolved.

The `+0x3ef` early-exit flag has no recovered semantic name.
