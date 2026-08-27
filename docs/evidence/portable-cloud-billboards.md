# Portable cloud billboards

## Scope

This note records installed native cloud evidence and the portable implementation boundary.

The native evidence comes from the installed Assetto Corsa SDK editor.

The portable billboard pass is WebGL-aligned and does not claim exact native pixel parity.

## Installed inputs

The inspected editor directory is `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor`.

The installed `ksNet.dll` SHA-256 value is `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

The installed `ksEditor.exe` SHA-256 value is `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`.

The installed `ksClouds_vs.fxo` SHA-256 value is `8f9f555bbfa7283fd6ccf6dfcf357381910d156ef5f1c1738c4d62b4a7276279`.

The installed `ksClouds_ps.fxo` SHA-256 value is `be5d824649ddeaeaccf6e906c162303bcb3c61614b45b79368b1d8c3cbaa8c43`.

The installed `ksClouds_meta.ini` SHA-256 value is `a0c5eb4eb8b26fcff3ad8bf9d1c1a492d098079ea4a73511432427435a91cf53`.

The installed `ksClouds.shader` SHA-256 value is `59e8ea19a92a2e48321ea0c30b604f37505d1d6c86229838e20a885b9a84065f`.

The installed metadata sets `ALPHATEST=0`, `SKINNED=0`, `PARTICLE=0`, and `2D=0`.

The installed cloud texture paths are:

```text
content/texture/clouds/cloud1C.dds
content/texture/clouds/cloud2C.dds
content/texture/clouds/cloud3C.dds
content/texture/clouds/cloud4C.dds
content/texture/clouds/cloud5C.dds
content/texture/clouds/cloud6C.dds
content/texture/clouds/cloud7C.dds
```

This note records paths only. It does not copy the installed assets.

The following commands disassemble the installed shader inputs:

```text
/tmp/dxbc-disasm.exe /mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/system/shaders/win/ksClouds_vs.fxo
/tmp/dxbc-disasm.exe /mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/system/shaders/win/ksClouds_ps.fxo
```

`/tmp/dxbc-disasm.exe` is a tool path. The commands do not require temporary output files.

## Native functions and order

The following addresses use the preferred image base from Ghidra.

`SkyBox::SkyBox` is at `0x10060bdb`.

The constructor reads cloud width, height, radius, count, and base speed from `weather.ini`.

The constructor loads every DDS file from `content/texture/clouds`.

`SkyBox::render` is at `0x100618af`.

`SkyBox::renderClouds` is at `0x10061b32`.

`SkyBox::render` draws the sky material and then calls `SkyBox::renderClouds`.

At `0x10061b55`, `renderClouds` calls `GraphicsManager::setCullMode` at `0x10046572` with `0`.

At `0x10061b62`, it calls `GraphicsManager::setDepthMode` at `0x100465b9` with `2`.

At `0x10061b69`, it calls `GraphicsManager::setBlendMode` at `0x10046550` with `1`.

Existing native state evidence maps these calls to front-face culling, disabled depth, and source-alpha blending.

The native pass draws adjacent texture runs in construction order.

It does not regroup overlapping billboards by texture.

Each billboard faces the camera and keeps local normal `(0, -1, 0)`.

`Camera::render` is at `0x10064657`.

It renders the skybox in a Classic PVS pass before ordinary scene geometry.

The cloud pass therefore follows the sky material and precedes the ordinary scene in the main view.

## DXBC vertex program

The following statements reproduce exact `ksClouds_vs.fxo` instructions.

```text
vs_4_0
dcl_constantBuffer cb0[8], immediateIndexed
dcl_constantBuffer cb1[4], immediateIndexed
dcl_constantBuffer cb2[1], immediateIndexed
dcl_input v0.xyzw
dcl_input v1.xyz
dcl_input v2.xy
dcl_output_siv o0.xyzw, position
dcl_output o1.xyz
dcl_output o2.xyz
dcl_output o3.xy
dcl_output o4.xyz
dp4 r0.w, v0.xyzw, cb1[3].xyzw
dp4 r0.x, v0.xyzw, cb1[0].xyzw
dp4 r0.y, v0.xyzw, cb1[1].xyzw
dp4 r0.z, v0.xyzw, cb1[2].xyzw
dp4 r1.x, r0.xyzw, cb0[0].xyzw
dp4 r1.y, r0.xyzw, cb0[1].xyzw
dp4 r1.z, r0.xyzw, cb0[2].xyzw
dp4 r1.w, r0.xyzw, cb0[3].xyzw
mov o1.xyz, r0.xyzx
dp4 o0.x, r1.xyzw, cb0[4].xyzw
dp4 o0.y, r1.xyzw, cb0[5].xyzw
dp4 o0.z, r1.xyzw, cb0[6].xyzw
dp4 o0.w, r1.xyzw, cb0[7].xyzw
mov o2.xyz, v1.xyzx
mov o3.xy, v2.xyxx
dp3 r0.x, cb2[0].xyzx, cb0[0].xyzx
dp3 r0.y, cb2[0].xyzx, cb0[1].xyzx
dp3 r0.z, cb2[0].xyzx, cb0[2].xyzx
dp3 o4.x, r0.xyzx, cb0[4].xyzx
dp3 o4.y, r0.xyzx, cb0[5].xyzx
dp3 o4.z, r0.xyzx, cb0[6].xyzx
```

The exact vertex equations are:

```text
world = v0 * cb1[0..3]
view = world * cb0[0..3]
o0 = view * cb0[4..7]
o1 = world.xyz
o2 = v1.xyz
o3 = v2.xy
o4 = cb2[0] transformed by cb0 view and projection rows
```

DXBC RDEF names `cb0` as `cbCamera`, `cb1` as `cbPerObject`, and `cb2` as `cbLighting`.

RDEF names `ksView`, `ksProjection`, `ksWorld`, and `ksLightDirection` identify the matrix and light fields.

The `o4` consumer is not present in the inspected pixel shader input declarations.

Its final native use remains unresolved.

## DXBC pixel program

The following statements reproduce exact `ksClouds_ps.fxo` instructions and register order.

```text
ps_4_0
dcl_constantBuffer cb2[8], immediateIndexed
dcl_sampler s0
dcl_resource_texture2d (float,float,float,float) t0
dcl_input_ps linear v2.xyz
dcl_input_ps linear v3.xy
dcl_output o0.xyzw
mul r0.x, cb2[5].w, cb2[5].w
mul r0.x, r0.x, l(1.11111114e-03)
div_sat r0.x, cb2[5].w, r0.x
dp3 r0.y, v2.xyzx, cb2[0].xyzx
sample r1.xyzw, v3.xyxx, t0.xyzw, s0
add r0.z, -r1.x, l(1.00000000e+00)
mul_sat o0.w, r1.w, cb2[7].x
mul r0.y, r0.z, r0.y
mul r0.y, r0.y, cb2[7].z
mad r0.z, cb2[1].x, l(7.50000000e-01), -r0.y
mad r0.x, r0.x, r0.z, r0.y
add r0.yzw, cb2[1].xxyz, cb2[2].xxyz
mul r0.yzw, r0.yyzw, l(0.00000000e+00, 5.00000000e-01, 5.00000000e-01, 5.00000000e-01)
add r1.x, -cb2[7].y, l(1.00000000e+00)
mul r0.yzw, r0.yyzw, r1.x
mad o0.xyz, r0.x, cb2[7].y, r0.yzwy
```

RDEF identifies `cb2` as the eight-register `cbLighting` block.

The exact named fields include `ksLightDirection`, `ksAmbientColor`, `ksLightColor`, `ksFogLinear`, `ksCloudCover`, `ksCloudCutoff`, and `ksCloudColor`.

The exact register equations are:

```text
f = saturate(F / (F * F * 0.00111111114))
light = dot(v2.xyz, cb2[0].xyz)
sample = t0.Sample(s0, v3.xy)
alpha = saturate(sample.a * cb2[7].x)
value = (1 - sample.r) * light * cb2[7].z
valueFogged = f * (0.75 * cb2[1].x - value) + value
base = cb2[1].xxyz + cb2[2].xxyz
base = base * (0, 0.5, 0.5, 0.5)
base = base * (1 - cb2[7].y)
o0.xyz = valueFogged * cb2[7].y + base.yzwy
```

The first equation preserves the exact divide and saturate order.

For finite positive `F`, it is algebraically `saturate(900 / F)`.

The vector equations interpret exact DXBC swizzles at a higher level.

The instruction text remains the authoritative evidence for channel placement.

The portable shader uses one 144-byte constant block.

The native shader uses separate camera, per-object, and lighting blocks.

This layout difference is a portable implementation choice, not native layout evidence.

Existing reconstructed source hashes are `31cda90f3239d561e62fe837b3caeacb5145d0bb570c2c97d673222ae855f07a` for pixel source and `c64f6d0057bdaebccb72cb0d65b632a214873b7d892af67eaadbeebebb6f8a0f` for vertex source.

## Billboard layout

The following formulas describe the recovered layout used by the portable generator.

```text
count = min(512, floor(configuredCount * worldDetail * 0.2))
phi = (((rand01 - 0.5) * 10) + 360 / count) * pi / 180 * index
band = (((rand01 - 0.5) * 5) + 15) * pi / 180 * ((index + 1) % 5)
theta = (((rand01 - 0.5) * 30) + 20) * pi / 180 + band
radius = (1 - cos(theta)) * 4 + configuredRadius
speed = baseSpeed == 0 ? 0 : max(0.0005, min(1, rand01 * baseSpeed))
ring = radius * sin(phi)
position = (ring * cos(theta), radius * cos(phi), -ring * sin(theta))
```

The default world detail value is five.

World detail five keeps the configured count because the native multiplier is `worldDetail * 0.2`.

The portable generator uses a local Visual C++ sequence with seed one.

The native editor uses process-global Visual C++ `rand()` state.

Earlier random calls can therefore change native cloud placement.

Seed-one placement is deterministic and useful for tests.

Seed-one placement is a portable and WebGL-aligned approximation.

It is not exact native placement.

## Main and reflection participation

The installed `race.ini` starts with weather preset `5_light_clouds`.

The native main path renders clouds inside `SkyBox::render` during `Camera::render`.

The cloud pass follows the sky material and runs before ordinary scene geometry.

`CubeMapRenderer::render` is at `0x101038c5`.

It renders all six faces, generates cube mips, and binds the completed cube to pixel-texture slot 10.

Each face calls `CubeMapRenderer::renderScene` at `0x10103a84`.

`renderScene` sets the active skybox and capture orientation.

`SkyBox::render` at `0x100618af` uses `cubemapSkyShader` during capture and then renders clouds.

`CameraForwardYebis::render` calls cube capture at `0x1001ab92` before Yebis updates and FXAA.

The native evidence proves cloud participation in the main view and reflection cube capture.

The portable WebGL check drew 50 billboards and 100 triangles for light clouds.

It captured clouds on all six initialized reflection faces.

The light-cloud frame hash was `c32f6f659d7838db`.

The clear-weather frame hash was `c29e2972e101d933`.

Both checks reported WebGL error zero and no browser errors.

## Portable boundary

The portable slice can draw camera-facing billboards with the recovered layout and texture order.

It can use the recovered front-face, depth-off, and alpha-blend state.

It can apply the WebGL-aligned cloud shader mapping from the recovered inputs.

The portable slice uses deterministic seed-one random state for repeatable previews.

It does not reproduce process-global random consumption from the original editor.

It does not claim exact native billboard placement or pixel parity.

The one-block portable constants do not reproduce native constant-buffer packing.

The unresolved `o4` vertex output consumer prevents a complete native shader contract.

The native pass state mapping comes from existing disassembly and state evidence.

The high-level field meanings and vector equations remain marked as interpretations where DXBC swizzles require them.
