import { AnimationMixer, LoadingManager, LoopOnce, Texture } from "three";
import { FBXLoader } from "three/addons/loaders/FBXLoader.js";
import { createAssetFileIndex, normalizeAssetPath, resolveAssetFile } from "./asset-files.js";

const IDENTITY = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
const MAX_KN5_VERTICES = 0xffff;

export class FbxImportError extends Error {
  constructor(message, cause) { super(message, cause ? { cause } : undefined); this.name = "FbxImportError"; }
}

function bytesOf(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError("FBX input must be an ArrayBuffer or typed array");
}

export function inspectFbxHeader(input) {
  const bytes = bytesOf(input), binaryMagic = "Kaydara FBX Binary  ";
  const prefix = new TextDecoder("latin1").decode(bytes.subarray(0, Math.min(bytes.length, 256)));
  if (prefix.startsWith(binaryMagic) && bytes.byteLength >= 27) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return { format: "binary", version: view.getUint32(23, true) };
  }
  const match = prefix.match(/FBXVersion\s*:\s*(\d+)/i);
  if (match) return { format: "ascii", version: Number(match[1]) };
  throw new FbxImportError("The file does not contain a recognized FBX header");
}

function property(name, value) {
  return { name, value: Number(value) || 0, value2: [0, 0], value3: [0, 0, 0], value4: [0, 0, 0, 0] };
}

function safeName(value, fallback) {
  const name = String(value || "").trim();
  return (name || fallback).slice(0, 1024);
}

function fbxObjectName(object, fallback) {
  return safeName(object?.userData?.originalName || object?.name, fallback);
}

function textureName(materialName, index, role = "diffuse") {
  const stem = safeName(materialName, `material_${index}`).replace(/[^a-z0-9._-]+/gi, "_").replace(/^_+|_+$/g, "") || `material_${index}`;
  const suffix = role === "diffuse" ? "" : `_${role}`;
  return `APEX_FBX_${String(index).padStart(3, "0")}_${stem}${suffix}.dds`;
}

function textureFormat(input) {
  const bytes = bytesOf(input);
  if (bytes.byteLength >= 4 && bytes[0] === 0x44 && bytes[1] === 0x44 && bytes[2] === 0x53 && bytes[3] === 0x20) return "dds";
  if (bytes.byteLength >= 8 && bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47) return "png";
  if (bytes.byteLength >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) return "jpg";
  if (bytes.byteLength >= 12 && String.fromCharCode(...bytes.subarray(0, 4)) === "RIFF" && String.fromCharCode(...bytes.subarray(8, 12)) === "WEBP") return "webp";
  return "";
}

function outputTextureName(value, fallback, format) {
  const basename = normalizeAssetPath(value).split("/").at(-1) || fallback;
  const stem = safeName(basename, fallback).replace(/\.[^.]+$/, "").replace(/[^a-z0-9._-]+/gi, "_").replace(/^_+|_+$/g, "") || fallback;
  return `${stem}.${format}`;
}

function boundTextureName(value, fallback, format, materialId, slot) {
  const name = outputTextureName(value, fallback, format), separator = name.lastIndexOf("."), stem = separator >= 0 ? name.slice(0, separator) : name, extension = separator >= 0 ? name.slice(separator) : "";
  const role = safeName(slot, "texture").replace(/[^a-z0-9_-]+/gi, "_");
  return `APEX_FBX_${String(materialId).padStart(3, "0")}_${role}_${stem}${extension}`;
}

function isEmbeddedSource(value) {
  return /^(?:blob|data):/i.test(String(value || ""));
}

function asFileIndex(filesOrIndex) {
  return filesOrIndex?.entries && filesOrIndex?.exact ? filesOrIndex : createAssetFileIndex(filesOrIndex || []);
}

function textureResolution(texture, source, status, extra = {}) {
  const resolution = { source, status, matchedBy: "", path: "", ...extra };
  texture.userData.apexFbxResolution = resolution;
  return resolution;
}

async function supportedFileResolution(file, source, status, matchedBy = "", path = "") {
  try {
    const data = new Uint8Array(await file.arrayBuffer()), format = textureFormat(data);
    if (!format) return { source, status: "unsupported", matchedBy, path, name: file.name || path, format: "" };
    return { source, status, matchedBy, path, name: outputTextureName(file.name || path, "FBX_Texture", format), format, data };
  } catch (error) {
    return { source, status: "error", matchedBy, path, name: file?.name || path, format: "", error: error.message };
  }
}

