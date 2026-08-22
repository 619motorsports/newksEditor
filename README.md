# Apex Editor

Apex Editor is an early cross-platform replacement for Assetto Corsa's Windows-only
ksEditor. It reads KN5 files and imports FBX source scenes directly in the browser.
It preserves the scene hierarchy and material model. It renders static and skinned
car and track meshes with a WebGL 2 preview.

## Run

Install Node.js 20 or newer. Then install the locked dependencies.

```sh
npm ci
```

Start the browser version.

```sh
npm start
```

Open <http://127.0.0.1:4173>, then choose one or more `.kn5` or `.fbx` files.
You can also open a CSP `.ini` configuration. Use **Open asset folder** to discover track `models*.ini`
layouts or a car's unpacked `data/lods.ini` or packed `data.acd`. Apex assembles static track models with
their positions and rotations, or the car's contiguous LOD set with its exact distance
bands. The car preview can follow camera distance or force an individual LOD. Use
**Open showroom** to add a separate showroom/environment KN5; its resources remain
file-scoped and its subtree becomes the automatic live-reflection source. The reflection
selector can instead supply any single named scene subtree, matching ksEditor.
Open the **Model** inspector to edit the active workspace manifest. For a track, edit each static model position and rotation.
For a car, edit each LOD file name and range, or the cockpit or driver distance switch. Press Enter to save a field.
The preview, undo history, recovery data, and project file use the same values. Select **Export models.ini** or **Export lods.ini** to download the manifest.
Discovered
`skins/<name>` folders can replace matching KN5 texture basenames just as they do in
the game. Select a skin and open the **Model** inspector to edit its `ui_skin.json`
name, driver, country, team, number, and priority. Skin metadata edits use the same
undo, recovery, and project history as other edits. Export writes a separate
`ui_skin.json` file and retains safe unknown source fields. Discovered `.ksanim`
files can be sampled on a normalized timeline with
their matching KN5 hierarchy transforms applied live. Imported FBX clips use the
same timeline. You can export a selected clip as a KSANIM v2 file. The same asset folder
resolves case-insensitive external CSP texture paths and reports missing or ambiguous
files. For FBX import, select the source folder before or after you open the FBX.
Apex embeds each resolved supported diffuse texture in the exported KN5. Files stay
on the local machine. The app does not upload them.

Start the desktop version during development.

```sh
npm run desktop
```

Create a package for the current operating system.

```sh
npm run dist:linux
npm run dist:windows
npm run dist:mac
```

The build writes packages to `release/`. Windows and macOS packages are not signed.
Run their package commands on their target operating systems before distribution.

```sh
npm test
```

## Native C++ port

The C++20 port is being developed alongside the feature-complete WebGL
implementation. It uses a backend-neutral render device with Vulkan and
Windows Direct3D 12 implementations. See [the native build guide](native/README.md)
and [the feature-parity roadmap](docs/CPP_PORT.md). The WebGL path remains the
reference until every documented parity and production-rendering gate passes.
The native command can now load current project files and export primary or
secondary authoring output through an in-memory C++ application service. The
secondary exports include collider KN5, `damage.ini`, `colliders.ini`, and
`surfaces.ini` output.

## Current scope

- Binary or ASCII FBX import through the portable Three.js loader. The importer
  triangulates geometry and splits meshes by material. It preserves node transforms,
  original object names, UV seams, normals, skin weights, and bone inverse-bind matrices.
  It flips the UV V coordinate to match ksEditor. It also limits each KN5 mesh to
  65,535 vertices. The importer preserves supported embedded PNG, JPEG, and WebP
  textures. It also resolves external DDS, PNG, JPEG, and WebP textures.
  A relative path, suffix, or basename must resolve to one file. Missing, ambiguous,
  and unsupported diffuse textures use an embedded one-pixel DDS color. Static normal
  maps use `ksPerPixelNM` and `txNormal`. Missing normal maps use an embedded flat-normal
  DDS. The inspector reports maps that have no safe stock KN5 binding. Each FBX
  animation clip is sampled into 100 local-transform frames. The timeline previews
  the result, and the export action writes the native KSANIM v2 layout.
