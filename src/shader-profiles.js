const STOCK_SHADER_NAMES = `
GL
GL2D
GLTextured
ksBrakeDisc
ksBrokenGlass
ksCameraDirt
ksCarPaintSimple
ksCircularRPM
ksClouds
ksColourShader
ksFXAA_0
ksFXAA_1
ksFXAA_2
ksFXAA_3
ksFXAA_4
ksFXAA_5
ksFakeCarShadows
ksFakeCarShadowsGen
ksFlags
ksFont
ksGrass
ksHighPass
ksIdealLine
ksMSDepthResolve
ksMegaShader
ksMultilayer
ksMultilayer_fresnel_nm
ksMultilayer_objsp
ksOrenNayar
ksParticle
ksPerPixel
ksPerPixelAT
ksPerPixelAT_NM
ksPerPixelAT_NS
ksPerPixelAlpha
ksPerPixelMultiMap
ksPerPixelMultiMapSimpleRefl
ksPerPixelMultiMap_AT
ksPerPixelMultiMap_AT_NMDetail
ksPerPixelMultiMap_NMDetail
ksPerPixelMultiMap_damage
ksPerPixelMultiMap_damage_dirt
ksPerPixelMultiMap_emissive
ksPerPixelNM
ksPerPixelNM_UV2
ksPerPixelNM_UVMult
ksPerPixelReflection
ksPerPixelSimpleRefl
ksPerPixel_dual_layer
ksPerPixel_nosdw
ksPostAdaptLum
ksPostBW
ksPostBlur
ksPostBlurH
ksPostBlurRadial
ksPostBlurRadialMS
ksPostBlurV
ksPostBlur_MS
ksPostCopy
ksPostCopyLuma
ksPostFOG
ksPostFOG_MS
ksPostToneMap
ksSelectedMesh
ksShadowGen
ksShadowGenAT
ksShadowGenSKIN
ksShadowGen_debug
ksSimpleShader
ksSkidMark
ksSkinnedMesh
ksSkinnedMesh_NMDetaill
ksSky
ksSkyBox
ksSkyCubemap
ksTest
ksTree
ksTyres
ksWindscreen
newStefano_ksTyres
stPerPixelNM_UVflow
`.trim().split(/\s+/);

const ALPHA_TESTED = new Set([
  "ksGrass", "ksPerPixelAT", "ksPerPixelAT_NM", "ksPerPixelAT_NS",
  "ksPerPixelMultiMap_AT", "ksPerPixelMultiMap_AT_NMDetail", "ksTree"
]);
const VERTEX_LAYOUTS = new Map([
  ["GL2D", "2d"], ["ksParticle", "particle"], ["ksShadowGenSKIN", "skinned"],
  ["ksSkinnedMesh", "skinned"], ["ksSkinnedMesh_NMDetaill", "skinned"]
]);
const LAYOUT_BY_CODE = ["mesh", "skinned", "particle", "2d"];
const byLowerName = new Map(STOCK_SHADER_NAMES.map((name) => [name.toLowerCase(), name]));

export const STOCK_SHADER_PROFILES = new Map(STOCK_SHADER_NAMES.map((name) => [name, Object.freeze({
  name,
  alphaTested: ALPHA_TESTED.has(name),
  vertexLayout: VERTEX_LAYOUTS.get(name) || "mesh"
})]));

function bytesOf(input) {
  return input instanceof Uint8Array ? input : new Uint8Array(input);
}

function dxbcAt(bytes, offset) {
  return bytes[offset] === 0x44 && bytes[offset + 1] === 0x58 && bytes[offset + 2] === 0x42 && bytes[offset + 3] === 0x43;
}

/** Decode the version-2 header used by Kunos' shipped `.shader` containers. */
export function parseStockShaderContainerHeader(input) {
  const bytes = bytesOf(input);
  if (bytes.byteLength < 14) throw new Error("Stock shader container is truncated");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = bytes[0], alphaTested = Boolean(bytes[1]), layoutCode = view.getUint32(2, true);
  if (version !== 2) throw new Error(`Unsupported stock shader container version ${version}`);
  if (!LAYOUT_BY_CODE[layoutCode]) throw new Error(`Unsupported stock shader vertex layout ${layoutCode}`);
  const vertexBytes = view.getUint32(6, true), vertexOffset = 10, pixelSizeOffset = vertexOffset + vertexBytes;
  if (!dxbcAt(bytes, vertexOffset) || pixelSizeOffset + 8 > bytes.byteLength) throw new Error("Invalid stock vertex shader payload");
  const pixelBytes = view.getUint32(pixelSizeOffset, true), pixelOffset = pixelSizeOffset + 4, geometrySizeOffset = pixelOffset + pixelBytes;
  if (!dxbcAt(bytes, pixelOffset) || geometrySizeOffset + 4 > bytes.byteLength) throw new Error("Invalid stock pixel shader payload");
  const geometryBytes = view.getUint32(geometrySizeOffset, true), geometryOffset = geometrySizeOffset + 4;
  if (geometryBytes && (!dxbcAt(bytes, geometryOffset) || geometryOffset + geometryBytes > bytes.byteLength)) throw new Error("Invalid stock geometry shader payload");
  return {
    version,
    alphaTested,
    vertexLayout: LAYOUT_BY_CODE[layoutCode],
    vertexBytes,
    pixelBytes,
    geometryBytes
  };
}

