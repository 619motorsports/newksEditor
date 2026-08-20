# Reverse-engineering notes

These notes record local evidence from the installed Assetto Corsa SDK. They are not
an assertion that every KN5 variant has been decoded.

## Inputs

- `sdk/editor/ksEditor.exe`: 32-bit .NET/WinForms front end (512,000 bytes)
- `sdk/editor/ksNet.dll`: mixed-mode .NET/native engine API (17,211,904 bytes)
- `sdk/editor/system/shaders/*.shader`: versioned containers with DXBC vertex and
  pixel shaders, reflected constant-buffer names, and resource bindings
- `dwrite.dll`: 64-bit CSP proxy/render extension (137,132,032 bytes in this install)

The managed metadata and strings expose the original editor's major concepts:
`kMaterial`, `kMaterialVar`, `kMaterialResource`, `kNode`, `kMesh`, blend/depth modes,
LOD ranges, node layers, cameras, weather, FBX loading, car projects, persistence,
KN5 export, texture review, and track/car export modes.

## KN5 v6 layout verified in this repository

Offsets and field widths were checked against Kunos' `ks_nissan_370z/collider.kn5`
and `nissan_370z_lodD.kn5`.

```text
v5 header:  "sc6969"[6], version:u32
v6 header:  "sc6969"[6], version:u32, source-marker:u32
textures:   count:u32, repeated(active:u32, name:string, size:u32, bytes[size])
materials:  count:u32, repeated material
scene:      one recursive node

string:     UTF-8 byte-count:u32, bytes[count]
material:   name:string, shader:string, alpha-blend:u8, alpha-to-coverage:u8, depth:u32,
            property-count:u32, properties, resource-count:u32, resources
property:   name:string, scalar:f32, vec2:vec2, vec3:vec3, vec4:vec4
resource:   slot:string, texture-id:u32, texture-name:string
node:       type:u32, name:string, child-count:u32, active:u8, payload, children
null node:  matrix:mat4
mesh:       cast-shadow:u8, visible:u8, transparent:u8, vertex-count:u32,
            vertices[11 floats], index-count:u32, indices[u16], material-id:u32,
            layer:u32, lod-in:f32, lod-out:f32, bounding-sphere:vec4,
            renderable:u8
vertex:     position:vec3, normal:vec3, uv:vec2, tangent:vec3
skinned:    cast-shadow:u8, visible:u8, transparent:u8, bone-count:u32,
            bones, vertex-count:u32, vertices[19 floats], index-count:u32,
            indices[u16], material-id:u32, layer:u32, lod-in:f32, lod-out:f32
bone:       name:string, inverse-bind-transform:mat4
skin vertex:position:vec3, normal:vec3, uv:vec2, tangent:vec3, weights/indices:8 floats
```

For the collider fixture, this consumes all 1,838 bytes, yielding 3 nodes, 28
vertices, and 156 indices. The larger LOD-D file validates the material record:
three materials and shader properties such as `ksAmbient`, `ksDiffuse`,
`ksSpecular`, `ksSpecularEXP`, `fresnelC`, and `fresnelMaxLevel`.

The two material bytes are flags, not an enum followed by an alpha-test boolean.
`KN5IO::loadMaterialsBinary` (`ksNet.dll` at `0x1003b9de`) starts with the shader's
default state, applies alpha blend when the first byte is nonzero, then applies
alpha-to-coverage when the second byte is nonzero. `saveMaterialsBinary` writes
`blendMode == eAlphaBlend` and `blendMode == eAlphaToCoverage` respectively. The
parser retains both raw flags for exact round trips and exposes the resulting
serialized mode as 0 (opaque), 1 (alpha blend), or 2 (alpha-to-coverage).

The installed v5 `abarth500/collider.kn5` confirms that the source-marker word was
added in v6: in v5 the texture count follows the version immediately. Treating that
count as a source marker shifted every subsequent field and was the cause of many
apparent mod-file failures. The corrected v5 path consumes the complete collider and
also parses the public scene of protected v5 cars.

### Scene visibility semantics

KN5 visibility is hierarchical. A mesh participates in the normal game preview only
when every node from the root to that mesh has `active != 0`, and the mesh itself has
both `visible != 0` and `renderable != 0`. Apex stores that derived result separately
from the source flags. Hidden rows stay in the hierarchy for inspection, and the
**Show hidden** mode changes only preview inclusion and framing; it does not mutate the
parsed KN5 state.

Production Chrome checks exercise both forms of hiding. The complete Imola main KN5
contains 1,239 meshes: 1,023 are game-visible and 216 non-renderable collision meshes
are hidden. Its normal and all-mesh captures hashed to `b620d56af64ea684` and
`d5f9126b08eeee35`. The complete Kunos Nissan 370Z contains 215 meshes: 193 are
game-visible and 22 descendants of inactive damage, low-resolution cockpit, belt, and
wheel-blur branches are hidden even though their own leaf flags are enabled. Its two
captures hashed to `d641f95f4d93900e` and `5c0726c6ccf2c622`. All four captures
returned WebGL error zero with no browser exception. The renderer diagnostics retain
both `gameVisible`/`gameHidden` and current `previewVisible`/`previewHidden` counts so
the inspection override cannot be mistaken for source visibility.

## Rendering implication

The stock `.shader` containers contain ordinary D3D11 DXBC. Reflection strings show
the common lighting blocks (`cbCamera`, `cbPerObject`, `cbLighting`, `cbShadowMaps`,
`cbMaterial`) and bindings such as `txDiffuse`, `txNormal`, `txMaps`, `txDetail`, and
`txCube`. `tools/dxbc-disasm.c` calls the installed Wine `D3DDisassemble`
implementation so the exact stock shader instructions can be inspected locally.

Version-2 shader containers begin with a version byte, an `isAlphaTested` byte, and
a 32-bit vertex-layout code before their DXBC payloads. Seven installed packages set
that flag: `ksGrass`, the three `ksPerPixel*AT*` variants, the two multilayer AT
variants, and `ksTree`. `Material::initShaderVars` (`0x10040420`) promotes those
packages to `eAlphaToCoverage`; an explicit KN5 alpha-blend flag can subsequently
override that default. The stock main pixel shaders output diffuse alpha without a
discard. The portable renderer therefore uses WebGL multisample
`SAMPLE_ALPHA_TO_COVERAGE` for mode 2 instead of a hard fragment cutoff.

`MaterialFilterSM::apply` (`0x100650c7`) selects `ksShadowGenAT` for every material
whose effective blend mode is not opaque. Its pixel shader subtracts `ksAlphaRef`
from diffuse alpha and discards below the threshold. This cutoff belongs to the
directional-shadow pass only. The same function forces opaque shadow blending and
uses the separate `doubleFaceShadow` field for shadow culling. Main-pass culling is
independent material state, so shader-family names are not evidence for disabling
culling.

The decoded `ksPerPixelMultiMap` instructions establish that tangent-space normals
use RGB in the KN5 tangent/`cross(tangent, normal)` basis; `txMaps.r` scales
specular, `.g` scales its exponent, and `.b` scales reflection. When `useDetail` is
enabled, inverse diffuse alpha controls multiplication by `txDetail`, while detail
alpha also scales the packed specular channel. `ksPerPixelMultiMap_NMDetail` uses
the same mask to blend `txNormalDetail` at `detailUVMultiplier`.

The decoded `ksMultilayer_fresnel_nm` path samples a mask at mesh UVs and uses its
RGBA channels to weight four detail textures. Those textures use world X/Z UVs
scaled by `multR`, `multG`, `multB`, and the vector-valued `multA`; `magicMult`
scales the result. `detailNMMult` is likewise a two-component world-space scale.
The WebGL renderer implements these stock paths along with CSP resource replacement
to embedded textures, solid colors, and user-selected project-relative external
DDS/PNG/JPEG/WebP files. External lookup normalizes Windows separators and the CSP
`?textures` prefix, prefers exact project paths, permits a unique path suffix or
basename, and reports ambiguous matches instead of guessing. It also follows the decoded Kunos Fresnel
formula (`min(fresnelC + facing^fresnelEXP, fresnelMaxLevel)`) and uses a bounded
procedural environment plus capped transmission for identified glass/windscreen
materials. The more specific transparent paths and their remaining limits are
described below. Runtime scene-cubemap capture now follows the recovered editor
budget and orientation. Its mesh pass uses the same stock/CSP material binder as the
viewport; unsupported shader families remain approximations.

### Reflection environment evidence

An audit of official Kunos car, track, and showroom KN5 textures found no DDS with
the legacy cubemap caps and no material-owned `txCube` resource. Shader reflection
shows `txCube` in the global texture block instead. The environment is therefore a
runtime renderer resource, not a KN5 asset that this editor can recover from an
opened car alone.

Ghidra recovery of `CubeMapRenderer::render` and `renderScene` shows that ksEditor
centers a 90° cubemap camera at the active camera position, temporarily clears the
current cubemap to prevent recursive reflection, renders the supplied scene and
skybox, generates mipmaps, and binds the result to shader slot 10. The six native
face directions are `-X`, `+X`, `+Y`, `-Y`, `+Z`, and `-Z`; Apex converts their
DirectX up vectors to WebGL cubemap orientation. Shipped `sdk/editor/cfg/video.ini`
overrides the constructor defaults with a 512² target, one refreshed face per frame,
and a 500 m far plane (`NEAR` remains 0.01 m). The distributed
`content/texture/cube.dds` is a separate 512², six-face static fallback.

The CLR IL makes selection semantics unambiguous. `ksGraphics` field token
`0x0400039A` is named `cubeMapParent`. Its constructor copies the scene-graph root
into that field; `clearCubeMapRenderer` stores null; and
`addCubeMapNode(string nodeName)` converts the managed string, calls the scene graph's
node lookup, and stores the returned single `Node*`. The main render method loads
that field immediately before invoking the native cubemap renderer. Apex therefore
offers the same one-subtree choice rather than an unrelated mesh-visibility filter.

Apex initializes all six faces on the first eligible track/environment draw and then
refreshes one face per draw from the active camera position. The shared material shader
draws the HDR sky plus opaque and back-to-front transparent scene geometry, then
regenerates mipmaps. It includes stock texture, normal, map, detail, multilayer and
glass paths plus CSP emissives, seasons, RainFX wet-material state, VAO, and active
local lights. Cubemap sampling and refraction are explicitly disabled during this pass,
which preserves the recovered recursion guard and steady-state budget. Three
probe-centered sun cascades cover the native 2/12/50 m ranges independently of face
direction. Whole-track captures draw generated GrassFX after opaque geometry and
before transparent layers. Visible blades use the weather HDR sun/ambient/fog state
and receive the same PCF cascades. When its shared CSP texture is available, the
configured or native-default atlas first row supplies deterministic blade silhouettes
and the same alpha cutout is used by the instanced viewport and probe depth passes; a
procedural taper remains the portable missing/unsupported-texture fallback. A
separately opened showroom KN5 is assembled as a resource-scoped auxiliary subtree
and becomes the automatic capture root, so same-named car and showroom textures do
not leak across files. Any named scene subtree can be selected explicitly; this also
allows deliberate reproduction of ksEditor's whole-car self-reflection. A standalone
car with no environment retains the procedural reflection fallback. GrassFX's full
unsampled GPU density remains fidelity work;
shader families outside the implemented stock/CSP material
path do as well.
The live cube uses the same calibrated 0.12 environment scale as the decoded fallback
before Fresnel and maps-blue modulation.

The official Hangar showroom also provided a required legacy texture case:
`Old_Hangar4.dds` is a 6000×3000 D3D9 format-116 (`A32B32G32R32F`) panorama rather
than a character FOURCC. Apex recognizes legacy formats 111–116 and downsamples
oversized float textures to a bounded 2048-pixel maximum dimension before GPU upload.
This reduced the Nissan 370Z + Hangar production run from 83/84 to 84/84 loaded
textures while preserving HDR samples and avoiding a 288 MB GPU allocation.

The recovered stock/CSP reflection path flips cubemap X for DirectX sampling, derives
its base blur as `saturate(1 - ksSpecularEXP / 255)`, and samples mip
`base² × 6`. Its Fresnel term adds `fresnelC` to the angle power, applies the engine's
smooth minimum against `fresnelMaxLevel`, and multiplies sampled radiance by the maps
texture's blue reflection channel. Multimap green modulates the specular exponent and
therefore reflection blur.

When no eligible runtime cubemap is available, CSP's decoded fallback blends zenith and horizon
with `(1 - direction.y)²`, attenuates below the horizon, mixes fog, and supplies a
dark green ground response. Apex mirrors those terms in linear HDR, adds the active
weather's sun lobe with roughness-controlled width, and exposes the resulting mip in
the material inspector. This is materially closer than the previous two-color
vertical gradient, but it remains an explicit fallback rather than scene reflection.

### Windscreen, reflection glass, and refraction evidence

Disassembly of the shipped `ksWindscreen_ps.fxo` shows that it is not a reflection
shader. It samples `txDiffuse`, computes ambient and direct diffuse lighting, and
omits both the specular and cubemap paths. For diffuse alpha below 0.5, output alpha
is `alpha × saturate(dot(toCamera, -lightDirection) × shadow)`; alpha at or above 0.5
passes through unchanged. Apex implements this branch separately so the low-alpha
official windscreen textures remain transmissive instead of receiving glass Fresnel.

Reflection glass uses the recovered Fresnel response to raise output alpha toward
one at grazing angles. `ksBrokenGlass` is different again: its visible mask is
`glassDamage × txNormal.a`, fragments below 0.05 are discarded, and the surviving
coverage is saturated after multiplication by four. The stock additive path emits
alpha 0.1 while scaling RGB by `coverage / 0.1`; the portable shader preserves that
behavior when CSP refraction is not selected for a fragment.

CSP's reconstructed refraction helper projects the world normal onto camera right
and up, scales X by viewport aspect, scales by camera tangent and inverse distance,
and reduces the offset from one to one-half as the surface faces the camera. It then
samples `txPrevFrame` at an explicit mip. Apex takes a color-only resolve of the
multisampled opaque HDR target immediately before the first transparent draw,
generates its mip chain, and samples that texture for refractive materials. This
avoids a framebuffer feedback loop and preserves the offset, strength, and blur
semantics. It is intentionally bounded: later transparent surfaces see the opaque
snapshot, not earlier transparent layers, and no depth-aware rejection or chromatic
rainbow split is implemented yet.

### Embedded texture audit

`tools/kn5-texture-audit.mjs` reads the texture table without depending on filename
extensions. A 200-file sample covering 100 installed `ks_*` car KN5s and 100 track
KN5s contained 7,360 readable embedded textures. The observed population was 5,022
BC3, 767 BC1, 75 BC2, 737 PNGs, and 759 legacy uncompressed DDS images in 8-, 16-,
24-, or 32-bit masked layouts. No BC5 or BC7 image appeared in that sample, so the
highest-impact missing paths were the uncompressed and PNG images rather than BC7.

The renderer now decodes arbitrary contiguous legacy channel masks, including BGR24,
BGRA/RGBA32, RGB565, luminance, and luminance-alpha, and uses browser-native image
decoding for embedded PNG, JPEG, and WebP. BC4 and BC5 have bounded CPU decoders;
BC5 reconstructs the positive normal Z channel. On the complete 43.9 MB Kunos Abarth
500 Assetto Corse, a production WebGL run loaded all 80 embedded textures: 43 BC1/2/3,
29 raw DDS images, and 13 PNGs, with zero unsupported textures or WebGL errors. The
raw underside texture and PNG grille texture were also isolated and visually checked.

A wider follow-up sample found 49 DX10 BC7 images. Modern WebGL implementations can
upload those blocks through `EXT_texture_compression_bptc`; BC6H uses the same path,
and DX10 sRGB and uncompressed RGBA/BGRA/R8 mappings are recognized. A 144.5 MB
Dallara IR18 fixture loaded 60 of 60 textures, including eight BC7 images, and its
isolated Firestone sidewall rendered legibly without WebGL or JavaScript errors.

After correcting the v5 header, both 100-file baseline groups audit with zero parse
failures. A separate 300-file mixed-car slice also has zero texture-table failures
and contains 4,477 textures, including the 49 BC7 images and 1,218 embedded PNGs.

### CSP-protected KN5 payloads

Some v5 mod KN5s contain a normal public scene with 1×1 placeholder textures followed
by an appended archive ending in `__AC_SHADERS_PATCH_KN5ENC_v1__`. The archive is a
sequence of length-prefixed named records. Observed record families include
`tex.<name>.i/.k/.x/.d` and `ver.<mesh>.i/.k/.x`; the footer stores the 30-byte marker
length, marker, and two additional words. Apex recognizes and validates this structure
without misreporting it as unparsed KN5 data.

