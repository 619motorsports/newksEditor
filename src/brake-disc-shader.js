import { lastValue, parseCspIni } from "./csp-config.js";

const STOCK_BRAKE_DISC_SHADER = "ksbrakedisc";
const REQUIRED_BRAKE_DISC_RESOURCES = Object.freeze(["txDiffuse", "txNormal", "txGlow", "txBlur", "txNormalBlur"]);
const CORNERS = Object.freeze({ LF: "front", RF: "front", LR: "rear", RR: "rear" });

function finiteNumber(value, label) {
  if (typeof value !== "number" && (typeof value !== "string" || value.trim() === "")) throw new TypeError(`${label} must be a number`);
  const number = Number(value);
  if (!Number.isFinite(number)) throw new TypeError(`${label} must be finite`);
  return number;
}

function nonnegative(value, label) {
  const number = finiteNumber(value, label);
  if (number < 0) throw new RangeError(`${label} cannot be negative`);
  return number;
}

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

function saturate(value) {
  return Math.max(0, Math.min(1, value));
}

function configNumber(section, key, fallback, source, warnings) {
  const raw = lastValue(section, key).trim(), label = `${section.name} ${key}`;
  if (!raw) { warnings.push(`${source}:${section.line}: ${label} is missing`); return fallback; }
  const value = Number(raw);
  if (!Number.isFinite(value)) { warnings.push(`${source}:${section.line}: ${label} must be finite`); return fallback; }
  if (value < 0) { warnings.push(`${source}:${section.line}: ${label} cannot be negative`); return fallback; }
  return value;
}

/** Return true only for the stock brake-disc shader package. */
export function isStockBrakeDiscShader(value) {
  return String(value || "").trim().toLowerCase() === STOCK_BRAKE_DISC_SHADER;
}

/** Parse the stock [DISCS_GRAPHICS] fields from data/brakes.ini. */
export function parseBrakeDiscConfig(text, source = "data/brakes.ini") {
  const config = parseCspIni(text, source), warnings = config.warnings.map((warning) => `${warning.source}:${warning.line}: ${warning.message}`);
  const sections = config.sections.filter((section) => section.name.toUpperCase() === "DISCS_GRAPHICS"), section = sections.at(-1);
  if (!section) return { source, configured: false, line: 0, nodes: {}, frontMaxGlow: 0, rearMaxGlow: 0, lagHot: 0, lagCool: 0, warnings: [...warnings, `${source}: DISCS_GRAPHICS section is missing`] };
  if (sections.length > 1) warnings.push(`${source}:${section.line}: duplicate DISCS_GRAPHICS section; the last section is used`);
  const nodes = {};
  for (const corner of Object.keys(CORNERS)) {
    const value = lastValue(section, `DISC_${corner}`).trim();
    if (value) nodes[corner] = value;
    else warnings.push(`${source}:${section.line}: DISCS_GRAPHICS DISC_${corner} is missing`);
  }
  return {
    source, configured: true, line: section.line, nodes,
    frontMaxGlow: configNumber(section, "FRONT_MAX_GLOW", 0, source, warnings),
    rearMaxGlow: configNumber(section, "REAR_MAX_GLOW", 0, source, warnings),
    lagHot: configNumber(section, "LAG_HOT", 0, source, warnings),
    lagCool: configNumber(section, "LAG_COOL", 0, source, warnings),
    warnings
  };
}

/** Match one brake-disc mesh to a configured or conventional wheel node. */
export function resolveBrakeDiscWheel(names, config = null) {
  const candidates = (Array.isArray(names) ? names : [names]).map((name) => String(name || "").trim()).filter(Boolean);
  const upper = candidates.map((name) => name.toUpperCase());
  for (const [corner, axle] of Object.entries(CORNERS)) {
    const configured = String(config?.nodes?.[corner] || "").trim().toUpperCase();
    if (configured && upper.includes(configured)) return { corner, axle, source: "brakes.ini" };
  }
  for (let index = upper.length - 1; index >= 0; index--) {
    const name = upper[index];
    if (!/(DISC|BRAKE)/.test(name)) continue;
    const corner = Object.keys(CORNERS).find((value) => name.endsWith(value) || new RegExp(`(?:^|[_ .-])${value}(?:$|[_ .-])`).test(name));
    if (corner) return { corner, axle: CORNERS[corner], source: "node-name" };
  }
  return { corner: null, axle: null, source: "unmapped" };
}

/** Reproduce the runtime target before the hot or cool lag is applied. */
export function brakeDiscGlowTarget(temperatureValue, maxGlowValue, multiplierValue = 1) {
  const temperature = finiteNumber(temperatureValue, "Brake temperature"), maxGlow = nonnegative(maxGlowValue, "Maximum brake glow"), multiplier = nonnegative(multiplierValue, "Brake glow multiplier");
  return maxGlow * multiplier * saturate((Math.abs(temperature) - 10) / 150);
}

