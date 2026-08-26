# Native editor D3D11 fixed-function state

This note records recovered D3D11 behavior from the installed Assetto Corsa editor. It does not claim exact D3D12 behavior.

## Evidence boundary

The addresses in this note use the loaded base address from the Ghidra project. The installed binary remains the primary evidence.

D3D12 has different state objects and binding rules. A D3D12 port must label each mapping as a backend translation.

## Default state

`kglSetDefaultState` at `0x1000c390` sets the input topology to value `4`. D3D11 defines this value as a triangle list.

The function also resets the input layout, vertex buffers, index buffer, and shader state. It then selects these state indexes:

- Blend state `1`.
- Cull state `2`.
- Depth state `0`.

`GraphicsManager::beginScene` calls this function near `0x10044d0c`. `CameraForwardYebis::resetYebisStates` calls it at `0x1001ac77`.

## Static mesh input layout

`getInputLayout` at `0x1000eea0` selects the input layout. The D3D11 device creates each layout through vtable offset `0x2c`.

Layout `0` is the static `MeshVertex` layout. It has a 44-byte stride and these attributes:

| Semantic | D3D format | Byte offset |
| --- | --- | ---: |
| `POSITION0` | `R32G32B32_FLOAT` | `0` |
| `NORMAL0` | `R32G32B32_FLOAT` | `12` |
| `TEXCOORD0` | `R32G32_FLOAT` | `24` |
| `TANGENT0` | `R32G32B32_FLOAT` | `32` |

All attributes use input slot `0`. All attributes are per-vertex data with a step rate of `0`.

## State application

`kglSetBlendState` at `0x1000c890` calls `OMSetBlendState`. It uses no blend-factor array and uses sample mask `0xffffffff`.

`kglSetDepthState` at `0x1000c8b0` calls `OMSetDepthStencilState`. It uses stencil reference `0`.

`kglSetCullState` at `0x1000c8d0` calls `RSSetState`.

`kglSetPrimitiveType` at `0x1000cbc0` maps editor values to these D3D11 topology values:

| Editor value | D3D11 value | Topology |
| ---: | ---: | --- |
| `0` | `4` | Triangle list |
| `1` | `2` | Line list |
| `2` | `3` | Line strip |
| `3` | `5` | Triangle strip |

## Blend states

`initBlendStates` at `0x1000e2d0` creates four blend states. The function calls `CreateBlendState` through vtable offset `0x50`.

The recovered descriptors contain these values:

| Index | Alpha-to-coverage | Color blend | Color factors | Alpha operation | Write mask |
| ---: | --- | --- | --- | --- | ---: |
| `0` | Disabled | Disabled | `SRC_ALPHA`, `INV_SRC_ALPHA`, `ADD` | `ONE`, `ONE`, `ADD` | `0xf` |
| `1` | Disabled | Enabled | `SRC_ALPHA`, `INV_SRC_ALPHA`, `ADD` | `ONE`, `ONE`, `MAX` | `0xf` |
| `2` | Enabled | Disabled | `SRC_ALPHA`, `INV_SRC_ALPHA`, `ADD` | `ONE`, `ONE`, `MAX` | `0xf` |
| `3` | Disabled | Enabled | `SRC_ALPHA`, `INV_SRC_ALPHA`, `ADD` | `ONE`, `ONE`, `MAX` | `0x0` |

State `0` is the opaque state. State `1` is the alpha-blend state, and state `2` is the alpha-to-coverage state.

`KN5IO::loadMaterialsBinary` at `0x1003b9de` reads the material flags. The alpha-to-coverage flag overrides the alpha-blend flag.

The `ksPerPixelAT` pixel shader contains no discard instruction. Its cutout behavior uses fixed-function alpha-to-coverage and a multisample render target.

## Depth states

`initDX11` at `0x1000d9f0` creates the depth states. The function calls `CreateDepthStencilState` through vtable offset `0x54`.

The recovered descriptors contain these values:

| Index | Depth test | Depth write | Comparison |
| ---: | --- | --- | --- |
| `0` | Enabled | Enabled | `LESS` |
| `1` | Enabled | Disabled | `LESS_EQUAL` |
| `2` | Disabled | Disabled | `LESS_EQUAL` |
| `3` | Enabled | Enabled | `LESS_EQUAL` |

`GraphicsManager::setDepthMode` at `0x100465b9` maps the editor depth modes to these indexes.

## Cull states

`initCullStates` at `0x1000e4a0` creates the rasterizer states. The function calls `CreateRasterizerState` through vtable offset `0x58`.

Constants at `0x1101d770` through `0x1101d800` identify these standard descriptors:

| Index | Fill mode | Cull mode | Front counterclockwise |
| ---: | --- | --- | ---: |
| `0` | `SOLID` | `FRONT` | `0` |
| `1` | `SOLID` | `BACK` | `0` |
| `2` | `SOLID` | `NONE` | `0` |
| `4` | `WIREFRAME` | `NONE` | `0` |
| `5` | `SOLID` | `FRONT` | `0` |

State `3` uses `SOLID`, `NONE`, depth bias `-100`, and depth-bias clamp `0.1`.

`GraphicsManager::setCullMode` at `0x10046572` selects state `5` for front culling when `overrideNoMS` is active.

## Main draw order

`Material::apply` at `0x1004016e` applies blend, cull, and opaque-pass depth state. It then binds resources, shaders, and material constant buffers.

Double-sided materials replace the selected cull state with `NONE`.

`PvsProcessor::doRenderCalls` at `0x10065b85` applies changed states. It then binds geometry and updates the world-matrix buffer at `b1`.

The function finishes the mesh command with `DrawIndexed`.

## D3D12 translation boundary

The current D3D12 native path can translate the following recovered facts:

- Triangle-list topology.
- The 44-byte static mesh input layout.
- The selected blend, depth, and cull descriptors.
- The `b0-b4`, `t0`, `t6-t8`, and `s0-s1` shader bindings.

The D3D11 binary contains no D3D12 root signature or pipeline-state object. Tests on Microsoft D3D12 remain necessary for the translated objects.