- KN5 v4/v5/v6 headers, embedded texture, material, hierarchy, static and skinned-mesh
  parsing, including recognition of appended CSP KN5ENC v1 protected payloads
- Stock shader parameters, texture-resource inspection, embedded BC1/2/3,
  CPU-decoded BC4/5/7, native BPTC BC6H/BC7 where available, generic
  8/16/24/32-bit masked DDS, bounded legacy D3D9 R/RG/RGBA 16/32-bit float DDS,
  PNG/JPEG/WebP,
  tangent-space normal maps, packed maps channels, diffuse/normal detail maps, and
  world-space four-layer track materials. The preview also reproduces the recovered
  five-texture blur and dirt path for `ksTyres` and `newStefano_ksTyres`. Stock
  `ksBrakeDisc` materials use their diffuse, normal, glow, blur, and blurred-normal
  textures. Live controls show the recovered temperature target and static blur state.
- Car- and track-scale orbit preview, hierarchy filtering, selected-mesh framing,
  isolation, and wireframe. Normal preview follows recursive KN5 game visibility:
  an inactive ancestor or a mesh with `visible`/`renderable` disabled stays hidden.
  Dimmed hierarchy rows remain selectable, and **Show hidden** exposes all such
  geometry explicitly for authoring inspection. The optional grid reproduces the
  old editor's 11 by 11-line magenta authoring overlay across a 10 m square.
  **View axis** reproduces the native one-meter red +X, green +Y, and blue +Z
  marker at the world origin. It draws after opaque geometry with depth disabled.
  A selected hierarchy node shows the native one-meter red X, green Y, and blue
  negative-Z world-axis marker through scene geometry.
- Direct hierarchy authoring for node names, active states, and local transforms.
  Transform controls use position, XYZ rotation in degrees, and scale. Stable
  root-relative paths keep duplicate names independent. Edits update the preview,
  support undo and recovery, save in project JSON, and bake into KN5 exports.
- Static-mesh geometry authoring for vertex position, rotation, and scale around the
  source bounds center. Edits update positions, inverse-transpose normals, tangents,
  GPU buffers, preview bounds, GrassFX, reflections, and the KN5 bounding sphere.
  Topology repair removes degenerate triangles, reverses faces, and rebuilds
  area-weighted normals.
  Stable node paths keep the edits in undo history, recovery data, project JSON, and
  KN5 exports. Skinned bind-pose geometry remains read-only.
- Ordered multi-KN5 track workspaces from manual selection or `models*.ini`, with
  per-file hierarchy roots, game-compatible placement transforms, material remapping,
  deterministic texture-name replacement, collision diagnostics, and native seeded
  previews for dynamic objects with probability, multiplicity, random positions, and
  velocity playback.
  The Model inspector reports exact parsed-scene totals, transformed world bounds,
  source-file hotspots, and the largest meshes. It also reports source-KN5,
  vertex/index, and embedded-texture payload sizes without claiming unsupported game limits.
  Static model positions and rotations support live editing, undo, recovery, project
  persistence, and `models.ini` export. Dynamic objects support edits to probability,
  multiplicity, position mode, position center and range, velocity mode, velocity base
  and range, and audio. These edits use the same undo, recovery, project, and
  `models.ini` export paths. The seed control makes previews reproducible. A live game
  can differ because it uses one shared random sequence.
- Packed or unpacked `data/surfaces.ini` physics inspection and assembled-layout validation
  using the game's built-in `WALL`, stock system surfaces, track override precedence,
  nonzero sector IDs, and unique substring binding; fallback and ambiguous physics
  meshes, contiguous starts and pits, paired timing gates, and the hotlap marker are
  diagnosed, with an exact-binding surface-physics viewport overlay. Local surfaces
  expose all 14 parsed physics fields for live editing. Edits support validation,
  undo, recovery, project persistence, binding refresh, and `surfaces.ini` export.
  Packed input stays read-only and exports a standalone file.
