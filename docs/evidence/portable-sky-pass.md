# Portable procedural sky pass

## Scope

This note records the evidence for the portable C++ sky pass.

The pass matches the production WebGL ray and color formula. It is not an exact reconstruction of the original editor skybox.

Cloud rendering is outside this pass.

## Production WebGL behavior

`public/app.js` builds one fullscreen triangle for the sky. The fragment shader reconstructs one world-space camera ray for each pixel.

The shader uses the camera forward, right, and up vectors. It also uses the vertical field of view and target aspect ratio.

The ray expression is:

```text
normalize(forward
  + right * ndc.x * tanHalfFov * aspect
  + up * ndc.y * tanHalfFov)
```

The shader blends the horizon and zenith colors with this expression:

```text
smoothstep(-0.08, 0.42, ray.y)
```

It adds a narrow solar lobe with this expression:

```text
smoothstep(cos(0.02618), cos(0.00524), dot(ray, normalize(sunDirection)))
```

The shader clamps negative color components to zero. It writes an alpha value of one.

The main WebGL frame draws the sky after it clears the HDR target. It draws the sky before clouds and scene geometry.

The WebGL reflection capture uses the same shader and lighting values. It rebuilds the camera basis for each cube face.

## Recovered native evidence

The source binary is `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`.

The original `ksNet.dll` SHA-256 value is `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

The addresses below use the preferred image base from Ghidra.

`CubeMapRenderer::renderScene` is at `0x10103a84`. It copies the selected face orientation and the active skybox pointer.

`SkyBox::render` is at `0x100618af`. It uses `cubemapSkyShader` during a cube capture, then renders clouds.

This evidence proves sky and cloud participation during original cube capture. It does not prove the portable gradient or sun-lobe formula.

## C++ port contract

The C++ pass runs inside the ordered static-scene render pass. Backends draw it after the attachment clear and before other scene draws.

The pass supports the main view and each reflection cube face. It supports LDR and HDR targets with one or four samples.

The CPU normalizes the camera basis and sun direction. It rejects non-finite values, degenerate bases, and invalid color values.

Vulkan and D3D12 use the same 112-byte constant record. Backend shaders apply the production WebGL formula without texture inputs.

The implementation and diagnostics must call this behavior `portable` or `WebGL-aligned`. They must not call it exact native skybox behavior.

## Verification requirements

Tests must cover valid constants and malformed values. Runtime tests must check the horizon, zenith, and solar lobe.

Runtime coverage must include Vulkan through SwiftShader. D3D12 coverage must use WARP when a Windows runner is available.

Tests must cover LDR, HDR, multisample, and cube-face targets. Visible changes also require the production WebGL smoke check.

One runtime test must combine the sky, a depth attachment, and retained material geometry in the same render pass.
