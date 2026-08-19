# Product roadmap and completion gates

The goal is a usable cross-platform editor for Assetto Corsa cars and tracks whose
preview includes the author-visible parts of Custom Shaders Patch. The current
vertical slice is only the foundation.

## 1. Asset fidelity

- Complete and fixture-test static, skinned, and version-specific KN5 node layouts.
  Recursive active-branch, mesh-visible, and renderable flags now control the normal
  preview, while an explicit authoring toggle reveals hidden geometry without changing
  its parsed game state.
- Complete embedded texture coverage, normal maps, maps channels, detail maps,
  UV multipliers, blending, depth modes, two-sided rendering, and all commonly used
  stock car/track shaders. The seven shipped cutout-package flags, native shader-default
  then KN5-override blend ordering, 4× MSAA alpha-to-coverage, alpha blending,
  opaque/transparent ordering, depth-write modes, platform-correct winding, cutout
  shadow selection, and CSP culling overrides now have a portable
  preview path. BC1/2/3, CPU-decoded BC4/5, generic masked 8/16/24/32-bit DDS,
  bounded legacy D3D9 R/RG/RGBA 16/32-bit float DDS,
  embedded PNG/JPEG/WebP, and native BPTC BC6H/BC7, tangent normals, packed MultiMap specular/gloss/reflection,
  diffuse/normal detail maps, and stock four-way world-space track multilayers are
  decoded and rendered. BC6H/BC7 still need a software fallback when BPTC is
  unavailable; other DX10 variants and the remaining shader families still need exact
  paths. Track/environment workspaces now use a recovered 512² runtime scene cubemap:
  six faces initialize from the active camera and one face refreshes per draw. The
  portable reflection fallback now uses
  the decoded smooth Fresnel cap, gloss-derived mip heuristic, and a weather-aware
  zenith/horizon/fog/ground/sun response when no runtime cubemap exists. Transparent
  car materials now distinguish the stock windscreen alpha rule, Fresnel-raised
  reflection-glass alpha, and `ksBrokenGlass` damage-mask/additive behavior. CSP
  refraction reads a mipmapped opaque HDR scene resolve with the recovered projected-
  normal offset. It does not yet feed earlier transparent layers back into later ones.
- The portable FBX importer now reads binary and ASCII FBX scenes through Three.js.
  It imports static and skinned geometry, material groups, transforms, normals, UVs,
  skin weights, bone inverse-bind matrices, original names, and animation clips. It
  applies the recovered ksEditor V-coordinate sign change and writes a KN5 v6 file.
  Generated one-pixel DDS textures preserve FBX material colors. The importer captures
  external texture references. It preserves supported DDS, PNG, JPEG, or WebP files
  that resolve uniquely in a selected source folder. It also preserves supported
  embedded images. Static normal maps use `ksPerPixelNM` and `txNormal`. Missing
  normal maps keep a generated flat-normal DDS. The inspector reports map types that
  have no safe stock binding. The importer samples each clip into 100 local-transform frames.
  The editor previews these frames and exports the native KSANIM v2 layout.
  Bump-map conversion, packed material-map composition, direct geometry and
  hierarchy editing, protected-payload preservation, and in-game output validation
  remain. The KN5 v5/v6 writer also round-trips real static, textured, and skinned
  assets byte-for-byte.
- Complete driver texture export/reload/review and hierarchy-edit workflows. Native
  KSANIM v1/v2 transform tracks now parse,
  bind to every matching hierarchy node, and preview with the game's normalized
  frame-selection and interpolation behavior. Four-weight KN5 meshes are CPU-skinned
  against their inverse-bind matrices, and selected shared driver models receive the
  configured KNH base pose before joining every car LOD. Packed or unpacked car `data/lods.ini` projects now load every
  contiguous LOD, preserve distance bands, expose cockpit/driver switches, and allow
  automatic or forced LOD inspection; authoring and exporting those manifests remain.

## 2. CSP authoring preview

- Complete general CSP Inipp template/mixin/default/include expansion without
  executing arbitrary Lua. Direct INI rules and safe built-in expansion for common
  car-paint, glass, interior PBR, license-plate, and distant-emissive material
  families already have a tested implementation. The same safe path now covers the
  common `CustomEmissive` and `CustomEmissiveMulti` atlas shapes, mirrored channels,
  dashboard items, and vehicle-input bindings.
- Complete the remaining custom-emissive color/vertex-mask/bounce-back operations,
  UV transforms, and exact shader channel-composition behavior. Installed color masks,
  normalized/soft atlases, vehicle bindings, and declarative `@MIXIN` calls now work;
  vertex masks, bounce-back, MirrorUV, fog/door light casting, and subtractive modes
  are explicitly labeled approximations.
