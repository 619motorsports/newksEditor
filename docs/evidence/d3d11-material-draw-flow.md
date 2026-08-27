# Native editor material and draw flow

This note records D3D11 behavior from the installed Assetto Corsa editor. It does not claim exact D3D12 or Vulkan behavior.

## Evidence boundary

The addresses use the loaded base address from the Ghidra project. The installed `ksNet.dll` and its PDB remain the primary evidence.

The recovered flow defines ownership and update requirements for the port. Backend descriptor and command models remain explicit translations.

## Shader selection

`KN5IO::loadMaterialsBinary` at `0x1003b9de` reads the shader name from each material record. It passes that name to `Material::setShader`.

`Material::setShader` at `0x100407fc` gets the named shader from `GraphicsManager`. A changed shader causes two operations:

- `Material::createCBuffers` at `0x1004026a` creates the reflected constant buffers.
- `Material::initShaderVars` at `0x10040420` rebuilds the material variables and resources.

The resource rebuild retains a texture when the old and new resources have the same name or slot. Other resources receive their reflected defaults.

If the shader has the alpha-tested flag, `Material::initShaderVars` selects alpha-to-coverage. Thus, `ksPerPixelAT` selects both bytecode and fixed state.

## Material application order

`Material::apply` at `0x1004016e` uses this order:

1. It selects the blend state.
2. It selects the cull state.
3. It selects the depth state for the opaque pass.
4. It disables culling for a double-sided material.
5. It binds each material texture resource.
6. It selects the shader.
7. It commits each material constant buffer.

`Material::setTexture` at `0x1004082b` finds a resource by name. It stores the texture handle and its file name.

`MaterialVar::set` at `0x1003e10c` selects storage from the reflected type. `ShaderVariable::set` at `0x1004985d` marks the constant buffer dirty.

## Shader binding

`Shader::apply` at `0x1004b11b` calls `kglSetShader` with the shader handle. The handle contains these objects:

| Offset | D3D11 object |
| ---: | --- |
| `0` | Vertex shader |
| `4` | Pixel shader |
| `8` | Input layout |

`kglSetShader` at `0x1000d330` binds the input layout only when it changes. It then binds both shader stages.

`GraphicsManager::setShader` at `0x10046811` skips the call when the `Shader` pointer does not change.

## Direct mesh draws

`Mesh::render` at `0x100494ff` rejects missing geometry and filtered meshes. It then binds geometry, commits shader changes, and draws the indexed mesh.

`SkinnedMesh::render` at `0x1004a87e` requires at least three indices. It updates the bone buffer before it draws the indexed mesh.

`GraphicsManager::drawPrimitive` at `0x10044ffc` ignores its vertex-count argument. It sends only the index count, start index, and base vertex to `kglDrawIndexed`.

The recovered direct draw has these arguments:

| Argument | Value |
| --- | ---: |
| Index count | Mesh index count |
| Start index | `0` |
| Base vertex | `0` |

## PVS draw traversal

`PvsProcessor::doRenderCalls` at `0x10065b85` sorts 100-byte draw records before traversal. The function commits shared camera, lighting, and shadow buffers first.

The function binds the three shadow maps at `t6`, `t7`, and `t8`. It binds the cube map at `t10`.

During traversal, the function changes an object only when its draw key changes. This rule applies to these objects:

- The shader.
- The material and its resources.
- The blend, cull, and depth states.
- The vertex and index buffers.

Material resource binding skips slots that contain external shadow maps. It also skips `t10` when a cube map exists.

If a material texture pointer is null, the function binds a null texture. It does not retain the previous material texture.

If a draw record has a world matrix, the function writes 64 bytes to `cbPerObject` at offset `0`. It commits that buffer immediately.

If the record has no world matrix, the function does not update `cbPerObject`. The previous GPU buffer value stays bound.

Each draw calls `kglDrawIndexed` with the index count, start index `0`, and base vertex `0`.

## Port requirements

The native batch port must retain the selected shader, material resources, and constant buffers for the complete command submission.

The port must update `cbPerObject` only when the recovered draw record contains a world matrix. A default matrix would change recovered behavior.

The port can cache bindings by identity. It must bind explicit null textures when the recovered material resource is null.

The Vulkan path needs an explicit source-equivalent shader. The installed DXBC and this D3D11 flow do not prove Vulkan shader equations.
