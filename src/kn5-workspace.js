import { lastValue, parseCspIni, splitCspList } from "./csp-config.js";

const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

function vector(value, fallback, warnings, label) {
  const parts = splitCspList(value).map(Number);
  if (parts.length === 3 && parts.every(Number.isFinite)) return parts;
  if (String(value).trim()) warnings.push(`${label} must contain three finite numbers`);
  return [...fallback];
}

function unquote(value) {
  return String(value || "").trim().replace(/^(['"])(.*)\1$/, "$2");
}

function finiteNumber(section, key, fallback, warnings, source, required = false) {
  const raw = lastValue(section, key);
  if (raw === "") {
    if (required) warnings.push(`${source}:${section.line}: ${section.name} has no ${key}`);
    return fallback;
  }
  const value = Number(raw);
  if (Number.isFinite(value)) return value;
  warnings.push(`${source}:${section.line}: ${section.name} ${key} must be a finite number`);
  return fallback;
}

export function parseModelsIni(text, source = "models.ini") {
  const config = parseCspIni(text, source), models = [], dynamicObjects = [], warnings = config.warnings.map((warning) => `${warning.source}:${warning.line}: ${warning.message}`);
  for (const section of config.sections) {
    const match = section.name.match(/^MODEL_(\d+)$/i);
    const dynamicMatch = section.name.match(/^DYNAMIC_OBJECT_(\d+)$/i);
    if (!match && !dynamicMatch) continue;
    const file = unquote(lastValue(section, "FILE"));
    if (!file) { warnings.push(`${source}:${section.line}: ${section.name} has no FILE`); continue; }
    if (dynamicMatch) {
      const probability = finiteNumber(section, "PROBABILITY", 100, warnings, source), multRaw = splitCspList(lastValue(section, "MULT", "1,1")).map(Number), multiplicity = multRaw.length === 2 && multRaw.every(Number.isFinite) ? multRaw : [1, 1];
      if (probability < 0 || probability > 100) warnings.push(`${source}:${section.line}: ${section.name} PROBABILITY should be from 0 to 100`);
      if (multRaw.length !== 2 || multRaw.some((value) => !Number.isFinite(value))) warnings.push(`${source}:${section.line}: ${section.name} MULT must contain two finite numbers`);
      dynamicObjects.push({ index: Number(dynamicMatch[1]), file, probability, multiplicity, posMode: lastValue(section, "POS_MODE", "RANDOM").trim().toUpperCase(), positionCenter: vector(lastValue(section, "RND_POS_CENTER", "0,0,0"), [0, 0, 0], warnings, `${source}:${section.line} RND_POS_CENTER`), positionRange: vector(lastValue(section, "RND_POS_RANGE", "0,0,0"), [0, 0, 0], warnings, `${source}:${section.line} RND_POS_RANGE`), velMode: lastValue(section, "VEL_MODE", "RANDOM").trim().toUpperCase(), velocityBase: vector(lastValue(section, "RND_VEL_BASE", "0,0,0"), [0, 0, 0], warnings, `${source}:${section.line} RND_VEL_BASE`), velocityRange: vector(lastValue(section, "RND_VEL_RANGE", "0,0,0"), [0, 0, 0], warnings, `${source}:${section.line} RND_VEL_RANGE`), playWav: unquote(lastValue(section, "PLAY_WAV")), section: section.name, line: section.line });
      continue;
    }
    models.push({
      index: Number(match[1]),
      file,
      position: vector(lastValue(section, "POSITION", "0,0,0"), [0, 0, 0], warnings, `${source}:${section.line} POSITION`),
      rotation: vector(lastValue(section, "ROTATION", "0,0,0"), [0, 0, 0], warnings, `${source}:${section.line} ROTATION`),
      section: section.name,
      line: section.line
    });
  }
  models.sort((a, b) => a.index - b.index); dynamicObjects.sort((a, b) => a.index - b.index);
  return { source, models, dynamicObjects, warnings, ignoredSections: config.sections.length - models.length - dynamicObjects.length };
}

export function parseCarLodsIni(text, source = "data/lods.ini") {
  const config = parseCspIni(text, source);
  const warnings = config.warnings.map((warning) => `${warning.source}:${warning.line}: ${warning.message}`);
  const sections = new Map();
  for (const section of config.sections) {
    const match = section.name.match(/^LOD_(\d+)$/i);
    if (!match) continue;
    const index = Number(match[1]);
    if (sections.has(index)) warnings.push(`${source}:${section.line}: duplicate LOD_${index} section replaces the earlier section`);
    sections.set(index, section);
  }

  const lods = [];
  for (let index = 0; sections.has(index); index++) {
    const section = sections.get(index);
    const file = unquote(lastValue(section, "FILE"));
    const lodIn = finiteNumber(section, "IN", 0, warnings, source, true);
    const lodOut = finiteNumber(section, "OUT", 0, warnings, source, true);
    if (!file) warnings.push(`${source}:${section.line}: ${section.name} has no FILE`);
    if (lodIn < 0 || lodOut < 0) warnings.push(`${source}:${section.line}: ${section.name} has a negative distance`);
    if (lodOut <= lodIn) warnings.push(`${source}:${section.line}: ${section.name} OUT must be greater than IN`);
    lods.push({ index, file, in: lodIn, out: lodOut, section: section.name, line: section.line });
  }
  if (!lods.length) warnings.push(`${source}: no contiguous LOD_0 section was found`);
  for (const index of [...sections.keys()].sort((a, b) => a - b)) {
    if (index >= lods.length) warnings.push(`${source}: LOD_${index} is ignored because the game stops at missing LOD_${lods.length}`);
  }
  for (let index = 1; index < lods.length; index++) {
    const previous = lods[index - 1], current = lods[index];
    if (current.in > previous.out) warnings.push(`${source}:${current.line}: ${previous.section} and ${current.section} leave a ${current.in - previous.out} m gap`);
    else if (current.in < previous.out) warnings.push(`${source}:${current.line}: ${previous.section} and ${current.section} overlap by ${previous.out - current.in} m`);
  }

  const distanceSwitch = (name) => {
    const section = config.sections.find((candidate) => candidate.name.toUpperCase() === name);
    return section?.values.has("DISTANCE_SWITCH") ? finiteNumber(section, "DISTANCE_SWITCH", null, warnings, source) : null;
  };
  return {
    source,
    lods,
    cockpitHrDistance: distanceSwitch("COCKPIT_HR"),
    driverHrDistance: distanceSwitch("DRIVER_HR"),
    warnings,
    ignoredSections: config.sections.length - sections.size
  };
}

export function carLodVisible(lod, distance, selectedIndex = null) {
  if (!lod) return true;
  if (selectedIndex !== null && selectedIndex !== undefined) return lod.index === Number(selectedIndex);
  const value = Math.max(0, Number(distance) || 0);
  return value >= lod.in && value < lod.out;
}

export function carLodDistance(cameraDistance, fovDegrees = 45, lodDistDivisor = 1, trackCamera = false) {
  const distance = Math.max(0, Number(cameraDistance) || 0), fov = Math.max(0, Number(fovDegrees) || 0);
  const divisor = Math.max(Number.EPSILON, Math.abs(Number(lodDistDivisor) || 1)) * (trackCamera ? 10 : 1);
  return distance * fov / 60 / divisor;
}

function multiply(a, b) {
  const output = new Array(16);
  for (let row = 0; row < 4; row++) for (let column = 0; column < 4; column++) {
    output[row * 4 + column] = 0;
    for (let index = 0; index < 4; index++) output[row * 4 + column] += a[row * 4 + index] * b[index * 4 + column];
  }
  return output;
}

export function modelPlacementMatrix(position = [0, 0, 0], rotation = [0, 0, 0]) {
  const radians = rotation.map((value) => Number(value || 0) * Math.PI / 180);
  const [heading, pitch, roll] = radians, cy = Math.cos(heading), sy = Math.sin(heading), cx = Math.cos(pitch), sx = Math.sin(pitch), cz = Math.cos(roll), sz = Math.sin(roll);
  // This reproduces acs.exe mat44f::createFromEuler(): Y(-x) * X(-y) * Z(-z)
  // in the row-major mat44f memory layout used by KN5 node transforms.
  const yaw = [cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0, 0, 0, 0, 1];
  const pitchMatrix = [1, 0, 0, 0, 0, cx, -sx, 0, 0, sx, cx, 0, 0, 0, 0, 1];
  const rollMatrix = [cz, -sz, 0, 0, sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const result = multiply(multiply(yaw, pitchMatrix), rollMatrix);
  result[12] = Number(position[0]) || 0; result[13] = Number(position[1]) || 0; result[14] = Number(position[2]) || 0;
  return result;
}

function remapNode(node, materialOffset) {
  const output = { ...node, children: (node.children || []).map((child) => remapNode(child, materialOffset)) };
  if ((node.kind === "mesh" || node.kind === "skinnedMesh") && Number.isInteger(node.materialId)) output.materialId = node.materialId + materialOffset;
  return output;
}

export function mergeKn5Models(entries, options = {}) {
  if (!Array.isArray(entries) || !entries.length) throw new Error("A KN5 workspace needs at least one model");
  const materials = [], textureMap = new Map(), scopedTextures = [], files = [], children = [], textureCollisions = [];
  const scopeResources = options.kind === "carLods" || entries.some((entry) => entry.auxiliary === "reflectionEnvironment");
  let bytesRead = 0, byteLength = 0, source = 0;
  const versions = new Set(), protectedFiles = [];
  for (const entry of entries) {
    const model = entry.model;
    if (!model?.root || !Array.isArray(model.materials) || !Array.isArray(model.textures)) throw new Error(`${entry.name || "Workspace entry"} is not a parsed KN5 model`);
    const materialOffset = materials.length;
    materials.push(...model.materials.map((material) => scopeResources ? { ...material, workspaceFile: entry.name || "" } : material));
    for (const texture of model.textures) {
      const key = texture.name.toLowerCase(), previous = textureMap.get(key);
      if (previous) textureCollisions.push({ name: texture.name, previousFile: previous.file, replacementFile: entry.name, sizesDiffer: previous.texture.size !== texture.size });
      textureMap.set(key, { texture, file: entry.name });
      if (scopeResources) scopedTextures.push({ ...texture, workspaceFile: entry.name || "" });
    }
    const root = remapNode(model.root, materialOffset), transform = modelPlacementMatrix(entry.position, entry.rotation);
    const workspaceLod = entry.lod ? { index: Number(entry.lod.index), in: Number(entry.lod.in) || 0, out: Number(entry.lod.out) || 0 } : null;
    children.push({ kind: "node", name: entry.name || `MODEL_${files.length}`, active: true, transform, children: [root], workspaceFile: entry.name || "", workspaceLod, workspaceAuxiliary:entry.auxiliary||null });
    files.push({ name: entry.name || "", size: Math.max(0, Number(entry.size) || model.byteLength || 0), version: model.version, materialOffset, materials: model.materials.length, textures: model.textures.length, position: entry.position || [0, 0, 0], rotation: entry.rotation || [0, 0, 0], lod: workspaceLod, auxiliary:entry.auxiliary||null, dynamic: entry.dynamic || null, protected: Boolean(model.encryption) });
    if (model.encryption) protectedFiles.push({ name: entry.name || "", encryption: model.encryption });
    bytesRead += model.bytesRead || 0; byteLength += model.byteLength || 0; source = Math.max(source, model.source || 0); versions.add(model.version);
  }
  return {
    magic: "sc6969",
    version: Math.max(...versions),
    source,
    textures: scopeResources ? scopedTextures : [...textureMap.values()].map((entry) => entry.texture),
    materials,
    root: { kind: "node", name: options.name || "KN5 workspace", active: true, transform: [...IDENTITY], children },
    bytesRead,
    byteLength,
    workspace: { name: options.name || "KN5 workspace", kind: options.kind || "track", manifest: options.manifest || "", files, versions: [...versions].sort((a, b) => a - b), textureCollisions, protectedFiles, warnings: [...(options.warnings || [])], cockpitHrDistance: options.cockpitHrDistance ?? null, driverHrDistance: options.driverHrDistance ?? null, scopeResources }
  };
}