- Complete CSP light shadows, occlusion, binding, fade/visibility behavior, and
  photometric matching. Point/spot lights, mirrored self-lights, finite line lights,
  geometry-derived track series, live conditions, the recovered
  packed linear `SPOT`/`SPOT_SHARPNESS` transfer, symmetric `FADE_AT`/`FADE_SMOOTH`
  interval, optional normalized-range `SECOND_SPOT` lobe, RGB `SPOT_EDGE` transfer,
  and the bounded radial-ESM local-shadow atlas already have a tested preview path for
  ordinary materials and GrassFX. Native car-light role and explicit headlight,
  high/low-beam, brake, extra A–T, and inverse-headlight bindings now drive live
  authoring controls. Authored car-light `OFF_MULT`, `OFF_POSITION`,
  `OFF_RANGE_MULT`, `OFF_MIRROR`, and `OFF_FADE_MULT` endpoints are interpolated,
  and zero mirror correctly emits one light. `VISIBILITY_LEVEL` is preserved as
  deprecated BLM Lights app metadata rather than incorrectly filtering CSP
  rendering. Popup headlights now use the native normalized `POPUP_START`/`POPUP_END`
  progress, `POPUP_SECOND_SPOT_*` output curve, and `POPUP_EDGE_*` RGB edge shift;
  the preview input follows a selected `lights.ksanim` timeline. Radial exponential
  caster encoding, exponent/range scaling, automatic or authored boost, clip sphere,
  normal-dependent receiver bias, and the public resolve equation now run in the
  viewport. Finite lines now use the public closest-segment diffuse/range equation,
  endpoint-color interpolation, and reflected-ray segment gloss instead of bounded
  point sampling. Installed wall/box `TRACK_OCCLUDER` records also cull whole light
  records through a tested camera-side silhouette proxy with exclusion polygons.
  Recovered interior/exterior view enums and tri-state track-receiver modes now gate
  bounded light selection and per-mesh ordinary/GrassFX receivers, including
  template-expanded `SelfLight` defaults. Local shadows use the recovered four-sample
  resolve and separable 7-tap or 15-tap filters. Native occluder cell/BVH ordering,
  32-slot atlas packing, dynamic refresh scheduling, and photometric behavior remain.
