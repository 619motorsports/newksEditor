# ksNet cubemap capture evidence

## Installed source

This note uses the installed Assetto Corsa SDK editor at this path:

`/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor`

The inspected files have these SHA-256 values:

- `ksNet.dll`: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- `ksEditor.exe`: `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`

The addresses in this note are preferred-image addresses from `ksNet.dll`.
Ghidra decompilation and instruction inspection supplied the evidence.

## Cube allocation

`CameraForward::CameraForward` at `0x1010254e` constructs a
`CubeMapRenderer`. It calls `CameraForward::setCubemapSize` at `0x101028f2`
with the value `0x200`.

`CameraForward::setCubemapSize` at `0x10103068` removes the old cube. It then
constructs `CubeMap(size, 1, 7)`. The stock cube size is 512, and the cube has
seven mip levels.

`KGLCubeMap::KGLCubeMap` at `0x100132c0` creates the color texture with these
D3D11 values:

| Field | Value |
|---|---|
| Width and height | 512 |
| Mip levels | 7 |
| Array size | 6 |
| Format | `DXGI_FORMAT_R16G16B16A16_FLOAT` |
| Sample count | 1 |
| Bind flags | Render target and shader resource (`0x28`) |
| Miscellaneous flags | Generate mips and texture cube (`5`) |

The function creates one render-target view for each face. It also creates one
cube shader-resource view.

The depth texture uses the same width and height. It uses
`DXGI_FORMAT_D16_UNORM`, one array layer, and one sample.

## Capture cameras

`CubeMapRenderer::CubeMapRenderer` at `0x10103695` sets these camera values:

| Field | Value |
|---|---:|
| Field of view | 90 degrees |
| Near plane | 0.01 |
| Far plane | 350 |
| Aspect ratio | 1 |
| Maximum layer | 0 |
| Faces per frame | 6 |

The constructor supplies these heading and up vectors to
`mat44f::setFromHeadingUp` at `0x10038e4c`:

| Capture index | Heading | Up |
|---:|---|---|
| 0 | `-X` | `+Y` |
| 1 | `+X` | `+Z` |
| 2 | `+Y` | `-Z` |
| 3 | `-Y` | `+Z` |
| 4 | `+Z` | `+Y` |
| 5 | `-Z` | `+Y` |

This order is not the native `CubeFace` enumeration order. Capture code must
map each heading to an explicit `CubeFace` value.

## Capture sequence

`CubeMapRenderer::render` at `0x101038c5` does these operations:

1. It enables `GraphicsManager::state.overrideNoMS`.
2. It binds `samplerLinearSimple` to pixel-sampler slot 0.
3. It clears pixel-texture slot 10 at `0x10044d9d`.
4. It sets `currentCubeMap` to null.
5. It renders all six faces.
6. It unbinds the render targets at `0x1000c730`.
7. It generates all cube mips at `0x1000cb90`.
8. It binds the cube shader-resource view to pixel-texture slot 10.
9. It sets `currentCubeMap` to the completed cube.
10. It restores `samplerAniso` and disables `overrideNoMS`.

The cleared texture slot prevents reflection feedback during capture. The
renderer does not sample a previous cube while it writes the new cube.

Each face pass calls `CubeMap::beginFace` at `0x101045bc`. It then calls
`renderScene` at `0x10103a84` and `CubeMap::endFace` at `0x101045cd`.

`kglCubeMapBeginFace` at `0x1000c4b0` binds one face view and the shared depth
view. It sets a viewport that covers the full cube face.

The capture camera clears each face to RGBA `(0,0,0,0)`. It clears depth to
`1.0`. `Camera::clearBuffers` at `0x100643cd` performs these clears.

## Scene scope

`CubeMapRenderer::renderScene` at `0x10103a84` uses the active camera position.
It copies the selected face orientation and the active skybox pointer.

The function sets `skyBox->cubemapPass` during the capture. `SkyBox::render` at
`0x100618af` uses `cubemapSkyShader` for this pass. It then renders clouds.

The capture camera uses the opaque mesh filter and maximum layer 0.
`PvsProcessor::doExclusion` at `0x10065a37` excludes transparent-pass meshes.
It also applies the mesh layer and flag filters.

The cube renderer has no direct self-object exclusion. It prevents reflection
feedback by clearing texture slot 10 and `currentCubeMap`.

`CameraForwardYebis::render` calls the cube capture at `0x1001ab92`. The call
occurs before Yebis updates, post-processing, buffer swap, and FXAA.

## Port boundary

The C++ port must label any different schedule or filter as an approximation.
The production fidelity path requires all six faces, seven mips, and the exact
face mapping in this note.

The first backend seam can support one face and mip zero without changing the
visible production path. Full native capture remains staged until mip
generation and all scene filters have production execution tests.
