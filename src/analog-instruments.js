import { lastValue, parseCspIni } from "./csp-config.js";
import { walkNodes } from "./kn5.js";

const MAX_CONFIG_CHARACTERS = 4 * 1024 * 1024;
const MAX_OBJECT_NAME_CHARACTERS = 1024;

function finiteField(section, key, source, warnings) {
  const raw = lastValue(section, key).trim();
  if (!raw) {
    warnings.push(`${source}:${section.line}: RPM_INDICATOR ${key} is missing`);
    return null;
  }
  const value = Number(raw);
  if (!Number.isFinite(value)) {
    warnings.push(`${source}:${section.line}: RPM_INDICATOR ${key} must be finite`);
    return null;
  }
  return value;
}

export function parseAnalogInstrumentsIni(text, source = "data/analog_instruments.ini") {
  const input = String(text);
  if (input.length > MAX_CONFIG_CHARACTERS) throw new RangeError(`${source}: analog instrument config is too large`);
  const config = parseCspIni(input, source);
  const warnings = config.warnings.map((warning) => `${warning.source}:${warning.line}: ${warning.message}`);
  const sections = config.sections.filter((section) => section.name.toUpperCase() === "RPM_INDICATOR");
  if (!sections.length) return { source, rpm: null, warnings };
  if (sections.length > 1) warnings.push(`${source}: multiple RPM_INDICATOR sections are present; the first section is used`);
  const section = sections[0], objectName = lastValue(section, "OBJECT_NAME").trim(), lut = lastValue(section, "LUT").trim();
  if (!objectName) warnings.push(`${source}:${section.line}: RPM_INDICATOR OBJECT_NAME is missing`);
  if (objectName.length > MAX_OBJECT_NAME_CHARACTERS) warnings.push(`${source}:${section.line}: RPM_INDICATOR OBJECT_NAME is too long`);
  const zero = finiteField(section, "ZERO", source, warnings), minValue = finiteField(section, "MIN_VALUE", source, warnings), step = finiteField(section, "STEP", source, warnings);
  if (lut) warnings.push(`${source}:${section.line}: RPM_INDICATOR LUT preview is not supported`);
  const valid = Boolean(objectName) && objectName.length <= MAX_OBJECT_NAME_CHARACTERS && zero !== null && minValue !== null && step !== null;
  return {
    source,
    rpm: valid ? { source, line: section.line, objectName, zero, minValue, step, lut, previewSupported: !lut } : null,
    warnings
  };
}

export function analogRpmAngle(config, rpm) {
  const value = Number(rpm), zero = Number(config?.zero), minimum = Number(config?.minValue), step = Number(config?.step);
  if (![value, zero, minimum, step].every(Number.isFinite)) throw new TypeError("A valid RPM config and finite RPM value are required");
  return (zero + (value - minimum) * step) * Math.PI / 180;
}

export function analogRpmRotation(config, rpm) {
  const angle = analogRpmAngle(config, rpm), cosine = Math.cos(angle), sine = Math.sin(angle);
  return [cosine, sine, 0, 0, -sine, cosine, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

export function analogRpmTransform(original, config, rpm) {
  if (!original || original.length !== 16 || ![...original].every(Number.isFinite)) throw new TypeError("A finite 4x4 original matrix is required");
  const rotation = analogRpmRotation(config, rpm), output = new Array(16);
  for (let column = 0; column < 4; column++) for (let row = 0; row < 4; row++) {
    output[column * 4 + row] = 0;
    for (let index = 0; index < 4; index++) output[column * 4 + row] += original[index * 4 + row] * rotation[column * 4 + index];
  }
  return output;
}

export function analogRpmPreviewEligible(config, assetFolderMatchesModel) {
  return Boolean(config?.previewSupported && assetFolderMatchesModel);
}

export function bindAnalogRpm(model, config) {
  const rows = model?.root && config?.objectName ? walkNodes(model.root).filter(({ node }) => node.name === config.objectName) : [];
  const matrixIsUsable = (node) => Array.isArray(node.transform) && node.transform.length === 16 && node.transform.every(Number.isFinite), nodes = rows.map(({ node }) => node).filter(matrixIsUsable);
  return {
    node: nodes[0] || null,
    nodes,
    objectName: config?.objectName || "",
    matches: rows.length,
    usableMatches: nodes.length,
    status: !config ? "unconfigured" : !rows.length ? "missing" : !nodes.length ? "no-transform" : nodes.length < rows.length ? "partial" : rows.length > 1 ? "resolved-multiple" : "resolved"
  };
}