async function resolveExternalSource(source, index, cache = new Map()) {
  const match = resolveAssetFile(index, source);
  if (match.status !== "resolved") return { source, status: match.status, matchedBy: match.matchedBy, path: "" };
  let pending = cache.get(match.file);
  if (!pending) {
    pending = supportedFileResolution(match.file, "", "resolved");
    cache.set(match.file, pending);
  }
  const result = await pending;
  return { ...result, source, matchedBy: match.matchedBy, path: match.path };
}

async function resolveCapturedTexture(capture, index, cache) {
  const { texture, source } = capture;
  if (!isEmbeddedSource(source)) {
    const resolution = await resolveExternalSource(source, index, cache);
    return textureResolution(texture, source, resolution.status, resolution);
  }
  try {
    const response = await fetch(source), blob = await response.blob(), data = new Uint8Array(await blob.arrayBuffer()), format = textureFormat(data);
    const reference = safeName(texture.name, "Embedded_Texture");
    if (!format) return textureResolution(texture, reference, "unsupported", { embedded: true });
    return textureResolution(texture, reference, "embedded", { embedded: true, name: outputTextureName(reference, "Embedded_Texture", format), format, data });
  } catch (error) {
    return textureResolution(texture, safeName(texture.name, "Embedded_Texture"), "error", { embedded: true, error: error.message });
  } finally {
    if (/^blob:/i.test(source)) globalThis.URL?.revokeObjectURL?.(source);
  }
}

/** Make a one-pixel BGRA8 DDS used when an FBX material color has no KN5 texture. */
export function createColorDds(color = [0.5, 0.5, 0.5], alpha = 1) {
  const output = new Uint8Array(132), view = new DataView(output.buffer);
  output.set([0x44, 0x44, 0x53, 0x20]);
  view.setUint32(4, 124, true); view.setUint32(8, 0x100f, true);
  view.setUint32(12, 1, true); view.setUint32(16, 1, true); view.setUint32(20, 4, true);
  view.setUint32(76, 32, true); view.setUint32(80, 0x41, true); view.setUint32(88, 32, true);
  view.setUint32(92, 0x00ff0000, true); view.setUint32(96, 0x0000ff00, true);
  view.setUint32(100, 0x000000ff, true); view.setUint32(104, 0xff000000, true);
  view.setUint32(108, 0x1000, true);
  const channel = (value) => Math.round(Math.min(1, Math.max(0, Number(value) || 0)) * 255);
  output.set([channel(color[2]), channel(color[1]), channel(color[0]), channel(alpha)], 128);
  return output;
}

function textureSource(texture) {
  const resolution = texture?.userData?.apexFbxResolution;
  return resolution?.source || texture?.userData?.apexFbxSource || texture?.name || "Unnamed texture";
}

function createTextureBinding(sourceTexture, options) {
  const { materialId, textureIndex, resourceIndex, material, slot, sourceField, fallbackName, fallbackData, opacity = 1 } = options;
  const resolution = sourceTexture?.userData?.apexFbxResolution;
  const resolved = resolution?.data && (resolution.status === "resolved" || resolution.status === "embedded");
  const selectedTexture = resolved ? boundTextureName(resolution.name, "FBX_Texture", resolution.format, materialId, slot) : fallbackName;
  const texture = { active: true, name: selectedTexture, data: resolved ? resolution.data : fallbackData };
  const resource = { slot, textureId: resourceIndex, texture: selectedTexture };
  if (!sourceTexture) return { texture, resource, reference: null };
  const source = textureSource(sourceTexture);
  return {
    texture,
    resource,
    reference: {
      materialId, textureIndex, resourceIndex, material, slot, sourceField, source,
      sourceKind: resolution?.embedded || isEmbeddedSource(source) ? "embedded" : "external",
      status: resolution?.status || "missing", matchedBy: resolution?.matchedBy || "",
      path: resolution?.path || "", format: resolution?.format || "", output: selectedTexture,
      fallbackName, fallbackData, opacity
    }
  };
}

const OPTIONAL_MAPS = [
  ["bumpMap", "bump map"], ["emissiveMap", "emissive map"], ["specularMap", "specular map"],
  ["aoMap", "ambient-occlusion map"], ["displacementMap", "displacement map"], ["envMap", "environment map"]
];

