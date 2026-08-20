import { lastValue, parseCspIni } from "./csp-config.js";

const FLOAT32_MAXIMUM = 3.4028234663852886e38;
const MAXIMUM_VISUAL_OBJECTS = 1024;
const SECTION_KEYS = Object.freeze({
  SCRATCHES: Object.freeze(["minSpeed", "maxSpeed"]),
  OSCILLATIONS: Object.freeze(["enabled"]),
  DAMAGE: Object.freeze(["initialLevel"]),
  VISUAL_OBJECT: Object.freeze([
    "name", "staticRotationAxis", "staticRotationAngle", "multG", "damageZone",
    "minSpeed", "fullSpeed", "oscillationAxis", "oscillationMinAngle",
    "oscillationMaxAngle", "allowedG"
  ])
});

const INI_KEYS = Object.freeze({
  SCRATCHES: Object.freeze({ minSpeed: "MIN_SPEED", maxSpeed: "MAX_SPEED" }),
  OSCILLATIONS: Object.freeze({ enabled: "ENABLED" }),
  DAMAGE: Object.freeze({ initialLevel: "INITIAL_LEVEL" }),
  VISUAL_OBJECT: Object.freeze({
    name: "NAME", staticRotationAxis: "STATIC_ROTATION_AXIS",
    staticRotationAngle: "STATIC_ROTATION_ANGLE", multG: "MULT_G",
    damageZone: "DAMAGE_ZONE", minSpeed: "MIN_SPEED", fullSpeed: "FULL_SPEED",
    oscillationAxis: "OSCILLATION_AXIS", oscillationMinAngle: "OSCILLATION_MIN_ANGLE",
    oscillationMaxAngle: "OSCILLATION_MAX_ANGLE", allowedG: "ALLOWED_G"
  })
});

function sectionType(name) {
  return /^VISUAL_OBJECT_\d+$/i.test(name) ? "VISUAL_OBJECT" : String(name || "").toUpperCase();
}

export function damageEditKeys(section) {
  return SECTION_KEYS[sectionType(section)] || [];
}

function finiteNumber(section, key, fallback, warnings, source) {
  const raw = lastValue(section, key, String(fallback)), value = Number(raw);
  if (!Number.isFinite(value) || Math.abs(value) > FLOAT32_MAXIMUM) {
    warnings.push(`${source}:${section?.line || 1}: ${section?.name || "damage.ini"} ${key} must be a finite float32 number`);
    return fallback;
  }
  return value;
}

function finiteVector(section, key, fallback, warnings, source) {
  const raw = lastValue(section, key, fallback.join(","));
  const values = raw.split(",").map((value) => Number(value.trim()));
  if (values.length !== 3 || values.some((value) => !Number.isFinite(value) || Math.abs(value) > FLOAT32_MAXIMUM)) {
    warnings.push(`${source}:${section?.line || 1}: ${section?.name || "damage.ini"} ${key} must contain three finite float32 numbers`);
    return [...fallback];
  }
  return values;
}

function safeToken(value, fallback, label, warnings, source, line) {
  const text = String(value || "").trim().toUpperCase();
  if (!text || text.length > 64 || !/^[A-Z0-9_-]+$/.test(text)) {
    warnings.push(`${source}:${line}: ${label} must contain 1 to 64 letters, numbers, underscores, or hyphens`);
    return fallback;
  }
  return text;
}

function extraEntries(section, type) {
  const known = new Set(Object.values(INI_KEYS[type] || {}));
  return (section?.entries || []).filter((entry) => !known.has(entry.key)).map((entry) => ({ key: entry.key, value: entry.value }));
}

function cloneEntries(entries) {
  return (entries || []).map((entry) => ({ ...entry }));
}

function defaultSection(name, values) {
  return { section: name, line: 0, ...values, extraEntries: [] };
}