- Version-3 `cameras*.ini` inspection with count, basis, FOV, lap-interval, and
  clip-plane diagnostics, plus fixed track-camera preview using the configured
  position, orientation, FOV, and near/far planes. Referenced camera-spline CSVs are
  resolved, rotated around world Y, and sampled with the game's exact normalized
  interpolation and spline-specific minimum-FOV rule; normalized scrubbing and
  one-shot playback use `SPLINE_ANIMATION_LENGTH`
- Packed `data.acd` browsing with bounds checks, unsafe-path rejection, duplicate
  diagnostics, and virtual read-only INI files; packed or unpacked car `data/lods.ini`
  workspaces with contiguous `LOD_n` loading, `IN`/`OUT`
  range validation, cockpit/driver distance-switch inspection, engine-compatible
  FOV-normalized automatic selection, and forced per-LOD inspection. LOD ranges and
  file names, distance switches, and ranges support live editing, undo, recovery,
  project persistence, and `lods.ini` export.
- Car collision validation for `collider.kn5` texture/material rules, SDK triangle
  budget, origin transform, dimensions, degenerate triangles, welded boundary and
  non-manifold edges, plus packed or unpacked `colliders.ini` bottom-box inspection
  and a non-destructive orange edge overlay on the active car LOD. Each static collider
  mesh supports offset, rotation, scale, degenerate-face removal, face reversal, and
  normal rebuilding. Edits update the overlay and audit immediately. They also support
  undo, recovery, project JSON, reset, and standalone `collider.kn5` export. A SHA-256
  source identity prevents saved edits from changing a different or replaced collider.
- Packed or unpacked `damage.ini` authoring for scratch thresholds, initial damage,
  oscillation, and visual-object fields. The editor validates bounded input and node
  names. Edits support undo, recovery, project JSON, reset, and standalone export.
  A SHA-256 source identity prevents saved edits from changing a different file.
  Runtime deformation and oscillation are not simulated in the viewport.
- Per-LOD car hierarchy preflight for all 14 SDK-listed suspension, wheel, cockpit,
  steering, and brake-disc nodes; duplicate/null-kind diagnostics; left/right and
  +Z-forward wheel-axis checks; related-pivot and cross-LOD alignment
- Car and track skin discovery with live selection, case-insensitive KN5-basename
  replacement, sparse inheritance, per-LOD texture scoping, and ambiguity/load diagnostics.
  Bounded `ui_skin.json` parsing and editing covers all six installed metadata fields,
  project persistence, undo, recovery, unknown-field preservation, and standalone export.
- KSANIM v1 matrix and v2 quaternion/position/scale parsing, exact runtime frame
  sampling, v2 serialization, hierarchical node binding, live timeline preview,
  and match diagnostics
- Recursive `driver_base_pos.knh` parsing plus packed `driver3d.ini` inspection,
  steering/shift timing, shared-driver reference, hidden-object metadata, and exact
  KSANIM-to-base-rig coverage diagnostics; a selected shared driver KN5 is posed from
  the KNH hierarchy, assembled into every car LOD, and CPU-skinned by live KSANIM
  tracks. Cockpit mode resolves configured hidden nodes case-insensitively and hides
  only their driver subtrees. A uniquely named configured driver is selected
  automatically when it is already inside the browser-granted file set
- Packed or unpacked `analog_instruments.ini` inspection and live linear RPM preview.
  The preview uses ksEditor’s recovered node lookup, local +Z axis, and angle formula.
  LUT-based RPM mappings stay disabled and labeled until their native behavior is recovered
- Editable material shader/blend/depth/cull state, scalar/vector shader properties,
  embedded or solid-color texture resources, and mesh transparency/layer/LOD/shadow
  state, with granular reset controls