function ignoredMapWarning(material, label, reason) {
  return `${material}: ${label} is not exported (${reason}).`;
}

function createMaterial(source, index, skinned, textureOffset, outputName = "") {
  const name = outputName || safeName(source?.name, `Material_${index}`), color = source?.color?.toArray?.() || [0.5, 0.5, 0.5];
  const specular = source?.specular?.toArray?.() || [0.04, 0.04, 0.04], specularLevel = Math.max(...specular);
  const opacity = Math.min(1, Math.max(0, Number(source?.opacity ?? 1))), transparent = Boolean(source?.transparent || opacity < 0.999);
  const properties = skinned ? [property("bones", 0)] : [];
  properties.push(property("ksAmbient", 0.35), property("ksDiffuse", 0.8), property("ksSpecular", specularLevel), property("ksSpecularEXP", source?.shininess ?? 10), property("ksEmissive", source?.emissiveIntensity ?? 0), property("ksAlphaRef", 0));
  if (skinned) properties.push(property("fresnelC", 0), property("fresnelEXP", 0), property("fresnelMaxLevel", 0), property("nmObjectSpace", 0), property("isAdditive", 0), property("useDetail", 0), property("detailUVMultiplier", 0), property("boh", 0));
  const textures = [], resources = [], textureReferences = [], materialMaps = [], materialMapWarnings = [];
  const addBinding = (sourceTexture, slot, sourceField, fallbackName, fallbackData, bindingOpacity = 1) => {
    const binding = createTextureBinding(sourceTexture, {
      materialId: index, textureIndex: textureOffset + textures.length, resourceIndex: resources.length,
      material: name, slot, sourceField, fallbackName, fallbackData, opacity: bindingOpacity
    });
    textures.push(binding.texture); resources.push(binding.resource);
    if (binding.reference) textureReferences.push(binding.reference);
  };
  addBinding(source?.map, "txDiffuse", "map", textureName(name, index), createColorDds(color, opacity), opacity);
  if (source?.map) materialMaps.push({ material: name, sourceField: "map", slot: "txDiffuse", source: textureSource(source.map), status: "preserved" });
  if (source?.normalMap && !skinned) {
    addBinding(source.normalMap, "txNormal", "normalMap", textureName(name, index, "normal"), createColorDds([0.5, 0.5, 1], 1));
    materialMaps.push({ material: name, sourceField: "normalMap", slot: "txNormal", source: textureSource(source.normalMap), status: "preserved" });
  } else if (source?.normalMap) {
    materialMaps.push({ material: name, sourceField: "normalMap", slot: "", source: textureSource(source.normalMap), status: "ignored" });
    materialMapWarnings.push(ignoredMapWarning(name, "normal map", "the stock skinned normal shader requires additional detail and packed-map inputs"));
  }
  if (source?.alphaMap) {
    if (source.alphaMap === source.map) materialMaps.push({ material: name, sourceField: "alphaMap", slot: "txDiffuse alpha", source: textureSource(source.alphaMap), status: "folded" });
    else {
      materialMaps.push({ material: name, sourceField: "alphaMap", slot: "", source: textureSource(source.alphaMap), status: "ignored" });
      materialMapWarnings.push(ignoredMapWarning(name, "separate opacity map", "stock single-layer shaders read opacity from txDiffuse alpha"));
    }
  }
  for (const [field, label] of OPTIONAL_MAPS) if (source?.[field]) {
    materialMaps.push({ material: name, sourceField: field, slot: "", source: textureSource(source[field]), status: "ignored" });
    materialMapWarnings.push(ignoredMapWarning(name, label, field === "bumpMap" ? "a height map must be converted to a tangent-space normal map" : "there is no direct stock KN5 resource binding"));
  }
  return {
    material: { name, shader: skinned ? "ksSkinnedMesh" : source?.normalMap ? "ksPerPixelNM" : "ksPerPixel", blendMode: transparent ? 1 : 0, depthMode: 0, properties, resources },
    textures, textureReferences, materialMaps, materialMapWarnings
  };
}

