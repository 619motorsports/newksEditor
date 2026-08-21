const STOCK_TYRE_SHADERS = new Set(["kstyres", "newstefano_kstyres"]);
const REQUIRED_TYRE_RESOURCES = Object.freeze(["txDiffuse", "txNormal", "txDirty", "txBlur", "txNormalBlur"]);

function finiteVector(value, length, label) {
  if ((!Array.isArray(value) && !ArrayBuffer.isView(value)) || value.length < length) throw new TypeError(`${label} needs ${length} components`);
  const result = Array.from(value).slice(0, length).map(Number);
  if (result.some((component) => !Number.isFinite(component))) throw new TypeError(`${label} needs finite components`);
  return result;
}

function normalized(value, label) {
  const vector = finiteVector(value, 3, label), length = Math.hypot(...vector);
  if (!(length > 1e-12)) throw new TypeError(`${label} cannot have zero length`);
  return vector.map((component) => component / length);
}

function mix(first, second, amount) {
  return first.map((value, index) => value + (second[index] - value) * amount);
}

/** Return true for the two stock shader packages that share the tyre pixel program. */
export function isStockTyreShader(value) {
  return STOCK_TYRE_SHADERS.has(String(value || "").trim().toLowerCase());
}

/** Accept the normalized live tyre values supplied by the game. */
export function normalizeTyrePreviewLevel(value, label = "Tyre preview level") {
  if (typeof value !== "number" && (typeof value !== "string" || value.trim() === "")) throw new TypeError(`${label} must be a number`);
  const number = Number(value);
  if (!Number.isFinite(number)) throw new TypeError(`${label} must be finite`);
  if (number < 0 || number > 1) throw new RangeError(`${label} must be from 0 to 1`);
  return number;
}

/** Reproduce the stock txDiffuse, txBlur, and txDirty pixel operations. */
export function stockTyreTexel(diffuseValue, blurValue, dirtyValue, blurLevelValue, dirtyLevelValue) {
  const diffuse = finiteVector(diffuseValue, 4, "Tyre diffuse texel");
  const blur = finiteVector(blurValue, 4, "Tyre blur texel");
  const dirty = finiteVector(dirtyValue, 4, "Tyre dirt texel");
  const blurLevel = normalizeTyrePreviewLevel(blurLevelValue, "Tyre blur level");
  const dirtyLevel = normalizeTyrePreviewLevel(dirtyLevelValue, "Tyre dirt level");
  const base = mix(diffuse, blur, blurLevel), dirtyMix = dirty[3] * dirtyLevel;
  return [...mix(base.slice(0, 3), dirty.slice(0, 3), dirtyMix), base[3]];
}

/** Reproduce the stock tangent-frame normal blend without an extra final normalization. */
export function stockTyreNormal(normalValue, normalBlurValue, tangentValue, bitangentValue, geometricNormalValue, blurLevelValue) {
  const normal = finiteVector(normalValue, 3, "Tyre normal texel").map((value) => value * 2 - 1);
  const normalBlur = finiteVector(normalBlurValue, 3, "Tyre blurred-normal texel").map((value) => value * 2 - 1);
  const tangent = normalized(tangentValue, "Tyre tangent"), bitangent = normalized(bitangentValue, "Tyre bitangent"), geometric = normalized(geometricNormalValue, "Tyre geometric normal");
  const mapped = normalized(tangent.map((value, index) => value * normal[0] + bitangent[index] * normal[1] + geometric[index] * normal[2]), "Mapped tyre normal");
  const mappedBlur = normalized(tangent.map((value, index) => value * normalBlur[0] + bitangent[index] * normalBlur[1] + geometric[index] * normalBlur[2]), "Mapped blurred tyre normal");
  return mix(mapped, mappedBlur, normalizeTyrePreviewLevel(blurLevelValue, "Tyre blur level"));
}

/** Reproduce the stock dirt reductions for direct specular and the Fresnel cap. */
export function stockTyreSpecular(diffuseAlpha, ksSpecular, dirtyLevelValue) {
  const alpha = Number(diffuseAlpha), specular = Number(ksSpecular), dirtyLevel = normalizeTyrePreviewLevel(dirtyLevelValue, "Tyre dirt level");
  if (!Number.isFinite(alpha) || !Number.isFinite(specular)) throw new TypeError("Tyre specular inputs must be finite");
  return alpha * (1 - dirtyLevel) * specular;
}

export function stockTyreFresnelCap(fresnelMaxLevel, dirtyLevelValue) {
  const level = Number(fresnelMaxLevel), dirtyLevel = normalizeTyrePreviewLevel(dirtyLevelValue, "Tyre dirt level");
  if (!Number.isFinite(level)) throw new TypeError("Tyre Fresnel level must be finite");
  return level * Math.max(0, Math.min(1, 1 - dirtyLevel));
}

/** Audit declared tyre slots, and only call a path exact when every resolved texture is usable. */
export function auditStockTyreMaterials(materials = [], resourceUsable = null) {
  if (!Array.isArray(materials)) throw new TypeError("Tyre material audit needs an array");
  if (resourceUsable !== null && typeof resourceUsable !== "function") throw new TypeError("Tyre resource validation must be a function");
  const entries = [];
  for (let materialId = 0; materialId < materials.length; materialId++) {
    const material = materials[materialId];
    if (!isStockTyreShader(material?.shader)) continue;
    const resources = Array.isArray(material?.resources) ? material.resources : [], bySlot = new Map(resources.map((resource) => [String(resource?.slot || "").toLowerCase(), resource]));
    const missingResources = REQUIRED_TYRE_RESOURCES.filter((slot) => {
      const resource = bySlot.get(slot.toLowerCase());
      return !resource || (resourceUsable ? !resourceUsable(material, resource, slot, materialId) : false);
    });
    const declaredComplete = REQUIRED_TYRE_RESOURCES.every((slot) => bySlot.has(slot.toLowerCase())), verified = Boolean(resourceUsable), complete = verified && missingResources.length === 0;
    entries.push({ materialId, name: String(material?.name || `Material ${materialId}`), shader: material.shader, declaredComplete, verified, complete, missingResources });
  }
  return {
    materials: entries.length,
    declaredCompleteMaterials: entries.filter((entry) => entry.declaredComplete).length,
    completeMaterials: entries.filter((entry) => entry.complete).length,
    incompleteMaterials: entries.filter((entry) => !entry.complete).length,
    entries
  };
}

export { REQUIRED_TYRE_RESOURCES };
