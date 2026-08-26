# Stock shader container extraction evidence

## Installed source

The canonical source is the installed Assetto Corsa SDK editor at
`sdk/editor/system/shaders`. The directory contains 81 `.shader` names.
Seventy-nine files are nonempty version-2 packages. These two files are empty:

- `ksPerPixelMultiMap_emissive.shader`
- `stPerPixelNM_UVflow.shader`

The matching `ksNet.dll` SHA-256 value is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
The matching `ksNet.pdb` SHA-256 value is
`0009d617e6d6840dcbf92a962f7b05eecd84e2626852a101ac869ccf0275b093`.

The duplicate `Assetto Corsa Editor/system/shaders` directory has the same
hashes. Older editor copies use a different envelope and are not evidence for
this parser.

## Native package and runtime paths

`GraphicsManager::initShaders` at `0x100456E9` enumerates
`system/shaders/*.shader`. It removes each extension and calls
`ShaderManager::getShader` at `0x1004ACB1`.

The native D3D11 runtime does not create shaders from the package bytes.
`ShaderManager::getShader` builds a path below `system/shaders/win/`. It checks
the `<name>_vs.fxo` and `<name>_ps.fxo` files.

`KGLShader::loadShaderBinary` at `0x1000F5C0` reads the split `.fxo` files and
the matching `_meta.ini` file. `KGLShader::reflectVars` at `0x1000FAB0` uses
`D3DReflect` to enumerate buffers, variables, and textures.

`KGLShader::createVertexShader` at `0x1000FA30` calls the D3D11 vertex-shader
creation function. The pixel path calls the D3D11 pixel-shader creation
function from `loadShaderBinary`.

Thus, the package is an enumeration and identity manifest for the installed
runtime. The package still contains complete DXBC programs that are suitable
for bounded extraction and inspection.

## Version-2 envelope

The recovered package layout is:

```text
offset       size  field
0            1     version (2)
1            1     isAlphaTested (0 or 1)
2            4     vertex-layout code, little-endian
6            4     vertex DXBC byte count, little-endian
10           N     vertex DXBC payload
10+N         4     pixel DXBC byte count, little-endian
14+N         M     pixel DXBC payload
14+N+M       4     geometry DXBC byte count, little-endian
18+N+M       G     optional geometry DXBC payload
```

The layout codes are mesh 0, skinned 1, particle 2, and 2D 3. All 79
nonempty installed packages end after the geometry payload. None has a
geometry program.

Each embedded program is a complete DXBC container. Its internal total size
equals the package stage size. Its chunk table contains one `SHDR` or `SHEX`
program chunk. The program token identifies its vertex, pixel, or geometry
stage.

## Bounded C++ boundary

`parse_stock_shader_container()` limits the package size, each stage size, and
the DXBC chunk count. It checks every offset before it reads a field. It rejects
overlapping chunks and stage-type mismatches. It copies each accepted stage to
owned storage only after all checks pass.

The malformed-input test covers every truncated prefix. It also covers invalid
flags, trailing bytes, size mismatches, excessive chunk counts, invalid chunk
offsets, missing program chunks, overlaps, and wrong stage types.

The installed-fixture test reads packages only when `ASSETTO_CORSA_ROOT` is
available. It validates 79 packages and counts the two empty placeholders.
The repository does not redistribute these binaries.

## `ksPerPixel` production candidate

`ksPerPixel` and `ksPerPixelAT` are the smallest high-value car-material
family. They share one 3,728-byte vertex payload with SHA-256
`55451102bc40c0b7cca57bdc4e6e51d12456c85e18b59eb69ef79196f82700a5`.

| Package | Size | Alpha flag | Pixel bytes | Package SHA-256 |
|---|---:|---:|---:|---|
| `ksPerPixel.shader` | 11,254 | 0 | 7,508 | `255d0228faa70d5b8454a2abe618447bef16ef29612ffab4a0d684dbedcfdb0b` |
| `ksPerPixelAT.shader` | 11,330 | 1 | 7,584 | `3e3ea224b80fadacb8070fe026e29626f7c60e991e6290902fb2341be27ba379` |

DXBC reflection identifies this native register contract:

- Vertex buffers: `cbCamera b0`, `cbPerObject b1`, `cbLighting b2`, and
  `cbShadowMaps b3`.
- Pixel buffers: `cbLighting b2`, `cbShadowMaps b3`, and `cbMaterial b4`.
- Textures: `txDiffuse t0` and three shadow maps at `t6` through `t8`.
- Samplers: a linear sampler at `s0` and a comparison sampler at `s1`.

Both `ksPerPixel` stages identify themselves as Shader Model 4.0. The current
D3D12 path uses another root-register contract. The Vulkan path accepts SPIR-V,
not DXBC. Extraction is exact, but stock execution remains staged.

## Required execution evidence

A future D3D adapter must prove support for the shader model and the complete
native register contract. It must pass a Windows WARP pipeline and readback
test before the port claims execution.

A future Vulkan adapter must use explicit SPIR-V. Any offline DXBC translation
must record its translator and source hash. Documentation must label that
output as translated or approximate until cross-backend pixel parity exists.

Any visible rendering change also requires the production WebGL check from the
repository guidance.