- Complete VAO split-animation blending, legacy normal overrides, dynamic-object
  extra samples, and tree samples; add higher-fidelity car
  paint/glass/interior rendering. Installed legacy/v3/v4/v5 `.vao-patch` containers
  now have a bounds- and CRC-checked portable loader. It reproduces native payload
  conversion and exact mesh identity matching, feeds primary per-vertex AO into
  indirect lighting, preserves the second car channel, and reports alternate states,
  normal overrides, dynamic samples, and tree samples without silently approximating them.
  Seasonal material adjustment is now implemented from CSP's public shader source and
  the installed DLL behavior: `YEAR_PROGRESS` drives the config LUTs, `seasonAutumn`
  and `seasonWinter` use the native diffuse transform, and compatibility-only
  `seasonSummer` is reported as the native no-op. Tree variation uses deterministic
  world-space preview noise until CSP's exact `txNoise` asset can be replaced portably.
  CSP replacement slots now
  resolve embedded textures, solid colors, and external DDS/PNG/JPEG/WebP files from
  a user-selected car, track, or CSP asset folder. Procedural textures remain missing.
  GrassFX now has a bounded authoring preview with selector evaluation, CPU-readable
  BC1/2/3 diffuse and `ksMultilayer` material-pass composition, CSP's continuous
  normalized-color/luminance threshold and stochastic acceptance, areas, shape
  width, spatial road/kerb occlusion, configured or
  native-default atlas loading, deterministic default-row cell selection, four
  weighted texture groups with up to eight weighted rectangular pieces each, piece
  width/height scaling, direct and decoded multilayer-mask configuration maps A–D,
  source-derived sequential configuration interpolation, per-configuration shape
  and mask controls, native per-piece wind response, deterministic spatial bend,
  weekday/numeric trimming cycles with date-driven endpoint interpolation,
  source-derived near-pass size/density, tidy height/lean, base-atlas cut remapping,
  inverse-transpose world-normal interpolation, packed KN5 tangent-frame decoding,
  and supported stock tangent-space normal maps for fin lean and seasonal sampling,
  a sparse packed-RGBA8 0.25 m world-space surface target with topmost `LESS_EQUAL` depth
  ownership, rejection of covered lower-source candidates, later occluder writes,
  four-texel bilinear height gathering with the native 1.92 m discontinuity cutoff,
  bilinear target-color acceptance, exact 2×2 color/alpha mips, fixed mip-2 edge-alpha
  squaring and mip-3 tall-grass clamping, a sparse packed-RGBA8 0.5 m adjustment
  target with four raw mips, the exact none/low/medium/high weighted `MASK_BLUR`
  kernels, quantized final-target writes, bilinear configuration sampling, ordered
  adjustment-section overwrite draws, rotated `CENTER`/`SIZE` texture pieces, and
  eight sequential range-faded ellipse extras,
  the physical 2048² camera-local surface window and 1024² adjustment window with
  clamp-sampled borders, two-metre center snapping, full target redraws, and native
  16 m refresh hysteresis,
  the native five-pass medium camera schedule with exact 32/64/128/256/512 m
  coverage, pass steps/divisors, two-metre snapping, PCG position jitter,
  depth-derived placement, frustum rejection, pass-width distance fade,
  alpha-to-coverage, and far-fin ground sinking, plus a deterministic 25,000-sample
  CPU budget and explicit full-source fallback when an authoring camera is outside
  all GrassFX bounds,
  the exact embedded 32×32 RGBA8 shared noise mip with native linear wrapping and
  unjittered 40 m generation coordinates,
  the RGBA8 normal/AO target round-trip used by supported stock terrain shaders,
  including their exact alpha-one AO output,
  the RGBA8 material-parameter target for diffuse, ambient, averaged specular color,
  and normalized exponent, plus native generation-time diffuse/ambient and substrate
  specular evaluation for stock per-pixel, multimap, and multilayer terrain,
  substrate-aligned fin width from `cross(rotation, normal)` and the native
  substrate-to-vertical per-vertex lighting-normal blend,
  CSP's 64×64 R16F atmospheric wind target with two ping-pong surfaces, 32 advected
  lifetime pulses, native prior-field shift/decay and pulse equation, adjustable
  world wind, and the exact two-frequency visible/shadow fin deformation,
  explicit static-editor zero fields for the native 1024² RG8 deformation history
  and 256² signed-RG8 vehicle-air targets, plus tested nonzero deformation response
  and deformation-occlusion equations,
  native 1–4-cell repeat widening with its edge/width/far-pass rules,
  `TEXTURE_BRIGHTNESS`, clamped `COLOR_SAMPLE_MIP_LEVEL`, and shared single-fin
  viewport/shadow geometry. The installed `custom_l` pixel core now also supplies
  gamma-fixed substrate/atlas colors, atlas-green surface-color mixing, exact
  0.7–1.3 saturation and brightness variation, distance-limited green detail,
  singleness/root dimming, view-angle ambient attenuation, the native direct-light
  root/fade factors, and derivative-smoothed alpha coverage kept independent from
  distance fade. Its substrate specular also uses the native view-AO, atlas-green,
  and substrate-light `fakeShadow` modulation. Native host tracing establishes that
  the four lighting aliases stored in texture-piece padding are zero in this build;
  Apex preserves those disabled local-specular, cubemap-reflection, saturation, and
  configurable-backlight additions while reproducing the still-active built-in
  directional backlight. RainFX wetness now drives the `custom_l` albedo multiplier
  and its near-field substrate-specular direction gain through the shared
  viewport/probe grass program. Active CSP lights now add the public GrassFX
  substrate-normal concentrated diffuse, fixed 52.8-exponent gloss, strongest-light
  backlighting, and fake-shadow detail response. A cached 1024² static-editor depth
  radial-ESM atlas shadows up to four camera-prioritized authored spotlights in four
  512² cells for both ordinary materials and GrassFX. Its recovered four-sample
  resolve and separable 7-tap or 15-tap filters run before receiver sampling. GrassFX
  uses CSP's 10 cm lowered receiver. Full unsampled compute density,
  moving-vehicle air/deformation stamping, 32-slot packing, dynamic/car atlas refresh scheduling,
  wet cubemap reflection,
  negative-wetness snow, and the rest of CSP's game-only lighting integration remain.
  RainFX now classifies puddle, soaking, smooth, rough, line, relief, and stream
  config entries and applies a wet-material authoring preview with adjustable
  intensity. Native accumulation/drainage, height-field puddles, rain occlusion,
  hits, spray, vehicle interaction, and windscreen effects remain.
  `MESH_ADJUSTMENT` transparency, layer, LOD,
  and shadow state now parse, preview where applicable, and round-trip through authored
  projects and CSP export.