- A 100-step undo/redo history, automatic browser recovery, portable `.apex.json`
  projects, deterministic CSP shader-replacement export, and game-compatible KN5
  copy export for unprotected single-file assets. KN5-native material/property/resource
  and mesh edits are baked while CSP-only edits are diagnosed; the source stays intact
- CSP INI parsing with per-mesh/material selectors, shader replacements, material
  adjustments, condition LUTs, live condition preview, and source attribution
- Safe built-in expansion of the common CSP car-paint, glass, interior PBR,
  license-plate, and distant-emissive material templates used by installed configs
- Safe `CustomEmissive`/`CustomEmissiveMulti` atlas preview for rectangles, circles,
  soft polygons, color matching, channel mirroring, exact `MirrorUV` half-plane
  folding, vertex-anchor approximation, multi-item dashboard channels, diffuse
  alpha/luminance, and live vehicle inputs such as reverse, brake, turn signals,
  lights, doors, fog lights, hazards, and RPM. The preview also reproduces CSP's
  view-dependent emissive bounce-back for the sun, reflection cube, and point or
  spot lights
- CSP point/spot lights, mirrored `SelfLight` expansion, finite line lights with
  closest-segment range/diffuse, endpoint-color interpolation, and segment gloss,
  geometry-derived track `LIGHT_SERIES` preview, native packed
  linear `SPOT`/`SPOT_SHARPNESS` cones, optional hollow `SECOND_SPOT` lobes,
  per-channel `SPOT_EDGE` shaping, and native `FADE_AT ± FADE_SMOOTH/2`
  camera-distance culling/intensity fading for ordinary materials and GrassFX;
  car light roles and explicit headlight, beam, brake, and extra bindings appear as
  live vehicle-input controls, including authored `OFF_*` color, position, range,
  mirror, and camera-fade endpoints. Popup headlights expose their animation
  position, use the recovered start/end normalization, second-spot output exponent,
  and RGB edge-offset exponent, and follow a selected `lights.ksanim` timeline.
  Installed wall/box track occluders use camera-side silhouette tests and optional
  exclusion polygons to reject hidden whole-light records before GPU selection.
  Native interior/exterior view modes and tri-state track-receiver modes gate bounded
  selection and per-mesh ordinary-material/GrassFX reception; built-in `SelfLight`
  expansion preserves those template defaults and authored overrides
- CSP `.vao-patch` ZIP loading for installed legacy, v3, v4, and v5 payloads with
  bounds and CRC validation, native legacy/v4/v5 AO conversion, exact mesh-name,
  vertex-count, and first-position binding, per-vertex indirect-light attenuation,
  split-channel diagnostics, and a live comparison toggle
- CSP seasonal material preview driven by the config's native `YEAR_PROGRESS` LUTs,
  with the recovered autumn yellowing, winter desaturation/brightness, green-channel
  and upward-normal masks, deterministic tree variation, an always-visible calendar
  slider when applicable, and explicit `seasonSummer` legacy no-op diagnostics