function textureReferenceWarning(reference) {
  const prefix = `${reference.material} ${reference.slot}: ${reference.source}`;
  const fallback = reference.slot === "txNormal" ? "The generated flat normal texture remains active." : "The generated color texture remains active.";
  if (reference.status === "missing") return `${prefix} was not found. ${fallback}`;
  if (reference.status === "ambiguous") return `${prefix} matched more than one source file. ${fallback}`;
  if (reference.status === "unsupported") return `${prefix} uses an unsupported image format. Use DDS, PNG, JPEG, or WebP.`;
  if (reference.status === "error") return `${prefix} could not be read. ${fallback}`;
  if ((reference.status === "resolved" || reference.status === "embedded") && reference.slot === "txDiffuse" && reference.opacity < 0.999) return `${prefix} uses the image alpha. The app cannot multiply the authored opacity ${reference.opacity} into this encoded image.`;
  return "";
}

function updateTextureReferenceSummary(model) {
  const references = model?.fbx?.textureReferences || [], counts = { referenced: references.length, resolved: 0, embedded: 0, missing: 0, ambiguous: 0, unsupported: 0, error: 0 };
  for (const reference of references) if (reference.status in counts) counts[reference.status]++;
  model.fbx.textureSummary = counts;
  model.fbx.warnings = [...(model.fbx.geometryWarnings || []), ...(model.fbx.materialMapWarnings || []), ...references.map(textureReferenceWarning).filter(Boolean)];
  return counts;
}

function applyTextureResolution(model, reference, resolution) {
  const texture = model.textures[reference.textureIndex], resource = model.materials[reference.materialId]?.resources?.[reference.resourceIndex];
  if (!texture || !resource) return;
  const resolved = resolution?.data && (resolution.status === "resolved" || resolution.status === "embedded");
  texture.name = resolved ? boundTextureName(resolution.name, "FBX_Texture", resolution.format, reference.materialId, reference.slot) : reference.fallbackName;
  texture.data = resolved ? resolution.data : reference.fallbackData;
  resource.texture = texture.name;
  resource.textureId = reference.resourceIndex;
  Object.assign(reference, { status: resolution?.status || "missing", matchedBy: resolution?.matchedBy || "", path: resolution?.path || "", format: resolution?.format || "", output: texture.name });
}

/** Resolve FBX texture references again after the user selects a source folder. */
export async function resolveFbxTextures(model, filesOrIndex) {
  if (!model?.fbx) throw new TypeError("The model is not an FBX import");
  const index = asFileIndex(filesOrIndex), cache = new Map();
  await Promise.all((model.fbx.textureReferences || []).map(async (reference) => {
    if (reference.sourceKind === "embedded") return;
    applyTextureResolution(model, reference, await resolveExternalSource(reference.source, index, cache));
  }));
  return updateTextureReferenceSummary(model);
}

/** Resolve every retained FBX source model before an assembled workspace is rebuilt. */
export async function resolveFbxModelTextures(models, filesOrIndex) {
  const imports = [...new Set(models || [])].filter((model) => model?.fbx);
  await Promise.all(imports.map((model) => resolveFbxTextures(model, filesOrIndex)));
  return imports.length;
}

function tangentForTriangle(positions, uvs) {
  const x1 = positions[3] - positions[0], y1 = positions[4] - positions[1], z1 = positions[5] - positions[2];
  const x2 = positions[6] - positions[0], y2 = positions[7] - positions[1], z2 = positions[8] - positions[2];
  const s1 = uvs[2] - uvs[0], t1 = uvs[3] - uvs[1], s2 = uvs[4] - uvs[0], t2 = uvs[5] - uvs[1];
  const denominator = s1 * t2 - s2 * t1;
  let tangent = Math.abs(denominator) > 1e-12 ? [(x1 * t2 - x2 * t1) / denominator, (y1 * t2 - y2 * t1) / denominator, (z1 * t2 - z2 * t1) / denominator] : [1, 0, 0];
  const length = Math.hypot(...tangent);
  if (length > 1e-12) tangent = tangent.map((value) => value / length);
  return tangent;
}

function materialIndexAt(groups, offset) {
  for (const group of groups || []) if (offset >= group.start && offset < group.start + group.count) return Number(group.materialIndex) || 0;
  return 0;
}

function boundsFor(vertices, stride) {
  const minimum = [Infinity, Infinity, Infinity], maximum = [-Infinity, -Infinity, -Infinity];
  for (let offset = 0; offset < vertices.length; offset += stride) for (let axis = 0; axis < 3; axis++) {
    minimum[axis] = Math.min(minimum[axis], vertices[offset + axis]); maximum[axis] = Math.max(maximum[axis], vertices[offset + axis]);
  }
  const center = minimum.map((value, axis) => (value + maximum[axis]) / 2); let radius = 0;
  for (let offset = 0; offset < vertices.length; offset += stride) radius = Math.max(radius, Math.hypot(vertices[offset] - center[0], vertices[offset + 1] - center[1], vertices[offset + 2] - center[2]));
  return [...center, radius];
}