export function parseCarDamageIni(text, source = "data/damage.ini") {
  const ini = parseCspIni(text, source);
  const warnings = ini.warnings.map((warning) => `${warning.source}:${warning.line}: ${warning.message}`);
  const byName = new Map(), visualObjects = [], extraSections = [];
  for (const section of ini.sections) {
    const upper = section.name.toUpperCase();
    if (byName.has(upper)) {
      warnings.push(`${source}:${section.line}: duplicate ${section.name} section`);
      continue;
    }
    byName.set(upper, section);
  }

  const scratchesSection = byName.get("SCRATCHES");
  const scratches = scratchesSection ? {
    section: scratchesSection.name, line: scratchesSection.line,
    minSpeed: finiteNumber(scratchesSection, "MIN_SPEED", 0, warnings, source),
    maxSpeed: finiteNumber(scratchesSection, "MAX_SPEED", 20, warnings, source),
    extraEntries: extraEntries(scratchesSection, "SCRATCHES")
  } : defaultSection("SCRATCHES", { minSpeed: 0, maxSpeed: 20 });

  const oscillationsSection = byName.get("OSCILLATIONS");
  const oscillations = oscillationsSection ? {
    section: oscillationsSection.name, line: oscillationsSection.line,
    enabled: finiteNumber(oscillationsSection, "ENABLED", 1, warnings, source) !== 0,
    extraEntries: extraEntries(oscillationsSection, "OSCILLATIONS")
  } : defaultSection("OSCILLATIONS", { enabled: true });

  const damageSection = byName.get("DAMAGE");
  const damage = damageSection ? {
    section: damageSection.name, line: damageSection.line,
    initialLevel: finiteNumber(damageSection, "INITIAL_LEVEL", 0, warnings, source),
    extraEntries: extraEntries(damageSection, "DAMAGE")
  } : defaultSection("DAMAGE", { initialLevel: 0 });

  if (scratches.minSpeed < 0 || scratches.maxSpeed < 0 || scratches.maxSpeed < scratches.minSpeed) warnings.push(`${source}: SCRATCHES needs nonnegative speeds and MAX_SPEED must not be less than MIN_SPEED`);
  if (damage.initialLevel < 0 || damage.initialLevel > 100) warnings.push(`${source}: DAMAGE INITIAL_LEVEL must be from 0 to 100`);

  const seenIndices = new Set();
  for (const section of ini.sections) {
    const match = section.name.match(/^VISUAL_OBJECT_(\d+)$/i);
    if (!match) continue;
    const index = Number(match[1]);
    if (!Number.isSafeInteger(index) || index < 0 || index >= MAXIMUM_VISUAL_OBJECTS) {
      warnings.push(`${source}:${section.line}: ${section.name} index must be from 0 to ${MAXIMUM_VISUAL_OBJECTS - 1}`);
      continue;
    }
    if (seenIndices.has(index)) continue;
    seenIndices.add(index);
    const name = lastValue(section, "NAME").trim();
    if (!name || name.length > 1024 || /[\r\n;]/.test(name)) {
      warnings.push(`${source}:${section.line}: ${section.name} NAME must contain 1 to 1024 safe characters`);
      continue;
    }
    const object = {
      index, section: section.name, line: section.line, name,
      staticRotationAxis: finiteVector(section, "STATIC_ROTATION_AXIS", [0, 0, 0], warnings, source),
      staticRotationAngle: finiteNumber(section, "STATIC_ROTATION_ANGLE", 0, warnings, source),
      multG: finiteNumber(section, "MULT_G", 0, warnings, source),
      damageZone: safeToken(lastValue(section, "DAMAGE_ZONE", "FRONT"), "FRONT", `${section.name} DAMAGE_ZONE`, warnings, source, section.line),
      minSpeed: finiteNumber(section, "MIN_SPEED", 0, warnings, source),
      fullSpeed: finiteNumber(section, "FULL_SPEED", 0, warnings, source),
      oscillationAxis: finiteVector(section, "OSCILLATION_AXIS", [0, 0, 0], warnings, source),
      oscillationMinAngle: finiteNumber(section, "OSCILLATION_MIN_ANGLE", 0, warnings, source),
      oscillationMaxAngle: finiteNumber(section, "OSCILLATION_MAX_ANGLE", 0, warnings, source),
      allowedG: finiteVector(section, "ALLOWED_G", [1, 1, 1], warnings, source),
      extraEntries: extraEntries(section, "VISUAL_OBJECT")
    };
    if (object.minSpeed < 0 || object.fullSpeed < 0 || object.fullSpeed < object.minSpeed) warnings.push(`${source}:${section.line}: ${section.name} needs nonnegative speeds and FULL_SPEED must not be less than MIN_SPEED`);
    if (object.oscillationMaxAngle < object.oscillationMinAngle) warnings.push(`${source}:${section.line}: ${section.name} OSCILLATION_MAX_ANGLE must not be less than OSCILLATION_MIN_ANGLE`);
    visualObjects.push(object);
  }
  visualObjects.sort((a, b) => a.index - b.index);
  const indices = new Set(visualObjects.map((object) => object.index));
  for (let index = 0; index < (visualObjects.at(-1)?.index || 0); index++) if (!indices.has(index)) warnings.push(`${source}: VISUAL_OBJECT_${index} is missing`);

  const recognized = new Set(["SCRATCHES", "OSCILLATIONS", "DAMAGE", ...[...seenIndices].map((index) => `VISUAL_OBJECT_${index}`)]);
  for (const section of ini.sections) if (!recognized.has(section.name.toUpperCase())) extraSections.push({ name: section.name, entries: cloneEntries(section.entries) });
  return { source, scratches, oscillations, damage, visualObjects, extraSections, warnings };
}

