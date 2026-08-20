import { walkNodes } from "./kn5.js";
import { normalizeCustomEmissiveMirrorDirection } from "./custom-emissive-uv.js";

function stripComment(line) {
  let quote = null;
  for (let i = 0; i < line.length; i++) {
    const char = line[i];
    if ((char === '"' || char === "'") && line[i - 1] !== "\\") quote = quote === char ? null : quote || char;
    if (char === ";" && !quote) return line.slice(0, i);
  }
  return line;
}

export function parseCspIni(text, source = "ext_config.ini") {
  const sections = [];
  const warnings = [];
  let current = null;
  let continued = "";
  const lines = String(text).replace(/^\uFEFF/, "").replace(/\r/g, "").split("\n");
  for (let index = 0; index < lines.length; index++) {
    let line = stripComment(continued + lines[index]).trim();
    if (line.endsWith("\\")) { continued = line.slice(0, -1); continue; }
    continued = "";
    if (!line) continue;
    const header = line.match(/^\[([^\]]+)\]\s*$/);
    if (header) {
      current = { name: header[1].trim(), line: index + 1, source, entries: [], values: new Map() };
      sections.push(current);
      continue;
    }
    const equals = line.indexOf("=");
    if (equals < 0 || !current) { warnings.push({ source, line: index + 1, message: "Entry outside a section or without '='" }); continue; }
    const key = line.slice(0, equals).trim().toUpperCase();
    const value = line.slice(equals + 1).trim();
    const entry = { key, value, line: index + 1 };
    current.entries.push(entry);
    if (!current.values.has(key)) current.values.set(key, []);
    current.values.get(key).push(value);
  }
  return { source, sections, warnings };
}

export function lastValue(section, key, fallback = "") {
  const values = section?.values.get(key.toUpperCase());
  return values?.[values.length - 1] ?? fallback;
}