- Deterministic, bounded GrassFX authoring preview with exact CSP selector matching,
  decoded BC1/2/3 diffuse and `ksMultilayer` surface-pass composition, CSP's
  continuous normalized-color/luminance threshold and stochastic acceptance,
  native mask/default fields, area height modifiers,
  projected road/kerb occluders, shape width, configured or native-default atlas
  loading, deterministic first-row species, weighted texture groups and rectangular
  pieces, native piece width/height scaling, direct and sampled multilayer
  configuration maps A–D with CSP's sequential interpolation, per-configuration
  masks and shape ranges/widths, native per-piece wind response,
  weekday/numeric trim cycles with source-derived near-pass density, tidy lean/height,
  base-atlas cut behavior, inverse-transpose world-normal interpolation, packed KN5
  tangent-frame decoding, supported stock tangent-space normal-map perturbation for
  blade lean and seasonal substrate color, the native RGBA8 normal/AO target
  quantization with the supported terrain shaders' exact alpha-one AO response,
  substrate-aligned fin width from `cross(rotation, normal)` and CSP's per-vertex
  substrate-to-vertical lighting-normal blend,
  the native RGBA8 material-parameter target carrying diffuse, ambient, averaged
  specular color, and normalized specular exponent, with the recovered gamma-space
  diffuse/ambient ratio and generation-time substrate specular response,
  a sparse packed-RGBA8 0.25 m world-space surface target
  with topmost `LESS_EQUAL` ownership, rejection of covered lower-source candidates,
  four-texel bilinear height gathering with the native 1.92 m discontinuity cutoff,
  bilinear target-color acceptance, later occluder writes, exact 2×2 color/alpha mips,
  the physical 2048² camera-local window, clamp-sampled outer texels, two-metre
  target snapping, and native 16 m center-refresh hysteresis,
  a sparse packed-RGBA8 0.5 m adjustment target with four raw mip levels, native
  none/low/medium/high weighted convolution into a quantized final target, and
  bilinear configuration sampling, native rule-order overwrite draws, rotated
  `CENTER`/`SIZE` texture pieces, and sequential eight-area ellipse overrides,
  the native five-pass medium camera schedule with 32/64/128/256/512 m windows,
  exact pass spacing, coverage divisors, two-metre snapping, stable PCG position
  jitter, depth-derived spawn relocation, frustum rejection, native pass-width
  distance fade, alpha-to-coverage, and far-fin ground sinking, with a deterministic
  25,000-sample portable CPU budget and explicit out-of-bounds authoring fallback,
  the exact shared 32×32 RGBA8 CSP noise mip with native linear wrapping and
  unjittered 40 m generation coordinates,
  the recovered 64×64 R16F atmospheric wind target with 32 native lifetime pulses,
  ping-pong shift/decay, two-frequency blade sampling, adjustable wind heading/speed,
  saturated bend and vertical length compensation shared by visible and shadow fins,
  plus exact zero 1024² RG8 deformation and 256² signed-RG8 vehicle-air targets for
  the static editor scene, with the recovered nonzero deformation response available
  as a tested authoring contract,
  native mip-2 edge-alpha squaring and mip-3 tall-grass clamping, matching single-fin
  visible/shadow geometry, native 1–4-cell repeat widening with adjacent base-atlas
  UV spans and stretched custom pieces, the installed linear/HDR pixel path's
  gamma-fixed substrate and atlas colors, green-channel surface-color selection,
  two-frequency per-fin color variation, near-field green detail, singleness-driven
  root dimming, view-angle ambient occlusion, direct-light distance/root fading kept
  separate from smooth atlas-alpha coverage, deterministic bend, texture brightness,
  generated substrate diffuse/specular response with native atlas/substrate
  fake-shadow modulation, the active zero-default `custom_l` directional backlight,
  active CSP local-light concentrated diffuse, fixed 52.8-exponent gloss,
  strongest-light backlighting and local-light fake-shadow detail response,
  a cached 1024² static-editor radial exponential-shadow atlas for up to four authored
  spotlights, using four 512² cells, CSP's exponent/normal-bias/boost resolve,
  optional extra filtering, clip sphere, and 10 cm lowered grass receiver,
  RainFX-driven wet-grass albedo darkening, near-field substrate-specular
  direction gain, CSP's exact height-weighted negative-wetness snow whitening,
  and a live visibility toggle
- RainFX material/mesh classification for puddles, soaking, smooth, rough, line,
  and relief surfaces, with adjustable wet darkening/gloss, deterministic puddle
  breakup, configured stream-edge/point diagnostics, and per-mesh inspection
- Effective CSP ambient, diffuse, specular, Fresnel, emissive, blend, shader,
  blend, and embedded, solid-color, or project-relative external texture-resource
  overrides in the renderer and inspector
- Native-ordered stock material state: shader-package cutouts default to 4× MSAA
  alpha-to-coverage, serialized alpha-blend/coverage flags override that default,
  transparent meshes sort back-to-front, and depth-write/cull modes plus CSP culling
  overrides remain independent