function attributeComponent(attribute, index, component) {
  return [attribute.getX, attribute.getY, attribute.getZ, attribute.getW][component].call(attribute, index);
}

function meshUsesSkinning(object) {
  return Boolean(object?.isSkinnedMesh && object.geometry?.getAttribute?.("skinWeight") && object.geometry?.getAttribute?.("skinIndex") && object.skeleton);
}

function meshParts(object, materialIds, warnings) {
  const geometry = object.geometry;
  if (!geometry?.getAttribute?.("position")) { warnings.push(`${object.name || "Unnamed mesh"}: no position data`); return []; }
  if (!geometry.getAttribute("normal")) geometry.computeVertexNormals();
  const position = geometry.getAttribute("position"), normal = geometry.getAttribute("normal"), uv = geometry.getAttribute("uv");
  const skinWeight = object.isSkinnedMesh ? geometry.getAttribute("skinWeight") : null, skinIndex = object.isSkinnedMesh ? geometry.getAttribute("skinIndex") : null;
  if (object.isSkinnedMesh && (!skinWeight || !skinIndex || !object.skeleton)) warnings.push(`${object.name}: incomplete skin data; imported as a static mesh`);
  const skinned = meshUsesSkinning(object), stride = skinned ? 19 : 11;
  const index = geometry.index, elementCount = index ? index.count : position.count, sources = Array.isArray(object.material) ? object.material : [object.material];
  const buckets = new Map();
  for (let offset = 0; offset + 2 < elementCount; offset += 3) {
    const materialIndex = Math.min(Math.max(0, materialIndexAt(geometry.groups, offset)), Math.max(0, sources.length - 1));
    let chunks = buckets.get(materialIndex); if (!chunks) { chunks = [[]]; buckets.set(materialIndex, chunks); }
    let values = chunks.at(-1); if (values.length / stride + 3 > MAX_KN5_VERTICES) { values = []; chunks.push(values); }
    const sourceIndices = [0, 1, 2].map((corner) => index ? Number(index.getX(offset + corner)) : offset + corner);
    const trianglePositions = sourceIndices.flatMap((sourceIndex) => [position.getX(sourceIndex), position.getY(sourceIndex), position.getZ(sourceIndex)]);
    const triangleUvs = sourceIndices.flatMap((sourceIndex) => [uv ? uv.getX(sourceIndex) : 0, uv ? -uv.getY(sourceIndex) : 0]);
    const tangent = tangentForTriangle(trianglePositions, triangleUvs);
    for (let corner = 0; corner < 3; corner++) {
      const sourceIndex = sourceIndices[corner];
      values.push(trianglePositions[corner * 3], trianglePositions[corner * 3 + 1], trianglePositions[corner * 3 + 2], normal.getX(sourceIndex), normal.getY(sourceIndex), normal.getZ(sourceIndex), triangleUvs[corner * 2], triangleUvs[corner * 2 + 1], ...tangent);
      if (skinned) for (let influence = 0; influence < 4; influence++) values.push(attributeComponent(skinWeight, sourceIndex, influence));
      if (skinned) for (let influence = 0; influence < 4; influence++) values.push(attributeComponent(skinIndex, sourceIndex, influence));
    }
  }
  const multiple = buckets.size > 1 || [...buckets.values()].some((chunks) => chunks.length > 1), parts = [];
  for (const [materialIndex, chunks] of buckets) for (let chunkIndex = 0; chunkIndex < chunks.length; chunkIndex++) {
    const values = new Float32Array(chunks[chunkIndex]), vertexCount = values.length / stride, indices = new Uint16Array(vertexCount);
    for (let indexValue = 0; indexValue < vertexCount; indexValue++) indices[indexValue] = indexValue;
    const suffix = multiple ? `_SUB${materialIndex}${chunks.length > 1 ? `_PART${chunkIndex}` : ""}` : "", name = `${safeName(object.name, "Mesh")}${suffix}`;
    const sourceMaterial = sources[materialIndex] || sources[0], ids = materialIds.get(sourceMaterial), materialId = ids?.get(skinned) ?? ids?.values?.().next().value ?? 0;
    const common = { name, children: [], active: object.visible !== false, castShadows: true, visible: object.visible !== false, transparent: Boolean(sourceMaterial?.transparent || Number(sourceMaterial?.opacity) < 0.999), vertices: values, vertexStride: stride, indices, materialId, layer: 0, lodIn: 0, lodOut: 1000000, renderable: true };
    if (skinned) parts.push({ type: 3, kind: "skinnedMesh", ...common, bones: object.skeleton.bones.map((bone, boneIndex) => ({ name: safeName(bone.name, `Bone_${boneIndex}`), transform: object.skeleton.boneInverses[boneIndex]?.toArray?.() || [...IDENTITY] })) });
    else parts.push({ type: 2, kind: "mesh", ...common, bounds: boundsFor(values, stride) });
  }
  return parts;
}

