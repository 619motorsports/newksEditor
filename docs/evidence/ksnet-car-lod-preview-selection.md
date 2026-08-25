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
`0.0125`, radius floor, initial PVS state, and explicit `NO_CULL` input. The
render planner does not enable this rule yet. Loader population and per-frame
submission remain unrecovered.

The predicate does not reinterpret the editor's LOD1–LOD4 menu as an
automatic distance selector.

The inspected evidence does not recover which loader populates each LOD file's
`lodIns`/`lodOuts`, how multiple car LOD files are submitted together, or the
initial values of those fields for a newly loaded model. Those remain staged
targets rather than assumptions.
