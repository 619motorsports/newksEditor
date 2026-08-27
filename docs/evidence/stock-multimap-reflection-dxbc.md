# Stock MultiMap reflection DXBC evidence

## Evidence source

The source is the installed Assetto Corsa SDK editor at this path:

`/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor`

The matching `ksNet.dll` SHA-256 value is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.

The four inspected packages and their SHA-256 values are:

| Package | Package SHA-256 |
|---|---|
| `ksPerPixelMultiMap.shader` | `76ea497f98506db7fbd332bd641f576058ab0afdb65e9721ffe1ea5a0163d698` |
| `ksPerPixelMultiMap_AT.shader` | `f501f2cb3194c20161cfc9e6f35bf3d03b4765220f96b9e3534fe3efa9bdeb8c` |
| `ksPerPixelMultiMap_NMDetail.shader` | `d4929337f453eb2d76c14cc5fd7cac4d8b3411524cb50346117389ef87cff99d` |
| `ksPerPixelMultiMap_AT_NMDetail.shader` | `a284bec37701e62b6edfb4dc1c259674594654cdb3c91669dc51e4ab2cab571c` |

The package parser extracted each bounded pixel stage. The repository tool
`tools/dxbc-disasm.c` called `D3DDisassemble` through Wine and D3DCompiler.

The installed split pixel files have these SHA-256 values:

| Pixel file | Pixel DXBC SHA-256 |
|---|---|
| `ksPerPixelMultiMap_ps.fxo` | `b5cc236cef21d3d01f7a80510e64b7a9a21bf5727c0f5a0b93b6e2fdf1f18a58` |
| `ksPerPixelMultiMap_AT_ps.fxo` | `0f354b59932a913f9e5e9b9ccbfda4fdeca15d271d07e5e4522f99821c72e8ab` |
| `ksPerPixelMultiMap_NMDetail_ps.fxo` | `2e45a59c20284802cda77ae02488d0f44aaa8955492767e75dcd0c29b7631222` |
| `ksPerPixelMultiMap_AT_NMDetail_ps.fxo` | `51fd6d6c1bc4f18c1d978ddde362d09391fd719fac80ca9b347fdb95e743e4d6` |

## Recovered mip equation

All four pixel programs use the same reflection mip structure. First, the
program multiplies the maps exponent by `ksSpecularEXP`. The result is the
effective exponent `e`.

The branch value `isAdditive` selects the divisor:

```text
isAdditive == 2: d = 8
all other values: d = 255
mip = 6 * saturate(1 - e / d)
```

The base program has this instruction chain for `isAdditive == 1`:

```text
mul     r0.x, r3.y, cb4[0].w
mad_sat r0.x, -r0.x, l(3.92156886e-03), l(1.00000000e+00)
mul     r0.x, r0.x, l(6.00000000e+00)
sample_l ..., r0.x
```

The `3.92156886e-03` constant is `1 / 255`. The special branch replaces this
constant with `1.25000000e-01`, which is `1 / 8`.

The AT and NMDetail programs contain the same `mad_sat` and `mul` pair. No
program has another multiply that squares the saturated value.

The WebGL source uses a squared mip expression. That expression is a portable
WebGL approximation. It is not the mip equation in the installed DXBC.

## Recovered reflection scope

The inspected instructions support these claims for the four package variants:

- The program normalizes the surface normal and the camera-to-surface vector.
- The program computes the reflected direction and negates its X coordinate.
- The program computes the Fresnel value from `fresnelC`, `fresnelEXP`, and
  `fresnelMaxLevel`.
- Branch `1` adds the scaled cube value to the lit value.
- Branch `2` and the default branch interpolate between the lit and cube values.
- The non-AT NMDetail program uses a bias sample at mip values not more than
  `0.5`. It changes the X orientation for explicit-level samples above `0.5`.
- The AT programs preserve the material alpha. The non-AT base additive branch
  writes alpha `2`. The other non-AT branches write alpha `1`.

The portable GLSL and HLSL fixtures execute only the shared base reflection
arithmetic. They do not execute the full installed shader register contract.
They also do not prove instruction-stream identity with the installed DXBC.

## Test boundary

`stock_multimap_reflection_tests` supplies fixed vectors for the recovered
direction, mip, Fresnel, blend, and alpha results. The Vulkan and D3D12 cube
tests compare their portable shader results with this bounded CPU oracle.

These tests establish source-equivalent execution for the tested reflection
arithmetic. They do not establish full pixel parity for the four stock shaders.
