# Installed stock shader discovery

## Scope

This note records the recovered shader discovery path in the original Assetto Corsa editor.

The analysis used this installed binary:

- File: `sdk/editor/ksNet.dll`
- SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`

All addresses are virtual addresses in the analyzed 32-bit image.

## Discovery path

`GraphicsManager::initShaders` at `0x100456e9` enumerates `system/shaders/*.shader` below the process directory.

The function keeps the file-system case and enumeration order. It removes the `.shader` suffix to make the shader key.

The native runtime then requests two files for this key:

- `system/shaders/win/<key>_vs.fxo`
- `system/shaders/win/<key>_ps.fxo`

`ShaderManager::getShader` at `0x1004acb1` loads these stage files. The function does not use an alternate root or case normalization.

If a stage file is absent, the load function returns a null pointer. `GraphicsManager::getShader` at `0x100450db` then calls `exit(1)`.

A missing `_meta.ini` file writes a diagnostic. The runtime continues after this condition.

## Material path

The material path uses this call chain:

1. `KN5IO::loadMaterialsBinary` at `0x1003b9de` reads the material shader key.
2. `Material::setShader` at `0x100407fc` applies the shader key.
3. `GraphicsManager::getShader` resolves the installed stage files.

## Port boundary

The native `.shader` files are discovery markers. The original runtime executes the separate `_vs.fxo` and `_ps.fxo` stage files.

The C++ port also validates executable stages inside a bounded `.shader` container. This package seam is specific to the port.

Do not describe the port package seam as the exact native discovery path.