/** Return the independent front and rear steady-state targets shown by the inspector. */
export function brakeDiscGlowTargets(temperatureValue, frontMaxGlowValue, rearMaxGlowValue) {
  return {
    frontGlowLevel: brakeDiscGlowTarget(temperatureValue, frontMaxGlowValue),
    rearGlowLevel: brakeDiscGlowTarget(temperatureValue, rearMaxGlowValue)
  };
}

/** Keep an unresolved wheel dark instead of guessing an axle. */
export function brakeDiscGlowForAxle(temperatureValue, axle, frontMaxGlowValue, rearMaxGlowValue) {
  const maximum = axle === "front" ? frontMaxGlowValue : axle === "rear" ? rearMaxGlowValue : 0;
  return brakeDiscGlowTarget(temperatureValue, maximum);
}

/** Reproduce one runtime hot/cool lag step. */
export function brakeDiscGlowStep(currentValue, targetValue, deltaTimeValue, lagHotValue, lagCoolValue) {
  const current = finiteNumber(currentValue, "Current brake glow"), target = finiteNumber(targetValue, "Target brake glow"), deltaTime = nonnegative(deltaTimeValue, "Brake glow delta time"), lagHot = nonnegative(lagHotValue, "Hot brake lag"), lagCool = nonnegative(lagCoolValue, "Cool brake lag");
  return current + (target - current) * saturate(deltaTime * (target >= current ? lagHot : lagCool));
}

/** Accept the normalized live blurLevel supplied by the game. */
export function normalizeBrakeDiscBlur(value) {
  const number = finiteNumber(value, "Brake-disc blur level");
  if (number < 0 || number > 1) throw new RangeError("Brake-disc blur level must be from 0 to 1");
  return number;
}

/** Reproduce the stock txDiffuse and txBlur RGBA blend. */
export function stockBrakeDiscTexel(diffuseValue, blurValue, blurLevelValue) {
  return mix(finiteVector(diffuseValue, 4, "Brake diffuse texel"), finiteVector(blurValue, 4, "Brake blur texel"), normalizeBrakeDiscBlur(blurLevelValue));
}

/** Reproduce the stock tangent-frame normal blend without a final normalization. */
export function stockBrakeDiscNormal(normalValue, normalBlurValue, tangentValue, bitangentValue, geometricNormalValue, blurLevelValue) {
  const normal = finiteVector(normalValue, 3, "Brake normal texel").map((value) => value * 2 - 1);
  const normalBlur = finiteVector(normalBlurValue, 3, "Brake blurred-normal texel").map((value) => value * 2 - 1);
  const tangent = normalized(tangentValue, "Brake tangent"), bitangent = normalized(bitangentValue, "Brake bitangent"), geometric = normalized(geometricNormalValue, "Brake geometric normal");
  const mapped = normalized(tangent.map((value, index) => value * normal[0] + bitangent[index] * normal[1] + geometric[index] * normal[2]), "Mapped brake normal");
  const mappedBlur = normalized(tangent.map((value, index) => value * normalBlur[0] + bitangent[index] * normalBlur[1] + geometric[index] * normalBlur[2]), "Mapped blurred brake normal");
  return mix(mapped, mappedBlur, normalizeBrakeDiscBlur(blurLevelValue));
}

/** Reproduce the stock base-color multiplied glow contribution. */
export function stockBrakeDiscGlow(baseValue, glowValue, glowLevelValue) {
  const base = finiteVector(baseValue, 3, "Brake base color"), glow = finiteVector(glowValue, 3, "Brake glow texel"), level = nonnegative(glowLevelValue, "Brake glow level");
  return base.map((value, index) => value * glow[index] * level);
}

/** Audit the five resources required for the exact stock brake-disc path. */
export function auditStockBrakeDiscMaterials(materials = []) {
  if (!Array.isArray(materials)) throw new TypeError("Brake material audit needs an array");
  const entries = [];
  for (let materialId = 0; materialId < materials.length; materialId++) {
    const material = materials[materialId];
    if (!isStockBrakeDiscShader(material?.shader)) continue;
    const slots = new Set((Array.isArray(material?.resources) ? material.resources : []).map((resource) => String(resource?.slot || "").toLowerCase()));
    const missingResources = REQUIRED_BRAKE_DISC_RESOURCES.filter((slot) => !slots.has(slot.toLowerCase()));
    entries.push({ materialId, name: String(material?.name || `Material ${materialId}`), shader: material.shader, complete: missingResources.length === 0, missingResources });
  }
  return { materials: entries.length, completeMaterials: entries.filter((entry) => entry.complete).length, incompleteMaterials: entries.filter((entry) => !entry.complete).length, entries };
}

export { REQUIRED_BRAKE_DISC_RESOURCES };