The tested Ferrari 488 GTE payload contains 1,002 records, 110 protected textures,
and 187 protected mesh groups after a complete 70-material, 454-node public scene.
The UI explicitly labels the file as CSP-protected and states that protected geometry
and textures remain placeholders. Payload decryption is not yet implemented, so this
recognition is not evidence of decrypted rendering fidelity.

## CSP boundary

CSP's `dwrite.dll` is game-version-specific native code and is not redistributable.
The planned editor should reproduce only observable authoring/preview behavior:
configuration parsing, material replacement/adjustment, emissive and light rules,
seasonal/track adjustments, and the required shader approximations. It should not
load or ship CSP's DLL.

## CSP configuration evidence

Static strings and RTTI from the installed `dwrite.dll` identify the native feature
as `shader_replacement` and expose `SHADER_REPLACEMENT`, `MATERIAL_ADJUSTMENT`,
`PROP_STRUCT`, `MATERIALS`, `MESHES`, `SHARED_MATERIALS`, `CONDITION`, and generated
`PROP_n=name,value...` forms. RTTI also exposes alternative material fields for
blend/depth modes, culling, shadow flags, emissive mode, textures, and render order.

The implementation was checked against the installed `ks_nissan_370z.ini` and
`imola.ini`, including these observed rules:

- section names can repeat and commonly use `...` suffixes;
- `?` is a multi-character wildcard and selectors can target material, shader,
  texture, or mesh names;
- braced selectors combine terms with `&` and negate terms with `!`;
- shader replacements apply `SHADER`, `BLEND_MODE`, `DEPTH_MODE`, and `PROP_*`;
- material adjustments pair `KEY_n` with `VALUE_n`, optionally interpolate from
  `OFF_VALUE_n`/`VALUE_n_OFF`, and are driven by named condition LUTs;
- `ORIGINAL` restores the value effective before that adjustment.

With direct sections only, the evaluator finds 36 overridden meshes from four
sections on the complete Kunos 370Z. On the complete Imola KN5, it finds 999 unique
overridden meshes from 27 sections.

The installed common libraries were then used to reproduce a bounded, safe subset
of Inipp expansion. The editor recognizes instances from `materials_carpaint.ini`,
`materials_glass.ini`, `materials_interior.ini`, `materials_license_plate.ini`, and
the distant-emissive material in `materials_track.ini`. It evaluates their defaults
and formulas directly into ordinary replacement sections; it does not execute Lua
or claim to be a general Inipp interpreter. Generated sections retain a synthetic
source location so the inspector distinguishes a built-in expansion from a direct
config rule.

For the complete Kunos 370Z, 17 recognized template instances increase the result
to 18 matched replacement/adjustment sections and 89 overridden meshes. A selected
`EXT_body` mesh resolves to `smCarPaint` with the expected metallic paint values,
including diffuse/specular, sun specular, Fresnel, flakes, colored specular, and
clear-coat parameters. Three of those expansions are `SelfLight` templates, which
produce five light instances after mirroring; three are active with the default
headlight condition. The remaining unresolved include in that config is
`no_popup_lights.ini`.

The other three expansions reproduce the installed `CustomEmissive` and
`CustomEmissiveMulti` formula library without executing its Lua. The descriptor
supports rectangular, circular, polygon, full-atlas, area-mask, and mirrored shapes;
multi-item channel allocation; diffuse alpha/luminance modulation; and common car
and dashboard input bindings. The renderer rasterizes evaluated channel colors into
a bounded RGBA atlas and uploads it as an additional emissive texture. On the real
370Z, this resolves four meshes and exposes reverse, left/right turn signal, light,
traction-control, hazard, and RPM controls. Isolated browser captures change for the
front turn lamp, rear reverse lamp, and dashboard highlight, with no WebGL error or
JavaScript exception.

Imola's ambulance rule also exercises repeated `KEY_...`/`VALUE_...` material
adjustment pairs. Preserving their source order applies all four `ksEmissive`
channels to five matching meshes. With the tested flag conditions enabled, the
complete track evaluation retains 999 overridden meshes, five custom-emissive
meshes, and 169 bounded light instances. Less-common color-mask, vertex-mask,
bounce-back, exact soft-edge, and UV-remapping operations remain unsupported or
approximated.

An audit of all 236 installed loaded car and track configs currently finds 501
recognized custom-emissive descriptors across 126 configs and 2,509 expanded
shapes, masks, and input bindings. This includes 152 color-mask instances, six
vertex-mask descriptors, 32 bounce-back rules, and one active MirrorUV rule.
Declarative `@MIXIN` invocations
are processed through the same bounded operation table, including their local atlas
resolution. No installed operation name is silently dropped. Native-only or
incompletely inferred behavior is surfaced as an approximation: bounce-back,
vertex-anchor selection, MirrorUV folding, fog/open-door cast lights, flat-normal
resource substitution, and the uncommon subtractive procedural-composition flags.

The reusable `tools/browser-smoke.mjs` check was run against production assets. The
Nissan 370Z changes independently for reverse and both rear turn channels; the AE86
changes for its diffuse-color-masked reverse and visible turn regions; the Mustang
changes for brake and the camera-visible vertex-anchor turn region. The complete
Imola KN5 loads with 999 overridden meshes, five emissive meshes,
and 169 lights; its red and night ambulance channels produce distinct captures.
The same verifier now exercises the Nissan fabric-seat normal/detail material,
Imola's four-way masked curb material, and the Yellowbird's solid-color `txMaps`
replacement. All these checks completed with WebGL error zero and no browser
exception.

Visual inspection, rather than hashes alone, exposed a Direct3D-to-WebGL winding
difference: copying the stock clockwise rasterizer state verbatim removed upward
track surfaces and large parts of assembled cars. Using CCW in the WebGL projection
restores Imola's curb and a coherent assembled Nissan. Isolated emissive channels
still change, and the assembled Nissan's camera-visible front signal produces a
distinct capture through its lens. Subsequent production checks covered
`GEO_windscreen`, isolated `dmgglass_front`, and a CSP `smGlass` override on
`GEO_glasses`. The renderer classified the three distinct paths, loaded all 82
textures, and returned WebGL error zero without browser exceptions. The remaining
glass gap is transparent-layer feedback and controlled game-image matching, not the
former absence of background-resolved transmission.

The installed DLL strings and CSP's config reference expose `LIGHT_EXTRA`,
`LIGHT_SERIES`, `POSITION`, `LINE_FROM`/`LINE_TO`, `DIRECTION`, `SPOT`, `RANGE`,
`COLOR`, `CONDITION`, and `CLUSTER_THRESHOLD`. The evaluator converts explicit and
mirrored lights directly, retains each line light as one finite segment, and samples
matching KN5 triangles while enforcing `CLUSTER_THRESHOLD` as the documented
minimum distance between generated sources. It retains source sections, mesh names,
condition values, range, direction, cone parameters, and independently faded endpoint
colors. The WebGL renderer shades with the nearest 32 active lights around the current
orbit target.

The public LightingFX include makes the finite-line receiver exact enough to avoid a
point-sample approximation. It projects the fragment onto the segment with
`saturate(dot(fragment - from, line) / dot(line, line))`, evaluates squared range and
concentrated diffuse from that closest point, and interpolates endpoint color with the
unclamped projection followed by saturation. Its gloss path instead finds the closest
point between the finite segment and the reflected camera ray, clamps it to the
segment, and shifts it toward the ray according to diffuse concentration. Apex uses
these same equations in ordinary-material and GrassFX programs. The public line path
does not apply the point-light-only secondary cone, RGB spot edge, or dynamic local
shadow lookup, and neither does Apex.

Both installed light loaders (`FUN_1806d6130` and `FUN_1806d4a70`) read `FADE_AT`
and `FADE_SMOOTH`. Scalar recovery shows defaults of 200 m and 80 m. The transfer
path at `FUN_1806d47e0` multiplies the smooth width by exactly 0.5 and writes the
runtime interval as `fadeAt - fadeSmooth/2` and `fadeAt + fadeSmooth/2`. Apex uses
that interval to reject lights beyond the far edge before its 32-light selection and
linearly scales uploaded light color through the interval. A zero-width interval is
a hard cutoff.

The spotlight constructor `FUN_1813bb930` converts authored degrees with the native
`0.017453294` scalar, halves and clamps the angle to `3.1414182` radians, and clamps
`SPOT_SHARPNESS` to `0.999`. It stores `cos(halfAngle)` as the outer boundary and
`cos(sharpness * halfAngle)` as the inner boundary. The downstream evaluator
`FUN_1813bc210` applies a linear saturated transfer across that cosine interval; it
does not use a Hermite smoothstep. This agrees with the public LightingFX record:
the signed inverse cosine width is premultiplied into `L_DirW_SpotCosS0_F.xyz`, and
the shader evaluates one subtract-and-saturate operation. Apex now emits the same
packed direction/start representation for both ordinary materials and GrassFX.

The optional car-light setter `FUN_1813bbb70` stores the secondary half-angle and
trigonometric cone terms at `+0xb0..+0xbc`, sharpness and its cosine at
`+0xc0..+0xc4`, and the authored `SECOND_SPOT_SKIP`, `SECOND_SPOT_RANGE`, and
`SECOND_SPOT_INTENSITY` scalars at `+0x8c`, `+0x90`, and `+0x94`. The config loader
only invokes this setter when angle, range, and intensity are positive; its authored
skip default is `0.3`. The public LightingFX pixel include adds the secondary cone
independently of the RGB edge-shaped primary cone and calls
`getTrimmedAttenuation(rangeInv, trimStart, trimLengthInv, distance)`. That helper
squares
`max(saturate(distance*trimLengthInv-trimStart)-saturate(distance*rangeInv),0)`.
Production configs use skip as a normalized part of the secondary range, so Apex
packs `rangeInv=1/range`, `trimStart=0`, and
`trimLengthInv=1/(range*skip)`. This creates CSP's hollow lobe: it rises over the
first `skip*range` metres and returns to zero at the secondary range. The same path
packs `SPOT_EDGE*SPOT_EDGE_SHARPNESS` as three channel offsets and normalized
`SPOT_UP*SPOT_EDGE_SHARPNESS` as the projection vector. The DLL field layout and
public shader equation are directly recovered; the normalized trim constant-buffer
packing is a portable derivation because the downstream native buffer writer has
not been isolated.

The extra-light loader `FUN_181363c40` also recovers runtime binding semantics. It
stores `BIND_TO_HEADLIGHTS` at byte `+0x15d`, with distinct enum values for excluding
or including high-beam flash, then stores `BIND_TO_HIGHBEAM`, `BIND_TO_LOWBEAM`, and
`BIND_TO_BRAKELIGHTS` in adjacent flags. It scans `BIND_TO_EXTRA_A` through
`BIND_TO_EXTRA_T` and stores the selected extra index, and records
`NOT_WITH_HEADLIGHTS` separately. Apex combines these authored gates with the
section's condition and exposes them as vehicle-input controls. Named headlight,
brake, reverse, turn-signal, and hazard sections receive their corresponding
implicit role when no explicit binding replaces it. A `BOUND_TO=head_lights`
extra receives the headlight state as a portable static-editor fallback.

`BOUND_TO` itself is not a condition name. `FUN_181358230` resolves every list item
to scene geometry, accepts a `name:index` suffix, stores the bound mesh reference,
and logs `Bound mesh not found` on failure. Apex therefore retains the authored list
as binding metadata rather than treating arbitrary mesh names as inputs. The public
track-config reference describes `VISIBILITY_LEVEL` as deprecated BLM Lights app
level-switching metadata, so Apex retains it for inspection but does not use it to
discard CSP lights.

The shared loader `FUN_181355e00` stores `INTERIOR_ONLY` and `EXTERIOR_ONLY` as one
enum at light offset `+0xf8`; interior wins when both are set. It stores
`AFFECTS_TRACK` as a tri-state byte at `+0x167`: zero affects track, one rejects
track receivers, and two is the literal `AFFECTS_TRACK=INTERIOR_ONLY` mode. In the
runtime transfer path `FUN_1813bddd0`, the owning car's `+0x2f6` cockpit state feeds
light byte `+0x419` as an exterior-view flag and gates the `+0xf8` enum.
`FUN_1813c0390` independently gates track receiver enumeration with `+0x167`, using
the owning car's interior state for mode two. Apex applies the recovered view gate
before selecting bounded local-light records, packs the track tri-state for per-mesh
ordinary-material filtering, and applies the same receiver rule to GrassFX. Built-in
`SelfLight` expansion preserves the template defaults (`AFFECTS_TRACK=0`, exterior
view) and authored overrides instead of flattening them.

The shared car-light parser `FUN_181355e00` reads paired on/off records, including
`OFF_RANGE_MULT` and `OFF_FADE_MULT`; the surrounding car loaders also expose
`OFF_POSITION` and `OFF_MIRROR`. Production presets combine `OFF_COLOR` with
`OFF_MULT`: for example, generated reverse lights can author a bright off color and
then reduce it to a dim residual glow with a 0.1 multiplier. Apex uses the current
vehicle-input factor to interpolate color, local position, range multiplier, mirror
spacing, and camera fade center/width between those endpoints. This is a portable
static authoring interpolation. `MIRROR=0` is disabled rather than a request for two
coincident instances.

The headlight loader `FUN_181358d00` stores `POPUP_START`, `POPUP_END`,
`POPUP_SECOND_SPOT_INITIAL_VALUE`, `POPUP_SECOND_SPOT_EXP`, `POPUP_EDGE_OFFSET`,
and `POPUP_EDGE_EXP` at light offsets `+0x248..+0x25c`, with defaults 0.05, 0.7,
0.4, 0.6, 0.5, and 0.3. Runtime update `FUN_1813bddd0` reads the normalized
`lights.ksanim` value at the car-animation record's `+0x24` and computes
`popup=saturate((animation-start)/(end-start))`; the equal-endpoint branch is zero
at or below the endpoint and one above it. While a headlight-bound popup is not
fully open, its output multiplier is
`initial+(1-initial)*pow(popup,secondExponent)`. Spotlight evaluator
`FUN_1813bc210` independently subtracts
`edgeOffset*(1-pow(popup,edgeExponent))` from all three authored `SPOT_EDGE`
components before the RGB edge transfer. Apex reproduces both equations, exposes
the raw normalized popup-animation input, and drives it automatically when the user
selects `lights.ksanim`. The electrical headlight input still controls the recovered
on/off color, position, range, mirror, and camera-fade interpolation; popup progress
does not replace that state in the native routine.

Installed configs and DLL strings also expose `[TRACK_OCCLUDERS]`,
`TRACK_OCCLUDER_WALL_n`, and `TRACK_OCCLUDER_BOX_n`. `FUN_1812b8700` loads native
defaults `CELL_SIZE=1000` and `SIZE_WEIGHT_FACTOR=0.5`; `FUN_180e254d0` reads two
wall or four box `POINT_n` values, an optional four-point `EXCLUSION_n` polygon,
`ACTIVE`, and `CULLING`. The constructed wall query `FUN_180e24dd0` uses the current
camera, authored top height, horizontal span, and exclusion test to emit a vertical
silhouette. The box query `FUN_1810a4fb0` chooses camera-facing silhouette edges from
its four corners and applies the same height and exclusion gates. These records are a
coarse camera-dependent culler for whole lights, meshes, and cars, not the
per-fragment radial shadow atlas and not the public shader's unrelated `gNoLightAt`.
Apex parses the installed topology and uses a portable finite-ray/silhouette test to
remove whole authored light records hidden from the camera target. It does not claim
the native cell partition, BVH ordering, or size-weight heuristic.

On the complete Imola main KN5, 18 light sections currently produce 169 bounded
preview instances when night and the tested flag conditions are enabled. This path
consumes world transforms and mesh normals and completes without a WebGL error on
3,036,891 triangles. It remains an authoring approximation: native occluder spatial
indexing, visibility levels, 32-slot shadow packing, dynamic refresh, and full
photometric matching are not implemented yet. Receiver/view filtering uses the
native enums described above.

## GrassFX evidence

Thirty-five installed loaded track configs contain an active `[GRASS_FX]` section.
The implementation parses its material and mesh selectors, alpha and opaque
occluders, height/edge/color controls, texture-mask thresholds, shape controls, and
`[GRASS_FX_AREA_...]` modifiers. It uses the same CSP selector evaluator as material
replacements. Eligible upward-facing source triangles are sampled deterministically
in world space, with either triangle winding accepted because production KN5 track
meshes contain both orientations. A bounded projected-triangle grid removes samples
covered by configured road and kerb occluders. Embedded BC1/2/3 diffuse textures now
have a portable RGBA decoder, so GrassFX surface-color sampling still works when the
browser does not expose S3TC texture upload.