export function splitCspList(value) {
  const result = [];
  let start = 0, quote = null, depth = 0;
  for (let i = 0; i <= value.length; i++) {
    const char = value[i];
    if ((char === '"' || char === "'") && value[i - 1] !== "\\") quote = quote === char ? null : quote || char;
    if (!quote && (char === "(" || char === "{")) depth++;
    if (!quote && (char === ")" || char === "}")) depth--;
    if (i === value.length || (char === "," && !quote && depth === 0)) {
      const item = value.slice(start, i).trim().replace(/^(['"])(.*)\1$/, "$2");
      if (item) result.push(item);
      start = i + 1;
    }
  }
  return result;
}

export function parseCspValue(value) {
  const text = String(value).trim().replace(/^(['"])(.*)\1$/, "$2");
  if (/^(ORIGINAL|DISCARD)$/i.test(text)) return text.toUpperCase();
  const parts = splitCspList(text);
  if (parts.length > 1 && parts.every((part) => Number.isFinite(Number(part)))) return parts.map(Number);
  const number = Number(text);
  return text !== "" && Number.isFinite(number) ? number : text;
}

function generatedSection(name, source, fields) {
  const section = { name, line: 0, source, entries: [], values: new Map(), generated: true };
  for (const [key, raw] of Object.entries(fields)) {
    if (raw === undefined || raw === null || raw === "") continue;
    const value = Array.isArray(raw) ? raw.join(",") : String(raw);
    const upper = key.toUpperCase();
    section.entries.push({ key: upper, value, line: 0 });
    if (!section.values.has(upper)) section.values.set(upper, []);
    section.values.get(upper).push(value);
  }
  return section;
}

function parameters(section, defaults = {}) {
  const result = Object.fromEntries(Object.entries(defaults).map(([key, value]) => [key.toUpperCase(), value]));
  for (const entry of section.entries) if (!entry.key.startsWith("@")) result[entry.key] = parseCspValue(entry.value);
  return result;
}

function parameter(values, name, fallback) {
  const value = values[name.toUpperCase()];
  return value === undefined || value === "" ? fallback : value;
}

function numberParameter(values, name, fallback = 0) {
  const value = Number(parameter(values, name, fallback));
  return Number.isFinite(value) ? value : fallback;
}

function vectorParameter(values, name, fallback) {
  const value = parameter(values, name, fallback);
  return Array.isArray(value) ? value.map(Number) : typeof value === "number" ? [value] : parseCspValue(value);
}

function propertyFields(base, properties) {
  const fields = { ...base };
  let index = 0;
  for (const [name, value] of Object.entries(properties)) {
    if (value === undefined || value === "") continue;
    fields[`PROP_APEX_${index++}`] = [name, ...(Array.isArray(value) ? value : [value])];
  }
  return fields;
}

function carPaintReplacement(section, globals, index) {
  const variant = section.name.toUpperCase();
  const defaults = {
    MATERIALS: globals.CARPAINTMATERIAL || "Carpaint", BRIGHTNESSADJUSTMENT: 1, SPECULARMULT: 1,
    SPECULARBASE: [0.5, 50], SPECULARSUN: [2, 5000], SUNMULTIPLIER: 6,
    FRESNELMAX: 0.8, FRESNELC: 0.16, FRESNELEXP: 5, NORMALIZEAO: 0,
    FLAKESK: 0.1, COLOREDSPECULAR: 0, PEARLESCENTSPECULAR: 0,
    AMBIENTSPECULAR: 0, AMBIENTSPECULAREXP: 2.5, CLEARCOATTHICKNESS: 0,
    CLEARCOATIOR: 1, CLEARCOATINTENSITY: 1, USEMETALLICREFLECTIONS: 0
  };
  if (/METALLIC/.test(variant)) Object.assign(defaults, { FRESNELMAX: 1, FRESNELC: 0.1, BRIGHTNESSADJUSTMENT: 0.9, COLOREDSPECULAR: 0.9, AMBIENTSPECULAR: 0.6, CLEARCOATTHICKNESS: 0.06 });
  if (/CHROME/.test(variant)) Object.assign(defaults, { FRESNELMAX: 1, FRESNELC: 0.4, BRIGHTNESSADJUSTMENT: 0.5, COLOREDSPECULAR: 0.9 });
  if (/PEARL/.test(variant)) Object.assign(defaults, { FRESNELMAX: 1, FRESNELC: 0.1, BRIGHTNESSADJUSTMENT: 0.9, PEARLESCENTSPECULAR: 1, AMBIENTSPECULAR: 1, CLEARCOATTHICKNESS: 0.09, SPECULARBASE: [1, 50] });
  if (/MATTE/.test(variant)) Object.assign(defaults, { FRESNELMAX: 1, FRESNELC: 0.1, AMBIENTSPECULAR: 1, SPECULARBASE: [0.3, 5], SPECULARSUN: [0, 1] });
  if (/METAL$/.test(variant)) Object.assign(defaults, { FRESNELMAX: 1, FRESNELC: 1, FLAKESK: 1, CLEARCOATTHICKNESS: 0.2, COLOREDSPECULAR: 1, AMBIENTSPECULAR: 0.5, BRIGHTNESSADJUSTMENT: 0.2, USEMETALLICREFLECTIONS: 1 });
  const p = parameters(section, defaults), specular = vectorParameter(p, "SPECULARBASE", [0.5, 50]), sun = vectorParameter(p, "SPECULARSUN", [2, 5000]);
  const brightness = numberParameter(p, "BRIGHTNESSADJUSTMENT", 1), specularMult = numberParameter(p, "SPECULARMULT", 1), sunMult = numberParameter(p, "SUNMULTIPLIER", 6);
  return generatedSection(`SHADER_REPLACEMENT_APEX_CARPAINT_${index}`, `<built-in:${section.name}>`, propertyFields({ MATERIALS: parameter(p, "MATERIALS", globals.CARPAINTMATERIAL || "Carpaint"), SHADER: /_OLD/i.test(section.name) ? "smCarPaint_old" : "smCarPaint" }, {
    ksDiffuse: 0.5 * brightness, ksAmbient: 0.45 * brightness,
    ksSpecular: Number(specular[0] ?? 0.5) * specularMult, ksSpecularEXP: Number(specular[1] ?? 50),
    sunSpecular: Number(sun[0] ?? 2) * specularMult * (1 + sunMult * sunMult), sunSpecularEXP: Number(sun[1] ?? 5000) * (1 + sunMult),
    fresnelMaxLevel: numberParameter(p, "FRESNELMAX", 0.8), fresnelC: numberParameter(p, "FRESNELC", 0.16), fresnelEXP: numberParameter(p, "FRESNELEXP", 5),
    extNormalizeAO: numberParameter(p, "NORMALIZEAO", 0), extFlakesK: numberParameter(p, "FLAKESK", 0.1),
    extColoredSpecular: Math.max(numberParameter(p, "COLOREDSPECULAR", 0), numberParameter(p, "PEARLESCENTSPECULAR", 0)),
    extPearlSpecular: numberParameter(p, "PEARLESCENTSPECULAR", 0), stAmbientSpec: numberParameter(p, "AMBIENTSPECULAR", 0) * specularMult * 0.2,
    stAmbientEXP: numberParameter(p, "AMBIENTSPECULAREXP", 2.5), extClearCoatIOR: numberParameter(p, "CLEARCOATIOR", 1),
    extClearCoatIntensity: numberParameter(p, "CLEARCOATINTENSITY", 1), extClearCoatThickness: numberParameter(p, "CLEARCOATTHICKNESS", 0),
    extColoredReflections: numberParameter(p, "USEMETALLICREFLECTIONS", 0)
  }));
}

function glassReplacement(section, supplied, index) {
  const p = parameters(section, { MATERIALS: supplied.MATERIALS, MESHES: supplied.MESHES, IOR: 1.5, FILMIOR: supplied.FILMIOR ?? 1.5, THICKNESSMULT: supplied.THICKNESSMULT ?? 1, PROFILEFIX: 0.04, EXTRALIGHTSADJUSTMENT: 0.03, NORMALMAPUVMULT: 1, REFRACTION: 0, REFRACTIONBIAS: 0, REFRACTIONRAINBOW: 0, MASKPASS: 0, DESATURATE: 0 });
  const ior = numberParameter(p, "FILMIOR", numberParameter(p, "IOR", 1.5)), f0 = Math.pow((ior - 1) / (ior + 1), 2);
  let shader = "smGlass"; if (/MULTIEMISSIVE/i.test(section.name)) shader = "smGlass_emissive"; if (/PHOTOELASTIC/i.test(section.name)) shader = "smGlass_phel";
  return generatedSection(`SHADER_REPLACEMENT_APEX_GLASS_${index}`, `<built-in:${section.name}>`, propertyFields({ MATERIALS: parameter(p, "MATERIALS", ""), MESHES: parameter(p, "MESHES", ""), SHADER: shader }, {
    extIOR: numberParameter(p, "IOR", 1.5), extThicknessMult: numberParameter(p, "THICKNESSMULT", 1), extThicknessProfileFix: numberParameter(p, "PROFILEFIX", 0.04),
    fresnelC: f0, fresnelMaxLevel: Math.min(1, 50 * f0), ksDiffuse: parameter(p, "BRIGHTNESSADJUSTMENT", ""), ksAmbient: numberParameter(p, "EXTRALIGHTSADJUSTMENT", 0.03),
    extSaturation: Math.max(0, Math.min(1, 1 - numberParameter(p, "DESATURATE", 0))), extNormalMult: numberParameter(p, "NORMALMAPUVMULT", 1),
    extRefraction: numberParameter(p, "REFRACTION", 0) * 0.06, extRefractionBias: numberParameter(p, "REFRACTIONBIAS", 0) * 6,
    extRefractionRainbow: 1 - 0.1 * numberParameter(p, "REFRACTIONRAINBOW", 0), extMaskPass: numberParameter(p, "MASKPASS", 0)
  }));
}

function interiorReplacement(section, index) {
  const p = parameters(section, { HASDETAILNORMALS: 1, BRIGHTNESSADJUSTMENT: "", DETAILNORMALBLEND: "", DETAILSCALE: "", APPLYTILINGFIX: 0, CUBEMAPREFLECTIONBLUR: 0 });
  const cloth = numberParameter(p, "USECLOTHSHADING", /FABRIC|VELVET/i.test(section.name) ? 1 : 0), detail = numberParameter(p, "HASDETAILNORMALS", 1);
  const brightness = parameter(p, "BRIGHTNESS", "") !== "" ? numberParameter(p, "BRIGHTNESS", 1) : parameter(p, "BRIGHTNESSADJUSTMENT", "") !== "" ? numberParameter(p, "BRIGHTNESSADJUSTMENT", 1) * 2.5 : undefined;
  return generatedSection(`SHADER_REPLACEMENT_APEX_INTERIOR_${index}`, `<built-in:${section.name}>`, propertyFields({ MATERIALS: parameter(p, "MATERIALS", ""), MESHES: parameter(p, "MESHES", ""), SHADER: cloth ? "nePBR_MultiMap_Cloth" : detail ? "nePBR_MultiMap_NMDetail" : "nePBR_MultiMap" }, {
    ksDiffuse: brightness, ksAlphaRef: numberParameter(p, "APPLYTILINGFIX", 0) ? -193 : 0,
    detailNormalBlend: parameter(p, "DETAILNORMALBLEND", ""), detailUVMultiplier: parameter(p, "DETAILSCALE", ""),
    pbReflectionBlurEnv: numberParameter(p, "CUBEMAPREFLECTIONBLUR", 0) * 6
  }));
}

function distantEmissiveReplacement(section, index) {
  const p = parameters(section, { BRIGHTNESSMULT: 1, DISTANTGLOWMULT_V2: 1, DISTANTGLOWEXP_V2: 1, DOTRADIUSMULT_V2: 1, DOTBRIGHTNESSMULT_V2: 1, DOTBRIGHTNESSCENTER_V2: 1, DISTANTGLOWTEXTURED: 1, DISTANTGLOWHEIGHTMULT_V2: 1, DISTANTGLOWRADIUS_V2: 1, DISTANCESOFTFACTOR_V2: 1, DOTMAXDISTANCE: 2000 });
  const brightness = numberParameter(p, "BRIGHTNESSMULT", 1);
  return generatedSection(`SHADER_REPLACEMENT_APEX_DISTANT_${index}`, `<built-in:${section.name}>`, propertyFields({ MATERIALS: parameter(p, "MATERIALS", ""), MESHES: parameter(p, "MESHES", ""), SHADER: "nePerPixel_light" }, {
    extDistantMult: Math.max(numberParameter(p, "DISTANTGLOWMULT_V2", 1), 0.001) * brightness * 0.2,
    extDistantEXP: numberParameter(p, "DISTANTGLOWEXP_V2", 1), extDotRadiusMult: numberParameter(p, "DOTRADIUSMULT_V2", 1) * 24,
    extDotBrightnessBase: numberParameter(p, "DOTBRIGHTNESSMULT_V2", 1) * 0.0625, extDotBrightnessCenter: numberParameter(p, "DOTBRIGHTNESSCENTER_V2", 1) * brightness * 40,
    extDotCenterEXP: brightness * 12, extDistantUseTexture: numberParameter(p, "DISTANTGLOWTEXTURED", 1), extDistantHeightMult: numberParameter(p, "DISTANTGLOWHEIGHTMULT_V2", 1) * 0.5,
    extDistantRadiusMult: numberParameter(p, "DISTANTGLOWRADIUS_V2", 1) * 50, extDistantSoftK: numberParameter(p, "DISTANCESOFTFACTOR_V2", 1) * 40,
    extMaxDistanceHalfInv: 2 / Math.max(1, numberParameter(p, "DOTMAXDISTANCE", 2000))
  }));
}

function licensePlateReplacement(section, index) {
  const defaults = { MATERIALS: "{ texture:Plate_D.dds & texture:Plate_NM.dds }", BRIGHTNESSADJUSTMENT: 0.4, LETTERSDARKENING: 0, LETTERSSPECULARDARKENING: 0, BOUNCEBACKBRIGHTNESS: 0, TEXTURERATIO: 4, EXTRUDELETTERS: 1, LETTERSBRIGHTNESSSTART: 0.67, LETTERSBRIGHTNESSFULL: 0.33, DEPTH: 1, EMISSIVETOTALMULT: 1.6, EMISSIVEFADEX: 0.8, EMISSIVEFADEY: 0.8 };
  if (/EUROPE/i.test(section.name)) Object.assign(defaults, { LETTERSDARKENING: 0.85, LETTERSSPECULARDARKENING: 0.5, BOUNCEBACKBRIGHTNESS: 0.5, TEXTURERATIO: 4 });
  if (/JAPAN/i.test(section.name)) Object.assign(defaults, { LETTERSDARKENING: 0.6, LETTERSSPECULARDARKENING: 0.5, BOUNCEBACKBRIGHTNESS: 0.5, TEXTURERATIO: 2 });
  const p = parameters(section, defaults), brightness = numberParameter(p, "BRIGHTNESSADJUSTMENT", 0.4), darkStart = numberParameter(p, "LETTERSBRIGHTNESSSTART", 0.67), darkFull = numberParameter(p, "LETTERSBRIGHTNESSFULL", 0.33), denominator = darkFull - darkStart || -0.34;
  return generatedSection(`SHADER_REPLACEMENT_APEX_LICENSE_${index}`, `<built-in:${section.name}>`, propertyFields({ MATERIALS: parameter(p, "MATERIALS", defaults.MATERIALS), MESHES: parameter(p, "MESHES", ""), SHADER: "smLicensePlate" }, {
    ksAmbient: 0.5 * brightness, ksDiffuse: 0.5 * brightness, ksSpecular: 0.2, ksSpecularEXP: 80,
    sunSpecular: 0.8 / Math.max(1 - numberParameter(p, "LETTERSSPECULARDARKENING", 0), 0.1), sunSpecularEXP: 20,
    fresnelMaxLevel: 1, fresnelEXP: 5, fresnelC: 0.01, isAdditive: 1, extExtraSharpLocalReflections: 1,
    extBounceBack: numberParameter(p, "BOUNCEBACKBRIGHTNESS", 0), extEmissiveShapeMult: numberParameter(p, "EMISSIVETOTALMULT", 1.6),
    extEmissiveFadeX: numberParameter(p, "EMISSIVEFADEX", 0.8), extEmissiveFadeY: numberParameter(p, "EMISSIVEFADEY", 0.8),
    extDarkeningFactor: numberParameter(p, "LETTERSDARKENING", 0), extOcclusionFactor: numberParameter(p, "LETTERSSPECULARDARKENING", 0),
    extWidthRatio: numberParameter(p, "TEXTURERATIO", 4), extLettersExtrude: numberParameter(p, "EXTRUDELETTERS", 1),
    extLettersMult: 1 / denominator, extLettersAdd: -darkStart / denominator, extParallaxHeight: numberParameter(p, "DEPTH", 1) * -0.007
  }));
}

function selfLightReplacement(section, index) {
  const headlights = /^SELFLIGHT_HEADLIGHTS$/i.test(section.name);
  const p = parameters(section, {
    ACTIVE: 1, POSITION: [0, 0, 0], COLOR: [1, 1, 1, 30], SPOT: 190,
    SPOT_SHARPNESS: 0.5, RANGE: 0.5, RANGE_GRADIENT_OFFSET: 0,
    SPECULAR_MULT: 1, DIFFUSE_CONCENTRATION: 0.9, FADE_AT: 10, FADE_SMOOTH: 10,
    AFFECTS_TRACK: 0, EXTERIOR_ONLY: 1, INTERIOR_ONLY: 0
  });
  return generatedSection(`LIGHT_EXTRA_APEX_SELFLIGHT_${index}`, `<built-in:${section.name}>`, {
    ACTIVE: parameter(p, "ACTIVE", 1),
    POSITION: vectorParameter(p, "POSITION", [0, 0, 0]),
    MIRROR: parameter(p, "MIRROR", ""),
    DIRECTION: parameter(p, "DIRECTION", headlights ? [0, 0, 1] : ""),
    SPOT: numberParameter(p, "SPOT", 190),
    SPOT_SHARPNESS: numberParameter(p, "SPOT_SHARPNESS", 0.5),
    RANGE: numberParameter(p, "RANGE", 0.5),
    RANGE_GRADIENT_OFFSET: numberParameter(p, "RANGE_GRADIENT_OFFSET", 0),
    COLOR: vectorParameter(p, "COLOR", [1, 1, 1, 30]),
    SPECULAR_MULT: numberParameter(p, "SPECULAR_MULT", 1),
    DIFFUSE_CONCENTRATION: numberParameter(p, "DIFFUSE_CONCENTRATION", 0.9),
    FADE_AT: numberParameter(p, "FADE_AT", 10),
    FADE_SMOOTH: numberParameter(p, "FADE_SMOOTH", 10),
    AFFECTS_TRACK: parameter(p, "AFFECTS_TRACK", 0),
    EXTERIOR_ONLY: numberParameter(p, "EXTERIOR_ONLY", 1),
    INTERIOR_ONLY: numberParameter(p, "INTERIOR_ONLY", 0),
    VISIBILITY_LEVEL: numberParameter(p, "VISIBILITY_LEVEL", 0),
    SPOT_EDGE: vectorParameter(p, "SPOT_EDGE", [0, 0, 0]),
    SPOT_EDGE_SHARPNESS: numberParameter(p, "SPOT_EDGE_SHARPNESS", 0),
    SPOT_UP: parameter(p, "SPOT_UP", ""),
    CONDITION: parameter(p, "CONDITION", ""),
    BIND_TO_HEADLIGHTS: numberParameter(p, "BIND_TO_HEADLIGHTS", 0),
    RELATIVE_TO: parameter(p, "RELATIVE_TO", ""),
    BOUND_TO: parameter(p, "BOUND_TO", "")
  });
}

function operationParameters(text) {
  const parts = splitCspList(text);
  const name = (parts.shift() || "").trim();
  const values = {};
  for (const part of parts) {
    const equals = part.indexOf("=");
    if (equals < 0) {
      values[part.trim().toUpperCase()] = 1;
      continue;
    }
    const key = part.slice(0, equals).trim().toUpperCase();
    const raw = part.slice(equals + 1).trim().replace(/^(['"])(.*)\1$/, "$2");
    values[key] = parseCspValue(raw);
  }
  return { name, upper: name.toUpperCase(), values };
}

function operationValue(operation, name, fallback = "") {
  const value = operation.values[name.toUpperCase()];
  return value === undefined || value === "" ? fallback : value;
}

function operationNumber(operation, name, fallback = 0) {
  const value = Number(operationValue(operation, name, fallback));
  return Number.isFinite(value) ? value : fallback;
}

function operationVector(operation, name, fallback) {
  const value = operationValue(operation, name, fallback);
  if (Array.isArray(value)) return value.map(Number);
  if (typeof value === "number") return [value, value];
  const parsed = parseCspValue(value);
  return Array.isArray(parsed) ? parsed.map(Number) : [...fallback];
}

function emissiveColor(value, fallback) {
  const parsed = Array.isArray(value) ? value : parseCspValue(value);
  const source = Array.isArray(parsed) ? parsed : fallback;
  const multiplier = source.length > 3 ? Number(source[3]) || 0 : 1;
  return [0, 1, 2].map((index) => (Number(source[index]) || 0) * multiplier);
}

function mirroredChannel(channel) {
  return channel === 1 ? 6 : channel === 2 ? 5 : channel === 3 ? 4 : channel;
}

function mirroredShape(shape, width) {
  if (shape.type === "poly") return { ...shape, channel: mirroredChannel(shape.channel), points: shape.points.map(([x, y]) => [width - x, y]), mirrored: true };
  return { ...shape, channel: mirroredChannel(shape.channel), start: [width - shape.start[0] - shape.size[0], shape.start[1]], mirrored: true };
}

function guessIndicatorColor(input) {
  const name = String(input).toUpperCase();
  if (name === "HIGHBEAM") return [0, 4, 20];
  if (/^(TURNSIGNAL(?:_LEFT|_RIGHT)?|LIGHT)$/.test(name)) return [0, 20, 0];
  if (/^(ABS_INACTION|TRACTIONCONTROL_INACTION|ENGINE_LIFE|TYRE_PRESSURE|HAZARD|TRACTIONCONTROL)$/.test(name)) return [20, 12, 0];
  return [20, 0, 0];
}

function bindingFromOperation(operation, channel, defaults = {}) {
  const input = String(operationValue(operation, "INPUT", defaults.input || "ONE")).toUpperCase();
  const intensity = operationNumber(operation, "INTENSITY", 1);
  const color = emissiveColor(operationValue(operation, "COLOR", defaults.color || guessIndicatorColor(input)), defaults.color || guessIndicatorColor(input)).map((value) => value * intensity);
  const offColor = emissiveColor(operationValue(operation, "OFFCOLOR", operationValue(operation, "OFF_COLOR", defaults.offColor || [0, 0, 0])), defaults.offColor || [0, 0, 0]).map((value) => value * intensity);
  return {
    channel, input, color, offColor,
    invert: operationNumber(operation, "INVERT", defaults.invert ? 1 : 0) !== 0,
    inputInverse: operationNumber(operation, "INPUTINVERSE", operationNumber(operation, "INPUT_INVERSE", 0)) !== 0,
    threshold: Number(operationValue(operation, "INPUTTHRESHOLD", operationValue(operation, "INPUT_THRESHOLD", Number.NaN))),
    inputMin: Number(operationValue(operation, "INPUTMIN", operationValue(operation, "INPUT_MIN", 0))),
    inputMax: Number(operationValue(operation, "INPUTMAX", operationValue(operation, "INPUT_MAX", Number.NaN))),
    inputLut: operationValue(operation, "INPUTLUT", operationValue(operation, "INPUT_LUT", "")),
    role: operationValue(operation, "ROLE", defaults.role || "")
  };
}

function customEmissiveDescriptor(section) {
  const p = parameters(section, { RESOLUTION: [1, 1], USEEMISSIVE0ASFALLBACK: 0, DASHHIGHLIGHTCOLOR: [2, 2, 2] });
  const resolution = vectorParameter(p, "RESOLUTION", [1, 1]);
  const mirrorDirection = vectorParameter(p, "MIRRORDIR", [1, 0, 0]);
  const mirrorLength = Math.hypot(...mirrorDirection) || 1;
  const descriptor = {
    name: section.name, source: section.source, line: section.line,
    multi: /^CUSTOMEMISSIVEMULTI$/i.test(section.name),
    resolution: [Math.max(1, Number(resolution[0]) || 1), Math.max(1, Number(resolution[1]) || Number(resolution[0]) || 1)],
    shapes: [], maskShapes: [], colorMasks: [], bindings: [], unsupportedOperations: [], approximatedOperations: [], extraProperties: new Map(),
    alphaFromDiffuse: false, diffuseLuminance: null, diffuseAlpha: null,
    vertexMask: null, bounceBack: [], mirrorUv: null, useRawUv: numberParameter(p, "USERAWUV", 0) !== 0, skipDiffuseMap: false,
    mirrorDirection: mirrorDirection.map((value) => { const component = (Number(value) || 0) / mirrorLength; return component === 0 ? 0 : component; }), mirrorOffset: numberParameter(p, "MIRROROFFSET", 0),
    colorMasksAsMultiplier: numberParameter(p, "COLORMASKSASMULTIPLIER", 0) !== 0,
    colorMasksSubtractive: numberParameter(p, "COLORMASKSSUBTRACTIVE", 0) !== 0,
    coverChannel0: String(parameter(p, "USEEMISSIVE0ASFALLBACK", "0")).toUpperCase() === "COVER_ALL" || numberParameter(p, "USEEMISSIVE0ASFALLBACK", 0) !== 0
  };
  for (const flag of ["AREASSUBTRACTNEXT", "COLORSUBTRACTNEXT", "POLYSUBTRACTNEXT", "AREASSUBTRACTIVE", "COLORMASKSSUBTRACTIVE", "POLYSSUBTRACTIVE", "POLYSMASKSASMULTIPLIER", "USEAREASASMASK", "STARTWITHWHITE", "USERAWUV"]) {
    if (numberParameter(p, flag, 0) !== 0) descriptor.approximatedOperations.push(flag);
  }
  let multiChannel = 0;
  const addShape = (shape, mirror) => {
    descriptor.shapes.push(shape);
    if (mirror) descriptor.shapes.push(mirroredShape(shape, descriptor.resolution[0]));
  };
  const addArea = (operation, channel, type = "rect", mask = false) => {
    let size = operationVector(operation, "SIZE", type === "circle" ? [1, 1] : [1, 1]);
    if (size.length === 1) size = [size[0], size[0]];
    const centerValue = operationValue(operation, "CENTER", "");
    let start = operationVector(operation, "START", [0, 0]);
    if (centerValue !== "") {
      const center = operationVector(operation, "CENTER", [0, 0]);
      start = [center[0] - size[0] / 2, center[1] - size[1] / 2];
    }
    const shape = { type, channel, start, size, cornerRadius: operationVector(operation, "CORNERRADIUS", type === "circle" ? [1, 1] : [0, 0]), exponent: operationNumber(operation, "EXPONENT", type === "circle" ? 0.1 : 1), sharpness: operationNumber(operation, "SHARPNESS", 1000), offset: operationNumber(operation, "OFFSET", 0), opacity: operationNumber(operation, "OPACITY", 1) };
    if (mask) descriptor.maskShapes.push(shape); else addShape(shape, operationNumber(operation, "MIRROR", 0) !== 0);
  };
  const addBindingRole = (operation, channel) => {
    const role = String(operationValue(operation, "ROLE", operation.name));
    const upperRole = role.toUpperCase();
    const aliases = {
      DASHHIGHLIGHT: { input: "LIGHT", color: vectorParameter(p, "DASHHIGHLIGHTCOLOR", [2, 2, 2]) },
      DASHWARNINGABS: { input: "ABS", color: [20, 12, 0], invert: true },
      DASHWARNINGTC: { input: "TRACTIONCONTROL", color: [20, 12, 0], invert: true },
      DASHWARNINGENGINE: { input: "ENGINE_LIFE", color: [20, 0, 0] },
      DASHWARNINGTEMPERATURE: { input: "OIL_TEMP", color: [20, 0, 0] },
      DASHTURNINGLIGHTS: { input: "TURNSIGNAL", color: [0, 20, 2] },
      INTERIORLIGHTS: { input: "LIGHT", color: [20, 20, 20] }
    };
    descriptor.bindings.push(bindingFromOperation(operation, channel, { ...(aliases[upperRole] || {}), input: aliases[upperRole]?.input || (upperRole === role ? upperRole : "ONE"), role }));
  };
  for (const entry of section.entries) {
    if (entry.key !== "@" && entry.key !== "@MIXIN") continue;
    const operation = operationParameters(entry.value);
    const operationResolution = operationValue(operation, "RESOLUTION", "");
    if (operationResolution !== "" && descriptor.resolution[0] === 1 && descriptor.resolution[1] === 1) {
      const parsedResolution = operationVector(operation, "RESOLUTION", [1, 1]);
      descriptor.resolution = [Math.max(1, Number(parsedResolution[0]) || 1), Math.max(1, Number(parsedResolution[1]) || Number(parsedResolution[0]) || 1)];
    }
    const channel = Math.max(0, Math.floor(operationNumber(operation, "CHANNEL", 0)));
    if (operation.upper === "MULTIITEM") {
      const itemChannel = ++multiChannel;
      addArea(operation, itemChannel, operationNumber(operation, "CIRCLE", 0) ? "circle" : "rect");
      addBindingRole(operation, itemChannel);
    } else if (/^CUSTOMEMISSIVE_(RECT|AREA)$/.test(operation.upper)) addArea(operation, channel);
    else if (operation.upper === "CUSTOMEMISSIVE_CIRCLE") addArea(operation, channel, "circle");
    else if (operation.upper === "CUSTOMEMISSIVE_COVERALL") addShape({ type: "rect", channel, start: [0, 0], size: [...descriptor.resolution], cornerRadius: [0, 0], exponent: 1, sharpness: 1000, offset: 0, opacity: operationNumber(operation, "OPACITY", 1) }, operationNumber(operation, "MIRROR", 0) !== 0);
    else if (operation.upper === "CUSTOMEMISSIVE_AREAMASK") addArea(operation, -1, "rect", true);
    else if (operation.upper === "CUSTOMEMISSIVE_POLY") {
      const shape = { type: "poly", channel, points: ["P1", "P2", "P3", "P4"].map((name) => operationVector(operation, name, [0, 0])), exponent: operationNumber(operation, "EXPONENT", 1), sharpness: operationNumber(operation, "SHARPNESS", 1000), offset: operationNumber(operation, "OFFSET", 0), opacity: operationNumber(operation, "OPACITY", 1) };
      addShape(shape, operationNumber(operation, "MIRROR", 0) !== 0);
    } else if (operation.upper === "CUSTOMEMISSIVE_COLOR") {
      const threshold = operationValue(operation, "THRESHOLD", [0.95, 20]);
      const thresholdValues = Array.isArray(threshold) ? threshold : [Number(threshold), 20];
      const normalization = Math.max(0, Math.min(1, operationNumber(operation, "NORMALIZATION", 1)));
      const rawColor = operationVector(operation, "COLOR", [1, 0.5, 0]).slice(0, 3), length = Math.hypot(...rawColor) || 1;
      const color = rawColor.map((value) => value * (1 - normalization) + value / length * normalization);
      const base = { channel, color, normalization, thresholdLevel: operationNumber(operation, "THRESHOLDLEVEL", Number(thresholdValues[0]) || 0.95), thresholdSharpness: operationNumber(operation, "THRESHOLDSHARPNESS", Number(thresholdValues[1]) || 20), alphaMinMax: operationVector(operation, "ALPHAMINMAX", [0, 0]), opacity: operationNumber(operation, "OPACITY", 1), mirrorSide: 0 };
      if (operationNumber(operation, "MIRROR", 0) !== 0) {
        descriptor.colorMasks.push({ ...base, mirrorSide: -1 });
        descriptor.colorMasks.push({ ...base, channel: mirroredChannel(channel), mirrorSide: 1 });
      } else descriptor.colorMasks.push(base);
    } else if (operation.upper === "CUSTOMEMISSIVE_VERTEXMASK") {
      descriptor.vertexMask = { points: ["POINT0", "POINT1", "POINT2", "POINT3"].map((name) => {
        const value = operationValue(operation, name, "");
        return value === "" ? null : operationVector(operation, name, [0, 0, 0]).slice(0, 3);
      }), additive: numberParameter(p, "VERTEXMASKADDITIVE", 0) !== 0, subtractive: numberParameter(p, "VERTEXMASKSUBTRACTIVE", 0) !== 0 };
      descriptor.approximatedOperations.push(operation.name);
    } else if (operation.upper === "ALPHAFROMTXDIFFUSE") descriptor.alphaFromDiffuse = true;
    else if (operation.upper === "CUSTOMEMISSIVE_ALPHA") descriptor.alphaFromDiffuse = operationNumber(operation, "FROMDIFFUSEMAP", 0) !== 0;
    else if (operation.upper === "CUSTOMEMISSIVE_USEDIFFUSELUMINOCITY") {
      descriptor.diffuseLuminance = { from: operationNumber(operation, "FROM", 0), to: operationNumber(operation, "TO", 1), exponent: operationNumber(operation, "EXPONENT", 1), opacity: operationNumber(operation, "OPACITY", 1) };
      descriptor.skipDiffuseMap = operationNumber(operation, "SKIPDIFFUSEMAP", 1) !== 0;
    } else if (operation.upper === "CUSTOMEMISSIVE_USEDIFFUSEALPHA") {
      descriptor.diffuseAlpha = { from: operationNumber(operation, "FROM", 0), to: operationNumber(operation, "TO", 1), exponent: operationNumber(operation, "EXPONENT", 1), opacity: operationNumber(operation, "OPACITY", 1) };
      descriptor.skipDiffuseMap = operationNumber(operation, "SKIPDIFFUSEMAP", 1) !== 0;
    } else if (operation.upper === "CUSTOMEMISSIVE_BOUNCEBACK") {
      const rawMask = operationValue(operation, "CHANNEL", "") !== "" ? [0, 1, 2, 3].map((index) => index === channel ? 1 : 0) : operationVector(operation, "MASK", [1, 1, 1, 1]);
      const mask = [0, 1, 2, 3].map((index) => Number(rawMask[index]) || 0);
      descriptor.bounceBack = [{ mask, intensity: operationNumber(operation, "INTENSITY", 20) }];
    } else if (operation.upper === "CUSTOMEMISSIVE_MIRRORUV") {
      const direction = normalizeCustomEmissiveMirrorDirection(operationVector(operation, "DIRECTION", [1, 0])).map((value) => value === 0 ? 0 : -value);
      descriptor.mirrorUv = { offsetPixels: operationNumber(operation, "OFFSET", 0.5), direction };
    }
    else if (operation.upper === "CUSTOMEMISSIVE_SKIPDIFFUSEMAP") descriptor.skipDiffuseMap = operationNumber(operation, "SKIPDIFFUSEMAP", 1) !== 0;
    else if (operation.upper === "USEALPHAFROMTXDIFFUSES") descriptor.alphaFromDiffuse = true;
    else if (/^FOGLIGHTS(?:FRONT|REAR)?$/.test(operation.upper)) {
      descriptor.bindings.push(bindingFromOperation(operation, channel, { input: "EXTRA_A", color: operation.upper === "FOGLIGHTSREAR" ? [20, 0, 0] : [20, 20, 20] }));
      descriptor.approximatedOperations.push(operation.name);
    } else if (operation.upper === "OPENDOORLIGHT") {
      const channels = operationValue(operation, "CHANNEL", 0), list = Array.isArray(channels) ? channels : [channels];
      for (const value of list) descriptor.bindings.push(bindingFromOperation(operation, Math.max(0, Math.floor(Number(value) || 0)), { input: "OPEN_DOORS", color: [16, 15.6, 6] }));
      descriptor.approximatedOperations.push(operation.name);
    } else if (operation.upper === "LIGHTMATERIAL") {
      for (const [name, value] of Object.entries({ extColoredReflection: 0.9, extColoredReflectionNorm: 0.8, extColoredBaseReflection: 0, fresnelEXP: 5, fresnelMaxLevel: 1, fresnelC: 0.5, ksAmbient: 0.1, ksDiffuse: 0.1, isAdditive: 0, ksSpecular: 0, ksSpecularEXP: 200, sunSpecular: 0 })) descriptor.extraProperties.set(name, value);
    } else if (operation.upper === "REARLIGHTSMASK") {
      descriptor.diffuseLuminance = { from: 0, to: 1, exponent: 1, opacity: 1 }; descriptor.skipDiffuseMap = true;
      for (const [maskChannel, center, size, cornerRadius] of [[0,[0.5,0.09],[0.9,0.08],[0.2,1]],[1,[0.5,0.44],[1,0.08],[0,1]],[2,[0.5,0.825],[1,0.08],[0,1]]]) {
        descriptor.shapes.push({ type: "rect", channel: maskChannel, start: [center[0]-size[0]/2,center[1]-size[1]/2], size, cornerRadius, exponent: 1, sharpness: 1000, offset: 0, opacity: 1 });
      }
      for (const [name, value] of Object.entries({ extColoredBaseReflection: 1, extColoredReflectionNorm: 1, fresnelMaxLevel: 1, fresnelC: 0.1, fresnelEXP: 5 })) descriptor.extraProperties.set(name, value);
    } else if (operation.upper === "SETTXNORMAL") descriptor.approximatedOperations.push(operation.name);
    else if (/^(DASH|INTERIORLIGHTS)/.test(operation.upper)) addBindingRole(operation, channel);
    else if (/^TURNINGLIGHTS/.test(operation.upper)) {
      const left = bindingFromOperation(operation, channel, { input: "TURNSIGNAL_LEFT", color: [25, 12, 0] });
      const right = bindingFromOperation(operation, mirroredChannel(channel), { input: "TURNSIGNAL_RIGHT", color: [25, 12, 0] });
      descriptor.bindings.push({ ...left, input: "TURNSIGNAL_LEFT" });
      descriptor.bindings.push({ ...right, input: "TURNSIGNAL_RIGHT" });
    } else if (operation.upper === "REVERSELIGHTS") descriptor.bindings.push(bindingFromOperation(operation, channel, { input: "REVERSE", color: [40, 40, 40] }));
    else if (operation.upper === "HEADLIGHTS") descriptor.bindings.push(bindingFromOperation(operation, channel, { input: "HEADLIGHTS", color: [20, 20, 20] }));
    else if (operation.upper === "BRAKINGLIGHTS") descriptor.bindings.push(bindingFromOperation(operation, channel, { input: "BRAKE", color: [40, 0, 0] }));
    else if (operation.upper === "PARKINGLIGHTS") descriptor.bindings.push(bindingFromOperation(operation, channel, { input: "LIGHT", color: [5, 0, 0] }));
    else if (operation.upper === "LICENSEPLATELIGHTS") descriptor.bindings.push(bindingFromOperation(operation, channel, { input: "LIGHT", color: [10, 10, 10] }));
    else descriptor.unsupportedOperations.push(operation.name);
  }
  if (descriptor.mirrorUv) descriptor.mirrorUv = { offset: descriptor.mirrorUv.offsetPixels / descriptor.resolution[0], direction: descriptor.mirrorUv.direction };
  if (descriptor.coverChannel0 || (descriptor.maskShapes.length && !descriptor.shapes.length)) descriptor.shapes.unshift({ type: "rect", channel: 0, start: [0, 0], size: [...descriptor.resolution], cornerRadius: [0, 0], exponent: 1, sharpness: 1000, offset: 0, opacity: 1, fallback: true });
  descriptor.approximatedOperations = [...new Set(descriptor.approximatedOperations)];
  return descriptor;
}

function customEmissiveReplacement(section, index) {
  const p = parameters(section);
  const descriptor = customEmissiveDescriptor(section);
  const output = generatedSection(`SHADER_REPLACEMENT_APEX_CUSTOMEMISSIVE_${index}`, `<built-in:${section.name}>`, propertyFields({
    MATERIALS: parameter(p, "MATERIALS", ""), MESHES: parameter(p, "MESHES", ""),
    SHADER: /^CUSTOMEMISSIVEMULTI$/i.test(section.name) ? "ksPerPixelMultiMap_emissiveExtra" : "ksPerPixelMultiMap_emissive"
  }, Object.fromEntries(descriptor.extraProperties)));
  output.customEmissive = descriptor;
  return output;
}

export function expandCspMaterialTemplates(config) {
  const globals = {};
  for (const section of config.sections) {
    if (!/^INCLUDE(?::|$)/i.test(section.name)) continue;
    for (const entry of section.entries) globals[entry.key] = parseCspValue(entry.value);
  }
  const sections = [], expanded = [], resolvedIncludes = new Set();
  let index = 0;
  for (const section of config.sections) {
    sections.push(section);
    const upper = section.name.toUpperCase();
    let output = null;
    if (/^MATERIAL_CARPAINT(?:_|$)/.test(upper)) output = carPaintReplacement(section, globals, index++);
    else if (/^MATERIAL_(?:MULTIEMISSIVEGLASS|PHOTOELASTICGLASS|GLASSSIDE|GLASS)$/.test(upper)) output = glassReplacement(section, {}, index++);
    else if (/^MATERIAL_(?:PLASTIC|LEATHER|FABRIC|CARPET|VELVET|ALUMINIUM|METAL|CHROME|CARBON|INTERIORPBR)/.test(upper)) output = interiorReplacement(section, index++);
    else if (upper === "MATERIAL_DISTANTEMISSIVE") output = distantEmissiveReplacement(section, index++);
    else if (/^MATERIAL_LICENSEPLATE(?:_|$)/.test(upper)) output = licensePlateReplacement(section, index++);
    else if (/^SELFLIGHT(?:_HEADLIGHTS)?$/.test(upper)) output = selfLightReplacement(section, index++);
    else if (/^CUSTOMEMISSIVE(?:MULTI)?$/.test(upper)) output = customEmissiveReplacement(section, index++);
    if (output) { sections.push(output); expanded.push({ template: section.name, output: output.name, sourceLine: section.line }); }
    if (/^INCLUDE:.*MATERIALS_GLASS\.INI/i.test(section.name)) {
      const mappings = [
        ["EXTERIORGLASSMATERIALS", "EXTERIORGLASSMESHES", 1.5, 1], ["EXTERIORGLASSTINTEDMATERIALS", "EXTERIORGLASSTINTEDMESHES", 3.2, 3],
        ["EXTERIORGLASSFILMEDMATERIALS", "EXTERIORGLASSFILMEDMESHES", 2.4, 1], ["EXTERIORGLASSHEADLIGHTSMATERIALS", "EXTERIORGLASSHEADLIGHTSMESHES", 2.2, 1]
      ];
      for (const [materialKey, meshKey, filmIor, thickness] of mappings) {
        if (!globals[materialKey] && !globals[meshKey]) continue;
        const fake = { ...section, name: "Material_Glass", entries: [], values: new Map() };
        const generated = glassReplacement(fake, { MATERIALS: globals[materialKey], MESHES: globals[meshKey], FILMIOR: filmIor, THICKNESSMULT: thickness }, index++);
        sections.push(generated); expanded.push({ template: "Material_Glass", output: generated.name, sourceLine: section.line });
      }
    }
  }
  if (expanded.some((item) => /^Material_CarPaint/i.test(item.template))) resolvedIncludes.add("materials_carpaint.ini");
  if (expanded.some((item) => /Glass/i.test(item.template))) resolvedIncludes.add("materials_glass.ini");
  if (expanded.some((item) => /^Material_(?:Plastic|Leather|Fabric|Carpet|Velvet|Aluminium|Metal|Chrome|Carbon|InteriorPBR)/i.test(item.template))) resolvedIncludes.add("materials_interior.ini");
  if (expanded.some((item) => item.template === "Material_DistantEmissive")) resolvedIncludes.add("materials_track.ini");
  if (expanded.some((item) => /^Material_LicensePlate/i.test(item.template))) resolvedIncludes.add("materials_license_plate.ini");
  if (expanded.some((item) => /^SelfLight/i.test(item.template))) resolvedIncludes.add("selflighting.ini");
  if (expanded.some((item) => /^CustomEmissive/i.test(item.template))) resolvedIncludes.add("custom_emissive.ini");
  return { ...config, sections, expandedTemplates: expanded, resolvedIncludes };
}

function wildcard(pattern, value) {
  const expression = pattern.split("?").map((part) => part.replace(/[.*+^${}()|[\]\\]/g, "\\$&")).join(".*");
  return new RegExp(`^${expression}$`, "i").test(value);
}

function matchTerm(term, node, material, target) {
  const colon = term.indexOf(":");
  if (colon > 0) {
    const kind = term.slice(0, colon).trim().toLowerCase();
    const pattern = term.slice(colon + 1).trim();
    if (kind === "material") return wildcard(pattern, material?.name || "");
    if (kind === "shader") return wildcard(pattern, material?.shader || "");
    if (kind === "texture") return material?.resources.some((resource) => wildcard(pattern, resource.texture)) || false;
  }
  return wildcard(term, target === "material" ? material?.name || "" : node?.name || "");
}

function matchExpression(expression, node, material, target) {
  const text = expression.trim().replace(/^(['"])(.*)\1$/, "$2").replace(/^\{(.*)\}$/, "$1").trim();
  const terms = text.split("&").map((term) => term.trim()).filter(Boolean);
  return terms.every((term) => {
    const negative = term.startsWith("!");
    const result = matchTerm(negative ? term.slice(1).trim() : term, node, material, target);
    return negative ? !result : result;
  });
}

export function matchesSelector(value, node, material, target = "mesh") {
  const selectors = splitCspList(value);
  return selectors.some((selector) => matchExpression(selector, node, material, target));
}

function numericActive(section) {
  const value = parseCspValue(lastValue(section, "ACTIVE", "1"));
  return typeof value !== "number" || value !== 0;
}

function parseLut(text) {
  const body = text.match(/^\(\|(.*)\|\)$/)?.[1];
  if (!body) return [];
  return body.split("|").map((entry) => {
    const equals = entry.indexOf("=");
    return equals < 0 ? null : { input: Number(entry.slice(0, equals).trim()), output: parseCspValue(entry.slice(equals + 1)) };
  }).filter((point) => point && Number.isFinite(point.input)).sort((a, b) => a.input - b.input);
}

function interpolate(a, b, t) {
  if (typeof a === "number" && typeof b === "number") return a + (b - a) * t;
  if (Array.isArray(a) && Array.isArray(b)) return Array.from({ length: Math.max(a.length, b.length) }, (_, i) => (a[i] ?? a.at(-1) ?? 0) + ((b[i] ?? b.at(-1) ?? 0) - (a[i] ?? a.at(-1) ?? 0)) * t);
  return t < 0.5 ? a : b;
}

function evaluateLut(points, input) {
  if (!points.length) return input;
  if (input <= points[0].input) return points[0].output;
  for (let i = 1; i < points.length; i++) {
    if (input <= points[i].input) return interpolate(points[i - 1].output, points[i].output, (input - points[i - 1].input) / (points[i].input - points[i - 1].input || 1));
  }
  return points.at(-1).output;
}

function conditionTable(config, context, usedInputs) {
  const table = new Map([["ALWAYS_ON", 1], ["ALWAYS OFF", 0], ["ALWAYS_OFF", 0]]);
  const inputs = { ONE: 1, YEAR_PROGRESS: 0.5, TIME: 43200, ...(context.inputs || {}) };
  for (const section of config.sections) {
    if (!/^CONDITION(?:_|$)/i.test(section.name)) continue;
    const name = lastValue(section, "NAME").toUpperCase();
    if (!name) continue;
    const inputName = lastValue(section, "INPUT", "ONE").toUpperCase();
    const input = inputs[inputName] ?? 0;
    if (inputName === "YEAR_PROGRESS") {
      const previous = usedInputs.get(inputName);
      usedInputs.set(inputName, { min: Math.min(previous?.min ?? 0, 0), max: Math.max(previous?.max ?? 1, 1), value: Number(input) || 0 });
    }
    const output = evaluateLut(parseLut(lastValue(section, "LUT")), input);
    table.set(name, Array.isArray(output) ? output[0] : Number(output) || 0);
  }
  for (const [name, value] of Object.entries(context.conditions || {})) table.set(name.toUpperCase(), Number(value) || 0);
  return table;
}

function originalProperty(material, key) {
  const property = material?.properties.find((item) => item.name.toLowerCase() === key.toLowerCase());
  if (!property) return 0;
  if (key.toLowerCase() === "ksemissive") return property.value3;
  return property.value;
}

function effectiveOriginal(material, override, key) {
  return override.properties.has(key.toLowerCase()) ? override.properties.get(key.toLowerCase()) : originalProperty(material, key);
}

function resolveOff(value, original) {
  return value === "ORIGINAL" || value === "" ? original : value;
}

function findTargets(model, section) {
  const materialSelector = lastValue(section, "MATERIALS");
  const meshSelector = lastValue(section, "MESHES");
  if (!materialSelector && !meshSelector) return [];
  return walkNodes(model.root).map(({ node }) => node).filter((node) => {
    if (node.kind !== "mesh" && node.kind !== "skinnedMesh") return false;
    const material = model.materials[node.materialId];
    return (!materialSelector || matchesSelector(materialSelector, node, material, "material")) && (!meshSelector || matchesSelector(meshSelector, node, material, "mesh"));
  });
}

function identityMatrix() {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

function multiplyMatrices(a, b) {
  const result = new Array(16).fill(0);
  for (let column = 0; column < 4; column++) {
    for (let row = 0; row < 4; row++) {
      for (let index = 0; index < 4; index++) result[column * 4 + row] += a[index * 4 + row] * b[column * 4 + index];
    }
  }
  return result;
}

function transformPosition(matrix, value) {
  const [x = 0, y = 0, z = 0] = value;
  return [
    matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
    matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
    matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]
  ];
}

function normalizeVector(value, fallback = [0, -1, 0]) {
  const vector = [Number(value?.[0]) || 0, Number(value?.[1]) || 0, Number(value?.[2]) || 0];
  const length = Math.hypot(...vector);
  return length > 1e-8 ? vector.map((component) => component / length) : [...fallback];
}

function transformDirection(matrix, value) {
  const [x = 0, y = 0, z = 0] = value;
  return normalizeVector([
    matrix[0] * x + matrix[4] * y + matrix[8] * z,
    matrix[1] * x + matrix[5] * y + matrix[9] * z,
    matrix[2] * x + matrix[6] * y + matrix[10] * z
  ]);
}

function sceneWorldMatrices(root) {
  const matrices = new Map(), byName = new Map();
  const visit = (node, parent) => {
    const world = node.transform ? multiplyMatrices(parent, node.transform) : parent;
    matrices.set(node, world);
    if (!byName.has(node.name.toLowerCase())) byName.set(node.name.toLowerCase(), world);
    for (const child of node.children) visit(child, world);
  };
  visit(root, identityMatrix());
  return { matrices, byName };
}

function vectorValue(section, key, fallback) {
  const value = parseCspValue(lastValue(section, key, Array.isArray(fallback) ? fallback.join(",") : String(fallback ?? "")));
  if (Array.isArray(value)) return value.map(Number);
  if (typeof value === "number") return [value];
  return Array.isArray(fallback) ? [...fallback] : [];
}

function lightColor(value) {
  const source = Array.isArray(value) ? value : [1, 1, 1, 1];
  let rgb = [Number(source[0]) || 0, Number(source[1]) || 0, Number(source[2]) || 0];
  if (Math.max(...rgb.map(Math.abs)) > 16) rgb = rgb.map((component) => component / 255);
  const intensity = source.length > 3 ? Number(source[3]) || 0 : 1;
  return rgb.map((component) => component * intensity);
}

function lightInputValue(name, conditions, context) {
  const supplied = context.inputs?.[name] ?? context.conditions?.[name] ?? conditions.get(name);
  return Math.max(0, Math.min(1, Number(supplied) || 0));
}

function lightBindings(section) {
  const bindings = [], add = (input, inverted = false, source = "explicit") => {
    if (!bindings.some((binding) => binding.input === input && binding.inverted === inverted)) bindings.push({ input, inverted, source });
  };
  const enabled = (key) => Number(parseCspValue(lastValue(section, key, "0"))) !== 0;
  const headlightsDirectiveAuthored = lastValue(section, "BIND_TO_HEADLIGHTS", "") !== "";
  if (enabled("BIND_TO_HEADLIGHTS")) add("HEADLIGHTS");
  if (enabled("BIND_TO_HIGHBEAM")) add("HIGHBEAM");
  if (enabled("BIND_TO_LOWBEAM")) add("LOWBEAM");
  if (enabled("BIND_TO_BRAKELIGHTS")) add("BRAKE");
  for (let index = 0; index < 20; index++) if (enabled(`BIND_TO_EXTRA_${String.fromCharCode(65 + index)}`)) add(`EXTRA_${String.fromCharCode(65 + index)}`);
  if (enabled("NOT_WITH_HEADLIGHTS")) add("HEADLIGHTS", true);
  if (!bindings.length) {
    if (/^LIGHT_HEADLIGHT(?:_|$)/i.test(section.name) && !headlightsDirectiveAuthored) add("HEADLIGHTS", false, "section-role");
    else if (/^LIGHT_BRAKE(?:_|$)/i.test(section.name)) add("BRAKE", false, "section-role");
    else if (/^LIGHT_REVERSE(?:_|$)/i.test(section.name)) add("REVERSE", false, "section-role");
    else if (/^LIGHT_TURNSIGNAL_LEFT(?:_|$)/i.test(section.name)) add("TURNSIGNAL_LEFT", false, "section-role");
    else if (/^LIGHT_TURNSIGNAL_RIGHT(?:_|$)/i.test(section.name)) add("TURNSIGNAL_RIGHT", false, "section-role");
    else if (/^LIGHT_HAZARD(?:_|$)/i.test(section.name)) add("HAZARD", false, "section-role");
    else if (/^head_?lights$/i.test(lastValue(section, "BOUND_TO", "").trim())) add("HEADLIGHTS", false, "bound-mesh-state");
  }
  return bindings;
}

function lightFactor(section, conditions, usedConditions, usedInputs, context) {
  let name = lastValue(section, "CONDITION", "").trim().toUpperCase();
  let value = 1;
  const names = [];
  if (name && name !== "ALWAYS_ON") {
    usedConditions.add(name);
    value *= Math.max(0, Math.min(1, conditions.get(name) ?? 0));
    names.push(name);
  }
  const bindings = lightBindings(section);
  for (const binding of bindings) {
    const input = lightInputValue(binding.input, conditions, context);
    const previous = usedInputs.get(binding.input);
    usedInputs.set(binding.input, { min: Math.min(previous?.min ?? 0, 0), max: Math.max(previous?.max ?? 1, 1), value: input });
    value *= binding.inverted ? 1 - input : input;
    names.push(`${binding.inverted ? "!" : ""}${binding.input}`);
  }
  return { name: names.join(" × ") || "ALWAYS_ON", value, bindings };
}

function popupLightState(section, condition, usedInputs, context) {
  const number = (key, fallback) => {
    const value = Number(parseCspValue(lastValue(section, key, String(fallback))));
    return Number.isFinite(value) ? value : fallback;
  };
  const authored = ["POPUP_ENABLED", "POPUP_START", "POPUP_END", "POPUP_SECOND_SPOT_INITIAL_VALUE", "POPUP_SECOND_SPOT_EXP", "POPUP_EDGE_OFFSET", "POPUP_EDGE_EXP"]
    .some((key) => lastValue(section, key, "") !== "");
  const eligible = authored || /^LIGHT_HEADLIGHT(?:_|$)/i.test(section.name);
  const enabled = eligible && number("POPUP_ENABLED", 1) !== 0;
  const supplied = context.popupPosition ?? context.inputs?.POPUP_POSITION;
  const animation = Math.max(0, Math.min(1, Number.isFinite(Number(supplied)) ? Number(supplied) : 1));
  if (enabled && (authored || supplied !== undefined)) {
    const previous = usedInputs.get("POPUP_POSITION");
    usedInputs.set("POPUP_POSITION", { min: Math.min(previous?.min ?? 0, 0), max: Math.max(previous?.max ?? 1, 1), value: animation });
  }
  const start = number("POPUP_START", 0.05), end = number("POPUP_END", 0.7);
  const progress = !enabled ? 1 : end === start
    ? (animation <= start ? 0 : 1)
    : Math.max(0, Math.min(1, (animation - start) / (end - start)));
  const secondInitial = number("POPUP_SECOND_SPOT_INITIAL_VALUE", 0.4);
  const secondExponent = number("POPUP_SECOND_SPOT_EXP", 0.6);
  const edgeOffset = number("POPUP_EDGE_OFFSET", 0.5);
  const edgeExponent = number("POPUP_EDGE_EXP", 0.3);
  const headlightsBound = condition.bindings.some((binding) => binding.input === "HEADLIGHTS" && !binding.inverted);
  const secondCurve = progress >= 1 ? 1 : secondInitial + (1 - secondInitial) * Math.pow(progress, secondExponent);
  const outputFactor = enabled && headlightsBound ? secondCurve : 1;
  const edgeShift = enabled ? edgeOffset * (1 - Math.pow(progress, edgeExponent)) : 0;
  return { popupEnabled: enabled, popupAnimation: animation, popupProgress: progress, popupStart: start, popupEnd: end, popupSecondSpotInitial: secondInitial, popupSecondSpotExponent: secondExponent, popupSecondSpotFactor: outputFactor, popupEdgeOffset: edgeOffset, popupEdgeExponent: edgeExponent, popupEdgeShift: edgeShift };
}

function componentCentres(node, world, normalDirection, clusterThreshold, limit) {
  const vertexCount = Math.floor(node.vertices.length / node.vertexStride);
  if (!vertexCount) return [];
  const threshold = Math.max(0.001, clusterThreshold), grid = new Map(), centres = [];
  const consider = (indices) => {
    const position = [0, 0, 0], direction = [0, 0, 0];
    for (const vertexIndex of indices) {
      const offset = vertexIndex * node.vertexStride;
      const transformed = transformPosition(world, [node.vertices[offset], node.vertices[offset + 1], node.vertices[offset + 2]]);
      const normal = transformDirection(world, [node.vertices[offset + 3], node.vertices[offset + 4], node.vertices[offset + 5]]);
      for (let axis = 0; axis < 3; axis++) { position[axis] += transformed[axis] / indices.length; direction[axis] += normal[axis]; }
    }
    const cell = position.map((component) => Math.floor(component / threshold));
    for (let x = -1; x <= 1; x++) for (let y = -1; y <= 1; y++) for (let z = -1; z <= 1; z++) {
      const nearby = grid.get(`${cell[0] + x},${cell[1] + y},${cell[2] + z}`) || [];
      if (nearby.some((index) => Math.hypot(...centres[index].position.map((component, axis) => component - position[axis])) < threshold)) return;
    }
    const index = centres.length;
    centres.push({ position, direction: normalDirection ? normalizeVector(direction) : null });
    const key = cell.join(",");
    if (!grid.has(key)) grid.set(key, []);
    grid.get(key).push(index);
  };
  if (node.indices.length >= 3) {
    for (let index = 0; index + 2 < node.indices.length && centres.length < limit; index += 3) consider([node.indices[index], node.indices[index + 1], node.indices[index + 2]]);
  } else {
    for (let index = 0; index < vertexCount && centres.length < limit; index++) consider([index]);
  }
  return centres;
}

function baseLight(section, color, condition) {
  const range = Math.max(0.001, Number(parseCspValue(lastValue(section, "RANGE", "10"))) || 10);
  const spot = Number(parseCspValue(lastValue(section, "SPOT", "0"))) || 0;
  const secondSpot = Math.max(0, Number(parseCspValue(lastValue(section, "SECOND_SPOT", "0"))) || 0);
  const secondSpotRange = Math.max(0, Number(parseCspValue(lastValue(section, "SECOND_SPOT_RANGE", "0"))) || 0);
  const secondSpotIntensity = Math.max(0, Number(parseCspValue(lastValue(section, "SECOND_SPOT_INTENSITY", "0"))) || 0);
  const spotEdgeText = lastValue(section, "SPOT_EDGE", "").trim();
  const shadowKeys = ["SHADOWS_SPOT", "SHADOWS_RANGE", "SHADOWS_EXP_FACTOR", "SHADOWS_BOOST", "SHADOWS_CLIP_PLANE", "SHADOWS_CLIP_SPHERE", "SHADOWS_EXTRA_BLUR"];
  const shadowToggle = lastValue(section, "SHADOWS", "");
  const shadowRangeText = lastValue(section, "SHADOWS_RANGE", "");
  const castsShadows = shadowToggle !== "" ? Number(parseCspValue(shadowToggle)) !== 0 : shadowKeys.some((key) => lastValue(section, key, "") !== "");
  const affectsTrackRaw = String(parseCspValue(lastValue(section, "AFFECTS_TRACK", "1"))).trim().toUpperCase();
  const affectsTrackMode = affectsTrackRaw === "INTERIOR_ONLY" ? "interior-only" : Number(affectsTrackRaw) === 0 ? "none" : "all";
  const interiorOnly = Number(parseCspValue(lastValue(section, "INTERIOR_ONLY", "0"))) !== 0;
  const exteriorOnly = Number(parseCspValue(lastValue(section, "EXTERIOR_ONLY", "0"))) !== 0;
  return {
    section: section.name,
    source: section.source,
    line: section.line,
    color,
    condition: condition.name,
    conditionValue: condition.value,
    bindings: condition.bindings,
    boundTo: splitCspList(lastValue(section, "BOUND_TO", "")),
    visibilityLevel: Math.max(0, Math.floor(Number(parseCspValue(lastValue(section, "VISIBILITY_LEVEL", "0"))) || 0)),
    affectsTrack: affectsTrackMode !== "none",
    affectsTrackMode,
    exteriorOnly,
    interiorOnly,
    viewMode: interiorOnly ? "interior" : exteriorOnly ? "exterior" : "both",
    range,
    spot,
    spotSharpness: Math.max(0, Math.min(1, Number(parseCspValue(lastValue(section, "SPOT_SHARPNESS", "0.8"))) || 0)),
    secondSpot,
    secondSpotSharpness: Math.max(0, Math.min(1, Number(parseCspValue(lastValue(section, "SECOND_SPOT_SHARPNESS", "0.8"))) || 0)),
    secondSpotSkip: Math.max(0, Number(parseCspValue(lastValue(section, "SECOND_SPOT_SKIP", "0.3"))) || 0),
    secondSpotRange,
    secondSpotIntensity,
    hasSecondSpot: secondSpot > 0 && secondSpotRange > 0 && secondSpotIntensity > 0,
    spotEdge: spotEdgeText ? vectorValue(section, "SPOT_EDGE", [0.12, 0.12, 0.12]).slice(0, 3) : null,
    spotEdgeSharpness: Math.max(0, Number(parseCspValue(lastValue(section, "SPOT_EDGE_SHARPNESS", "0"))) || 0),
    spotUp: vectorValue(section, "SPOT_UP", [0, 1, 0]).slice(0, 3),
    hasSpotEdge: Boolean(spotEdgeText) && Math.max(0, Number(parseCspValue(lastValue(section, "SPOT_EDGE_SHARPNESS", "0"))) || 0) > 0,
    rangeGradientOffset: Math.max(0, Math.min(0.999, Number(parseCspValue(lastValue(section, "RANGE_GRADIENT_OFFSET", /^LIGHT_SERIES/i.test(section.name) ? "0.2" : "0"))) || 0)),
    specularMult: Math.max(0, Number(parseCspValue(lastValue(section, "SPECULAR_MULT", "1"))) || 0),
    diffuseConcentration: Math.max(0, Number(parseCspValue(lastValue(section, "DIFFUSE_CONCENTRATION", "0"))) || 0),
    fadeAt: Math.max(0, Number(parseCspValue(lastValue(section, "FADE_AT", "200"))) || 0),
    fadeSmooth: Math.max(0, Number(parseCspValue(lastValue(section, "FADE_SMOOTH", "80"))) || 0),
    castsShadows,
    shadowSpot: Math.max(0, Number(parseCspValue(lastValue(section, "SHADOWS_SPOT", String(spot)))) || 0),
    shadowRange: shadowRangeText === "" ? Math.min(range, 30) : Math.max(0.001, Number(parseCspValue(shadowRangeText)) || range),
    shadowRangeAuthored: shadowRangeText !== "",
    shadowExpFactor: Math.max(0.001, Number(parseCspValue(lastValue(section, "SHADOWS_EXP_FACTOR", "20"))) || 20),
    shadowBoost: Math.max(0, Number(parseCspValue(lastValue(section, "SHADOWS_BOOST", "0"))) || 0),
    shadowClipPlane: Math.max(0.001, Number(parseCspValue(lastValue(section, "SHADOWS_CLIP_PLANE", "0.5"))) || 0.5),
    shadowClipSphere: Math.max(0, Number(parseCspValue(lastValue(section, "SHADOWS_CLIP_SPHERE", "0.5"))) || 0),
    shadowExtraBlur: Number(parseCspValue(lastValue(section, "SHADOWS_EXTRA_BLUR", "0"))) !== 0
  };
}

function evaluateLights(model, config, conditions, usedConditions, usedInputs, context) {
  const lights = [], worlds = sceneWorldMatrices(model.root);
  const maximum = Math.max(1, Math.floor(context.maxLights ?? 4096));
  const maximumPerSeries = Math.max(1, Math.floor(context.maxLightsPerSeries ?? 256));
  let matchedSections = 0;
  for (const section of config.sections) {
    const series = /^LIGHT_SERIES(?:_|$)/i.test(section.name);
    const explicitName = /^LIGHT_/i.test(section.name) && (/^LIGHT_(?:EXTRA(?:_|$)|(?:\d+|\.\.\.)$)/i.test(section.name) || lastValue(section, "POSITION") !== "" || lastValue(section, "LINE_FROM") !== "");
    const explicit = explicitName && (lastValue(section, "COLOR") !== "" || lastValue(section, "COLOR_FROM") !== "" || lastValue(section, "COLOR_TO") !== "");
    if ((!series && !explicit) || !numericActive(section) || lights.length >= maximum) continue;
    const condition = lightFactor(section, conditions, usedConditions, usedInputs, context);
    const popup = popupLightState(section, condition, usedInputs, context);
    const hasLineColors = lastValue(section, "COLOR_FROM") !== "" || lastValue(section, "COLOR_TO") !== "";
    const lineOnFrom = lightColor(vectorValue(section, "COLOR_FROM", [1, 1, 1, 10]));
    const lineOnTo = lightColor(vectorValue(section, "COLOR_TO", [1, 1, 1, 10]));
    const onValue = lastValue(section, "COLOR") !== ""
      ? vectorValue(section, "COLOR", [1, 1, 1, 10])
      : interpolate(vectorValue(section, "COLOR_FROM", [1, 1, 1, 10]), vectorValue(section, "COLOR_TO", [1, 1, 1, 10]), 0.5);
    const on = lightColor(onValue);
    const offColorText = lastValue(section, "COLOR_OFF", ""), offMultText = lastValue(section, "OFF_MULT", "");
    const offSource = offColorText ? lightColor(vectorValue(section, "COLOR_OFF", [0, 0, 0, 0])) : on;
    const offMult = offMultText === "" ? (offColorText ? 1 : 0) : Math.max(0, Number(parseCspValue(offMultText)) || 0);
    const off = offSource.map((component) => component * offMult);
    const color = interpolate(off, on, condition.value).map((component) => component * popup.popupSecondSpotFactor);
    const endpointColor = (endpoint) => interpolate(offColorText ? off : endpoint.map((component) => component * offMult), endpoint, condition.value).map((component) => component * popup.popupSecondSpotFactor);
    const lineColorFrom = endpointColor(lineOnFrom), lineColorTo = endpointColor(lineOnTo);
    const base = baseLight(section, color, condition);
    if (base.spotEdge) base.spotEdge = base.spotEdge.map((component) => component - popup.popupEdgeShift);
    const offRangeMult = Math.max(0, Number(parseCspValue(lastValue(section, "OFF_RANGE_MULT", "1"))) || 0);
    const offFadeMult = Math.max(0, Number(parseCspValue(lastValue(section, "OFF_FADE_MULT", "1"))) || 0);
    const rangeMult = interpolate(offRangeMult, 1, condition.value), fadeMult = interpolate(offFadeMult, 1, condition.value);
    const stateBase = { ...base, ...popup, range: base.range * rangeMult, fadeAt: base.fadeAt * fadeMult, fadeSmooth: base.fadeSmooth * fadeMult, stateFactor: condition.value, onRange: base.range, offRangeMult, offFadeMult, offMult };
    const offset = vectorValue(section, "OFFSET", [0, 0, 0]);
    const directionText = lastValue(section, "DIRECTION", "").trim();
    const useNormals = /^NORMAL$/i.test(directionText);
    const fixedDirection = !directionText || useNormals ? null : normalizeVector(parseCspValue(directionText));
    const created = [];
    if (series) {
      const targets = findTargets(model, section);
      const clusterThreshold = Math.max(0.001, Number(parseCspValue(lastValue(section, "CLUSTER_THRESHOLD", "10"))) || 10);
      for (const node of targets) {
        const remaining = Math.min(maximumPerSeries - created.length, maximum - lights.length - created.length);
        if (remaining <= 0) break;
        for (const point of componentCentres(node, worlds.matrices.get(node), useNormals, clusterThreshold, remaining)) {
          created.push({
            ...stateBase,
            position: point.position.map((component, axis) => component + (Number(offset[axis]) || 0)),
            direction: point.direction || fixedDirection,
            derivedFrom: node.name
          });
        }
      }
    } else {
      const relativeName = lastValue(section, "RELATIVE_TO", "").trim().toLowerCase();
      const relative = worlds.byName.get(relativeName) || identityMatrix();
      let direction = fixedDirection ? transformDirection(relative, fixedDirection) : null;
      const spotUp = transformDirection(relative, base.spotUp);
      const lineFromText = lastValue(section, "LINE_FROM", ""), lineToText = lastValue(section, "LINE_TO", "");
      if (lineFromText && lineToText) {
        const from = transformPosition(relative, vectorValue(section, "LINE_FROM", [0, 0, 0]));
        const to = transformPosition(relative, vectorValue(section, "LINE_TO", from));
        const vector = to.map((component, axis) => component - from[axis]), lengthSquared = vector.reduce((sum, component) => sum + component * component, 0);
        created.push({ ...stateBase, position: interpolate(from, to, 0.5), direction, spotUp, lineLight: true, lineFrom: from, lineTo: to, lineVector: vector, lineDistanceInverse: lengthSquared > 1e-12 ? 1 / lengthSquared : 0, lineColorFrom: hasLineColors ? lineColorFrom : color, lineColorTo: hasLineColors ? lineColorTo : color, castsShadows: false });
      } else {
        const onPosition = vectorValue(section, "POSITION", [0, 0, 0]), offPositionText = lastValue(section, "OFF_POSITION", "");
        const offPosition = offPositionText ? vectorValue(section, "OFF_POSITION", onPosition) : onPosition;
        let position = transformPosition(relative, interpolate(offPosition, onPosition, condition.value));
        const mirrorText = lastValue(section, "MIRROR", "").trim();
        const onMirror = mirrorText === "" ? Number.NaN : Number(parseCspValue(mirrorText));
        const offMirrorText = lastValue(section, "OFF_MIRROR", "").trim(), offMirror = offMirrorText === "" ? onMirror : Number(parseCspValue(offMirrorText));
        const mirror = Number.isFinite(onMirror) ? interpolate(Number.isFinite(offMirror) ? offMirror : onMirror, onMirror, condition.value) : Number.NaN;
        const positionedBase = { ...stateBase, onPosition, offPosition, onMirror, offMirror };
        if (Number.isFinite(mirror) && mirror > 0) {
          if (Math.abs(position[0]) < 1e-8) position = [-mirror, position[1], position[2]];
          created.push({ ...positionedBase, position, direction, spotUp });
          const mirroredDirection = direction ? [-direction[0], direction[1], direction[2]] : null;
          const mirroredSpotUp = [-spotUp[0], spotUp[1], spotUp[2]];
          created.push({ ...positionedBase, position: [mirror, position[1], position[2]], direction: mirroredDirection, spotUp: mirroredSpotUp, mirrored: true });
        } else {
          created.push({ ...positionedBase, position, direction, spotUp });
        }
      }
    }
    if (created.length) { matchedSections++; lights.push(...created.slice(0, maximum - lights.length)); }
  }
  return { lights, matchedSections };
}

function evaluateTrackOccluders(config) {
  const occluders = [];
  for (const section of config.sections) {
    const match = section.name.match(/^TRACK_OCCLUDER_(WALL|BOX)(?:_|$)/i);
    if (!match || !numericActive(section) || Number(parseCspValue(lastValue(section, "CULLING", "1"))) === 0) continue;
    const type = match[1].toLowerCase(), count = type === "box" ? 4 : 2, points = [];
    for (let index = 0; index < count; index++) {
      const text = lastValue(section, `POINT_${index}`, "");
      if (!text) break;
      const point = vectorValue(section, `POINT_${index}`, []).slice(0, 3);
      if (point.length !== 3 || point.some((component) => !Number.isFinite(component))) break;
      points.push(point);
    }
    if (points.length !== count) continue;
    const exclusion = [];
    for (let index = 0; index < 4; index++) {
      const text = lastValue(section, `EXCLUSION_${index}`, "");
      if (!text) break;
      const point = vectorValue(section, `EXCLUSION_${index}`, []).slice(0, 3);
      if (point.length !== 3 || point.some((component) => !Number.isFinite(component))) break;
      exclusion.push(point);
    }
    occluders.push({ section: section.name, source: section.source, line: section.line, description: lastValue(section, "DESCRIPTION", section.name), type, points, exclusion: exclusion.length === 4 ? exclusion : [], culling: true });
  }
  const settings = config.sections.find((section) => /^TRACK_OCCLUDERS$/i.test(section.name));
  return { occluders, settings: { cellSize: Math.max(1, Number(parseCspValue(lastValue(settings, "CELL_SIZE", "1000"))) || 1000), sizeWeightFactor: Math.max(0, Number(parseCspValue(lastValue(settings, "SIZE_WEIGHT_FACTOR", "0.5"))) || 0) } };
}

function overrideFor(map, node) {
  if (!map.has(node)) map.set(node, { properties: new Map(), resources: new Map(), sources: [], shader: null, blendMode: null, depthMode: null, cullMode: null, isTransparent: null, layer: null, lodIn: null, lodOut: null, castShadows: null });
  return map.get(node);
}

function meshAdjustmentValue(section, key) {
  const text = lastValue(section, key, "");
  return text === "" ? null : parseCspValue(text);
}

function replacementProperties(section) {
  const result = [];
  for (const entry of section.entries) {
    if (!entry.key.startsWith("PROP_")) continue;
    const values = splitCspList(entry.value);
    if (values.length >= 2) result.push({ key: values[0], value: parseCspValue(values.slice(1).join(",")) });
  }
  return result;
}

function replacementResources(section) {
  const result = [];
  for (let index = 0; index < section.entries.length; index++) {
    const entry = section.entries[index], match = entry.key.match(/^RESOURCE_(\d+)$/);
    if (!match) continue;
    const suffix = match[1], nearby = [];
    for (let cursor = index + 1; cursor < section.entries.length && !/^RESOURCE_\d+$/.test(section.entries[cursor].key); cursor++) nearby.push(section.entries[cursor]);
    const value = (key) => nearby.find((candidate) => candidate.key === `${key}_${suffix}`)?.value ?? lastValue(section, `${key}_${suffix}`, "");
    const texture = value("RESOURCE_TEXTURE"), file = value("RESOURCE_FILE"), colorText = value("RESOURCE_COLOR");
    result.push({ slot: splitCspList(entry.value)[0] || entry.value, texture, file, color: colorText === "" ? null : parseCspValue(colorText) });
  }
  return result;
}

function materialAdjustmentProperties(section) {
  const result = [];
  for (let index = 0; index < section.entries.length; index++) {
    const entry = section.entries[index], match = entry.key.match(/^KEY_(\d+|\.\.\.)$/);
    if (!match) continue;
    const suffix = match[1], values = [];
    for (let cursor = index + 1; cursor < section.entries.length && !/^KEY_(?:\d+|\.\.\.)$/.test(section.entries[cursor].key); cursor++) values.push(section.entries[cursor]);
    const valueKey = `VALUE_${suffix}`, offKey = `OFF_VALUE_${suffix}`, alternateOffKey = `VALUE_${suffix}_OFF`;
    result.push({
      key: entry.value,
      onText: values.find((candidate) => candidate.key === valueKey)?.value ?? lastValue(section, valueKey, ""),
      offText: values.find((candidate) => candidate.key === offKey)?.value ?? values.find((candidate) => candidate.key === alternateOffKey)?.value ?? lastValue(section, offKey, lastValue(section, alternateOffKey, "ORIGINAL"))
    });
  }
  return result;
}

function previewInput(binding, context, usedInputs) {
  const name = binding.input || "ONE";
  const threshold = Number.isFinite(binding.threshold) ? binding.threshold : null;
  const minimum = Number.isFinite(binding.inputMin) ? binding.inputMin : 0;
  let maximum = Number.isFinite(binding.inputMax) ? binding.inputMax : threshold !== null ? Math.max(threshold * 2, 1) : 1;
  if (/RPM/i.test(name)) maximum = Math.max(maximum, 8000);
  if (/SPEED/i.test(name)) maximum = Math.max(maximum, 300);
  const supplied = context.inputs?.[name] ?? context.conditions?.[name];
  const value = Number(supplied ?? (name === "ONE" ? 1 : 0)) || 0;
  if (name !== "ONE") {
    const previous = usedInputs.get(name);
    usedInputs.set(name, { min: Math.min(previous?.min ?? minimum, minimum), max: Math.max(previous?.max ?? maximum, maximum), value });
  }
  let factor;
  if (binding.inputLut) {
    const output = evaluateLut(parseLut(String(binding.inputLut)), value);
    factor = Array.isArray(output) ? Number(output[0]) || 0 : Number(output) || 0;
  } else if (threshold !== null) {
    factor = value >= threshold ? 1 : 0;
  } else {
    factor = (value - minimum) / Math.max(1e-9, maximum - minimum);
  }
  factor = Math.max(0, Math.min(1, factor));
  if (binding.inputInverse) factor = 1 - factor;
  if (binding.invert) factor = 1 - factor;
  return factor;
}

function shaderEmissiveColor(value) {
  if (typeof value === "number") return [value, value, value];
  if (!Array.isArray(value)) return [0, 0, 0];
  let color = [Number(value[0]) || 0, Number(value[1]) || 0, Number(value[2]) || 0];
  if (Math.max(...color.map(Math.abs)) > 16) color = color.map((component) => component / 255);
  const strength = value.length > 3 ? Number(value[3]) || 0 : 1;
  return color.map((component) => component * strength);
}

function evaluateCustomEmissive(template, override, context, usedInputs) {
  const colors = new Map();
  for (const binding of template.bindings) {
    const factor = previewInput(binding, context, usedInputs);
    const color = interpolate(binding.offColor, binding.color, factor);
    const previous = colors.get(binding.channel) || [0, 0, 0];
    colors.set(binding.channel, previous.map((component, index) => component + (Number(color[index]) || 0)));
  }
  for (let channel = 0; channel < 25; channel++) {
    if (colors.has(channel) && Math.max(...colors.get(channel).map(Math.abs)) > 1e-8) continue;
    const key = channel === 0 ? "ksemissive" : `ksemissive${channel}`;
    if (override.properties.has(key)) colors.set(channel, shaderEmissiveColor(override.properties.get(key)));
  }
  return { ...template, channelColors: [...colors.entries()] };
}

export function evaluateCspConfig(model, config, context = {}) {
  const nodeOverrides = new Map();
  const usedConditions = new Set();
  const usedInputs = new Map();
  const conditions = conditionTable(config, context, usedInputs);
  let matchedSections = 0, matchedMeshes = 0;
  for (const section of config.sections) {
    const replacement = /^SHADER_REPLACEMENT(?:_|$)/i.test(section.name);
    const adjustment = /^MATERIAL_ADJUSTMENT(?:_|$)/i.test(section.name);
    const meshAdjustment = /^MESH_ADJUSTMENT(?:_|$)/i.test(section.name);
    if ((!replacement && !adjustment && !meshAdjustment) || !numericActive(section)) continue;
    const targets = findTargets(model, section);
    if (!targets.length) continue;
    matchedSections++; matchedMeshes += targets.length;
    for (const node of targets) {
      const material = model.materials[node.materialId];
      const override = overrideFor(nodeOverrides, node);
      override.sources.push({ section: section.name, line: section.line, source: section.source });
      if (replacement) {
        override.shader = lastValue(section, "SHADER", override.shader);
        override.blendMode = lastValue(section, "BLEND_MODE", override.blendMode);
        override.depthMode = lastValue(section, "DEPTH_MODE", override.depthMode);
        override.cullMode = lastValue(section, "CULL_MODE", override.cullMode);
        for (const property of replacementProperties(section)) override.properties.set(property.key.toLowerCase(), property.value);
        for (const resource of replacementResources(section)) override.resources.set(resource.slot.toLowerCase(), resource);
        if (section.customEmissive) override.customEmissiveTemplate = section.customEmissive;
      } else if (adjustment) {
        const conditionName = lastValue(section, "CONDITION", "ALWAYS_ON").toUpperCase();
        if (conditionName !== "ALWAYS_ON" && conditionName !== "ALWAYS OFF" && conditionName !== "ALWAYS_OFF") usedConditions.add(conditionName);
        const factor = Math.max(0, Math.min(1, conditions.get(conditionName) ?? 0));
        for (const property of materialAdjustmentProperties(section)) {
          const original = effectiveOriginal(material, override, property.key);
          const on = parseCspValue(property.onText || String(original));
          const off = resolveOff(parseCspValue(property.offText), original);
          override.properties.set(property.key.toLowerCase(), interpolate(off, on, factor));
        }
      } else {
        const transparent = meshAdjustmentValue(section, "IS_TRANSPARENT"), layer = meshAdjustmentValue(section, "LAYER"), lodIn = meshAdjustmentValue(section, "LOD_IN"), lodOut = meshAdjustmentValue(section, "LOD_OUT"), castShadows = meshAdjustmentValue(section, "CAST_SHADOWS");
        if (typeof transparent === "number") override.isTransparent = transparent !== 0;
        if (typeof layer === "number" && Number.isFinite(layer)) override.layer = layer;
        if (typeof lodIn === "number" && Number.isFinite(lodIn)) override.lodIn = lodIn;
        if (typeof lodOut === "number" && Number.isFinite(lodOut)) override.lodOut = lodOut;
        if (typeof castShadows === "number") override.castShadows = castShadows !== 0;
      }
    }
  }
  let customEmissiveMeshes = 0;
  for (const override of nodeOverrides.values()) {
    if (!override.customEmissiveTemplate) continue;
    override.customEmissive = evaluateCustomEmissive(override.customEmissiveTemplate, override, context, usedInputs);
    customEmissiveMeshes++;
  }
  const lightEvaluation = evaluateLights(model, config, conditions, usedConditions, usedInputs, context);
  const trackOcclusion = evaluateTrackOccluders(config);
  const unresolvedIncludes = config.sections.filter((section) => /^INCLUDE(?::|$)/i.test(section.name)).flatMap((section) => {
    const fromName = section.name.includes(":") ? [section.name.slice(section.name.indexOf(":") + 1).trim()] : [];
    return [...fromName, ...splitCspList(lastValue(section, "INCLUDE"))];
  }).filter(Boolean).filter((include) => !config.resolvedIncludes?.has(include.replace(/\\/g, "/").split("/").at(-1).toLowerCase()));
  return { nodeOverrides, conditions, usedConditions, usedInputs, matchedSections, matchedMeshes, customEmissiveMeshes, lights: lightEvaluation.lights, matchedLightSections: lightEvaluation.matchedSections, trackOccluders: trackOcclusion.occluders, trackOccluderSettings: trackOcclusion.settings, unresolvedIncludes };
}
