# Recovered viewport frame border

## Scope

This slice reproduces the original editor's scene-panel frame border.

The border is exact for the recovered draw order, geometry, colors, and fixed-function state.

The Vulkan path uses source-equivalent shaders and the Vulkan clip-space convention.

The D3D12 path uses source-equivalent shaders and the recovered D3D screen-space transform.

## Installed files

The inspected `ksEditor.exe` has this SHA-256 value:

`7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`

The inspected `ksNet.dll` has this SHA-256 value:

`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`

## Control state

`Form1.panScene_MouseEnter` at RVA `0x6472` sets `Form1.controlsActive` to true.

`Form1.panScene_MouseLeave` at RVA `0x6487` sets the same field to false.

This state is mouse presence in the scene panel. It is not keyboard focus.

`Form1.onIdle` at RVA `0x3fd2` passes this state and the selected node to `Editor.onIdle`.

The method body SHA-256 value is:

`d48fb41a7a3196bc2454be8c273be9b88e41dd2a8f112742ba424012c0fc3bed`

Its IL SHA-256 value is:

`51e0560b9feb161f0711c6545dd664a6d37d77dda2514bdfab83070b22c743e2`

`Editor.onIdle` at RVA `0x2fdc` returns when the selected node is null.

Otherwise, it calls `ksGraphics.render(bool, kNode)`.

The method body SHA-256 value is:

`93b5f831ec4304bbf29db9c30825b5c766664ee6b55991726bb01902b494224`

Its IL SHA-256 value is:

`95c23813a9dfd74c51cc28717825b5c766664ee6b55991726bb01902b494224`

## Draw order

`ksGraphics.render` starts the scene at CIL offset `0x08d3`.

It restores normal depth at offset `0x0c49`.

It sets screen-space mode at offset `0x0c54`.

It draws fonts and the editor HUD next.

It calls `renderFrame` at offset `0x0d7c`.

Optional later HUD and sprite work starts at offset `0x0d81`.

The scene ends at offset `0x0e87`.

The frame therefore follows the world, selection, grid, and existing HUD work.

The C++ batch appends the frame after scene and overlay draws. It appends the frame before an MSAA resolve.

Reflection cube frames omit the border.

## Geometry and colors

`renderFrame` is at RVA `0x20f10` in `ksNet.dll`.

Its method body SHA-256 value is:

`acf840d5c9dc70ea8364a1e11505122c5eb609486c4702291a6f82fc4f5c870a`

Its IL SHA-256 value is:

`89d68c771d51b06cfbd28daba27314e39a6dccccc7e7b34fda5af84063a54679`

The method submits four untextured quads in this order:

1. Left: `(0, 0, 2, height)`
2. Top: `(0, 0, width, 2)`
3. Right: `(width - 2, 0, 2, height)`
4. Bottom: `(0, height - 2, width, 2)`

The method does not clamp dimensions below two pixels.

`GLRenderer::quad` is at VA `0x100478cb` and has a 285-byte PDB code range.

Each quad uses `z = 0` and this vertex order:

1. `(x, y, 0)`
2. `(x, y + height, 0)`
3. `(x + width, y + height, 0)`
4. `(x + width, y, 0)`

The active color is `(0, 0.7, 0, 1)`.

The inactive color is `(0.3, 0.3, 0.3, 1)`.

## Render state

`GraphicsManager::setDepthMode` is at VA `0x100465b9`.

Normal depth enables testing and writing. It uses the strict `LESS` comparison.

`GraphicsManager::beginScene` at VA `0x10044cfd` disables culling and blending.

The frame inherits normal depth after the explicit restore.

The translated pipeline therefore uses these states:

- Solid triangles
- No culling
- No blending
- Depth test enabled
- Depth write enabled
- Strict `LESS` comparison

The validator rejects line topology, `LESS_EQUAL`, missing depth, changed shaders, and truncated geometry.

## Maintained shader identity

The Vulkan vertex source SHA-256 value is:

`f61c144b285e9c0ee2c1390c87c2643b3cea3ab0d9b35ed8c0f3eb4533c1be43`

The Vulkan fragment source SHA-256 value is:

`6698c243b943df204358a9dde88d9584f4328a7f79ca3f99853fa7a5a6ff0ff2`

The D3D12 HLSL source SHA-256 value is:

`5a3bca3306daa6362e71b37e268a1b64e3e6fd50c0ee61599ce3c8274064d124`

The compiled artifacts have these SHA-256 values:

- Vulkan vertex: `fca7d07484b327b1707294be4f0c01bd45c425418d4fe0eeb097754ab4942bed`
- Vulkan fragment: `4d4c1f3c32610219fd7aab87893aaf40fcf9928f3d7efc8d581786a37df15ec1`
- D3D12 vertex: `757e7133ab13c6facd560e7a3713edf816f782a1a8089b27629ff1911f5dedee`
- D3D12 pixel: `8f02c0d83ec5982c2c1b25cd5073085b6e273e44ce62c5b0bdbd36620464500f`

Tests compare each maintained source and embedded artifact with these values.

## Fidelity boundary

The original editor has no recovered HDR, bloom, tone-map, or FXAA stage after this frame.

This port keeps HDR and FXAA as explicit options. Those options process the frame with the scene.

The default LDR path preserves the recovered ordering.

The browser renderer has no equivalent focus border. This feature changes only the native application path.