The native GrassFX config loader at `FUN_180c266b0` initializes its atlas grid to
`16, 1`, accepts legacy `TEXTURE_SLOTS` and current `TEXTURE_GRID`, defaults to
`grass_fx/generic.dds`, and reads `TEXTURE_BRIGHTNESS` with a default of one. The
installed Kunos Highlands config independently documents that the first grid row is
the default species set. Apex therefore chooses a deterministic column from that row
for every blade and preserves the current procedural taper only when the requested
atlas cannot be decoded. A separate shared-CSP-texture folder can point at
`assettocorsa/extension/textures` without changing the active track/car workspace.

The same native loader enumerates at most four `GRASS_FX_TEXTURE_GROUP` records and
eight `PIECE_n` records per group. It subtracts one from the piece X coordinate,
leaves Y unchanged, defaults width and height to one cell, normalizes the rectangle
by `TEXTURE_GRID`, reads `_SIZE_MULT`, `_CHANCE`, and `_WIND`, and accumulates each
group’s total piece chance. `FUN_180c22e70` resolves adjustment `MAP=A` through
`MAP=D` to one-hot four-channel vectors and multiplies the selected channel for the
two-value form such as `MAP=A,0.5`. Installed configs also prove that base and group
chances are weights rather than clamped probabilities (`TEXTURE_BASE_CHANCE=5` is
used at Monza). Apex applies those confirmed limits and weight semantics, maps direct
material/mesh adjustments into the base and separately default-initialized A–D
configurations, and samples rectangular atlas pieces deterministically. The CSP
generation shader establishes that the two
`PIECE_n_SIZE_MULT` components are width and height multipliers rather than a random
range; Apex preserves their order and applies those separate dimensions.

Adjustment geometry uses a separate native piece path. `FUN_180c23680` accepts an
adjustment section with either material/mesh selectors or `CENTER` plus `SIZE`, reads
`ANGLE`, `TEXTURE`, `TEXTURE_OFFSET`, and `TEXTURE_SCALE`, and selects
`flgGrassAdjustment_vs_piece.fxo` for the generated six-vertex quad. The public
vertex shader maps that quad into world X/Z and applies the configured texture UV
offset and scale. Apex emits the same two rotated triangles on the adjustment
lattice and supports an optional decoded texture sampler before the section map is
multiplied into the four configuration channels.

The piece parser also initializes `_WIND` to
`1 / max(1, PIECE_n_SIZE_MULT.max)` before reading an explicit override. This is
visible at `FUN_180c266b0` where the maximum size multiplier is clamped to at least
one and inverted immediately before the `_WIND` lookup. Apex packs that response per
instance and applies it in the visible and shadow vertex shaders.

The installed shader archive manifest maps `custom/accWind_ps.fxo` to member
`py51om`. Disassembling that DXBC payload recovers the full atmospheric-wind update.
`FUN_180c319b0` creates two named 64×64 targets, `Wind offsets: 0` and
`Wind offsets: 1`, using native format code `0x36` (`R16_FLOAT`). Every update draws
the previous target through `accWind_ps`, then swaps the pair. Its 33-register
constant buffer contains 32 float4 pulses followed by wind displacement and speed.
The prior field samples at `uv - windDelta * 0.05` and decays from `0.99` at zero
wind to `0.97` at 35 m/s. Each pulse wraps its position on the unit target, searches
seven offsets along the current wind displacement, applies the recovered radial and
triangular-lifetime profile, and adds a bounded contribution before the result is
clamped to 0–4.

`FUN_180c31ba0` supplies the CPU half of that update. It maintains exactly 32
particles, advects their X/Z coordinates by wind velocity times frame delta, and
decrements lifetime by `(random + 0.5) * delta / 2`. Expired pulses respawn at
life one with two positions distributed over a 2,000-unit periodic domain and a new
random scalar. Apex reproduces this state update and the disassembled pixel equation
on a 64×64 R16F ping-pong target; the local RNG is deliberately seeded so authoring
previews remain reproducible, while the installed runtime consumes its shared random
stream.

The public `flgGrass_vs.hlsl` establishes how the target moves each fin. It samples
the field at `(position - sceneOffset).xz / 80 + phase * 0.05` and again at `/21.71`
with a `0.471` weight, multiplies by the per-fin wind response, then applies the world
wind vector through `offset / (1 + abs(offset))`. Horizontal bend is proportional to
fin height; a square-root correction lowers the tip to preserve apparent length.
Apex preserves the random base-atlas column as a nineteenth instance float because
`phase = column / 2` remains active even when a custom atlas piece replaces the UV
rectangle. The visible and cascade-shadow programs consume the same current wind
target. CSP's separate 50 m `txAir` generation target is stamped by moving cars; with
no dynamic cars in an editor scene its exact value is zero, which is the state Apex
uses.

`FUN_180c22200` maps `MONDAY` through `SUNDAY` to native float codes `-1` through
`-7`; positive numeric values remain durations in days. CSP’s GrassFX authoring
reference confirms that two-value shape fields are trim endpoints, with the first
value used immediately after cutting and the second at the end of the cycle. Apex
therefore derives one phase from the preview year/date and `TRIM_PERIOD_OFFSET`, then
interpolates configuration height, shape size, shape width, cut, and tidy values
uniformly. This fixes the previous incorrect randomization of
`SHAPE_SIZE=0.5, 2.25`.