function damageNumber(value, label) {
  const number = Number(value);
  if (!Number.isFinite(number) || Math.abs(number) > FLOAT32_MAXIMUM) throw new TypeError(`${label} must fit a finite float32 value`);
  return Number(number.toFixed(6)).toString();
}

function damageVector(value, label) {
  if (!Array.isArray(value) || value.length !== 3) throw new TypeError(`${label} must contain three numbers`);
  return value.map((component) => damageNumber(component, label)).join(", ");
}

function safeIniText(value, label, maximum = 1024) {
  const text = String(value ?? "").trim();
  if (!text || text.length > maximum || /[\r\n;]/.test(text)) throw new TypeError(`${label} must contain 1 to ${maximum} safe characters`);
  return text;
}

function writeExtraEntries(lines, entries, label) {
  for (const entry of entries || []) {
    const key = String(entry?.key || "").trim().toUpperCase(), value = String(entry?.value ?? "");
    if (!/^[A-Z0-9_.-]{1,128}$/.test(key) || /[\r\n]/.test(value) || value.length > 4096) throw new TypeError(`${label} contains an unsafe extra entry`);
    lines.push(`${key}=${value}`);
  }
}

function writeSection(name, fields, extras = []) {
  const sectionName = safeIniText(name, "Section name", 128);
  if (sectionName.includes("[") || sectionName.includes("]")) throw new TypeError("Section name contains a bracket");
  const lines = [`[${sectionName}]`, ...fields];
  writeExtraEntries(lines, extras, sectionName);
  return lines.join("\n");
}

