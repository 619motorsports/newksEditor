# Native view-axis evidence

This note records the installed-binary evidence for the editor's world-origin axis.
The binaries examined were:

- `ksEditor.exe`: SHA-256 `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`
- `ksNet.dll`: SHA-256 `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`

## Managed menu and camera mode

`ksEditor.Form1.viewAxisMenuItem_Click`, token `0x060000a4`, RVA `0x60e4`,
toggles the menu check and calls `ksGraphics.setRenderAxisMode` with `3` when
checked and `0` when unchecked:

```text
IL_0018: callvirt ToolStripMenuItem.get_Checked
IL_001d: ldc.i4.0
IL_001e: ceq
IL_0020: callvirt ToolStripMenuItem.set_Checked
...
IL_002b: callvirt ToolStripMenuItem.get_Checked
IL_0030: brfalse.s IL_0044
IL_003d: ldc.i4.3
IL_003e: callvirt ksGraphics.setRenderAxisMode
...
IL_004f: ldc.i4.0
IL_0050: callvirt ksGraphics.setRenderAxisMode
```

`ksNet.ksGraphics.setRenderAxisMode`, token `0x060003d3`, RVA `0x20cc8`, does
not read its argument. It reads the camera field at byte offset `108` (`0x6c`)
and toggles it between `2` and `0`:

```text
IL_000d: ldfld camera1
IL_0012: ldc.i4.s 108
IL_0014: add
IL_0016: ldind.i4
IL_0017: ldc.i4.2
IL_0018: beq.s IL_001d
IL_001a: ldc.i4.2
IL_001b: br.s IL_001e
IL_001d: ldc.i4.0
IL_001e: stind.i4
```

PDB type information names these values `eAxisAfter3d` (`2`) and `eAxisNone`
(`0`).

## Native draw order and geometry

PDB-guided Ghidra decompilation identifies `CameraShadowMapped::renderPass` at
`0x1005e681`. The function renders the opaque pass, calls
`Camera::renderAxis`, then constructs and renders the transparent pass:

```text
PvsProcessor::begin(..., Default, ...);
param_1->render(...);                    // opaque
PvsProcessor::end(...);
Camera::renderAxis((Camera *)this, ...);
...
CameraMeshFilter(..., Transparent, ...);
GraphicsManager::setDepthMode(..., eDepthNoWrite);
PvsProcessor::begin(..., Classic, ...);
param_1->render(...);                    // transparent
```

`Camera::renderAxis` at `0x10064788` selects the immediate line path for
`eAxisAfter3d` or `eAxisBefore3d`. It installs an identity world matrix and,
for the ordinary non-preserving call, disables depth before drawing and restores
normal depth afterward:

```text
GraphicsManager::setDepthMode(this->graphics, eDepthOff);
GraphicsManager::setWorldMatrix(this->graphics, mat44f::createIdentity(...));
if (axisRenderingMode == eAxisAfter3d || axisRenderingMode == eAxisBefore3d) {
  color4f(3, 0, 0, 1); vertex3f(0, 0, 0); vertex3f(1, 0, 0);
  color4f(0, 3, 0, 1); vertex3f(0, 0, 0); vertex3f(0, 1, 0);
  color4f(0, 0, 3, 1); vertex3f(0, 0, 0); vertex3f(0, 0, 1);
}
GraphicsManager::setDepthMode(this->graphics, eDepthNormal);
```

Therefore, the editor menu controls a one-meter, world-origin `+X/+Y/+Z` line
axis drawn after opaque geometry and before transparent geometry, with depth off
and the immediate RGB colors scaled to `3`.

## Portable backend mapping

The C++ port stores the six recovered vertices in one immutable buffer. The
portable line pipeline uses an identity world matrix, the frame view-projection
matrix, and disabled depth tests and writes.

The backend-neutral batch assigns the view axis to the first transparent scene
position. The port's single selected-mesh draw at that same position runs
first. Vulkan and D3D12 then use this order:

```text
opaque scene
selected mesh
world-origin view axis
transparent scene
late grid and selected-node axis
MSAA resolve
```

The scene-position and resource limits are validated before backend work. The
Linux SwiftShader fixture checks the ordering at 1x and 4x MSAA. The D3D12 code
uses the same neutral visitor, but its pixel result still requires Windows WARP
verification.

The view-axis boundary in this mapping is recovered. The one-draw selected-mesh
placement is a portable choice because native shadow-mapped transparent-pass
participation remains unresolved.