function convertObject(object, materialIds, warnings) {
  object.updateMatrix?.();
  const name = fbxObjectName(object, object.isBone ? "Bone" : object.type || "Node"), node = { type: 1, kind: "node", name, children: [], active: object.visible !== false, transform: object.matrix?.toArray?.() || [...IDENTITY] };
  if (object.isMesh) node.children.push(...meshParts(object, materialIds, warnings));
  for (const child of object.children || []) node.children.push(convertObject(child, materialIds, warnings));
  return node;
}

function animationObjects(scene) {
  const objects = [];
  scene.traverse((object) => {
    if ((object !== scene || object.name) && (object.isBone || object.isMesh || object.type === "Group")) objects.push(object);
  });
  return objects;
}

function animationFrame(object) {
  return {
    quaternion: object.quaternion.toArray().map(Math.fround),
    position: object.position.toArray().map(Math.fround),
    scale: object.scale.toArray().map(Math.fround)
  };
}

function animationFrameChanged(first, candidate) {
  return ["quaternion", "position", "scale"].some((name) => first[name].some((value, index) => !Object.is(value, candidate[name][index])));
}

/** Sample Three.js FBX clips with the 100-frame local-transform rule used by ksEditor. */
export function convertFbxAnimations(scene, sourceName = "scene.fbx") {
  const objects = animationObjects(scene), snapshots = objects.map((object) => ({ object, position: object.position.clone(), quaternion: object.quaternion.clone(), scale: object.scale.clone() }));
  const animations = [];
  try {
    for (const clip of scene.animations || []) {
      const duration = Math.max(0, Number(clip.duration) || 0), frameCount = duration > 0 ? 100 : 0;
      const tracks = objects.map((object) => ({ name: fbxObjectName(object, object.isBone ? "Bone" : object.type || "Node"), frames: [] }));
      const mixer = new AnimationMixer(scene), action = mixer.clipAction(clip);
      action.setLoop(LoopOnce, 1); action.clampWhenFinished = true; action.play();
      for (let frameIndex = 0; frameIndex < frameCount; frameIndex++) {
        mixer.setTime(duration * frameIndex / frameCount);
        objects.forEach((object, objectIndex) => tracks[objectIndex].frames.push(animationFrame(object)));
      }
      action.stop(); mixer.uncacheClip(clip); mixer.uncacheRoot(scene);
      for (const track of tracks) track.animated = track.frames.length > 1 && track.frames.slice(1).some((frame) => animationFrameChanged(track.frames[0], frame));
      animations.push({
        source: `${sourceName}:${safeName(clip.name, "Animation")}`, name: safeName(clip.name, "Animation"), version: 2,
        duration, frameCount, tracks, sourceTrackCount: clip.tracks?.length || 0, bytesRead: 0, byteLength: 0,
        warnings: duration > 0 ? [] : [`${safeName(clip.name, "Animation")} has no duration and contains no sampled frames`]
      });
    }
  } finally {
    for (const snapshot of snapshots) {
      snapshot.object.position.copy(snapshot.position); snapshot.object.quaternion.copy(snapshot.quaternion); snapshot.object.scale.copy(snapshot.scale);
      snapshot.object.updateMatrix();
    }
    scene.updateMatrixWorld(true);
  }
  return animations;
}