- Native-shaped directional sun shadows with three 2048² camera cascades at the
  shipped 2/12/50 m splits, exact SDK biases, per-mesh and CSP caster flags,
  diffuse-alpha cutouts for every non-opaque native blend mode, stable light-space
  fitting, a live toggle, and diagnostics
- CSP local spotlight shadows for ordinary materials and GrassFX through a bounded,
  camera-prioritized four-cell R32F exponential atlas (with an RGBA8 normalized-depth
  fallback). Scene geometry uses the same
  opaque/alpha-tested caster rules as the sun path, and the camera-independent atlas
  is cached until its selected lights or caster set changes. Radial depth encoding,
  authored/default exponent, automatic/authored boost, normal-dependent bias,
  clip sphere, and the public one-sample receiver equation match the recovered path
- All seven stock SDK weather-lighting presets with the native version-3
  RGB × intensity / 255 conversion and sun-angle interpolation, authorable sun
  heading/height, HDR sky and sun, distance fog, camera-relative cloud billboards,
  all seven native cloud DDS textures, the recovered `ksClouds` pixel formula,
  an RGBA16F composition target with
  supported MSAA, full-frame automatic or manual exposure, and the recovered embedded
  Yebis default display curve, reciprocal gamma, pre-curve saturation, quality-3
  threshold bright pass, five-level bloom core, native 15-sample separable Gaussian
  kernels, screen composition, and output dither
- Decoded Kunos/CSP smooth-capped Fresnel response, native gloss-to-reflection-mip
  heuristic, a ksEditor-shaped 512² runtime scene cubemap with six-face initialization
  and one-face-per-draw refresh, explicit single-subtree selection, separately loaded
  showroom geometry, shared viewport/capture material shading (including transparent
  layers, stock maps and multilayers, CSP emissives, seasons, RainFX wet materials,
  VAO, and active local lights), probe-centered directional cascades, weather-lit
  whole-track GrassFX with instanced cascade casting/receiving, and a weather-aware
  zenith/horizon/fog/ground/sun fallback for
  standalone cars without an environment,
  plus distinct stock windscreen, reflection-glass, and broken-glass alpha paths.
  CSP refraction samples a mipmapped opaque HDR scene resolve with normal-, distance-,
  aspect-, FOV-, strength-, and blur-dependent offsets
- Defensive bounds checks and explicit errors for unsupported node layouts

A portable BC6H fallback for systems without BPTC, dynamic RainFX accumulation,
drainage, spray and occlusion, GrassFX's full 868,352-thread density rather than the
portable CPU sampling budget, CSP's full 32-slot local-shadow packing and dynamic/car
atlas refresh scheduling,
wet cubemap reflection and ground-snow response,
transparent-layer feedback,
general CSP template/include expansion,
dynamic-object audio, subtractive procedural-emissive
composition, FBX bump-map conversion and packed material maps, arbitrary vertex and
face editing, skinned bind-pose geometry editing,
full local-light photometric fidelity,
Yebis star, ghost, light-shaft, non-default max-61 Gaussian passes, controlled pixel matching,
the remaining post-processing stack, VAO split-animation blending, dynamic extra
samples, and tree samples remain on the implementation roadmap. Legacy VAO
normal overrides now use the recovered decoder, identity key, and normalized WebGL
vertex input. CSP's exact
tree-season variation noise texture is represented by deterministic world-space preview
noise; the recovered color transform and all non-tree fixed-variation paths are exact. See
[`docs/ROADMAP.md`](docs/ROADMAP.md) for the full goal and completion gates.