- Reproduce the required exposure, tone mapping, reflections, and ambient lighting
  in a portable rendering backend; validate with controlled game captures. All seven
  stock SDK presets now use the native version-3 HDR color conversion and sun-angle
  interpolation. A procedural HDR sky and sun, distance fog, supported multisample
  RGBA16F composition, full-frame automatic exposure within the shipped 0.2–0.5
  limits and manual exposure share the same linear pass. The display stage now executes
  the embedded Yebis `FUNCTION=-1` shader's exponential cubic curve, pre-curve
  saturation, reciprocal gamma, input floor, and output epsilon with the native
  characteristic-curve initialization. The stock
  directional path uses three 2048² cascades, the shipped 2/12/50 m camera splits,
  SDK biases, mesh/CSP caster flags, and native non-opaque-material cutouts. Runtime
  reflection capture follows ksEditor’s 90° face orientation, 0.01–500 m clip range,
  recursion guard, mip generation, and one-face-per-frame budget for eligible track
  or environment geometry. A separately loaded showroom is resource-scoped and becomes
  the automatic capture subtree; a scene-node selector can supply any single subtree,
  matching the recovered `addCubeMapNode` behavior. Standalone cars retain the
  procedural environment when neither is selected. Capture now shares the viewport
  material path for opaque and sorted transparent geometry, including stock maps,
  normal/detail/multilayer shading, CSP emissives, seasons, RainFX wet-material state,
  VAO, and active local lights. Probe-centered 2/12/50 m directional cascades cover
  all six faces, and whole-track capture includes generated GrassFX between opaque and
  transparent layers. Grass blades share HDR weather/fog lighting, receive the same
  cascades, and use their tapered instanced geometry in viewport and probe shadow maps;
  reflection sampling and refraction are disabled during capture
  to avoid recursive feedback. The default Yebis path now uses its recovered quality-3
  quarter-resolution source, five bloom levels, threshold bright pass, equal native
  composite weights, screen blend, and output-dither scale. Its separable bloom uses the
  active native 29-tap fallback, including the 15 bilinear samples, level radii, wavelength
  dispersion, and tail cutoff. Yebis star, ghost, light-shaft, non-default max-61 Gaussian,
  non-default tone functions, cloud billboards, full local-shadow packing/scheduling, and controlled
  game-image matching remain. GrassFX full-density GPU dispatch, moving-vehicle
  air/deformation stamps, CSP's full local-shadow atlas packing, wet cubemap
  reflection, and transparent-layer refraction feedback remain separate fidelity gates.

## 3. Track and car workflows

- Car workspace: packed `data.acd` is safely decoded for read-only browsing, and its
  virtual `data/lods.ini` follows the same path as unpacked data. All contiguous LODs load with validated
  ranges and game-normalized preview selection. Installed `skins/<name>` folders are
  discovered and their matching texture basenames override every LOD with live switching
  and diagnostics. The inspector now checks SDK-listed hierarchy coverage, node kinds,
  wheel axes, related pivots, and cross-LOD placement. Packed-data editing/repacking,
  skin metadata/editing, animation authoring, cross-folder shared-driver access without
  a browser file grant, lights,
  damage/dirt, instruments, steering/driver alignment, collider editing, and LOD-manifest
  authoring remain.
- Track workspace: static multi-KN5 layouts now load from ordered `models*.ini`
  manifests or manual multi-file selection, including placement transforms and
  collision diagnostics. `DYNAMIC_OBJECT_n` entries now parse and load at their
  deterministic range centers with probability, multiplicity, velocity, and audio
  diagnostics; randomized motion playback remains. Packed or unpacked `surfaces.ini` physics,
  the runtime `WALL` plus system/track definition merge, exact nonzero-sector and
  unique-substring physical-mesh bindings, contiguous starts/pits, timing-gate pairs,
  and hotlap markers now have an assembled-layout audit and spatial physics overlay.
  Version-3 camera sets are validated and can
  drive the viewport with their configured basis, FOV, and clip planes. Referenced
  spline CSVs resolve and use the game's world-Y rotation and normalized sampling for
  manual path-position preview and configured-duration playback; live focused-car
  targeting remains. Layout authoring, exact environment matching, remaining shaders
  and CSP effects, and performance/spatial diagnostics remain.
- Searchable hierarchy, multi-select, undo/redo, validation findings, batch editing,
  autosave/recovery, recent projects, and portable project files. Search, material
  editing, 100-step undo/redo, autosave/recovery, portable project JSON, and CSP
  export now form a tested vertical slice; track and archive validation are also
  exposed in the inspector. Multi-select, batch editing, recent projects, and
  geometry/hierarchy edits remain.

## 4. Distribution and proof

- Package signed applications for Windows, macOS, and Linux with no Wine dependency.
  The Electron shell now uses a sandboxed renderer and a loopback-only local server.
  Reproducible Linux AppImage and tar.gz packages now pass a production WebGL check.
  Target-native Windows and macOS packages, signing, and release automation remain.
- Run parser/writer round trips against representative official and mod assets.
- Maintain golden-image comparisons for stock and CSP rendering on cars and tracks.
  A reusable CDP browser-smoke tool now provides deterministic capture hashes and
  WebGL/JavaScript error checks as the foundation for those comparisons. A separate
  authoring smoke check proves edit, undo, redo, recovery, and CSP serialization on
  a production KN5 in a real WebGL browser.
- Verify generated KN5/config outputs in Assetto Corsa and document unsupported CSP
  features. Completion requires passing all these gates, not merely launching the UI.