The installed DLL contains `shaders.zip`; its archive comment identifies the
[public CSP shader repository](https://gitlab.com/ac-custom-shaders-patch/public/acc-shaders)
as the matching source. `custom/grass/flgGrass_cs_generation.fx` makes the remaining
shape behavior explicit. `SHAPE_SIZE` rejects a candidate when a random value is
greater than `(passID < 1 ? 1 : 2) / shape_size`, then scales pass 0 by `0.5`, pass 1
by `0.75`, and later passes by one. `SHAPE_TIDY` interpolates both the procedural
height term toward `0.75` and random lean from `0.5` toward `0.1`.
`SHAPE_CUT` shifts base-row atlas V coordinates by a per-fin value in the
`0.4`–`0.6` range; custom texture pieces deliberately bypass that cut remap. Their
first size component sets half-width and their second component scales height.

Apex applies those equations to each native medium-profile pass, generates one
oriented fin per accepted instance, and shares absolute width, lean, cut, atlas, and
wind values between visible and shadow shaders. The current 26-float instance record is
position/height, orientation/color, atlas rectangle, width/wind/cut, and two lean
components followed by distance fade, the native wind-phase atlas column, the
quantized substrate normal XZ, singleness, two per-fin pixel random factors, and the
generated material diffuse/specular response. The
vertex path reconstructs normal Y, derives fin
width with `normalize(cross(float3(rotation.x, 0, rotation.y), substrateNormal))`,
and blends the lighting normal from substrate normal at the root to world up at the
tip. The shadow path uses the same substrate-aligned geometry. Accepted fin width and height are multiplied by the same continuous
`colorTest` value as the generation shader. KN5 vertex normals are interpolated at
each candidate after inverse-transpose world transformation. Static-mesh tangents are
decoded from the three low bytes of the packed tangent float, transformed with the
world 3×3, and interpolated with `cross(tangent, normal)` bitangents as in the stock
vertex shader. Supported tangent-space maps then perturb the substrate normal before
it feeds both the seasonal transform and CSP's base up-vector shape; tidy-driven
random tilt is added afterwards. Its low-frequency variation now samples the exact
32×32 RGBA8 mip-0 payload from CSP's shared `textures/common/noise.dds`, with D3D
normalized-coordinate linear wrapping. Camera-ring generation preserves the shader's
unjittered `(gSpawnPointAWorld + basePosOffset) / 40` coordinate; the bounded
full-source authoring fallback uses the resolved world XZ coordinate. The portable
editor samples the complete native ring schedule within an explicit CPU budget rather
than attempting all GPU threads.

The same generation shader computes `repeatK` after final fin sizing. It sets
`repeatBase` to `(passID == 0 && sizeMult < 0.5 ? 0.2 : 0.1) * (1 + RAND)`, then
rounds `clamp(edgeK * repeatBase / finWidth, 1, 4)`. Passes four and above force two
cells when `sizeMult <= 1`; tall grass next to an edge forces one when mip-3 alpha is
not above `0.99`. `flgGrass_vs.fx` still draws six vertices, multiplies physical fin
width by `repeatK`, and spans that many adjacent columns for base atlas cells. Custom
pieces keep their configured UV rectangle and stretch it across the widened fin.
Apex folds both operations into width and atlas rectangle before upload, so visible
and shadow paths share the result without another instance attribute.

The installed packed shader manifest maps `custom_l/flgGrass_ps.fxo` to `1yyveel`
and the legacy `custom/flgGrass_ps.fxo` to `1c91uee`. DXBC disassembly distinguishes
the linear/HDR branch used by Apex from the older gamma branch. The generation
shader first gamma-fixes captured substrate RGB as
`pow(max(surfaceRGB * 4.444444, 0), 2.2)`. The pixel shader applies the same exponent
to `atlasRGB * 4.444444 * TEXTURE_BRIGHTNESS`, computes
`texSat=saturate((atlas.g-max(atlas.r,atlas.b))*2)`, and selects the substrate color
where the atlas is bright green. It then varies saturation and brightness by
`0.7+rnd.x*0.6` and `0.7+rnd.y*0.6`. The legacy branch instead uses the narrower
`0.85+rnd*0.3` interval.

Those two random factors combine the exact shared noise texture at world XZ divided
by 177 and 87, weighted 0.6/0.4, then interpolate 40 percent toward the fin RNG.
Generation initializes `singleness` with `saturate((1-mip2Alpha)*2)` and raises it
to at least `saturate(1-height/0.35)^2`. The vertex shader turns that into
`TexDimming=advFade^2*(1-singleness)*0.88*(1-bladeY)`, where
`advFade=saturate(1.2-distance/60)`. The pixel shader multiplies lighting by
`lerp(1,0.5,TexDimming^2)` and uses `atlas.g^2` as a near-only detail multiplier,
attenuated by `1/(1+distance^2*0.05)`. Atlas alpha is derivative-smoothed around
0.4 and clipped below 0.25. DXBC writes that coverage directly to output alpha;
distance fade does not multiply it. Instead, native direct light is scaled by
`saturate(fade)*saturate(0.5+3*bladeY)`, while ambient alone is multiplied by
`lerp(1,abs(toCamera.y)*0.5,TexDimming)`. Apex now carries all three generation
outputs and uses this recovered color/coverage and available lighting core; its
portable weather, shadow filtering, and fog remain the surrounding renderer
integration rather than a claim of complete CSP lighting parity.

The public `GBUFFER_GRASSFX` material path defines the separate material-parameter
target as `(ksDiffuse, ksAmbient, average(specularColor), specularExponent / 255)`.
Native setup in `FUN_180c293a0` creates `GrassFX: material parameters` with format
code `0x1c`, the same `RGBA8_UNORM` format used by the color and normal targets.
Generation decodes it as `pow(max(diffuse / max(ambient, 0.01), 0), 2.2)` and
`pow(saturate(dot(normalize(toSun - toSurface), normal)), exponent)` multiplied by
the averaged specular color and `saturate(dot(normal, toSun) * 4)`. For
`ksPerPixelMultiMap`, the maps texture red channel scales specular color and green
sets `green * ksSpecularEXP + 1`; `ksMultilayer` scales specular color by the composed
detail alpha. Apex quantizes these parameters into the sparse target, bilinearly
samples mip zero during generation, and carries the resulting diffuse and substrate
specular scalars into each visible fin. Dynamic deformation, reflection-extension,
and local-light specular branches are not claimed.

The pixel shader's `fakeShadow` is also self-contained. It computes substrate-light
detail as `1 - 0.5 * Lambert²`, converts view-dependent `texAO` through the saturated
0.2–0.7 inverse-lerp, and blends that response from one by detail multiplied by atlas
green saturation. In the active source this factor modulates the substrate and local
specular additions only; the nearby diffuse-light multiplication is commented out.
Apex applies the recovered factor to its implemented substrate specular and leaves
the diffuse path unchanged.

The native host resolves the four otherwise ambiguous lighting aliases. In
`FUN_180c293a0`, CSP constructs `cbuffer_render` at register `b11` with size `0x230`
(560 bytes): the 48-byte scalar/deformation/grid prefix followed by 32 packed
16-byte `CustomTexVSPiece` records. The public shader aliases
`gSpecularIntensity`, `gReflectivity`, `gBacklit`, and `gSaturation` to the padding
float of records zero through three. `FUN_180c266b0` first clears the complete
512-byte host record array, then explicitly writes zero to each parsed piece's
padding field. Immediately afterward, `FUN_180c2ab80` copies that host array from
object offset `0x15b4` to constant-buffer offset `0x30` and marks the buffer dirty.
The aliases therefore correspond to host offsets `0x15c0`, `0x15d0`, `0x15e0`, and
`0x15f0`, all zero in the installed build. The per-frame update writes only texture
brightness, wetness, flags, and interior clipping in the first 16 bytes; deformation
has its own two-point writer. Class-local dirty-state and whole-image displacement
searches found no later GrassFX writer for those padding fields.

This disables the shader's added local specular and cubemap reflection, makes the
extra saturation interpolation an identity, and leaves the configurable portion of
backlighting at zero. Backlighting itself is not fully disabled: the vertex shader
adds a built-in `0.25 * pow(direction^4 + saturate(viewSun)^32, 2)` term. It is
multiplied by the 72 m fade, squared substrate diffuse response, view/sun opposition,
fin-height/root fade, cascade shadow, and an over-saturated substrate color; the
pixel shader then applies its dark-color gate before common texture dimming and
near-field detail. Apex reproduces that active zero-pad branch and deliberately does
not expose non-native controls for the three disabled experimental additions.

The public `custom/grass/flgGrass_ps.fx` wet branch computes
`wetK = saturate(gWetK) * Occlusion` and multiplies the base color by
`lerp(1, pow(0.65, 2.2), wetK)`. Its matching vertex shader derives a separate
near-field specular factor as `saturate(extSceneWetness * 100) * AO * 0.5`, then
multiplies the substrate-specular direction term by `lerp(1, 2, factor)`. Supported
terrain shaders write AO one to the normal/AO target, so Apex can reproduce both
terms directly from the live RainFX wetness slider. The same wet uniform is used by
the viewport and non-recursive scene-probe grass draw. The source's negative-wetness
snow whitening, local-light specular and cubemap-reflection extension remain outside
this editor subset.

GrassFX has a separate active LightingFX path that is independent of the zeroed
experimental padding aliases above. Public `custom/grass/flgGrass_vs.fx` selects
the substrate normal, sets diffuse concentration support, fixes the local specular
exponent at 24, and scales specular color by half the fin fade. In the active gamma
branch the exponent becomes 52.8 and the final gloss multiplier becomes twice the
fin fade. The LightingFX include computes concentrated diffuse as
`saturate(lerp(1, dot(normal, toLight), concentration))`, applies squared range and
spot attenuation, and selects the main local light by the luminance of its
concentration-weighted contribution. Grass then uses that main direction for a
quarter-strength, fourth-power bent-normal backlight and blends `ShadowDetails`
toward one in proportion to local versus total illumination. Apex evaluates these
equations for the nearest 32 active decoded CSP lights in the shared viewport and
reflection-probe grass program. Point and spot records use the recovered packed
receiver, while line records use the public finite-segment closest-point,
endpoint-color, and reflected-ray gloss equations. Apex does not claim byte-identical
native per-vertex interpolation.

The public GrassFX shader enables `ALLOW_DYNAMIC_SHADOWS` and binds
`txShadowsAtlas` at texture slot 16. Its 32-entry shadow table uses 112-byte records:
atlas center and size, inverse exponential range factor, maximum range, bias and
thickness controls, shadow position, and one 4×4 transform. The receiver is lowered
by exactly 0.1 m before projection. The shader compares an exponential receiver
depth against the resolved atlas value, then applies the authored bias and thickness
terms. Native constructor `FUN_180c6a060` creates a 2048² atlas, or 4096² at the high
setting, plus 512² and 128² resolve/blur work maps. It allocates 32 cached slots, with
the latter 16 reduced, and the runtime distinguishes static, dynamic, and car-driven
refresh schedules. `FUN_180c6d560` performs exponential resolve, separable filtering, and
slot composition; `FUN_180c6e6c0` schedules the complete atlas update.

The shipped caster shaders make the encoding concrete: `accShadowDynamic_main_ps`
clips casters inside `gLightClip`, writes radial distance times reciprocal shadow
range, and `accResolveDynamicShadow_ps` averages four MSAA samples of
`exp(gExponent * depth)`. Native state construction defaults the exponent to 20,
caps an omitted track range at 30 m (120 m for the identified car-attached path),
and computes automatic boost as
`10 - 7.5 * saturate((spotRadians - 0.6*pi) / (0.3*pi))`. The public receiver uses
`exp(bias - receiverDistance * exponent / range)` and the standard thickness form
`saturate(value * boost + 1 - boost)`.

Apex implements the editor-relevant static subset. Up to four authored spotlight
shadows among the nearest 32 active lights receive 512² cells in a cached 1024² R32F
atlas. A normalized RGBA8 fallback covers systems without float render targets.
Opaque and alpha-tested scene geometry writes recovered radial exponential depth.
When supported, the atlas uses four-sample color and depth renderbuffers. A fixed
resolve then averages the samples into the single-sample atlas. The renderer uses
the recovered 7-tap standard, value-aware 7-tap headlight, and 15-tap extra-blur
separable filters. Ordinary materials and GrassFX use the public one-sample receiver
equation, native normal-dependent bias, automatic or authored boost, and clip sphere.
GrassFX preserves the recovered 0.1 m receiver offset. Generated grass does not cast
local shadows. The portable path does not claim CSP's 32-slot packing or its
moving-car and dynamic refresh behavior.

A production Electron run tested the four-sample path on Monza's seven-file,
1,428-mesh workspace. `NIGHT_SMOOTH=1` activated 114 lights, including two authored
shadow lights. The camera selected one shadow light and 307 scene casters for the
local atlas. Chromium selected the four-sample R32F resolve and the standard
separable filter. The night frame hashed to `4fc183e6cbcda773`; every captured state
returned WebGL error zero, and the browser logged no exception. Animated GrassFX
wind makes this hash run-specific. The reported light, caster, and sample counts
prove that the local resolve executed.

The native surface setup names a separate `GrassFX: normals and AO` target and creates
it with format code `0x1c` (`RGBA8_UNORM`). Disassembly of the shipped
`sample_normal` pixel shaders used by Apex's supported `ksPerPixel`,
`ksPerPixelNM`, `ksMultilayer`, and multilayer-normal terrain paths shows encoded
normal XYZ in RGB and a literal `o0.w = 1`. Apex performs the same byte encode,
decode, and renormalization before using a substrate normal. This preserves even the
small positive X/Z bias produced when mathematical zero encodes as byte 128, while
the target AO and therefore the base wind-response multiplier remain exactly one for
those shader families.

The noise binding is independently visible in the installed binary. The generic
shader parser at `FUN_18109ac20` treats `txNoise` as a shared engine resource.
`FUN_18115e140` lazily loads `textures\\common\\noise.dds`, while the GrassFX
generation caller binds that shared SRV first in the eight-texture compute array,
followed by depth, color, normal/AO, deformation, adjustments, material parameters,
and air. The embedded DDS is 32×32 RGBA8 with five mips and SHA-256
`0de3092cc1207f8d0ae52d9b6e510b39d0831733a8a80a813b1bf433149adc72`; only mip 0
is required because both generation samples use explicit level zero.

The deformation path is likewise separable from the static editor. The installed
`[DEFORMATION]` profile specifies a 1024² map with a 100 m radius. In
`FUN_180c293a0`, `GrassFX: deformation 1`, `deformation 2`, and the resolved
`deformation` target all use DXGI format code `0x31` (`R8G8_UNORM`); the two air
targets use code `0x33` (`R8G8_SNORM`). `flgGrassMove_ps` preserves/decays the red
deformation history while replacing green occlusion from the current frame, and
`flgGrassPiece_ps` stamps moving billboards or meshes. With no moving runtime pieces,
both channels are zero, so the exact static-editor target is a neutral zero field.
Apex reports that state explicitly rather than inventing vehicle motion.

For future interactive-scene support, the recovered generation response is retained
as a tested pure contract. Red determines a maximum blade height; intermediate
contact forms a triangular squash term, an atlas-V exponent of
`1 / (1 + 2 * squash)`, up to 10% width growth/color darkening, a 20–100% height
multiplier, a 0.5–1 fade multiplier, and a 1–2 cm scaled lift. A red value of exactly
one selects the native cut state instead. The visible vertex path derives deformation
occlusion from four green taps as
`saturate(average * 1.6 - (flag ? 0.6 : 0.8))`, multiplied by its edge fade. Dynamic
stamping is intentionally outside the static track/car authoring scene.

Headless analysis of the installed CSP `dwrite.dll` found the settings constructor
at `FUN_180c35aa0`. Its literals confirm defaults of `0.5` for
`MASK_MAIN_THRESHOLD`, `0.05` for `MASK_RED_THRESHOLD`, `0.02` for
`MASK_MIN_LUMINANCE`, `0.35` for `MASK_MAX_LUMINANCE`, zero for `SHAPE_TIDY` and
`SHAPE_CUT`, and one for `SHAPE_SIZE` and `SHAPE_WIDTH`. The generation object at
`FUN_180c293a0` directly groups those fields with `cbuffer_mask_params`,
`cbuffer_generation`, `data_ac_ext_grass_fx::cbuffer_count`, and
`data_ac_ext_grass_fx::cbuffer_piece`. `FUN_180c31480` reads
`ORIGINAL_GRASS_MESHES` and `ORIGINAL_GRASS_MATERIALS` from the same `GRASS_FX`
section. These optimized native functions support field grouping and scalar defaults;
they do not establish the exact shader mask equation or edge rasterizer.

The public generation and material-pass shaders close the earlier mask-equation gap.
`colorThreshold()` normalizes the sampled RGB, rewards green plus red weighted by
`MASK_RED_THRESHOLD`, penalizes blue or half of red plus blue, multiplies that
difference by 10, and subtracts `MASK_MAIN_THRESHOLD`. The result is saturated and
then limited by two 20-wide linear gates around `MASK_MIN_LUMINANCE` and
`MASK_MAX_LUMINANCE`. Generation first rejects alpha below 0.7, then accepts only
when that continuous result is at least `0.2 + random * 0.3`. The GrassFX material
pass also clips below 0.2. This is not a binary red-channel mask.

The same generation shader samples the color target's alpha twice more after that
acceptance. Mip 2 is squared to form `edgeK`, and `colorTest` is multiplied by that
factor before fin width and height are scaled. When the post-pass shape multiplier is
greater than one, mip 3 clamps it with
`min(sizeMult, lerp(1, sizeMult, mip3Alpha))`. The shared mip generator builds each
level from 2×2 averages, so these alpha reads represent progressively wider surface
coverage rather than extra diffuse-mask thresholds. Apex applies the exact two
generation equations to a sparse world-space raster of decoded material color. Each
tile stores packed `R8G8B8A8` values, candidate color and mask decisions use bilinear
target samples at `COLOR_SAMPLE_MIP_LEVEL`, and recursively averaged target alpha
feeds the edge equations. Only tiles reached by preview samples are rasterized, but
each allocated tile uses CSP's recovered base texel spacing, source order, occluder
order, and depth ownership. Mesh boundaries and the top surface therefore feed both
color acceptance and edge shaping instead of per-material texture samples.

Further headless analysis closes the native surface-target layout and depth-state
gaps. `FUN_180c329f0` defaults `MAP_STEP` to 0.25, `MAP_SNAP` to 0.5, and the surface
`MAP_RESOLUTION` to 2048. `FUN_180c293a0` creates the color target as 2048²
`R8G8B8A8_UNORM` with up to eight mips and creates same-resolution one-mip normal/AO
and material-parameter targets. `FUN_180c2d890` derives a 256 m half-span from
`resolution * 0.5 * MAP_STEP`, moves the map center 246 m forward from the camera,
and rounds its X/Z position to a `2 * MAP_SNAP` grid. The top-down projection spans
-1200 to +1200 m vertically. The public generation shader reconstructs world height
as `1200 - depth * 2400`.

The render path at `FUN_180c2ea50` binds the engine depth state created by
`FUN_1810b5c90`: depth enabled, writes enabled, and comparison `LESS_EQUAL`.
Consequently a higher world-space surface has smaller projected depth and owns the
pixel; an equal-depth later draw also wins. Grass sources render in list order, then
occluders render through the inverse-alpha material pass. A lower occluder cannot
erase a higher source, while an equal or higher occluder can. Apex reproduces those
rules on a globally aligned 0.25 m lattice in lazy 32×32 tiles and generates mip
texels from exact 2×2 child averages. Camera-local generation bounds that lattice to
the physical 2048², 512 m target and clamps every base and mip lookup to its outer
texels, matching `samLinearClamp` and `samPointClamp`. Local
generation bilinearly gathers four cached surface texels, rejects missing/occluding
samples and the native discontinuity, and relocates the fin to the gathered height
instead of resolving a source triangle again. This follows the public compute shader's
depth-derived spawn path. The full-source stratifier remains an explicit authoring
fallback only when the framed camera window does not intersect any GrassFX bounds.

The target lifetime is a snapped-window redraw, not an incremental border copy.
`FUN_180c2d890` computes the desired center 10 m inside the 256 m half-span, rounds
each XZ component to the `2 * MAP_SNAP` grid, and compares it with the accepted
center. The embedded squared-distance threshold at `DAT_1821133bc` is exactly
`256.0`, so the accepted center remains unchanged until the snapped candidate moves
at least 16 m. `FUN_180c2ea50` returns immediately while that center is unchanged;
after a change it binds and redraws the complete color, depth, normal/AO, and
material-parameter targets. `FUN_180c2f990` applies the same accepted-center change
test before rebuilding the adjustment target. Apex preserves the accepted center
between debounced camera rebuilds and applies the same 16 m hysteresis.

The installed `extension/config/grass_fx.ini` supplies the generation profiles that
the native parser stores as `(spacing multiplier, coverage divisor)` pairs. The
installed medium profile (`QUALITY=2`) uses a 1024 generation-map resolution and
passes `(1,16)`, `(0.5,8)`, `(2,4)`, `(2,2)`, and `(4,1)`, with `MAP_STEP=0.25` and
`MAP_SNAP=1`. `FUN_180c2dbb0` iterates those pairs in order and
`FUN_180c2ad30` dispatches
`surfaceResolution / (32 * divisor * spacingMultiplier)` groups per axis. With the
shader's 32×32 threads, a pass therefore covers `512 / divisor` metres and spaces
candidates by `0.25 * spacingMultiplier` metres. The compute shader adds a stable
PCG-based jitter of ±half a cell, bilinearly gathers top-surface depth, rejects a
four-texel depth discontinuity above 0.0008 (1.92 m across the 2400 m depth span),
then applies frustum and distance-ring fading. This fully defines the native grid
schedule. Apex now rebuilds these five rings after debounced orbit, zoom, resize, and
track-camera changes. The exact native dispatch contains 868,352 threads; the UI CPU
samples 25,000 positions evenly across all passes and reports both counts.

The generation constant-buffer layout and `FUN_180c2ad30` close the distance-fade
inputs. Native code stores the unsnapped interpolated pass center in `gMapOrigin` and
stores `1 / (spawnEnd.x - spawnStart.x)`, equivalently `1 / passWidth`, in
`gMaxDistanceInv`. The shader computes
`fadeK=saturate(2-distance(pos, float3(gMapOrigin.x, cameraY, gMapOrigin.y))*3.2/passWidth)`,
rejects values at or below 0.01, and uses `lodFix=saturate(2*fadeK)` to sink the fin
by `min(height,0.2)*(1-lodFix)^2`. Apex carries `fadeK` as the eighteenth instance
float and emits it through multisample alpha-to-coverage while applying the same
cutoff and ground sink.

For `ksMultilayer`, the same source shows that the four detail textures are combined
as `detailR * mask.r + detailG * mask.g + detailB * mask.b + detailA * mask.a`, then
multiplied by the diffuse texture and `magicMult`. The GrassFX pass applies the
material ambient scalar to that albedo before the threshold texture is generated.
Apex reproduces this composition with bilinear repeating CPU samples, seasonal
diffuse adjustment, and `ksAmbient`; ordinary diffuse materials take the equivalent
single-texture path. The public stock helpers establish that `ksPerPixelNM`,
`ksPerPixelAT_NM`, and `ksPerPixelMultiMap` variants resolve tangent maps with
R/B/G coefficients in the tangent/normal/bitangent basis. Apex applies that exact
channel order to decoded `txNormal` mips. `ksMultilayer` remains geometric because
its GrassFX material pass does not sample a normal map; object-space maps, override-
resource textures remain fidelity gaps.

The native settings constructor at `FUN_180c266b0` shows that `MASK_BLUR` is not a
surface-color blur. It selects `flgGrassAdjustmentsBlur_ps_%s.fxo` from the four
`none`, `low`, `medium`, and `high` adjustment-map variants; the native default is
numeric level 1. Those public shaders sample the adjustment render target at fixed
mip biases and weighted cross-shaped neighborhoods. `none` reads mip 0 once; `low`
reads the five non-corner samples at mip 1 with unit offsets; `medium` uses the same
five-sample cross at mip 2 with two-texel offsets; and `high` reads 21 samples from a
radius-two mip-2 neighborhood with the four corners removed and two-texel offsets.
Every tap is weighted by `1 / (length(offset) + 0.25)`. The denominator begins at
0.01 and accumulates a tap only when `saturate(dot(sample, 10))` is nonzero. The same constructor reads
`COLOR_SAMPLE_MIP_LEVEL` separately, defaults it to zero, and caps it at the float
constant 2. Apex reports both values and uses the color mip when sampling decoded
material textures.

The native resource rebuild at `FUN_180c26440` closes the adjustment-target layout:
it creates a 1024² `R8G8B8A8_UNORM` raw target with at most four mip levels and a
separate one-mip `R8G8B8A8_UNORM` final target. `FUN_180c28490` clears the raw target
to zero, draws the ordered adjustment geometry through `FUN_180c25890`, builds the
raw mip chain, clears and binds the final target, and runs the selected fullscreen
blur shader. Grass generation then samples that final target bilinearly at mip 0.
The 512 m world window therefore gives the adjustment map a 0.5 m base texel step.
Apex reproduces that pipeline on a camera-local sparse lattice: lazy 32×32 raw
tiles retain ordered last-write geometry, recursively quantized 2×2 mips feed the
native kernel, each filtered texel is quantized back to RGBA8, and candidates
bilinearly sample four final texels. Raw-mip blur and final-target reads clamp at the
same 512 m outer border as the surface target.

The adjustment draw loop at `FUN_180c28490` walks the stored section vector from
beginning to end, so later geometry overwrites earlier RGBA values rather than taking
a per-channel maximum. `FUN_180c23680` also enumerates `EXTRA_n` records and stops
after eight. For each `_RANGE=inner,outer`, it packs
`RangeInv=1/(inner-outer)` and `MinDivRange=-outer/(inner-outer)`; equal endpoints
are separated by 0.001. The matching public shader rotates world X/Z around
`_CENTER`, scales the second axis by `_ASPECT_RATIO`, smoothsteps the packed range,
and sequentially lerps toward the extra's `MAP` color by that coverage times
`_OPACITY`, saturating after each area. `_ANGLE` is in degrees. Apex reproduces this
rule-major draw order and ellipse equation. It also safely expands the shipped
`GrassFX_ExtraArea` inline mixin syntax, where `Range-Fade` becomes the inner radius.

`flgGrassAdjustment.hlsl` establishes the texture-mask equation as
`saturate(MASK_BASE + r*MASK_R + g*MASK_G + b*MASK_B + a*MASK_A)`. For
`USE_MULTILAYER_MASK`, native code binds `txMask`; for
`USE_MULTIMAP_DETAIL_MASK`, it binds `txDiffuse`. The result is not a random choice:
`flgGrass_configurations.hlsl` starts from the base parameters and sequentially
lerps toward A, B, C, and D by the four sampled channels. Apex now applies that same
channel transform and interpolation for decoded embedded masks. Direct `MAP=A`–`D`
uses the same path. Negative channel vectors such as `MASK_G=-20` saturate the map
back to the base configuration, matching the intent of installed configs.

A production Chrome run on Mugello loaded all 122 embedded textures and
`grass_fx/highlands.dds` as BC3 with its configured 8×3 grid and brightness 0.85.
All 25 source meshes used decoded `ksMultilayer` surface samplers. Of 36,701 bounded
candidates, the source-derived continuous threshold rejected 22,378 and emitted
14,323 fins; the near pass rejected none for shape at preview year progress 0.5.
All source meshes routed to configuration A, with 12,724 base-row pieces and 1,599
weighted group-0 pieces. At that milestone, the GPU consumed the then-17-float instance layout in the
viewport, scene probe, and three 2048² shadow cascades. GrassFX-on, shadows-off, and
GrassFX-off captures all completed with WebGL error zero and no browser exception.
These state changes validate generation and GPU plumbing, not frame-exact agreement
with an in-game capture.

A production Zandvoort run exercises the sampled adjustment path. All 293 embedded
textures decoded; 52 source meshes provided surface samplers and 16 provided
multilayer adjustment samplers. The 38,511 bounded candidates produced 18,516 fins:
16,844 base, 1,439 A, 140 B, and 93 continuously mixed configurations after 16,617
color-mask, 20 occluder, and 3,358 shape rejections. The atlas, viewport, scene probe,
and three shadow cascades completed with WebGL error zero and no browser exception.
After world-normal propagation, counts remained identical and the GrassFX capture
hash changed from `05678d0fe8eb6c2b` to `1c8a80ae5c53ec01`; the GrassFX-off hash
remained `d186492775428464`. This isolates a visible change to generated fins while
preserving the underlying scene and generation population.

A follow-up production run exercised tangent-space substrate normals on the four
Zandvoort GrassFX source meshes using `ksPerPixelNM` and
`Collina401_d_normal_001_1_a.dds`. Four normal-map samplers evaluated 3,058 candidate
samples. Candidate, rejection, configuration, and final fin counts stayed identical;
the renderer reported `interpolated-world-vertex+tangent-space-map` and evaluated
43,748 fixed mip-2/mip-3 edge-alpha samples. Full-scene
GrassFX, shadows-off, and GrassFX-off captures produced hashes
`e1a2773c51dd3040`, `790e1f62954b67a0`, and `d92654add48f025a`, respectively, with
WebGL error zero and no browser exception.

The first sparse surface-target production run kept the earlier candidate and fin
counts while proving raster and mip stability. After top-owner candidate rejection,
the same 38,511 candidates exposed 13,566 covered lower-source/occluder samples and
3,175 depth discontinuities. With packed RGBA8 target color and bilinear candidate sampling,
814 candidates failed the material mask and 2,659 failed shape, leaving 18,297 fins.
The target rasterized 8,650 lazy 32×32 tiles (8,857,600 base texels), 6,292,601
accepted source fragments, and 741,816 occluder fragments, then evaluated 41,912
mip-2/mip-3 samples. The packed color tiles retain the same four raw bytes per base
texel as the previous float-alpha-only tiles. Full-scene GrassFX, shadows-off, and
GrassFX-off captures produced `b899adf70514e974`, `79adc3f80a273327`, and
`d92654add48f025a`; all three reported WebGL error zero and no browser exception.
The unchanged GrassFX-off image isolates the visible change to generated fins and
their shadow path.

The adjustment-target production run kept the same 38,511 candidates, 13,566
surface-ownership rejects, 3,175 depth-discontinuity rejects, and 814 material-mask
rejects. Its native-default low blur allocated 3,908 lazy raw tiles (4,001,792 base
texels), rasterized 1,145,281 ordered adjustment fragments, and evaluated 418,010
weighted taps for 83,602 cached final texels across 3,438 final tiles. The exact
five-tap count is visible as `finalTexels * 5`. Convolution shifted the surviving
configuration population to 14,679 base, 216 A, one B, and 2,829 continuous mixes;
3,231 shape rejects left 17,725 fins. Full-scene GrassFX, shadows-off, and GrassFX-off
captures produced `38874ef5e04081ee`, `abfcba0bc840eb81`, and
`d92654add48f025a`, respectively. All reported WebGL error zero and no browser
exception; the unchanged GrassFX-off hash again isolates the visible change to the
generated-fin path.

Brands Hatch is the only installed loaded-track config with active spatial adjustment
extras: four `GrassFX_ExtraArea` mixin calls attached to the `grass_ext` rule. A
production assembled run parsed all four, retained three supported adjustment draws,
and rasterized 2,029,212 rule-ordered fragments into 2,167 sparse raw tiles. Native
low blur evaluated 118,235 taps for 23,647 final texels. Of 25,322 bounded candidates,
the surface target rejected 14,692 ownership conflicts and 4,546 discontinuities;
the remaining mask and adjustment stages emitted 4,472 fins, including 78 pure B,
200 pure C, and 4,194 continuous mixes. The GrassFX, shadows-off, and GrassFX-off
capture hashes were `1e966de7e9d8cab3`, `934b67c93548f0ee`, and
`483089aacecb3ff5`. Every state returned WebGL error zero with no browser exception.

After camera scheduling was enabled, framing `Sector_EXT_5_SUB0` produced the exact
medium-profile native schedule: 868,352 possible threads across 32, 64, 128, 256,
and 512 m passes. The portable 25,000-sample budget assigned 471, 7,547, 1,887,
7,547, and 7,548 samples respectively. Sparse depth gathering rejected 17,320
positions without a four-texel surface and the camera frustum rejected 7,235. The
native distance cutoff rejected 160 more; 64 mask rejects left 221 pass-4 fins with
fade factors from 0.0122 to 0.408, rendered through alpha-to-coverage. The surface
path touched 227 lazy tiles and the adjustment path touched 102 raw tiles, 1,140
final texels, and 5,700 blur taps. GrassFX, shadows-off, and
GrassFX-off hashes remained `1e966de7e9d8cab3`, `934b67c93548f0ee`, and
`483089aacecb3ff5`; WebGL error was zero and the browser reported no exception.
The assembled whole-scene frame was displaced far outside source bounds by auxiliary
geometry, so its initial generation correctly reported the authoring fallback before
the selected-mesh frame activated native rings.

After replacing the preview value noise with the recovered shared texture, the same
selected Brands Hatch frame retained all deterministic scheduling and rejection
counts: 25,000 sampled threads, 17,320 no-surface rejects, 7,235 frustum rejects,
160 distance rejects, 64 mask rejects, and 221 surviving fins. Runtime diagnostics
reported `csp-common-noise-rgba8-mip0-linear-wrap` and the source DDS hash above;
WebGL error remained zero and the full unit suite passed 166/166.

After replacing the stable sinusoidal bend with the recovered atmospheric target,
an assembled Brands Hatch production run loaded 1,986 meshes and generated 2,672
camera-ring fins using the extended instance record. Diagnostics reported
`csp-r16f-linear-repeat`, a 64×64 target, 32 pulses, the static-editor zero air map,
and the selected 5 m/s at 50° authoring wind. The target advanced 380 update steps
during the run; visible, cubemap, cascade-shadow, and probe-shadow grass programs all
compiled, and WebGL error remained zero. A follow-up production run reported
`interpolated-world-vertex+tangent-space-map+rgba8-normal-ao-target-alpha1`, retained
the same 2,672 fins, and kept WebGL error zero. The full unit suite passed 169/169,
including scalar checks for pulse respawn order, lifetime/advection, target decay,
the centered DXBC pulse equation, and the normal-target byte round-trip.

After carrying the target-quantized substrate normal into the instance buffer, a
second assembled Brands Hatch production run generated the same 2,672 fins with a
21-float stride. Visible, cubemap, cascade-shadow, and probe-shadow grass paths used
the substrate-aligned local frame and compiled without browser exceptions. The
GrassFX and GrassFX-off captures produced `6e7e71a7474d95db` and
`ded879e9cc5825d8`; WebGL error remained zero and the full unit suite passed 169/169.

After enabling the physical target border and refresh lifecycle, the same production
frame accepted target center `(-202, 174)`, reported the native 2048² surface window
and 16 m gate, and generated 2,671 fins. Clamp sampling removed one outer-pass fin;
the sparse surface allocation fell from 776 to 775 tiles and the filtered adjustment
target from 9,408 to 9,402 texels. Visible, cubemap, cascade-shadow, and probe-shadow
paths remained active. GrassFX and GrassFX-off hashes were `63fe0798588e9d63` and
`6c0c40b1f7f04808`, WebGL error was zero, no browser exception occurred, and the
full unit suite passed 170/170.

With native repeat widening enabled, the accepted target and 2,671-instance count
remained unchanged in the production Brands Hatch frame. GrassFX and GrassFX-off
hashes became `b71feb1d5fb7e4a5` and `c93ebceb0d39a76d`; visible, cubemap,
cascade-shadow, and probe-shadow paths remained active, WebGL error was zero, and no
browser exception occurred. The full unit suite passed 171/171.

After recovering the installed `custom_l` generation and pixel equations, the same
Brands Hatch frame retained target center `(-202, 174)` and all 2,671 fins with a
24-float instance stride. The viewport, cubemap, cascade-shadow, and probe-shadow
paths stayed active; the GrassFX and GrassFX-off hashes were
`32b63ff56097e405` and `33b0ab4f549d7120`. WebGL error was zero, the browser
reported no exception, and the full unit suite passed 172/172.

Tracing the `custom_l` pixel output and vertex lighting payload then separated
coverage from distance fade and added the native direct root ramp plus view-angle
ambient attenuation. The same production scene retained 2,671 fins and all four
GrassFX render paths; on/off hashes became `33546226fbbe159a` and
`095d9e6c87a72361`. WebGL error remained zero, no browser exception occurred, and
the full unit suite passed 173/173.

Adding the native RGBA8 material-parameter target kept the same Brands Hatch target
center and all 2,671 fins while extending the instance record to 26 floats. All 47
source meshes supplied material samples, including 12 multilayer paths. Viewport,
cubemap, cascade-shadow, and probe-shadow grass remained active; GrassFX on/off
hashes became `c940589c263be366` and `defdfcbab4d37453`. Both states returned WebGL
error zero, the browser reported no exception, and the full suite passed 174/174.

The static deformation-contract run retained the same target center, 26-float layout,
and 2,671 fins. Production diagnostics report
`static-editor-zero-rg8-unorm`, resolution 1024, radius 100 m, and deformation
occlusion disabled. GrassFX on/off captures were `3f5909384dcce936` and
`a0961d1464618b6d`; both returned WebGL error zero with no browser exception. The
time-driven wind field means those framebuffer hashes are run-specific, while the
unchanged instance population and explicit target diagnostics validate this neutral
editor state. The full suite passed 175/175.

The wet-grass production run enabled both GrassFX and RainFX at wetness one. Brands
Hatch retained the same target center, 26-float layout, all 2,671 fins, 47 material
samplers, and 12 multilayer material paths. The shared viewport/cubemap grass path,
all cascade and probe shadows, and the RainFX material overlay remained active. The
wet GrassFX capture hash was `fa63715f0c776b81`; the GrassFX-disabled and
RainFX-disabled hashes were `da93a5f770bfbd05` and `5d5e9e58671ac32b`. Every state
returned WebGL error zero with no browser exception, and the full suite passed
176/176. Wind makes these framebuffer hashes run-specific; the explicit wetness-one
diagnostic and distinct toggle states validate the integrated path rather than pixel
equality with CSP.

Adding the pixel-side `fakeShadow` term preserved the same 2,671 fins, 26-float
instances, material/normal target coverage, shared viewport/cubemap grass, and all
grass shadow paths. With RainFX wetness one, the GrassFX capture hash became
`2ade74fe01cfa02e`; GrassFX-disabled and RainFX-disabled captures were
`da93a5f770bfbd05` and `3d8a1f8a5bac95a1`. All states returned WebGL error zero,
the browser logged no exception, and the full suite passed 177/177. As with the wet
run, animated wind makes the framebuffer hashes run-specific.

After adding the host-confirmed zero-pad directional backlight, a fresh assembled
Brands Hatch GP run generated 890 camera-visible fins from the exact 868,352-thread
ring schedule through the 25,000-position CPU preview budget. All 47 surface and
material samplers, 12 multilayer paths, the viewport/cubemap grass draws, and cascade
and probe shadow paths remained active; RainFX wetness was one. GrassFX,
shadows-disabled, GrassFX-disabled, RainFX-disabled, and low-sun captures hashed to
`3923bdacee721f84`, `17fb5b0be59792f2`, `5307e5f40347a75e`,
`1f8787ab3144daff`, and `87474c6c30242fed`. Every state returned WebGL error zero,
the browser recorded no exception, and the full suite passed 178/178. Animated wind
and the current camera make these framebuffer hashes run-specific.

The local-light production run loaded the seven-file Brands Hatch GP workspace and
explicitly set `SMOOTH_SUN_B=1`. The condition-state assertion observed two active
CSP lights bound to the GrassFX program before capture. The frame retained 1,971
camera-visible fins, all 47 surface/material samplers, 12 multilayer paths, the
shared reflection-probe grass draw, and all three grass shadow cascades. Baseline
GrassFX and active-condition captures hashed to `df3edb67242843dd` and
`84c010bb9c51a6b0`; the active state returned WebGL error zero and the browser logged
no exception. Animated wind makes the hashes run-specific, while the asserted light
count proves that this capture exercised the new path. The complete suite passed
179/179.

The local-shadow production run loaded Monza's seven-file, 1,428-mesh workspace and
set `NIGHT_SMOOTH=1` while framing `Object092`, 4.5 m from the first authored shadow
light. Evaluation produced 151 lights: 149 active, two shadow-capable, 32 bound to
GrassFX, and one selected into the local atlas. The cached cell used 307 effective
scene casters while the camera-local GrassFX regeneration emitted 246 fins. The
atlas-enabled condition capture hashed to `cac0bcb584a4988b`; baseline GrassFX,
shadows-disabled, reflections-disabled, GrassFX-disabled, RainFX-disabled, and
low-sun captures hashed to `278e769ccf9a4717`, `3d84ca744d2a100a`,
`32b46c62a9d51f4b`, `5ebb7a64a4f4ff32`, `f13984d7be4c6fd9`, and
`0db1cfe6888a2549`. Every state returned WebGL error zero, the browser logged no
exception, and the complete suite passed 180/180. Animated wind makes framebuffer
hashes run-specific; the asserted shadow-light and caster counts prove atlas use.

A follow-up Monza run enabled the recovered camera-distance fade. The sampled nearby
shadow light exposed its authored 500 m center and 200 m smooth width, remained in
the 32-light set, and retained one local-shadow atlas cell. Its night-condition frame
hashed to `f3a4b20a30f1b3b0`; baseline GrassFX, shadows-disabled, GrassFX-disabled,
and low-sun frames hashed to `7a41375e4a539624`, `f26c4fee01b15213`,
`657c001c49c785b5`, and `39caf9b42c546cb7`. All returned WebGL error zero with no
browser exception, and the complete suite passed 181/181.

The packed-cone production run used the same Monza workspace and nearby
`NIGHT_SMOOTH` spotlight (`SPOT=147.1`, `SPOT_SHARPNESS=0.5`). The active condition
bound 32 lights to GrassFX and retained one local-shadow atlas cell. Its condition,
baseline GrassFX, shadows-disabled, GrassFX-disabled, RainFX-disabled, and low-sun
captures hashed to `0f7ba0e670b9bb45`, `410b66d31163423d`,
`351699219cc3eaa7`, `58fe552423aa8e16`, `9ca4c22edbbf6d19`, and
`2297e14547eb4786`. Every capture returned WebGL error zero and the browser logged no
exception. The Nissan 370Z car-side check evaluated five authored self-lights; its
baseline and `HEADLIGHTS=1` captures differed at `d7d0ca682a1c3d4a` and
`baa33f0d88c54c7a`, again with WebGL error zero and no browser exception. Animated
wind makes the track hashes run-specific. The complete suite passed 182/182.

The radial-ESM follow-up repeated the assembled Monza night case after replacing the
portable PCF atlas. It loaded 1,428 meshes, selected one of two active authored shadow
lights into the R32F cell atlas, bound 32 local lights to 246 generated fins, and used
307 effective opaque/cutout scene casters. The ESM-enabled night frame hashed to
`84842b0d9b6f2c6e`; baseline GrassFX, shadows-disabled, and GrassFX-disabled captures
hashed to `b16756e995341815`, `27a4662cd098d557`, and `137a01e9adcca7bd`. Every
capture returned WebGL error zero, no browser exception was logged, and the complete
suite passed 189/189. Animated wind makes the framebuffer hashes run-specific.

The finite-line follow-up loaded the same seven-file Monza workspace. It retained the
three authored line sections as three logical segments rather than expanding them into
point samples; the full config evaluated 116 lights, with 114 active under
`NIGHT_SMOOTH=1`. One inspected segment ran from
`[-143.233, 2.82469, 72.852]` to `[-149.749, 2.82469, 130.984]`, stored the derived
inverse squared length `0.00029224490047845394`, and preserved distinct active endpoint
colors `[17.6518605, 17.6518605, 17.6518605]` and
`[7.93987, 7.93987, 7.93987]`. Camera selection uploaded 15 nearby lights to both the
ordinary and GrassFX receivers. The programs compiled and returned WebGL error zero.

The camera-occluder production check loaded the Brands Hatch GP layout and all five
installed wall records, with recovered global settings `1000/0.5`. Forcing its ten
condition controls active produced 31 active CSP light records. At the inspected
camera and target, four records from `LIGHT_SERIES_7` and `LIGHT_8` were behind an
authored wall; 24 survived both occlusion and normal camera relevance/fade selection
for GPU upload. The complete scene returned WebGL error zero. The reusable smoke
driver's combined GrassFX-condition mode was stopped because it expects a
shadow-capable active light, which this configuration does not contain; the
application itself had completed normally.

The receiver/view production check loaded the 41.8 MB Kunos Lamborghini Sesto
Elemento and its installed CSP config. Five generated light instances included two
exterior, track-disabled headlamp records, one authored interior-only, track-disabled
dashboard record, and two unrestricted rear records. With the vehicle inputs off,
the main pass selected two active records in exterior view and three after switching
to interior view; the change is the dashboard record joining the two unrestricted
rear lights. The template-expanded records retained 10 m camera fade rather than the
generic 200 m default. All 78 textures decoded, the frame used 193 shadow casters,
and both view states returned WebGL error zero.
The complete unit suite passed 195/195.

The optional-light production run loaded the complete Kunos P4/5 2011 KN5 and its
installed CSP config. Final evaluation produced three lights. The two expanded headlamps
uploaded secondary cone starts `-1.36602499`, primary-relative inverse widths
`0.23619821`, and range records `[0.0625, 0, 0.20833333, 0.21]`, exactly matching
the authored 16 m range, 0.3 normalized skip, and 0.21 intensity. Their authored
`SPOT_EDGE=0.12,0.12,0.12`, sharpness 10, and transformed `SPOT_UP` uploaded as
`[1.2,1.2,1.2]` and `[0,10,0]`. The main and GrassFX programs compiled, the frame
returned WebGL error zero with no browser exception, and the capture hashed to
`bf7b6534dd9a0f72`. The complete suite passed 184/184, including CPU/GPU packing,
configuration expansion, and an end-to-end hollow-cone/RGB-edge GrassFX response.

The binding follow-up reloaded the same production car with a clean input state.
All three decoded lights reported `HEADLIGHTS`; at input zero, the renderer selected
zero active lights and uploaded `cspLightCount=0`. Moving the generated HEADLIGHTS
control to one activated all three lights: the blue bound extra evaluated to
`[0,0,6.7]`, the two primary headlamps to `[10,8.40915,7.21182]`, and the GPU
uploaded `cspLightCount=3`. Both states returned WebGL error zero, and the active
frame hashed to `bf7b6534dd9a0f72`.

At a half-state production check, the headlamps moved from their authored off height
`0.501263` m to `1.7506315` m, range interpolated from 30 m to 135 m, and camera fade
center/width moved from `135/15` m to `292.5/32.5` m. The GPU position/range record
was `[±0.807086,1.7506315,1.71771,135]`; at full state it became
`[±0.807086,3,1.71771,240]`. The formerly duplicated `MIRROR=0` blue lamp is now one
instance. Off, half, and on states all returned WebGL error zero, and the complete
suite passed 186/186.

A production Chrome run on Imola matched 78 source meshes and 17,398 source
triangles. An earlier per-source-mesh stratification run considered 22,724 bounded candidates over
99,537,300.64 square metres, rejected 8,407 through the decoded diffuse/multilayer
mask and 2,780 through configured occluders, and uploaded 11,537 instances. All 110 embedded textures
loaded, including 60 BC3, 26 BC1, and two BC2 textures. The selected infield patch
produced distinct assembled on/off hashes `d555fd5e3a6c501e` and
`3e7f1373dee7e26e`; both captures had WebGL error zero and no JavaScript exceptions.
The preview deliberately does not claim full GPU-thread density.

## RainFX evidence

Thirty installed loaded track configs contain `[RAIN_FX]`. Of those, 29 declare
puddle, soaking, and rough material selectors; 28 declare smooth selectors; and 21
declare line selectors. Across the same configs, 371 `STREAM_EDGE_...` entries, 120
`STREAM_POINT_...` entries, and four `STREAM_WALL_EDGE_...` entries provide explicit
world-space drainage geometry. Less-common fields include puddle, soaking, rough,
and line mesh selectors, line-filter materials, and relief materials.

Apex parses those categories independently because a mesh can be both soaked and
puddle-capable. Material and mesh expressions use the same CSP wildcard/compound
selector evaluator as shader replacements. Each mesh retains a bit set and readable
category list; malformed stream vectors are diagnosed instead of partially applied.
The renderer exposes a continuous wetness control. Soaking darkens the material,
smooth and line categories raise gloss, rough materials receive a weaker response,
and upward-facing puddle surfaces receive deterministic world-space breakup with a
flattened water-layer normal. Configured stream edges, wall edges, and points are
drawn as an authoring diagnostic.

Native strings in the installed `dwrite.dll` expose the category-format fields
`PUDDLES_%s`, `SOAKING_%s`, `LINES_%s`, `LINES_FILTER_%s`, and `RELIEF_%s`, together
with `STREAM_EDGE`, `STREAM_WALL_EDGE`, and `STREAM_POINT`. All three stream keys are
read by `FUN_180d099d0`, which then constructs the `RAIN_FX_STREAM` data. The RainFX
object constructor `FUN_1811daf30` allocates and initializes a `0x490`-byte
`ac_ext::data_ac_ext_rain_fx::cbuffer_data`; nearby native paths reference the CSP
puddles, hit, and drop textures. This confirms category/stream grouping and a
separate runtime shader data path, but optimized code and shader bytecode do not
establish Apex's procedural puddle equation as native behavior.

On the complete Imola KN5, the production evaluator classifies 314 unique meshes:
94 puddle-capable, 47 soaking, 179 smooth, 39 rough, and two line meshes. Its four
stream edges upload as eight line vertices. A Chrome run at full wetness loaded all
110 embedded textures and produced wet/dry hashes `b84509f5429e529f` and
`bfe00c1943136ff0`, with WebGL error zero and no JavaScript exceptions. Visual
inspection showed a darkened, reflective road surface and the configured stream in
the expected scene region. Dynamic water accumulation, drainage simulation,
height-field puddles, rain occlusion, surface hits, spray, tyre interaction, and
windscreen effects remain explicit limitations.

### Ghidra light-parser evidence

A bounded headless analysis of the installed `dwrite.dll` was saved as an x86-64 PE
project outside the repository. The reusable scripts in `tools/ghidra` locate raw
ASCII/UTF-16 fields, list native xrefs, and emit either full decompilation or small
address-centered snippets. Relevant results from this installed CSP build are:

- `LIGHT_SERIES` at `1821052e0` is referenced by `FUN_1813b4190`;
- the shared `CLUSTER_THRESHOLD` string at `1820c8618` is read by several paths,
  including `FUN_1813b0cb0`, whose default literal is `0x41200000` (`10.0f`);
- the series parser `FUN_1813b6080` reads `RANGE_GRADIENT_OFFSET` and
  `SPOT_SHARPNESS`; its sharpness default is `0x3f4ccccd` (`0.8f`);
- `SELF_LIGHTNING` and `BOUND_EMISSIVE_MAX` both lead to `FUN_181355e00`, together
  with a `RANGE_GRADIENT_OFFSET` read, confirming they belong to the same explicit
  light construction path.

The whole-DLL auto-analysis hit its five-minute bound and produced speculative
decompiler warnings in unrelated optimized regions. These addresses are therefore
used only to confirm local field grouping and scalar defaults, together with the
installed configs and published config reference—not as proof of full CSP behavior.

## Packed data.acd evidence

The car data archive has an optional little-endian `-1111` marker and a second
header value. Each record then stores an unencrypted length-prefixed path, a
plaintext byte count, and four container bytes per plaintext byte. Only the first
byte in each group participates in decoding. The password is the hyphen-separated
decimal form of eight bytes derived from the lower-case car directory name; its
ASCII characters are subtracted cyclically from the selected container bytes.

The implementation preserves C# signed 32-bit overflow and truncating division in
that derivation, checks every length before allocation, retains decoded values as
bytes, rejects unsafe virtual paths, and diagnoses duplicates rather than guessing.
It does not write or replace archives. The installed official
`ks_nissan_370z/data.acd` consumes all 164,326 container bytes, yields 46 files and
40,817 decoded bytes, and exposes readable `car.ini` and `lods.ini`. A second
installed archive's decoded `lods.ini` matches its unpacked copy byte-for-byte.

A production Chrome run selected the Nissan directory, discovered the virtual
`data/lods.ini`, assembled four KN5 files (43.5 MB), loaded all 82 textures, and
forced LOD 2 without a JavaScript or WebGL error.

## Car collider evidence

Page 36 of the installed Kunos car-pipeline guide requires `collider.kn5` to be a
simple closed solid, use the non-rendered `GL` collision material, contain no
textures, keep its pivot at `0,0,0` with wheel-dummy orientation, stay above the car
floor, and target no more than 40–60 triangles. Apex validates the directly
measurable parts, welds coincident positions before counting topology edges, and
labels transform and visible-floor comparisons as warnings where author review is
still required. It also reads packed or unpacked `colliders.ini` bottom-contact
boxes with finite-vector, positive-size, duplicate, and sequence diagnostics.

The installed Nissan collider has one mesh, 28 vertices, 52 triangles, no textures,
one `GL` shader, identity hierarchy transforms, zero degenerate triangles, and 78
edges each shared by exactly two faces. The combined production-browser car test
reports zero collider errors or warnings and also exposes its packed bottom box.
A wider audit parsed 124 installed `ks_*` colliders. It showed that many shipping
Kunos cars exceed the older guide's polygon budget, so budget excess remains a
warning; material, texture, and open/non-manifold topology violations remain errors.
The renderer can upload this separate model to an independent edge buffer and draw
an orange, always-visible cage over any automatic or forced car LOD without adding
the collider to the editable/exportable scene. The retained overlay capture is
coherent and completes with WebGL error zero.

## Car hierarchy evidence

Pages 13–14 of the installed car-pipeline guide list 14 hierarchy nodes that should
exist in every LOD: four `SUSP_*`, four `WHEEL_*`, `COCKPIT_LR`, `STEER_LR`, and
four `DISC_*` nodes. Pages 17–18 define +Z as the front direction and require wheel,
rim, and brake-disc pivots to share the wheel center. The guide also says LOD 0 owns
`COCKPIT_HR` and `STEER_HR`, while reduced LODs should omit them.

Apex audits each source KN5 independently, so repeated functional names across LOD
files are valid. It checks required-name coverage, node kinds, duplicate hierarchy
names, left/right X ordering, +Z front ordering, wheel/suspension/disc/rim pivot
distance, and wheel-pivot drift from LOD 0. The official Nissan has all 14 listed
nodes in all four LODs and no axis or pivot error. One duplicated non-required brake
helper in LOD C remains visible as a warning.

A 124-car Kunos sweep showed that shipping reduced LODs often omit guide-listed
cockpit, steering, disc, or suspension nodes while continuing to work in game.
Missing and duplicated guide nodes are therefore compatibility warnings, not hard
errors. A required name bound to a mesh instead of a hierarchy node, or reversed
left/right and +Z-forward wheel axes, remains an error.

## Driver rig evidence

The installed `driver_base_pos.knh` files use a compact recursive layout: UTF-8 name
length and bytes, 16 little-endian floats for the local transform, a 32-bit child
count, and then each child. The Nissan file consumes all 6,430 bytes and contains 71
nodes, including the center, head, both hands, fingers, legs, and feet. The parser
bounds every field, rejects non-finite matrices, limits depth/counts, and rejects
trailing data.

The packed Nissan `driver3d.ini` selects `driver_no_HANS`, positions it at the
origin, references `steer.ksanim` with 360° lock, supplies shift timing, and hides
the helmet and face in cockpit view. Its 100-frame steering clip has 41 animated
tracks and all 41 match the KNH pose; its shift clip has 17 animated tracks and all
17 match. The browser exposes these counts with zero findings.

Because the referenced driver KN5 lives in the shared `content/driver` directory,
the browser accepts that model separately, verifies its configured basename, applies
59 matching KNH local transforms, and attaches it as auxiliary geometry that remains
visible across car LOD switches. Skinned KN5 vertices use four float weights and four
float bone indices; each live palette matrix is the current bone-world matrix times
the stored inverse-bind matrix. On the installed driver, applying only the base pose
reproduces 1,000 sampled source positions with average error below `0.00001` m and
maximum error below `0.002` m.

A production Chrome run assembled the four Nissan LOD files and shared driver into a
five-KN5, 57.3 MB workspace, loaded all 99 textures, and rendered the driver posed in
the seat. Moving the steering clip from frame 0 to frame 99 bound all 41 tracks,
skinned four meshes and 5,268 vertices, produced `0.793157` m maximum vertex motion,
changed an isolated glove capture hash, and reported no JavaScript or WebGL errors.
The two configured cockpit-hidden names both match exactly and hide six mesh
descendants, including the helmet glass and plastic, only inside the auxiliary driver
subtree. A second assembled capture kept forced LOD 0 active and visibly removed the
helmet and face without affecting the car; it also reported no JavaScript or WebGL
errors. Automatic shared-driver lookup remains unfinished because the browser cannot
read the sibling `content/driver` directory without the user granting access. Within
the granted file set, Apex now auto-selects one exact configured KN5 basename and
rejects ambiguous duplicates instead of guessing.

## Track camera evidence

Installed Kunos track camera files use a version-3 `[HEADER]` with a declared
`CAMERA_COUNT` and contiguous `[CAMERA_n]` sections. Each camera supplies a
world-space position, normalized forward/up basis, min/max FOV, normalized lap
interval (or `-1/-1` for unbound start cameras), clip planes, exposure and DOF
metadata, an optional spline, and a fixed-camera flag. Apex checks the declared
count, section sequence, finite vectors, basis normalization and orthogonality, FOV
ordering, lap interval, and clip planes before exposing the camera to the viewport.

All 12 cameras in Imola's primary TV set and all six fixed start cameras parse
without warnings. Across its five installed sets, the only warning identifies a
shipping `cameras_2.ini` entry whose `MIN_FOV=20` exceeds `MAX_FOV=12`; it is
retained and previewable rather than silently reordered. A production Chrome run
assembled Imola's four KN5 files (398.5 MB), loaded all 122 textures, discovered five
sets and 62 cameras, and rendered the primary `start` camera at its configured
position with a 5° midpoint FOV. The retained pit-straight capture is spatially
coherent and reports no JavaScript or WebGL errors. Spline motion and live
car-follow targeting were not simulated in that first proof.

PDB-guided Ghidra decompilation resolves the spline runtime precisely.
`CameraTrack::loadSet` at `0x1400cb670` loads each referenced CSV through
`Spline::loadFromCSV` at `0x1401ee3c0`, then applies a world-Y axis-angle matrix
using `SPLINE_ROTATION × π/180` through `Spline::applyMatrix` at
`0x1401ed730`. `CameraTrack::update` at `0x1400cbe30` advances animation time,
resets it when a camera is triggered, divides by `SPLINE_ANIMATION_LENGTH`, samples
`Spline::getPointSmooth` at `0x1401ee050`, and adds the resulting XYZ offset
directly to the camera's base translation. Spline cameras always use `MIN_FOV` and
target the focused car's transformed steering origin plus the manager's upward
offset.

`getPointSmooth` is adjacent-point linear interpolation, but its shipping
implementation intentionally or accidentally selects a segment with
`(pointCount−1) × clamp(t) × 0.9990000129` and divides the remainder by
`1/pointCount`. Apex reproduces that observed behavior, including its possible
control-point overshoot, rather than substituting conventional interpolation.

A second production Chrome run sampled Imola's `up2_5m_soft.csv` at normalized
position 1. The preview moved from `(-22.396, -83.1896, -429.544)` to
`(-22.396, -80.689645, -429.544)`, an observed `+2.499955 m` Y offset, and used
the runtime `MIN_FOV=4°`. The capture hash changed and both JavaScript and WebGL
error lists remained empty. Without a live focused car, Apex retains the saved
forward/up basis while moving along the exact position path; focused-car targeting
remains an explicit limitation. A browser playback check advanced the same 12-second
spline to normalized `0.153733`, produced the sampled `+0.151475 m` Y offset,
kept FOV at 4°, loaded all 122 textures, and reported no JavaScript or WebGL errors.

## Track surface physics evidence

PDB-guided decompilation also resolves the stock surface pipeline. The
`SurfacesManager` constructor at `0x1401ae9b0` first inserts a built-in `WALL`
definition with collision category 2, loads `system/data/surfaces.ini`, then loads
the selected track or layout's `data/surfaces.ini`. `loadSurfaceDefinitions` at
`0x1401afad0` assigns each definition through the map's keyed `operator[]`, so a
track key replaces the same system key while new keys remain additive.

`TrackAvatar::processPhysicsNode` at `0x1401cc5e0` calls
`NKUtils::getSectorID` at `0x14018d2b0`; every mesh whose leading parsed integer is
nonzero is submitted through `TrackAvatar::addPhysicsMesh` at `0x1401c78e0`.
`TrackAvatar::getSurfaceDescFromMeshName` at `0x1401c8300` uppercases the complete
mesh name, and `SurfacesManager::getSurface` at `0x1401af340` collects every runtime
key found anywhere in that name. Exactly one match selects a definition. No match
uses the game's default surface fallback, while multiple matches are reported as
ambiguous and also use the fallback. This is substring matching, not removal of a
fixed-width numeric prefix.

A production Chrome audit assembled Imola's four KN5 files and resolved all 375
physics meshes against 15 runtime surfaces: built-in `WALL`, the stock system keys,
and the track definitions with its local `KERB` override. It reported 89 wall, 20
road, 32 grass, 35 kerb, 37 sand, and 162 track-specific bindings, with zero
fallbacks, zero ambiguities, zero audit errors, and zero audit warnings. All 122
textures loaded and the exact spline-camera endpoint proof remained unchanged.
The same resolver now drives a viewport mode that renders the physics geometry even
when those KN5 meshes are hidden from the normal visual scene. A production overlay
capture colored all 375 bindings, retained a coherent continuous circuit and surface
boundaries, produced hash `a4d6d0ca8bfbe84b`, and reported no JavaScript or WebGL
errors.

The surface editor preserves the native section index and all 14 parsed fields.
These fields cover matching, grip, damping, dirt, penalties, validity, pitlane state,
surface waves, vibration, sound, pitch, and force feedback. The project stores edits
separately from CSP overrides and writes a standalone `surfaces.ini`.

A production WebGL test changed `KEY=ROAD` to `KEY=TARMAC`. The live audit changed
one mesh from fallback physics to an exact match. The same test covered all fields,
invalid input, undo, redo, recovery, the surface overlay, and export. A serializer
round trip preserved every field and rejected unsafe INI text.

## Track layout assembly evidence

Installed tracks use ordered `[MODEL_n]` sections in `models*.ini`, with `FILE`,
`POSITION`, and `ROTATION` fields. Other section families such as
`[DYNAMIC_OBJECT_n]` have different runtime semantics and are not treated as static
layout geometry.

Dynamic sections preserve `PROBABILITY`, `MULT`, `POS_MODE`, `RND_POS_CENTER`,
`RND_POS_RANGE`, `VEL_MODE`, `RND_VEL_BASE`, `RND_VEL_RANGE`, and `PLAY_WAV`.
Apex edits these native fields and writes them back to `models.ini`. The preview uses
`RND_POS_CENTER` as the deterministic model-root position.

The preview does not simulate position modes, velocity modes, or random ranges.
Those behaviors remain a separate native-motion recovery task. A production WebGL
test covered all nine fields, validation, undo, redo, recovery, and export.
The serializer round-trip test also proves that audio removal omits `PLAY_WAV`.

The installed `acs.exe` includes matching PDB symbols. Its `TrackAvatar::init3D`
function at `1401c8740` reads `POSITION` and `ROTATION` with `INIReader::getFloat3`,
multiplies all rotation components by `0x3c8ef998` (pi/180), calls
`mat44f::createFromEuler`, right-multiplies the loaded model-root transform by that
matrix, and then writes the requested position into matrix fields M41–M43. The
`mat44f::createFromEuler` function at `140118ef0` constructs Y(-x), X(y), and Z(z)
axis rotations in sequence and performs the engine's final coordinate-sign
conversion. Apex reproduces the resulting row-major transform as
Y(-x) × X(-y) × Z(-z), stored in the same layout as KN5 node matrices.

The workspace merger wraps each KN5 root with that placement, offsets mesh material
IDs as material tables are concatenated, and applies later same-name textures in
manifest order while reporting every collision. A complete installed
`hickory/models.ini` check assembled `hickory.kn5` and `pobjects.kn5`: 272 nodes,
207 meshes, 88 materials, 87 textures, 137,870 triangles, and 24,682,423 bytes were
consumed with no texture-name collision.

## Car LOD workspace evidence

Installed unpacked cars define an ordered, contiguous sequence of `[LOD_n]` sections
in `data/lods.ini`. Each section supplies `FILE`, `IN`, and `OUT`; optional
`[COCKPIT_HR]` and `[DRIVER_HR]` sections supply `DISTANCE_SWITCH`.

The PDB-labeled `CarLodManager::CarLodManager` function at `1400e2f80` in the
installed `acs.exe` constructs `LOD_0`, `LOD_1`, and subsequent section names until
`INIReader::hasSection` fails. It reads `FILE` with `INIReader::getString` and both
`IN` and `OUT` with `INIReader::getFloat`. Apex therefore stops at the first missing
index and reports any later sections as game-ignored instead of silently compacting
the sequence.

`CarLodManager::updateLodVisibility` at `1400e5810` computes camera-to-car distance,
then normalizes it as `distance × FOV / 60 / lodDistDivisor`; track-camera mode
multiplies the divisor by ten. Its visibility comparison is exactly
`IN <= normalizedDistance && normalizedDistance < OUT`. Apex uses those half-open
bands and applies the same FOV normalization for its fixed 45° orbit camera. A forced
LOD mode remains available for authoring inspection independent of camera position.

The complete installed `traffic_toyota_prius/data/lods.ini` fixture loads four KN5s
covering 0–15, 15–45, 45–201, and 201–2000 metres. The merged result has 272 nodes,
165 meshes, 157 material records, and 11 unique texture names; all 5,799,334 KN5
bytes are consumed. Its `COCKPIT_HR` switch is preserved at 70 metres and the manifest
produces no range diagnostic.

A Chrome 151 WebGL smoke loaded that folder through the same directory-picker and
manifest-discovery path as the UI. All 11 embedded textures decoded (five BC3, two
BC1, and four masked raw DDS images), with zero unsupported textures, JavaScript
exceptions, or WebGL errors. The automatic LOD-0 assembled capture and a forced
LOD-1 capture were both coherent and produced distinct image hashes. Car workspaces
keep repeated embedded texture names scoped to their source KN5, so a lower LOD
cannot silently replace the higher LOD's same-name payload.

## Stock skin texture evidence

`CarLodManager::loadLod` at `1400e4370` calls `CarAvatar::getCurrentSkin`, constructs
`content/cars/<unixName>/skins/<skin>`, and passes that directory to
`KN5IO::addTextureFolder` before `KN5IO::load` reads each LOD. `KN5IO::loadTexture`
at `1402171a0` calls `Path::getFileName` for the KN5 texture and then
`KN5IO::getSkinOverridenTexturePath`. The latter function at `140214f90` checks
`<textureFolder>/<filename>` in folder order with `Path::fileExists` and returns the
first match. This confirms that replacement is by texture basename, not by a relative
path carried inside the KN5.

Apex discovers immediate renderable files in `skins/<name>`, matches their basenames
case-insensitively to the loaded KN5 texture set, and surfaces case-colliding files as
ambiguous. Sparse skins inherit every unmatched embedded texture. Skin handles are
resolved at draw time, so switching a skin does not require rebuilding mesh buffers;
CSP file/color resource overrides retain precedence over the selected stock skin.

The installed four-LOD Prius fixture has eight generated skins. Selecting
`generated-1` matched and loaded its `metal_detail.dds` as a raw 32-bit DDS while ten
other KN5 textures remained inherited. Chrome 151 produced a visibly darker gold body
than the embedded-texture preview and a distinct capture hash, with no unsupported
skin texture, JavaScript exception, or WebGL error.

An installed-data audit found 15,067 `ui_skin.json` files. After accepting the UTF-8
BOM in 44 files, 14,971 files had readable JSON and 96 were malformed. The strict
parser accepted 14,960 files. It rejected 11 additional files with invalid UTF-8.
The readable objects used only six top-level keys. `skinname` occurred in every file.
The `drivername`, `country`, `team`, and `number` fields occurred in most files. A
numeric car number occurred in 1,591 files, so Apex accepts it and writes it as text.
The optional integer `priority` field occurred in 4,497 files. Values ranged from
0 through 35.

Apex limits metadata input to 1 MiB, requires valid UTF-8 and one JSON object, and
reports malformed files without replacing them. The editor retains safe unknown
fields for compatibility with other installations. It stores only the six authored
field changes in the Apex project and exports a standalone `ui_skin.json` file.
The browser file loader rejects an oversized file from its declared size before it
reads the payload. A packaged WebGL check loaded the official Alfa `00_rosso`
metadata and produced hash `ebf462b1040fd758`. The browser log and WebGL reported
zero errors, and the sandbox exposed neither `process` nor `require`.

## ksEditor directional-shadow evidence

The installed SDK editor configuration fixes `SHADOW_MAP_SIZE=2048` in
`sdk/editor/cfg/video.ini`. Its `cfg/dx11.ini` supplies three independent comparison
biases: `0.000002`, `0.000015`, and `0.0003`. The shipped showroom camera config uses
`SHADOW_SPLIT0=2`, `SHADOW_SPLIT1=12`, and `SHADOW_SPLIT2=50`. These are camera-space
metres, consistent with `kACCamera` exposing three scalar `ShadowSplit` properties and
`ksNet.kCamera` exposing the same values as a `Vector3`.

DXBC reflection strings in `ksPerPixel.shader` identify `ksShadowMatrix0` through
`ksShadowMatrix2`, `txShadow0` through `txShadow2`, `bias`, and `textureSize` in the
shadow constant buffer and pixel shader. The SDK ships separate `ksShadowGen`,
`ksShadowGenAT`, and `ksShadowGenSKIN` programs. Reflection of the alpha-tested
generator confirms that its pixel stage samples `txDiffuse` with `ksAlphaRef`.
`ksNet.kMesh` and `kSkinnedMesh` each expose a `CastShadow` property, while
`ksGraphics` exposes `setShadowMapBias` and `setSunDirection`.

Apex implements this as three portable WebGL depth targets. Each projection is fitted
to its camera-frustum slice and snapped to shadow texels, then sampled with 3×3 PCF.
Only direct sun diffuse/specular is attenuated; ambient, environment, emissive, and CSP
local-light terms remain independent. KN5 and effective CSP cast-shadow flags control
the caster set. As recovered from `MaterialFilterSM::apply`, every material whose
effective blend mode is non-opaque uses its effective diffuse texture and
`ksAlphaRef` in the cutout depth pass; opaque materials use the null-pixel path.

Production Chrome checks covered both asset scales. The complete Imola preview used
273 caster meshes and 274,960 caster triangles; its enabled capture hash was
`c53117000c2a8d81`, versus `b84509f5429e529f` disabled. The assembled Nissan 370Z used
164 caster meshes and 235,947 caster triangles; its hashes were `c564c14eeaf2312f` and
`f7b0b9064cafbbf1`. Both loaded every embedded texture, returned WebGL error zero, and
logged no browser exception. These checks prove the portable implementation and its
toggle, not pixel equality with a controlled ksEditor or game capture; that comparison
remains a lighting acceptance gate.

## ksEditor weather, HDR, and exposure evidence

The installed editor starts with `5_light_clouds` in `sdk/editor/cfg/race.ini`.
Its `cfg/lighting.ini` bounds exposure at 0.2–0.5, while
`system/cfg/ppfilters/default.ini` sets automatic-exposure target 0.32, gamma 1.2,
saturation 0.95, and Yebis tone-mapping function −1. Each of the seven installed
`sdk/editor/content/weather/*` directories supplies a version-3 `colorCurves.ini`
and a `weather.ini`; tests parse every installed file and compare every curve, fog,
and cloud field with Apex's portable preset table.

PDB-guided Ghidra decompilation of the installed 32-bit `ksNet.dll` resolves the
data path. `GraphicsManager::loadLightingSettings` at `0x100457c1` and its default
loader at `0x100464ba` read the version-3 curves. Each HDR component is exactly
`RGB component × intensity / 255`; disabling the post-processing HDR path additionally
applies `HDR_OFF_MULT`. `GraphicsManager::updateLightingSetttings` at `0x10046c2c`
updates its constant buffer with sun direction, ambient, sun, horizon, sky, fog, and
cloud fields. For Apex's upward-pointing sun vector, its curve interpolation amount is
`pow(1 − clamp(sunDirection.y, 0, 1), ANGLE_GAMMA)`: zero selects each `HIGH` color
at zenith and one selects `LOW` at the horizon. `setCustomSunDirection` at
`0x100465a5` confirms the editor's internally downward-pointing light-vector
convention. `WeatherGenerator::loadPreset` at `0x10062940` loads cloud cover,
cutoff, and color plus fog color, blend, and distance, and substitutes one metre for
a zero fog distance.

Apex evaluates that native curve model once per frame and feeds one linear-HDR path:
procedural horizon/sky gradient and sun disc, weather ambient and direct light,
environment response, emissive and CSP local lights, and distance fog. It resolves a
supported multisample RGBA16F target, builds the full-frame mip chain, derives exposure
from the final 1×1 average luminance, clamps automatic exposure to the installed
0.2–0.5 range, and supports an explicit manual value.

The installed `ksNet.dll` contains 2,649 embedded DXBC programs. Its effect table maps
`tech_TonemapHDR_Dither_Exposure_Gamma` to pixel program 2095 for the configured
`FUNCTION=-1` default. D3D reflection identifies `fParam_GammaCorrection` at byte 3232
with default 0.454545468 and `fParam_TonemapMaxMappingLuminance` at byte 3328 with
default `(1, 1, 1.015625, 1)`. The program clamps input to `2^-14`, computes
`q = exp(-input × curve.x)`, then `saturate((1-q) × (1-q×curve.y)^2)`, adds `2^-22`,
and applies the gamma exponent. Ghidra resolves `CPostEffect::SetTonemapParameters`
at `0x100933a0`, `GetEffectiveTonemapParameters` at `0x100894b0`,
`CRenderGlare::TonemapToSurface` at `0x100e2f00`, and
`CTextureUtil::DrawRectGPU_TonemapHDR` at `0x100b9310`. Together they prove that the
configured 1.2 gamma is passed through the effective settings and uploaded as its
reciprocal. The saturation matrix is applied before the curve. Apex now implements
those recovered instructions rather than the previous Reinhard approximation.

The reflected mapping vector is only the effect-file initializer. `CPostEffect::Initialize`
overwrites it after setting the characteristic-curve control to 0.5. With the editor's
default feature flags, `UpdateParameters_ChangeFormat` selects `EHDRTONEMAP=10` and
`CTextureUtil::SetTonemapEffectParameters` computes `p = float(pow(0.5, 0.3333333433))`.
Its first curve coefficient is `float(1 + p × 2.0891273022) = 2.6581413746`. The second
is the linear interpolation of the binary's 21-value, 0.05-step normalization table at
`p`, yielding `0.6653175950`. Those initialized coefficients, rather than the reflected
`(1, 1)`, are the default values uploaded for the active editor display pass.

The same pass includes the default glare result and output dither. Program 2095 samples
the glare texture after it evaluates the display curve. It clamps glare to one and uses
`curve + (1 - curve) × glare`, then applies reciprocal gamma. Finally, it adds a sampled
dither value with scale `1/255` and offset `-0.5/255`.

The default filter sets glare quality 3, luminance 1.6, threshold 5, bright-pass mode 0,
and `BLOOM_NUM_LEVELS=0`. `CPostEffect::EndPostEffectScene_InternalProcess` at
`0x100846c0` maps mode 0 to Yebis bright-pass type 1. Embedded program 493,
`tech_BrightPassT1_THRESHOLD_S1`, computes
`clamp(max(source × exposure - threshold, 0) × remap, 0, 64000)`. The shipped remap is
one. Automatic exposure reads the original HDR scene before this pass, so glare does not
change the exposure measurement.

`CPostEffect::Initialize` selects the quality table entry `(0.25, 5)` for quality 3.
Thus, the glare source uses one quarter of the viewport dimensions and the default bloom
uses five levels. `CRenderGlare::GenerateGlare_Bloom_CompositeSubLevels` at `0x100dbdd0`
and embedded program 407, `tech_MadT5`, prove an equal RGB weight for all five float-HDR
levels. `GetBloomCompositeLuminanceScale` at `0x100e14f0` calculates each weight as
`1 × 0.035 × 5 × 1.6 × 0.038 = 0.01064` for the installed custom shape.

Apex now reproduces the recovered source scale, level count, bright-pass equation,
composite weights, screen blend, and dither scale. The glare call enters
`GaussianFilterMax61x61_2Pass` at `0x100d0d90`. The configured bloom dispersion passes
RGB radii in the wavelength ratios 615:540:465 and sets the alpha radius to zero. These
unequal radii make the function delegate to `GaussianFilterMax29x29_2Pass` at
`0x100cfde0`. Thus, the max-61 implementation is not the active default kernel.

`GPUTexUtil_GetGaussianArray` at `0x10078150` evaluates
`exp(-x²/(2σ²))`. `GPUTexUtil_GetColorWeight15_Gauss29` at `0x1007bd10` pairs taps for
linear texture sampling, normalizes the full 29-tap kernel, and emits the center plus
seven positive and seven negative samples. The active scalar-weight path averages the
four channel offsets and uses the red-channel weights. The 0.002 threshold selects
5, 7, 13, 15, and 15 samples across the five default levels. The source level is two,
and the recovered scale is `2^(1-2) × 0.95 × 2.2`. This gives sigma values 1.045, 2.09,
4.18, 8.36, and 16.72. Apex now uploads these native offsets and weights for both axes.
The star, ghost, light-shaft, and non-default max-61 branches remain outside this
bloom-core increment.

The same binary preserves distinct linear, linear-saturated, Reinhard, luminance,
logarithmic, and pre-map shader variants. The default post filter sets adaptation delay
to zero, so Apex's immediate event-driven exposure is consistent with this shipped
editor configuration. Yebis star, ghost, light-shaft, non-default max-61 Gaussian, non-default curves,
and controlled editor/game pixel matching remain fidelity work. The legacy non-Yebis
SDK shaders separately expose a temporal adaptation rate, but that dormant path is not
substituted for the active default.

The corrected curve was checked in the production browser path at both asset scales.
The compact Nissan 370Z LOD rendered three meshes and 2,343 triangles. Full Imola
rendered 1,023 of 1,239 meshes and 3,036,891 triangles; its live scene cubemap reported
238 draws and 107,886 triangles while the directional pass reported 179 mesh casters.
Both checks used Light Clouds, automatic exposure 0.2, and an RGBA16F target with 4×
MSAA. Neither check produced a JavaScript or browser warning/error. These checks prove
the portable runtime path, not pixel equality with ksEditor or Assetto Corsa.

A follow-up production Electron run used the full textured Nissan 370Z with manual
exposure 0.35. It loaded all 82 textures, rendered 193 of 215 scene meshes, and used
all five active bloom kernels. Runtime diagnostics reported 5/7/13/15/15 samples and
sigma values 1.045/2.09/4.18/8.36/16.72. The lit frame hashed to
`bd999be64c9d42ed`; shadows-off and low-sun frames hashed to `1fd352c0372fba95`
and `4be34d217367d96a`. Every state returned WebGL error zero, and the browser logged
no exception. This proves the recovered Gaussian code executes in the production
WebGL path. It does not prove pixel equality with the native editor.

## CSP vertex ambient-occlusion evidence

The installed CSP distribution contains 225 `.vao-patch` files: 22 legacy
`Patch.data` archives, 196 `Patch_v4.data` archives, and seven `Patch_v5.data`
archives. Each patch is a ZIP container with optional `Config.ini`, dynamic
`ExtraSamples.data`, and tree-sample entries. Apex reads the central directory,
checks local-entry bounds, expands only stored or Deflate entries, verifies the
declared uncompressed size and CRC-32, and rejects encryption or unsupported methods.

Ghidra analysis of the installed 64-bit `dwrite.dll` resolves CSP's loader at
`FUN_18110f450` (`0x18110f450`) and its application path at `FUN_181110cb0`
(`0x181110cb0`). The loader prefers v5, then v4, v3, and legacy payloads. Each
ordinary record is a length-prefixed mesh name, a 32-bit record type, three float32
identity coordinates, a 32-bit vertex count, and encoded per-vertex data. Types 1
and 3 carry a scalar AO channel; the application writes them to two separate vertex
bytes at offsets `0x28` and `0x29`. Installed car patches commonly carry both,
while track patches generally use the primary type-1 channel. Legacy type 0 carries
three half-float color components and type 2 carries a half-float normal vector.

`FUN_1811123e0` at `0x1811123e0` confirms the binding key. CSP first requires the
record's vertex count to equal the candidate mesh count, then compares the record XYZ
with the candidate's first vertex and accepts only a squared distance below exactly
`0.01`. Record names prefixed with `@@__ALT@:` are stripped and treated as alternative
states. Apex uses the same case-sensitive name, count, and `< 0.01` first-position
test and retains alternate records for diagnostics rather than binding them as the
default state.

The scalar decoder at `FUN_181110890` (`0x181110890`) distinguishes payload versions.
Version 5 copies its byte directly. Version 4 copies the byte and then computes
`trunc(sqrt(byte / 255) × 255)`. Legacy/v3 payloads decode IEEE-754 half floats,
apply `[LIGHTING]` opacity, brightness, and gamma, quantize to a byte, and apply the
same square-root conversion. The installed Barcelona legacy fixture exercises types
0, 1, and 2 across 1,945 records; installed Nissan v4 and BMW v5 fixtures exercise
both modern conversions. The portable parser fixture-tests all three generations.

The renderer uploads the primary result as one normalized unsigned byte per matching
KN5 vertex and attenuates ambient diffuse and environment response. Direct sun,
directional shadows, CSP local lights, specular, and emissive terms remain independent.
This corresponds to vertex ambient occlusion rather than treating it as another
shadow map. The secondary car channel is preserved and reported, but correct blending
for `SPLIT_AO` door, steering, headlight, and wing animation states remains. Legacy
normal-override type-2 records, `@@__ALT@:` states, embedded v1 extra samples, v2
`ExtraSamples.data`, and tree samples are also diagnosed but not yet applied.

Production Chrome proofs covered both asset scales. The Nissan 370Z patch supplied
908 records; 416 default records bound both channels on 201 meshes, with 174 alternate
records retained and 174,543 primary AO vertices uploaded. Its VAO-enabled capture
hash was `d06df473339731ce`, versus `b80f55a3001edef9` disabled. The disabled hash
exactly matches the preceding weather-lighting proof without VAO. Imola supplied 830
ordinary records plus 4,791 embedded v1 extra samples; 769 records bound 769 meshes
and 2,526,961 AO vertices. Its enabled hash was `8976f1ffdd951540`, versus
`9bbb5f0950b8088a` disabled, again exactly matching the preceding no-VAO proof.
Both scenes retained complete texture coverage, RGBA16F with 4× MSAA, directional
shadows and weather lighting, returned WebGL error zero for every state, and logged no
browser exception. These comparisons prove the portable primary-channel path and its
toggle, not the remaining animated or spatial extra-sample behavior.

## CSP seasonal-material evidence

The installed 0.3.0-preview520 `dwrite.dll` has a 98,033,352-byte appended ZIP
payload beginning after the PE image. Its nested 31,315,250-byte `shaders.zip`
identifies the public source repository at
`https://gitlab.com/ac-custom-shaders-patch/public/acc-shaders`. The matching public
`recreated/include_new/base/utils_ps.fx` defines `adjustTextureColor`. It constructs
the foliage mask as `saturate(seasonWinter * saturate(normal.y) +
(2g - r - b) * 20)`. Autumn interpolates toward `(0.2r + 1.6g,
g * (1.3 - variation²) + 0.2r, b - 0.4g)`. Winter interpolates toward luminance
multiplied by a cold `(1, 1.1, 1.2)` response, with the same `1.6`, `0.4`, and
`0.85` constants as the installed shader build. All channels, masks, and winter
blend factors use the source's saturation points.

Ghidra resolves the installed config application path at `FUN_180d9c290`
(`0x180d9c290`). It references `seasonWinter` at `0x1820c87e8`, `seasonAutumn` at
`0x1820c8818`, `seasonSummer` at `0x1820c88e0`, and `YEAR_PROGRESS` at
`0x1820c8908`. When a requested material variable is missing, the function compares
its name with `seasonSummer` and suppresses the normal “variable not found” path;
it creates no shader binding. `seasonWinter` and `seasonAutumn`, in contrast, receive
dedicated material flags and values. This matches CSP's current documentation, which
lists only autumn and winter as diffuse-color shader inputs. Apex therefore retains
and reports `seasonSummer` values for config fidelity but intentionally does not
invent a rendering effect for them.

The 39 installed loaded-track configs contain 165 seasonal assignments across 20
files: 80 winter, 66 autumn, and 19 compatibility summer entries. Their winter values
range from 0.35 to 10 and autumn values from 0 to 1.2, so Apex preserves the source
values and applies saturation only at the same shader operations. `YEAR_PROGRESS`
defaults to 0.5, is exposed as a 0–1 calendar slider, and evaluates each config's own
piecewise-linear LUT. An installed Imola fixture proves that midpoint behavior:
`SEASON_AUTUMN` is exactly `1/15`, `SEASON_WINTER` is zero, and its asphalt winter
parameter is `0.4/15 = 0.026666…`; at year progress 0.07, winter reaches one and the
effective asphalt parameter reaches 0.8.

The renderer applies the recovered transform after diffuse/detail/multilayer
composition and normal evaluation, before RainFX and lighting. Ordinary shaders use
CSP's fixed variation of 0.5. `ksTree` uses deterministic world-space value noise in
place of CSP's sampled `txNoise`; this is the only deliberately approximated input to
the color formula and is reported in the inspector. A production Chrome comparison
loaded all 110 Imola textures and the installed config, rendered the assembled scene
with manual exposure 0.35, and evaluated hundreds of seasonal meshes. Year progress
0.07 produced hash `c5ce614805e4fa00`; midpoint 0.5 produced
`3093ba7be063a1a4`. Both captures returned WebGL error zero and no JavaScript or
browser-log errors. Brown bark and other non-green, non-upward materials correctly
remain unchanged under the native mask.

## Viewport camera state after reflection capture

The live scene cubemap uses the same material program as the main viewport. Each
cubemap face uploads a different view-projection matrix. The capture path previously
left the last face matrix active after it returned. The sky used the viewport camera,
but the main geometry used the stale cubemap camera.

Apex now uploads the viewport matrix again before it draws the main geometry. A
production Monza run used a saved TV camera, 1,428 meshes, 130 textures, and live
scene reflections. A second run loaded all four Nissan 370Z LODs, all 82 embedded
textures, and the selected skin. Both runs returned WebGL error zero and no browser
errors. The Nissan frame also proves that the correction does not invert the car.

The browser smoke check now waits for folder input and manifest discovery separately.
This sequence removes a clean-session race that occurred before the renderer existed.

## FBX import behavior

The installed `ksNet.pdb` identifies `FBXImporter::loadNode` at `0x10005880`.
The matching code calls `FbxGeometryConverter::TriangulateInPlace` before it reads
the mesh. It reads polygon-corner normals and UVs after triangulation.

The native importer changes the sign of the V coordinate. It also reads the polygon
material index and sends the corners to one `MeshBuilder` for each material.
Ordinary nodes use `EvaluateLocalTransform` with the source pivot. The geometry path
also applies the geometric translation, rotation, and scale matrix.

The skin path reads the first skin deformer. It imports every cluster link, inverse
bind matrix, control-point index, and weight. This path matches the 19-float KN5
vertex layout and its four weight and index fields.

The PDB identifies `FBXImporter::loadAnimation` at `0x100071a0` and
`FBXImporter::loadAnimationNode` at `0x10007550`. The node walk accepts FBX mesh,
skeleton, and null attributes. It evaluates each accepted node in local space.

The native sample step is one percent of the selected animation time span. The loop
stops before the end time. As a result, each nonempty animation contains 100 frames.

`Animation::save` at `0x10043dd0` writes version 2, the track count, and each UTF-8
track name. It then writes the frame count and 40 bytes for each frame. A frame contains
one quaternion, one position, and one scale.

Apex uses the [Three.js FBXLoader](https://threejs.org/docs/pages/FBXLoader.html) for
portable FBX decoding. The adapter applies the recovered ksEditor rules after the
loader triangulates the scene. It expands triangle corners to preserve UV and normal
seams. It splits each output mesh before the 16-bit KN5 index limit.

The official SDK sphere imports as one mesh with 224 triangles. The official animated
GT40 suspension scene imports as 42 meshes with 16,514 triangles. Four spring meshes
retain their skin data. Its clip contains 104 source curves. Apex exports 112 object
tracks with 100 frames, which matches the native object-selection rule.

Both scenes serialize to KN5 v6 and parse again. A live Electron run loaded all
generated DDS textures and returned WebGL error zero. The packaged Linux application
also imported the sphere with no JavaScript or browser-log errors.

Three.js removes the directory from an external FBX image path before it loads the
image. Apex captures the remaining filename from the loader. It resolves that name
against the selected source folder with the shared path resolver. The resolver uses
an exact path, a unique suffix, or a unique basename. It does not select an ambiguous
file.

Apex embeds resolved DDS, PNG, JPEG, and WebP bytes in the KN5 output. The importer
also captures supported embedded FBX images through their temporary blob or data URL.
Missing, ambiguous, unreadable, and unsupported images retain the material-color DDS.
The inspector shows the slot, state, and output name for each texture reference.

`FBXImporter::load` at `0x10004200` starts each material with `ksPerPixel`.
It also sets `ksSpecularEXP` to 1. For recognized surface materials, it reads
the first component of the ambient, diffuse, and specular colors. Values of
`ksSpecularEXP` that are less than 1 become 10.

ksEditor loops through all 32 entries in `FbxLayerElement::sTextureChannelNames`.
It accepts only `FbxFileTexture` objects and reduces each path to a basename.
It searches the configured folders and the automatic sibling `texture` folder.
The first file found fills `txDiffuse`. Later channels cannot replace this resource.
The importer never requests `txNormal`.

Apex keeps the explicit FBX diffuse channel as `txDiffuse`. For static materials,
it maps `normalMap` to `txNormal` and selects `ksPerPixelNM`. Unresolved normal
references use a flat tangent-space DDS. The inspector reports every preserved map.
It also reports maps that have no safe stock KN5 binding.

An installed production FBX contains 22 materials and 23 texture references.
Five static materials contain true normal-map connections. Apex assigns
`ksPerPixelNM` and both required resources to all five materials. The converted
scene contains 17 `ksPerPixel` materials and five `ksPerPixelNM` materials.

A live browser import loaded all 27 generated DDS textures. The selected normal-mapped
material showed both `txDiffuse` and `txNormal` in the inspector. The scene drew
241,432 triangles, and WebGL returned error zero. The browser log contained no errors.

The official GT40 FBX contains `Grey.dds` and `exterior_engine_diffuse.dds`
references. A selected source folder resolved `Grey.dds` by suffix. The exported KN5
retained the DDS bytes and the `txDiffuse` name after a write/read round trip. The
second missing reference kept its generated DDS.

The animation adapter preserves `userData.originalName` because Three.js removes
colons from binding names. This rule preserves names such as `DRIVER:RIG_HAND_L` in
the exported file. A production driver FBX and its KSANIM file both contain 59 named
tracks, 100 frames, and 38 animated tracks.

The live browser preview selected the GT40 clip and moved its timeline to 0.500. The
renderer matched 42 animated nodes. The export action reported 112 tracks and 100
frames. The browser log contained no errors.

A Blender-generated binary FBX embedded the SDK `Arrows.png` image. The browser
imported the image as `base_color_texture.png` without a source folder. WebGL loaded
the PNG, rendered the textured cube, and returned error zero. The browser log had no
errors.

## Static geometry authoring evidence

Apex stores static geometry edits by stable hierarchy path. Each application starts
from captured source vertex and index buffers. This rule prevents cumulative changes
during preview refresh, undo, recovery, and export.

The transform path uses the source bounds center as its pivot. It transforms positions,
applies the inverse-transpose matrix to normals, and preserves both supported tangent
encodings. It also recalculates the KN5 bounding sphere.

The topology path removes repeated-index and zero-area triangles with a scale-aware
tolerance. Face reversal swaps the second and third indices and reverses source
normals. Normal rebuilds use area-weighted triangle cross products.

Unit checks cover ordinary and packed tangents, normal transforms, topology repair,
baseline restoration, source-buffer immutability, and KN5 write/read round trips. A
live WebGL check reduced a two-triangle mesh to one triangle. Undo, redo, and recovery
preserved all three topology operations, and KN5 export completed. CSP export stayed
disabled because CSP cannot replace source geometry.

## Desktop packaging evidence

The desktop shell uses Electron 43.4.1 and serves the existing application from an
ephemeral loopback port. The renderer has no Node.js integration. Context isolation,
the Chromium sandbox, web security, and the application content security policy are
active. The shell rejects new windows, external navigation, and permission requests.

The production check loaded the installed Nissan 370Z KN5 in the packaged Linux
application. It loaded all 82 textures and reported no unsupported textures. The
normal view showed 193 of 215 scene meshes. The application reported WebGL error zero,
no browser errors, and no renderer access to `process` or `require`.

The Linux build produced these unsigned artifacts:

- `Apex Editor-0.1.0-linux-x86_64.AppImage`: SHA-256
  `65815f009eb7d5c67284698b0840188a583607bdb2d75a8fd77dd00a5bc9802b`
- `Apex Editor-0.1.0-linux-x64.tar.gz`: SHA-256
  `6b72c3d15f8ae5e1f274aac64d9eacbf20c81347a3850745d43df226fcec5229`

The artifacts prove the Linux package path. They do not prove Windows or macOS
packaging, code signing, or installer behavior.