For repeatable GPU checks, launch Chrome with a remote-debugging port and run
`node tools/browser-smoke.mjs --model … --config … --mesh … --assets … --csp-assets … --input NAME=VALUE`.
The tool isolates or assembles the selected mesh, hashes browser captures, and
reports JavaScript and WebGL errors. Pass `--screenshot FILE.png` to retain the
initial capture for visual inspection. Pass `--assembled --show-hidden` to compare
the game-visible scene with the all-mesh authoring view. Pass `--shadows` to capture
and compare the native-shaped directional preview with shadows disabled. Pass `--lighting`, plus
optional `--weather`, `--sun-heading`, `--sun-height`, `--compare-sun-height`, or
`--manual-exposure`, to verify and compare the HDR weather-lighting path. Pass
`--clouds --csp-assets PATH` to load the stock cloud DDS folder and compare cloud
weather with the clear preset. Pass
`--vao FILE.vao-patch` to bind a CSP vertex-AO patch and capture its enabled and
disabled states. Pass `--seasons --year-progress 0.07 --compare-year-progress 0.5`
to drive seasonal conditions through the track's calendar LUTs and compare captures.
Pass `--rpm 1000 --compare-rpm 6000` to compare a linear analog RPM needle.
Pass `--reflection-compare` to capture and hash the live scene-cubemap and procedural
fallback states independently. Use `--reflection-environment SHOWROOM.kn5` and,
optionally, `--reflection-root NODE` to verify showroom assembly and subtree selection.
Pass `--selection-axis` to require the native selected-node marker and capture a
comparison after the tool clears the selection.
Pass `--view-axis` to require the native world-origin marker and capture its on and
off states.

The authoring round-trip has its own production-browser check:

```sh
node tools/browser-authoring-smoke.mjs --model car.kn5 --config car.ini \
  --mesh GEO_body --property ksDiffuse --value 0.25 --reset
```

The same renderer smoke can open a discovered track or car manifest directly:

```sh
node tools/browser-smoke.mjs --workspace models.ini --assets path/to/track \
  --track-camera 'TV 1 · 0: start' --track-camera-position 1 \
  --play-track-camera --assembled --mesh AC_POBJECT_05

node tools/browser-smoke.mjs --model track.kn5 --config track.ini \
  --assets path/to/track --csp-assets path/to/assettocorsa/extension/textures \
  --grass-fx --assembled --mesh GRASS_PATCH --screenshot grassfx.png

node tools/browser-smoke.mjs --model track.kn5 --config track.ini \
  --rain-fx --rain-wetness 1 --assembled --mesh ROAD_PATCH --screenshot rainfx.png

node tools/browser-smoke.mjs --workspace data/lods.ini --assets path/to/car \
  --driver path/to/content/driver/driver_no_HANS.kn5 \
  --driver-cockpit --skin 00_red --lod 1 --collider --mesh BODY_LOD_B

node tools/browser-smoke.mjs --model car.kn5 --assets path/to/car \
  --animation animations/door_l.ksanim --animation-position 1 --mesh DOOR_L
```

Use `--resource txDiffuse --resource-value 'color: 1, 0, 0, 1'` to exercise a
resource override, or `--mesh-field lodOut --mesh-value 25` for a mesh adjustment.
Use `--node-field active --node-value false` for a hierarchy edit. The node fields
also include `name`, `position`, `rotation`, and `scale`. Use `--node NODE` instead
of `--mesh MESH` to edit the transform of a non-mesh node.
The check verifies rendering, undo, redo, autosave recovery, portable-project reopen,
project state, generated CSP, JavaScript errors, and WebGL errors.

Collider identity has a focused production-browser check. The matching and replacement
folders must contain the same visual KN5. The different-car folder must not match it:

```sh
node tools/browser-collider-identity-smoke.mjs --model car-a/car.kn5 \
  --matching-assets car-a --different-car-assets car-c \
  --replacement-assets car-b --screenshot collider-identity.png
```

The packaged desktop application has a separate production check:

```sh
node tools/desktop-smoke.mjs --model car.kn5 --assets path/to/car \
  --mesh BODY --screenshot desktop.png
```

This check connects to the Electron application through the Chrome DevTools Protocol.
It checks the renderer sandbox, navigation policy, CSP, model load, and WebGL state.