/** Convert a Three.js FBX scene into the same model shape used by the KN5 reader/writer. */
export function convertFbxScene(scene, sourceName = "scene.fbx", inputBytes = 0, header = { format: "unknown", version: 0 }) {
  const warnings = [], usage = new Map();
  scene.traverse((object) => {
    if (!object.isMesh) return;
    for (const material of (Array.isArray(object.material) ? object.material : [object.material]).filter(Boolean)) {
      let modes = usage.get(material);
      if (!modes) { modes = new Set(); usage.set(material, modes); }
      modes.add(meshUsesSkinning(object));
    }
  });
  if (!usage.size) usage.set({ name: "DefaultMaterial", color: { toArray: () => [0.5, 0.5, 0.5] } }, new Set([false]));
  const sourceMaterials = [...usage].flatMap(([source, modes]) => [...modes].sort().map((skinned) => ({ source, skinned })));
  const materialNames = new Map(), nextMaterialName = (source, index) => {
    const base = safeName(source?.name, `Material_${index}`), key = base.toLowerCase(), ordinal = (materialNames.get(key) || 0) + 1;
    materialNames.set(key, ordinal);
    return ordinal === 1 ? base : `${base}_${ordinal}`;
  };
  const materials = [], textures = [], textureReferences = [], materialMaps = [], materialMapWarnings = [], materialIds = new Map();
  sourceMaterials.forEach(({ source, skinned }, index) => {
    const converted = createMaterial(source, index, skinned, textures.length, nextMaterialName(source, index));
    materials.push(converted.material); textures.push(...converted.textures); textureReferences.push(...converted.textureReferences);
    materialMaps.push(...converted.materialMaps); materialMapWarnings.push(...converted.materialMapWarnings);
    let ids = materialIds.get(source); if (!ids) { ids = new Map(); materialIds.set(source, ids); } ids.set(skinned, index);
  });
  const rootObjects = scene.name ? [scene] : (scene.children || []);
  const root = { type: 1, kind: "node", name: `FBX: ${safeName(sourceName, "scene.fbx")}`, children: rootObjects.map((child) => convertObject(child, materialIds, warnings)), active: true, transform: [...IDENTITY] };
  const animations = convertFbxAnimations(scene, sourceName);
  const mapSummary = materialMaps.reduce((summary, entry) => { summary[entry.status]++; return summary; }, { preserved: 0, folded: 0, ignored: 0 });
  const model = { magic: "sc6969", version: 6, source: 0, textures, materials, root, bytesRead: inputBytes, byteLength: inputBytes, fbx: { sourceName, format: header.format, version: header.version, materials: materials.length, animations, geometryWarnings: [...warnings], materialMaps, materialMapWarnings, mapSummary, textureReferences, warnings: [] } };
  updateTextureReferenceSummary(model);
  return model;
}

function managerWithTextureCapture() {
  const manager = new LoadingManager(), captures = [], placeholder = { path: "", setPath(path) { this.path = path || ""; return this; }, load(source) { const texture = new Texture(); texture.userData.apexFbxSource = String(source || ""); captures.push({ texture, source: String(source || "") }); return texture; } };
  manager.addHandler(/./, placeholder);
  return { manager, captures };
}

function parseFbxScene(bytes) {
  const capture = managerWithTextureCapture(), buffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  return { scene: new FBXLoader(capture.manager).parse(buffer, ""), captures: capture.captures };
}

export function parseFbx(input, sourceName = "scene.fbx") {
  const bytes = bytesOf(input), header = inspectFbxHeader(bytes);
  try {
    const { scene, captures } = parseFbxScene(bytes), model = convertFbxScene(scene, sourceName, bytes.byteLength, header);
    for (const capture of captures) if (/^blob:/i.test(capture.source)) globalThis.URL?.revokeObjectURL?.(capture.source);
    return model;
  } catch (error) {
    if (error instanceof FbxImportError) throw error;
    throw new FbxImportError(`Could not import ${sourceName}: ${error.message}`, error);
  }
}

/** Import an FBX and preserve supported embedded images or selected source-folder textures. */
export async function parseFbxWithTextures(input, sourceName = "scene.fbx", filesOrIndex = []) {
  const bytes = bytesOf(input), header = inspectFbxHeader(bytes);
  try {
    const { scene, captures } = parseFbxScene(bytes), index = asFileIndex(filesOrIndex), cache = new Map();
    await Promise.all(captures.map((capture) => resolveCapturedTexture(capture, index, cache)));
    return convertFbxScene(scene, sourceName, bytes.byteLength, header);
  } catch (error) {
    if (error instanceof FbxImportError) throw error;
    throw new FbxImportError(`Could not import ${sourceName}: ${error.message}`, error);
  }
}