export function stockShaderProfile(name) {
  const canonical = byLowerName.get(String(name || "").trim().toLowerCase());
  return canonical ? STOCK_SHADER_PROFILES.get(canonical) : null;
}

function modeName(value, fallback = "") {
  return String(value === null || value === undefined || value === "" ? fallback : value).trim().toUpperCase();
}

function explicitCullMode(value) {
  const mode = modeName(value);
  if (!mode) return null;
  if (mode === "NONE" || mode === "DOUBLESIDED" || mode === "DOUBLE_SIDED" || mode === "OFF") return "none";
  if (mode === "FRONT") return "front";
  return "back";
}

/** Resolve render state without depending on WebGL constants. */
export function resolveMaterialRenderProfile(material = {}, node = {}, override = null) {
  const shader = String(override?.shader || material.shader || ""), stock = stockShaderProfile(shader);
  const serializedBlendMode = Number(material.blendMode) || 0;
  const packageBlendMode = stock?.alphaTested ? 2 : 0;
  const nativeBlendMode = serializedBlendMode || packageBlendMode;
  const overrideBlendMode = modeName(override?.blendMode);
  let effectiveBlendMode = nativeBlendMode, blendSource = serializedBlendMode ? "kn5" : packageBlendMode ? "shader-package" : "default";
  if (overrideBlendMode) {
    blendSource = "override";
    if (overrideBlendMode === "2" || /ALPHA_TEST|ALPHA_TO_COVERAGE|A2C/.test(overrideBlendMode)) effectiveBlendMode = 2;
    else if (overrideBlendMode === "1" || /ALPHA_BLEND|TRANSPARENT_AS_BLACK|MULTIPLY/.test(overrideBlendMode)) effectiveBlendMode = 1;
    else if (overrideBlendMode === "0" || /OPAQUE|NONE|OFF/.test(overrideBlendMode)) effectiveBlendMode = 0;
  }
  const alphaToCoverage = effectiveBlendMode === 2;
  const shadowAlphaTested = effectiveBlendMode !== 0;
  const depthMode = modeName(override?.depthMode, material.depthMode ?? 0);
  const transparent = override?.isTransparent !== null && override?.isTransparent !== undefined
    ? Boolean(override.isTransparent)
    : Boolean(node.transparent || effectiveBlendMode === 1);
  const blendEnabled = effectiveBlendMode === 1;
  let blend = "opaque";
  if (blendEnabled) blend = overrideBlendMode.includes("MULTIPLY") ? "multiply" : overrideBlendMode.includes("TRANSPARENT_AS_BLACK") ? "transparent-as-black" : "alpha";
  const depthTest = !(depthMode === "2" || depthMode === "OFF" || depthMode === "NONE");
  const depthWrite = depthTest && !transparent && depthMode !== "1" && !/NOWRITE|READ_ONLY/.test(depthMode);
  const explicitCull = explicitCullMode(override?.cullMode);
  const cull = explicitCull || "back";
  const shaderKey = shader.trim().toUpperCase();
  const windscreen = shaderKey === "KSWINDSCREEN";
  const brokenGlass = shaderKey === "KSBROKENGLASS";
  const configuredRefraction = Number(override?.properties?.get?.("extrefraction") ?? 0) > 0;
  const refractive = brokenGlass || /REFRACT/.test(shaderKey) || configuredRefraction;
  const reflectionAlpha = !windscreen && transparent && /KSPERPIXELREFLECTION|SMGLASS|KSBROKENGLASS|REFRACT/.test(shaderKey);
  const glassMode = brokenGlass ? "broken glass · CSP refraction" : refractive ? "refractive" : windscreen ? "windscreen" : reflectionAlpha ? "reflection" : "none";
  return {
    shader,
    stock,
    serializedBlendMode,
    nativeBlendMode,
    effectiveBlendMode,
    blendSource,
    alphaToCoverage,
    shadowAlphaTested,
    transparent,
    blendEnabled,
    blend,
    blendMode: overrideBlendMode || String(effectiveBlendMode),
    depthMode,
    depthTest,
    depthWrite,
    cull,
    cullSource: explicitCull ? "override" : "default",
    windscreen,
    brokenGlass,
    reflectionAlpha,
    refractive,
    glassMode
  };
}

export function auditMaterialShaderProfiles(materials = []) {
  const status = { materials: materials.length, knownStock: 0, alphaBlend: 0, alphaToCoverage: 0, shadowCutout: 0, packageDefaults: 0, serializedOverrides: 0, windscreens: 0, reflectionGlass: 0, refractive: 0, unknownShaders: [] };
  const unknown = new Set();
  for (const material of materials) {
    const stock = stockShaderProfile(material.shader), profile = resolveMaterialRenderProfile(material);
    if (stock) status.knownStock++;
    else if (material.shader) unknown.add(material.shader);
    if (profile.effectiveBlendMode === 1) status.alphaBlend++;
    if (profile.alphaToCoverage) status.alphaToCoverage++;
    if (profile.shadowAlphaTested) status.shadowCutout++;
    if (stock?.alphaTested && profile.serializedBlendMode === 0) status.packageDefaults++;
    if (profile.serializedBlendMode !== 0) status.serializedOverrides++;
    if (profile.windscreen) status.windscreens++;
    if (profile.reflectionAlpha) status.reflectionGlass++;
    if (profile.refractive) status.refractive++;
  }
  status.unknownShaders = [...unknown].sort((a, b) => a.localeCompare(b));
  return status;
}
