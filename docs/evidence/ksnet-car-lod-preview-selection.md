# ksNet car LOD preview selection

Status: native distance/FOV LOD culling and the editor's explicit LOD menu
behavior are recovered. No code change is made by this note.

## Binary evidence

- `ksNet.dll`: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksNet.dll`
- SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- `ksEditor.exe`: `/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/ksEditor.exe`
- SHA-256: `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`
- Matching PDBs are installed beside both binaries.

The native addresses below use the loaded ksNet image base `0x10000000`.

## Editor LOD menu behavior

The managed `Form1` handlers are:

- `loadLod1ToolStripMenuItem_Click`, RVA `0x7328`
- `loadLod2ToolStripMenuItem_Click`, RVA `0x7374`
- `loadLod3ToolStripMenuItem_Click`, RVA `0x73c0`
- `loadLod4ToolStripMenuItem_Click`, RVA `0x740c`

Each handler first returns when `carProjectModel` is null. Otherwise it reads
`carProjectModel.ProjectPathFilename`, indexes `carProjectModel.Lods` with
`0`, `1`, `2`, or `3`, reads that entry's `FBXName`, combines the directory
with the name using `System.IO.Path.Combine`, and calls `Form1.loadFBXFile`
with the resulting path and literal `false`. These handlers select and load a
specific asset on demand. They do not set a runtime LOD index, camera value,
or distance threshold.

The list index is not checked before `get_Item`; a project with fewer entries
can raise the normal managed list exception. This is an editor UI boundary,
not native render selection. No automatic LOD-menu selection or startup LOD
index is present in these handlers.

## Native distance/FOV rule

The PDB names `PvsProcessor::doDistanceAndLod` at segment address decimal
`411894`, virtual address `0x100658f6`, code size 321. PDB type information
identifies the exact arrays in `PvsProcessor`:

| field | type/offset |
| --- | --- |
| `meshCount` | `int`, `+16` |
| `flags` | 4096 `uint16`, `+16404` |
| `bounds` | 4096 `sphere`, `+40980`; `sphere` is `{vec3 center; float radius}` |
| `lodIns` | 4096 `float`, `+106516` |
| `lodOuts` | 4096 `float`, `+122900` |
| `inPvs` | 4096 `bool`, `+139284` |

`RenderContext::camera` is at `+12`. PDB type information identifies
`Camera::fov` at camera `+4` and `Camera::matrix` at camera `+8`. The native
function performs this bounded loop for `i = 0 .. meshCount - 1`:

1. Process only entries with `inPvs[i] != false` and without the
   `MESH_FLAG_NO_CULL` bit (`0x10`) in `flags[i]`.
   Skip distance culling when both `lodIns[i]` and `lodOuts[i]` are zero.
2. Read the camera FOV and compute `s = clamp(camera.fov * 0.0125f, 0, 1)`.
   The binary constant at `0x10115628` is the exact float `0.0125`.
3. Read camera position from the translation components of the camera matrix
   and subtract the LOD sphere center. The squared distance is multiplied by
   `s * s`.
4. Let `near = lodIns[i]` and `far = max(lodOuts[i], bounds[i].radius)`.
   Set `inPvs[i] = false` when `scaledDistanceSquared < near * near` or when
   `far * far < scaledDistanceSquared`. Otherwise retain the entry.

The strict comparisons make the boundaries inclusive: equality at the near
or effective far boundary is retained. The sphere radius prevents the
effective far limit from being less than the object's bound radius. The
function does not choose a discrete “LOD number”; it culls each submitted
mesh entry according to its own interval. A car's separate LOD files must
therefore be submitted as separate model content before this native rule can
select among them.

## Per-frame reset/cache behavior

PDB `PvsProcessor::begin` is at `0x10065859`; it stores the current
`RenderContext`, render mode, shadow-map vector, and cube-map pointer. The
PDB-named `PvsProcessor::end` is at `0x10065f2f`; its first calls are the
exclusion pass, `doDistanceAndLod`, and frustum-culling pass, then it renders
entries whose `inPvs` byte remains set. `doDistanceAndLod` has no previous
camera/distance comparison, hysteresis, or last-selected-LOD cache: it
re-evaluates every submitted mesh on each end pass and only mutates the
`inPvs` byte to false when outside the interval.

The PvsProcessor constructor is at `0x1004496c`. It initializes the context
and counters to zero and clears the 4096 sphere records; no default LOD
selection is established there. The first usable intervals and `inPvs` values
come from mesh submission/loading, which is outside this isolated culling
function.

## Integration boundary and unknowns

The native port now exposes a bounded per-mesh predicate as
`ksnet_mesh_lod_visible()`. It preserves the strict comparisons, FOV scale
`0.0125`, radius floor, zero-limit bypass, initial PVS state, and explicit
`NO_CULL` input.

The render planner now has an opt-in `KsNetMeshLodOptions` mode. Each caller
must supply the camera FOV. The mode accepts default and per-node PVS and
`NO_CULL` state. It uses each node's world-space center and radius. It also
uses effective LOD limits after CSP node-state overrides.

The stock-scene facade rejects invalid state IDs, duplicate state IDs, and
non-finite inputs. It also rejects LOD overrides that do not fit in a float.
Its preflight budget includes the optional per-node state array. The planner
saturates finite distances that exceed the float range. It rejects non-finite
distances before it applies the opt-in predicate.
The default planner path still uses the WebGL-compatible distance rule.

Further disassembly identifies `CameraMeshFilter::isVisible` at `0x10064C8C`
as the active installed runtime path. `Mesh::render` at `0x100494FF` and
`SkinnedMesh::render` at `0x1004A87E` call this filter before direct drawing.
They do not submit array entries to `PvsProcessor`.

The filter first applies the maximum layer, render pass, shadow-caster,
transparency, and visibility rules. `NO_CULL` or a null camera then accepts the
mesh. Otherwise, the filter transforms a non-static bounding sphere by the
supplied world matrix. It keeps a static sphere unchanged. It applies the
recovered distance rule and then tests the camera frustum.

The active path reads `Renderable` fields directly. These fields are
`castShadows +188`, `isVisible +189`, `isTransparent +190`, `noCull +191`,
`boundingSphere +192`, `layer +208`, `lodIN +212`, `lodOUT +216`, and
`isStatic +220`. Thus, PVS array population is not an integration dependency.

`Renderable::Renderable` at `0x1004C229` sets `noCull` and `isStatic` to false.
The KN5 loader does not replace either value. Ordinary KN5 meshes use the
dynamic-sphere path.

`sphere::transform` at `0x10064EB6` transforms the sphere center with the world
matrix. `mat44f::getScale` at `0x100438A3` gets the three matrix basis lengths.
If any length is exactly 1, the function keeps the source radius. Otherwise,
the function multiplies the radius by the largest length.

`BoundingFrustum::setMatrix` at `0x10063FA1` creates six normalized planes.
The plane order is near, far, left, right, top, and bottom. For the native
zero-to-one depth convention, the raw row-vector planes are:

```text
near   = (-M13,      -M23,      -M33,      -M43)
far    = ( M13-M14,   M23-M24,   M33-M34,   M43-M44)
left   = (-M14-M11,  -M24-M21,  -M34-M31,  -M44-M41)
right  = ( M11-M14,   M21-M24,   M31-M34,   M41-M44)
top    = ( M12-M14,   M22-M24,   M32-M34,   M42-M44)
bottom = (-M14-M12,  -M24-M22,  -M34-M32,  -M44-M42)
```

If a plane normal has zero length, the native fallback is `(0, 1, 0, 0)`.
`BoundingFrustum::intersect` at `0x10063F57` rejects a sphere only when the
signed plane distance is more than its radius. Therefore, tangency is visible.

The port now retains each KN5 local sphere with its source. Static meshes use
the serialized KN5 sphere. Skinned meshes use the recovered vertex-mean and
maximum-distance rule from `SkinnedMesh::updateBoundingSphere` at `0x1004A97E`.

The backend-neutral `camera_mesh_filter_visible()` function implements this
active path. It supports the port's column-major matrices and all three clip
conventions. It rejects non-finite inputs that the selected path consumes.
The native comparisons permit NaN values. A native bypass does not consume
the bypassed fields.

The native viewport now uses this function. Preparation retains packets across
their authored mesh LOD intervals. It also copies one filter record for each
packet with recovered KN5 bounds.

Each frame uses the current camera position, FOV, view-projection matrix, and
clip convention. A refreshed packet supplies its current world matrix. The
viewport does not rebuild graphics resources after camera movement.

The mesh mask combines with the workspace-file LOD mask by logical AND. An
explicit caller mask stays authoritative and bypasses both automatic masks.
The final automatic mask applies to color and directional-shadow submission.

Packets without recovered KN5 bounds stay visible. A fractional CSP layer also
uses this conservative fallback because the native field is an integer. These
fallbacks are explicit and do not claim native culling parity.

The production native window enables the live filter. The WebGL renderer keeps
its existing visibility path.

The production native window also enables WebGL-compatible live color order.
KN5 conversion retains each local vertex-AABB center separately from the native
culling sphere. Each frame transforms these centers and sorts packet indices.

The sort keeps opaque packets first and uses ascending layers. It sorts
transparent packets by descending squared camera distance. Stable ties keep the
prepared traversal order.

This sort retains the existing WebGL feature. It does not reproduce the
original editor's transparent order. The original Classic pass uses scene
traversal order, as recorded in `ksnet-transparent-pass-order.md`.

Two active-path gaps remain. Runtime mutation of `noCull` and `isStatic` is not
connected. The existing render plan also removes invisible shadow casters before
the native shadow-pass visibility difference can apply.

The separate PVS-array option implements only the recovered array distance
stage. It does not implement the active `CameraMeshFilter` path. The production
viewport uses the complete live filter described above.

`Camera::Camera` at `0x1006421E` sets the native default FOV to 60 degrees.
The port's workspace cameras commonly start at 45 degrees. For this reason,
the opt-in mode has no implicit FOV value.

The predicate does not reinterpret the editor's LOD1–LOD4 menu as an
automatic distance selector.

The inspected evidence does not recover which loader populates each LOD file's
`lodIns`/`lodOuts`, how multiple car LOD files are submitted together, or the
initial values of those fields for a newly loaded model. Those remain staged
targets rather than assumptions.

## Submission ABI and inactive processor path

The PDB gives the public submission signature, but it does not give an emitted
body. Type `0x1549F` is a `thiscall void PvsProcessor::submitMesh(Mesh*)`
method. The `PvsProcessor` field-list type is `0x154A6`. The argument-list type
is `0x5CDC`.

The field list identifies the submission arrays. It also gives the bounded
capacity, `MAX_MESH_ENTRIES = 4096`. This signature is the exact ABI target for
a future loader or render-traversal port. It does not make an unchecked
`Mesh*` safe.

The PDB module record is `Release\\PvsProcessor.obj`. Its emitted records include
`begin`, `doDistanceAndLod`, `doExclusion`, and `doFrustumCulling`. They also
include `doRenderCalls`, `doRenderClassic`, `end`, and the draw-call helpers.
They contain no `S_GPROC32` or `S_THUNK32` record for `submitMesh`.

The installed `ksNet.dll` does not expose an address for this method. A direct
`.text` search found constructor clearing at `0x1004496c`. It also found reads
in the named culling and render methods. It found no write-side submission
body.

The per-frame call envelope is exact. PDB symbol
`CameraShadowMapped::renderPass` starts at `0x1005e681`. Disassembly calls
`PvsProcessor::begin` at `0x1005e782`. It then traverses the scene through the
callback at `0x100618af`. Other passes use a root virtual `render` call at
`0x1005e7e2` or `0x1005e89a`.

Each pass calls `PvsProcessor::end`. The call is at `0x1005e7a2`, `0x1005e7f1`,
or `0x1005e8a9`. `Node::render` at `0x1003f5dc` visits only active children.
`Mesh::render` at `0x100494ff` dispatches `IMeshRenderFilter::isVisible` and the
material filter. Neither function contains a named `submitMesh` call.

Root traversal occurs inside each `begin` and `end` envelope. However,
`Mesh::render` and `SkinnedMesh::render` use the direct filter and draw path.
They do not write `meshPtrs`, `flags`, `layers`, `bounds`, `lodIns`, `lodOuts`,
or `inPvs`. The processor arrays are inactive in this installed path. The port
must use the recovered direct filter contract instead of inferring array data.

## Editor preview resource lifetime

The managed editor prepares one car LOD file per preview. Each of the four LOD
menu handlers builds one FBX path. The handler then calls
`Form1.loadFBXFile(path, false)`. This method is at RVA `0x48d0` in
`ksEditor.exe`. It creates a new `FormProgress` and calls `Editor.loadFBX`
once. Then it closes the progress form. This path has no loop over
`carProjectModel.Lods`. It also has no runtime distance selection.

`Editor` constructs one `ksGraphics` instance and keeps it in a field. Its
constructor is at RVA `0x2c12`. `Editor.loadFBX` is at RVA `0x315c`. The
editor reuses this wrapper across menu loads. This reuse does not prove reuse
of the loaded model.

`ksNet.ksGraphics.loadFBX` is at RVA `0x25830`. It allocates a new
`0xd4`-byte native `Model` at IL offset `0x17`. It runs `Model.{ctor}` at
`0x46` and calls `Model.load` at `0xb9`. Then it calls
`GraphicsManager.compile` at `0x10a`. An indirect call at `0x15e` gives the
model to the persistent `sceneGraph`. The method then calls `initLayers` and
`reorderNodes`.

The installed PDB names three native model routines. `Model::Model` is at
`0x100621cb`. `Model::load` is at `0x1006225f`. `Model::~Model` is at
`0x10062223`, with the ksNet image base at `0x10000000`. `ksGraphics` creates
the `sceneGraph` once in `initSceneGraph`. This method is at RVA `0x25490`.
The constructor at RVA `0x26824` calls it. `loadFBX` does not create a new
wrapper graph.

Thus, an original-editor LOD selection crosses a load boundary. The editor
imports, prepares, and compiles one file again. Then it gives the model to the
scene graph. The original preview does not keep all LODs ready for a frame-time
visibility switch.

The evidence does not identify the indirect scene-graph method at IL `0x15e`.
It also does not prove when the previous root is replaced or destroyed.
`ksGraphics.loadFBX` contains no explicit call to `Model::~Model`. Thus, the
native ownership and reclamation point remain unresolved.

The Apex stable packet catalog is a port optimization for live distance
selection. It is not a claim of exact original-editor resource behavior.
