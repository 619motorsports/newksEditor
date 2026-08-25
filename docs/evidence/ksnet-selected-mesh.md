# Native selected-mesh evidence

The inspected files come from the installed Assetto Corsa SDK editor:

- `ksNet.dll` has SHA-256
  `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
- `ksNet.pdb` has SHA-256
  `0009d617e6d6840dcbf92a962f7b05eecd84e2626852a101ac869ccf0275b093`.
- `ksSelectedMesh_vs.fxo` has SHA-256
  `33dc991b380733d18ed033234c7e25b1e7bf8cdd3f757462df64b36740a3142f`.
- `ksSelectedMesh_ps.fxo` has SHA-256
  `977c8dfba9cd48be314b9861cee2c564d4e1b9a95f8507bd5af1adef28563e69`.

The shader metadata sets `ALPHATEST=0`, `SKINNED=0`, `PARTICLE=0`, and `2D=0`.

## Selected-mesh object

PDB type `0xF06A` identifies `SelectedMesh`. The type has size 204 and derives
from `Node`. It contains these fields:

- `material` at byte offset 188.
- `mesh` at byte offset 196.
- `selectionTime` at byte offset 200.

The two `setSelectedMesh` methods have tokens `0x060003D1` and `0x060003D2`.
Their RVAs are `0x21128` and `0x21158`. Each method writes one native mesh
pointer to the scene object at byte offset 196. It then calls
`SelectedMesh.touchTime`. This slot is separate from the selected-node axis.

## Color and fade

`SelectedMesh.render` has token `0x060000F6` and RVA `0x2D360`. Its loaded
address is `0x1002D36C`. The first draw creates a selected-mesh material and
sets `ksSelectedMeshColor` to RGBA `(1, 0, 1, 0.5)`.

Later draws calculate this value:

```text
fade = (GetTickCount() - selectionTime) * 0.0005
alpha = (1 - fade) * 0.5
```

The draw stops after `fade` becomes more than one. Thus, the alpha decreases
linearly from 0.5 to zero during 2000 ms.

## Draw contract

`SelectedMesh.render` does these operations in order:

1. It validates the mesh and its flags.
2. It calls `commitShaderChanges`.
3. It calls `setDepthMode(2)`.
4. It binds the recovered mesh index buffer.
5. It binds the recovered mesh vertex buffer when that buffer exists.
6. It calls `drawPrimitive`.
7. It calls `setDepthMode(0)`.
8. It calls `Node.render`.

Existing enum evidence identifies depth mode two as off and zero as normal.
The method calculates these counts:

```text
indexCount  = (*(mesh + 240) - *(mesh + 236)) >> 1
vertexCount = (*(mesh + 228) - *(mesh + 224)) / 44
```

The vertex stride is 44 bytes. The draw strongly indicates triangle-list
geometry, but the backend topology enum is not recovered completely.

## Shader ABI

DXBC reflection shows that the vertex shader uses `cbCamera` at `b0` and
`cbPerObject` at `b1`. `cbCamera` is 224 bytes. `cbPerObject` contains the
64-byte `ksWorld` matrix. The shader only reads the position input.

The vertex shader calculates this sequence:

```text
worldPosition = ksWorld * position
viewPosition = ksView * worldPosition
clipPosition = ksProjection * viewPosition
```

The pixel shader uses `cbSelectedMesh` at `b5`. This buffer contains one
16-byte `ksSelectedMeshColor` value. The complete pixel shader is:

```text
ps_4_0
dcl_constantBuffer cb5[1], immediateIndexed
dcl_output o0.xyzw
mov o0.xyzw, cb5[0].xyzw
ret
```

Thus, the original highlight is a separate flat-color mesh pass. It is not a
wireframe pass and is not the orange material tint in the WebGL preview.

## Material and render state

`Material::Material` at `0x1003fcc7` sets these default values:

- Opaque blend mode with blending and alpha-to-coverage disabled.
- Front-face culling.
- Solid fill.
- Normal depth mode.
- `doubleFace=false`.

`MaterialFilter::apply` at `0x10064fa2` only caches the last material and calls
`Material::apply`; it does not classify render passes. `Material::apply` at
`0x1004016e` applies the material blend and cull modes. It changes the cull mode
to none only when `doubleFace` is true. It applies the material depth mode only
when the mesh filter pass ID is opaque. The recovered pass IDs are opaque zero,
transparent one, and shadow generation two.

`SelectedMesh.render` does not change these blend or cull values. It changes
the depth mode to off before the draw. It restores normal depth after the draw.
Thus, the changing shader alpha is written to the target but does not blend
the selected RGB value with the scene color.

The exact packed fields of the native D3D11 rasterizer state remain unresolved.
No inspected selected-mesh code changes the default blend or cull values.

## Schedule and transform

`ksGraphics.render` updates world matrices before it starts the scene draw.
`Node::render` sets the current world matrix before it renders active children.
The selected object draws at its child position during this scene traversal.

`SceneGraph.{ctor}` has token `0x06000063` and RVA `0x24C38`. It adds these
children to the root in the listed order:

1. `SCENE_ROOT`
2. `SelectedMesh`
3. `SCENE_FINISHED`

`loadKN5` has token `0x060003A6` and RVA `0x28410`. It adds the model below
`SCENE_ROOT`. Thus, the hierarchy is:

```text
ROOT
|-- SCENE_ROOT
|   `-- model
|-- SelectedMesh
`-- SCENE_FINISHED
```

`Node::render` traverses the child vector in insertion order. `Camera::render`
uses an opaque mesh filter, so model packets draw before the selected mesh in
that path. `SelectedMesh.render` has no pass-ID branch. Its material callback
does not classify the pass. `CameraShadowMapped::renderPass` traverses its root
with opaque and transparent filters. The exact attachment that proves selected
participation in the second traversal remains unresolved.

The grid and the selected-node axis draw after the scene traversal and before
the final frame operation. Their exact order relative to a possible second
selected draw inherits the shadow-mapped attachment boundary above.

The installed DXBC cannot use the portable native draw-matrices ABI directly.
A faithful Vulkan and D3D12 port needs a dedicated selected-mesh contract.

## Portable backend mapping

The C++ port keeps opaque and transparent model draws, the selected draw, and
line overlays in one ordered batch. It places the selected draw before the
world-origin axis at the opaque-to-transparent boundary. The late grid and
selected-node axis follow transparent draws. It resolves multisample color only
after all these draws.

The one-draw selected placement is a labeled portable mapping. It does not claim
that native shadow-mapped transparent-pass participation is resolved.

The portable vertex contract uses the 128-byte `DrawMatrices` value. This
contract is a labeled translation of the original `b0` and `b1` split.

Vulkan keeps `DrawMatrices` in vertex push constants. It reads the selected
color from a 256-byte uniform view at set zero, binding zero.

D3D12 maps `DrawMatrices` to root constants at `b0`. It maps the selected
color view to the recovered pixel constant-buffer register `b5`.

The viewport accepts explicit shader modules for this contract. It does not
infer the contract from opaque shader bytes.
