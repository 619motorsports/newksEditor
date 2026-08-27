# Native shell HDR activation

## Scope

This note defines the native shell policy for portable HDR output.

It separates recovered native behavior from the portable implementation.

The policy keeps LDR output as the default for compatibility.

## Shell policy

The shell accepts these options:

- `--hdr` enables the portable HDR path.
- `--hdr` selects portable automatic exposure.
- `--hdr` uses the recovered-default tone-map constants.
- `--hdr` enables portable bloom with the recovered production controls.
- `--exposure <value>` selects manual exposure.
- `--exposure <value>` requires `--hdr`.

The shell rejects a manual exposure value that is not finite.

The shell rejects a manual exposure value less than zero.

The shell keeps the current LDR path when no HDR option is present.

`prepare_viewport` applies this policy in `native/src/app/main.cpp`.

It sets `hdr_tone_map` only when the user supplies `--hdr`.

It sets automatic exposure unless the user supplies `--exposure`.

It enables portable bloom in the same opt-in profile.

## Native selection evidence

The installed `ksNet.dll` SHA-256 is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

`VideoSettings::VideoSettings` starts at `0x10044a19`.

It sets `ppHDREnabled` to false and sets `ppGlare` to five.

The installed `cfg/video.ini` sets `DISABLE_HDR=0` and does not override glare.

The conversion from `DISABLE_HDR=0` to enabled HDR remains an inference.

The opt-in shell policy does not depend on that inferred conversion.

## Recovered-default tone-map constants

The native API stores the portable values in
`native/include/apex/render/device.hpp:526-533`.

The values include gamma `1.2`, saturation `0.95`, curve scale
`2.6581413745880127`, curve shoulder `0.6653175950050354`, and dither scale
`1.0 / 255.0`.

The bloom defaults include threshold `5.0`, remap `1.0`, composite scale
`0.01064`, kernel threshold `0.002`, radius scale `0.95`, source level `2`,
and display scale `2.2`.

The recovered native default shader mask is `0x52`.

It selects dither, exposure, and gamma in
`docs/evidence/ksnet-yebis-tone-map-handoff.md:156-175`.

The installed `default.ini` enables automatic exposure and sets manual exposure
to `0.28`.

It sets the target to `0.32` and limits exposure from `0.2` through `0.5`.

The shell uses `0.28` as the stored manual fallback for automatic mode.

Recovered initialization sets output exposure to `1.0` and middle gray to
`0.18` in `docs/evidence/ksnet-auto-exposure-adaptation.md:246-266`.

The C++ values combine recovered constants with a portable execution model.

They do not prove exact Yebis curve parity.

## Existing native path

The viewport prepares an HDR scene target when `hdr_tone_map` is present.

It validates the target and tone-map parameters at
`native/src/app/workspace_viewport.cpp:1426-1467`.

It resolves the scene before tone mapping when the scene uses multisampling.

It measures luminance and selects immediate automatic exposure at
`native/src/app/workspace_viewport.cpp:2255-2270`.

The device validates finite tone-map values and nonnegative exposure at
`native/src/render/device.cpp:658-682`.

The existing bloom path uses the production WebGL five-level topology.

The path remains a portable approximation, as described in
`docs/evidence/ksnet-hdr-bloom-glare.md:163-190`.

## Fidelity boundary

The recovered behavior proves the native tone-map handoff, shader mask,
constant fields, dither resource, and frame order.

The portable tone-map pass approximates the selected Yebis curve.

The portable exposure pass reads the final luminance mip synchronously.

It does not implement the recovered 17-sample ring or six-frame analysis
cadence (`docs/evidence/ksnet-auto-exposure-adaptation.md:284-296`).

The portable bloom pass uses five levels and WebGL-aligned filters.

It does not implement native temporal exposure order, channel dispersion, or
the native intermediate glare composite
(`docs/evidence/ksnet-hdr-bloom-glare.md:173-181`).

Describe these paths as portable approximations.

Do not describe them as exact Yebis parity.

## Verification requirements

Add a shell option test for each policy state.

Cover LDR by default, `--hdr`, and `--exposure` with and without
`--hdr`.

Reject negative and non-finite manual exposure.

Run a fixed-scene Vulkan test through SwiftShader.

Run the same test with D3D12 WARP on a Windows runner.

Check that both backends produce a valid tone-mapped presentation.

Check that the HDR source remains unchanged after tone mapping and bloom.

Run the production WebGL smoke test with the same camera and lighting values.

Record backend validation results and output hashes.

Keep the existing LDR output unchanged when the shell has no HDR option.

## Slice verification

The Linux build completed with GCC and all warning checks enabled.

CTest passed all 102 tests with SwiftShader enabled.

Two Windows-only D3D12 tests reported the expected skip status.

The native shell completed one validated automatic-exposure frame.

It completed a second validated frame with manual exposure `0.28`.

Both runs used SwiftShader and the checked-in KN5 model.

The production WebGL run rendered three meshes and 2,343 triangles.

Its frame hashes were `b0782090f3b83e5e` and `09177c61d521e938`.

Both WebGL states reported error zero and no browser exceptions.

The JavaScript suite passed 382 tests and skipped 34 optional tests.

This Linux host cannot execute D3D12 WARP.