export function serializeCarDamageIni(config) {
  if (!config || typeof config !== "object") throw new TypeError("A car damage configuration is required");
  const sections = [];
  for (const extra of config.extraSections || []) sections.push(writeSection(extra.name, [], extra.entries));
  const scratches = config.scratches || {};
  if (Number(scratches.minSpeed) < 0 || Number(scratches.maxSpeed) < Number(scratches.minSpeed)) throw new TypeError("SCRATCHES needs nonnegative ordered speeds");
  sections.push(writeSection("SCRATCHES", [
    `MAX_SPEED=${damageNumber(scratches.maxSpeed, "SCRATCHES MAX_SPEED")}`,
    `MIN_SPEED=${damageNumber(scratches.minSpeed, "SCRATCHES MIN_SPEED")}`
  ], scratches.extraEntries));
  sections.push(writeSection("OSCILLATIONS", [`ENABLED=${config.oscillations?.enabled ? 1 : 0}`], config.oscillations?.extraEntries));
  const initialLevel = Number(config.damage?.initialLevel);
  if (!Number.isFinite(initialLevel) || initialLevel < 0 || initialLevel > 100) throw new TypeError("DAMAGE INITIAL_LEVEL must be from 0 to 100");
  sections.push(writeSection("DAMAGE", [`INITIAL_LEVEL=${damageNumber(initialLevel, "DAMAGE INITIAL_LEVEL")}`], config.damage?.extraEntries));
  const used = new Set();
  for (const object of config.visualObjects || []) {
    const index = Number(object.index);
    if (!Number.isSafeInteger(index) || index < 0 || index >= MAXIMUM_VISUAL_OBJECTS || used.has(index)) throw new TypeError(`Visual-object indices must be unique integers from 0 to ${MAXIMUM_VISUAL_OBJECTS - 1}`);
    used.add(index);
    if (Number(object.minSpeed) < 0 || Number(object.fullSpeed) < Number(object.minSpeed)) throw new TypeError(`VISUAL_OBJECT_${index} needs nonnegative ordered speeds`);
    if (Number(object.oscillationMaxAngle) < Number(object.oscillationMinAngle)) throw new TypeError(`VISUAL_OBJECT_${index} needs ordered oscillation angles`);
    const zone = safeIniText(object.damageZone, `VISUAL_OBJECT_${index} DAMAGE_ZONE`, 64).toUpperCase();
    if (!/^[A-Z0-9_-]+$/.test(zone)) throw new TypeError(`VISUAL_OBJECT_${index} DAMAGE_ZONE is invalid`);
    sections.push(writeSection(`VISUAL_OBJECT_${index}`, [
      `NAME=${safeIniText(object.name, `VISUAL_OBJECT_${index} NAME`)}`,
      `STATIC_ROTATION_AXIS=${damageVector(object.staticRotationAxis, `VISUAL_OBJECT_${index} STATIC_ROTATION_AXIS`)}`,
      `STATIC_ROTATION_ANGLE=${damageNumber(object.staticRotationAngle, `VISUAL_OBJECT_${index} STATIC_ROTATION_ANGLE`)}`,
      `MULT_G=${damageNumber(object.multG, `VISUAL_OBJECT_${index} MULT_G`)}`,
      `DAMAGE_ZONE=${zone}`,
      `MIN_SPEED=${damageNumber(object.minSpeed, `VISUAL_OBJECT_${index} MIN_SPEED`)}`,
      `FULL_SPEED=${damageNumber(object.fullSpeed, `VISUAL_OBJECT_${index} FULL_SPEED`)}`,
      `OSCILLATION_AXIS=${damageVector(object.oscillationAxis, `VISUAL_OBJECT_${index} OSCILLATION_AXIS`)}`,
      `OSCILLATION_MIN_ANGLE=${damageNumber(object.oscillationMinAngle, `VISUAL_OBJECT_${index} OSCILLATION_MIN_ANGLE`)}`,
      `OSCILLATION_MAX_ANGLE=${damageNumber(object.oscillationMaxAngle, `VISUAL_OBJECT_${index} OSCILLATION_MAX_ANGLE`)}`,
      `ALLOWED_G=${damageVector(object.allowedG, `VISUAL_OBJECT_${index} ALLOWED_G`)}`
    ], object.extraEntries));
  }
  return `${sections.join("\n\n")}\n`;
}

function cloneEditable(section) {
  return Object.fromEntries(damageEditKeys(section.section).map((key) => [key, Array.isArray(section[key]) ? [...section[key]] : section[key]]));
}

export function captureCarDamageBaseline(config) {
  if (!config) return null;
  return Object.fromEntries([
    ["SCRATCHES", cloneEditable(config.scratches)],
    ["OSCILLATIONS", cloneEditable(config.oscillations)],
    ["DAMAGE", cloneEditable(config.damage)],
    ...(config.visualObjects || []).map((object) => [object.section.toUpperCase(), cloneEditable(object)])
  ]);
}

export function carDamageEditCount(edits) {
  return Object.entries(edits || {}).reduce((count, [section, edit]) => count + damageEditKeys(section).filter((key) => edit?.[key] !== undefined).length, 0);
}

export function applyCarDamageEdits(config, edits = {}, baseline) {
  if (!config || !baseline) return 0;
  const targets = new Map([
    ["SCRATCHES", config.scratches], ["OSCILLATIONS", config.oscillations], ["DAMAGE", config.damage],
    ...(config.visualObjects || []).map((object) => [object.section.toUpperCase(), object])
  ]);
  let applied = 0;
  for (const [section, target] of targets) {
    const source = baseline[section];
    if (!source) continue;
    for (const key of damageEditKeys(section)) target[key] = Array.isArray(source[key]) ? [...source[key]] : source[key];
    const edit = edits?.[section];
    if (!edit) continue;
    for (const key of damageEditKeys(section)) if (edit[key] !== undefined) {
      target[key] = Array.isArray(edit[key]) ? [...edit[key]] : edit[key];
      applied++;
    }
  }
  return applied;
}
