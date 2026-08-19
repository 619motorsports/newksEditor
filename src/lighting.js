export const KS_EDITOR_EXPOSURE = Object.freeze({ min: 0.2, max: 0.5, target: 0.32, gamma: 1.2, saturation: 0.95 });
export const KS_EDITOR_TONEMAP = Object.freeze({
  function: -1,
  mappingFactor: 32,
  characteristicCurve: 0.5,
  curveScale: 2.6581413745880127,
  curveShoulder: 0.6653175950050354,
  inputFloor: 1 / 16384,
  outputEpsilon: 1 / 4194304
});
export const KS_EDITOR_GLARE = Object.freeze({
  enabled: true,
  quality: 3,
  sourceScale: 0.25,
  levels: 5,
  luminance: 1.6,
  threshold: 5,
  brightPassType: 1,
  brightPassRemap: 1,
  bloomFilterThreshold: 0.002,
  bloomGaussianRadiusScale: 0.95,
  bloomLuminanceGamma: 2,
  generationRangeScale: 1,
  shapeLuminance: 5,
  shapeBloomLuminance: 0.038,
  compositeBase: 0.035,
  ditherScale: 1 / 255,
  ditherOffset: -0.5 / 255
});
export const CSP_LIGHT_FADE_AT_DEFAULT = 200;
export const CSP_LIGHT_FADE_SMOOTH_DEFAULT = 80;
export const CSP_SPOT_DEGREES_TO_RADIANS = 0.017453294;
export const CSP_SPOT_ANGLE_MIN_RADIANS = 0.01;
export const CSP_SPOT_HALF_ANGLE_MAX = 3.1414182;
export const CSP_SPOT_SHARPNESS_MAX = 0.999;

export function ksEditorAutoExposure(luminance, target = KS_EDITOR_EXPOSURE.target, minimum = KS_EDITOR_EXPOSURE.min, maximum = KS_EDITOR_EXPOSURE.max) {
  const measured = Math.max(0.0001, Number(luminance) || 0);
  const low = Math.max(0, Number(minimum) || 0), high = Math.max(low, Number(maximum) || 0);
  return Math.max(low, Math.min(high, (Number(target) || 0) / measured));
}

export function ksEditorYebisToneMap(rgb, exposure = 1, { gamma = KS_EDITOR_EXPOSURE.gamma, saturation = KS_EDITOR_EXPOSURE.saturation, curveScale = KS_EDITOR_TONEMAP.curveScale, curveShoulder = KS_EDITOR_TONEMAP.curveShoulder } = {}) {
  const source = Array.isArray(rgb) ? rgb.slice(0, 3).map((value) => Math.max(0, Number(value) || 0)) : [0, 0, 0];
  while (source.length < 3) source.push(0);
  const luminance = source[0] * 0.2126 + source[1] * 0.7152 + source[2] * 0.0722;
  const colored = source.map((value) => Math.max(KS_EDITOR_TONEMAP.inputFloor, (luminance + (value - luminance) * saturation) * Math.max(0, Number(exposure) || 0)));
  const exponent = 1 / Math.max(Number.EPSILON, Number(gamma) || 1);
  return colored.map((value) => {
    const decay = Math.exp(-value * curveScale);
    const shoulder = 1 - decay * curveShoulder;
    const curve = Math.max(0, Math.min(1, (1 - decay) * shoulder * shoulder));
    return Math.pow(Math.min(1, curve + KS_EDITOR_TONEMAP.outputEpsilon), exponent);
  });
}

export function ksEditorGlareBrightPass(rgb, exposure = 1, threshold = KS_EDITOR_GLARE.threshold, remap = KS_EDITOR_GLARE.brightPassRemap) {
  const source = Array.isArray(rgb) ? rgb.slice(0, 3).map((value) => Math.max(0, Number(value) || 0)) : [0, 0, 0];
  while (source.length < 3) source.push(0);
  const gain = Math.max(0, Number(exposure) || 0), cutoff = Math.max(0, Number(threshold) || 0), scale = Math.max(0, Number(remap) || 0);
  return source.map((value) => Math.min(64000, Math.max(0, value * gain - cutoff) * scale));
}

export function ksEditorBloomCompositeScale({ range = KS_EDITOR_GLARE.generationRangeScale, glareLuminance = KS_EDITOR_GLARE.luminance, shapeLuminance = KS_EDITOR_GLARE.shapeLuminance, bloomLuminance = KS_EDITOR_GLARE.shapeBloomLuminance } = {}) {
  return Math.max(0, Number(range) || 0) * KS_EDITOR_GLARE.compositeBase * Math.max(0, Number(shapeLuminance) || 0) * Math.max(0, Number(glareLuminance) || 0) * Math.max(0, Number(bloomLuminance) || 0);
}

export function cspLineClosestPoint(from, to, position) {
  const a = Array.isArray(from) ? from.slice(0, 3).map(Number) : [], b = Array.isArray(to) ? to.slice(0, 3).map(Number) : [], p = Array.isArray(position) ? position.slice(0, 3).map(Number) : [];
  if (a.length < 3 || b.length < 3 || p.length < 3 || [...a, ...b, ...p].some((value) => !Number.isFinite(value))) return Object.freeze({ point: Object.freeze([0, 0, 0]), value: 0, clampedValue: 0, distanceInverse: 0 });
  const ab = b.map((value, index) => value - a[index]), lengthSquared = ab.reduce((sum, value) => sum + value * value, 0);
  const distanceInverse = lengthSquared > 1e-12 ? 1 / lengthSquared : 0;
  const value = distanceInverse ? ab.reduce((sum, value, index) => sum + (p[index] - a[index]) * value, 0) * distanceInverse : 0;
  const clampedValue = Math.max(0, Math.min(1, value));
  return Object.freeze({ point: Object.freeze(a.map((value, index) => value + ab[index] * clampedValue)), value, clampedValue, distanceInverse });
}

export function cspLineLightSample(from, to, position, colorFrom = [0, 0, 0], colorTo = colorFrom) {
  const closest = cspLineClosestPoint(from, to, position), first = Array.isArray(colorFrom) ? colorFrom : [0, 0, 0], second = Array.isArray(colorTo) ? colorTo : first;
  return Object.freeze({ ...closest, color: Object.freeze(Array.from({ length: 3 }, (_, index) => (Number(first[index]) || 0) + ((Number(second[index]) || 0) - (Number(first[index]) || 0)) * closest.clampedValue)) });
}

export function cspLightReceiverVisible(light, { interiorView = false, trackReceiver = null } = {}) {
  const viewMode = light?.viewMode || (light?.interiorOnly ? "interior" : light?.exteriorOnly ? "exterior" : "both");
  if (viewMode === "interior" && !interiorView) return false;
  if (viewMode === "exterior" && interiorView) return false;
  if (trackReceiver === true) {
    const trackMode = light?.affectsTrackMode || (light?.affectsTrack === false ? "none" : "all");
    if (trackMode === "none") return false;
    if (trackMode === "interior-only" && !interiorView) return false;
  }
  return true;
}

export function cspLightDistanceFade(distance, fadeAt = CSP_LIGHT_FADE_AT_DEFAULT, fadeSmooth = CSP_LIGHT_FADE_SMOOTH_DEFAULT) {
  const center = Math.max(0, Number(fadeAt) || 0), width = Math.max(0, Number(fadeSmooth) || 0), value = Math.max(0, Number(distance) || 0);
  if (width <= 1e-6) return value < center ? 1 : 0;
  const near = center - width * 0.5, far = center + width * 0.5;
  return Math.max(0, Math.min(1, (far - value) / width));
}

export function cspSpotConePacking(direction, spotDegrees, spotSharpness = 0.8) {
  const source = Array.isArray(direction) ? direction.slice(0, 3).map(Number) : [];
  const spot = Number(spotDegrees);
  if (source.length < 3 || source.some((value) => !Number.isFinite(value)) || !(spot > 0)) return Object.freeze({ enabled: false, direction: Object.freeze([0, 0, 0]), start: 0, outerCos: -1, innerCos: 1, inverseWidth: 0, halfAngle: 0, sharpness: 0 });
  const magnitude = Math.hypot(...source);
  if (!(magnitude > 1e-8)) return Object.freeze({ enabled: false, direction: Object.freeze([0, 0, 0]), start: 0, outerCos: -1, innerCos: 1, inverseWidth: 0, halfAngle: 0, sharpness: 0 });
  const halfAngle = Math.min(CSP_SPOT_HALF_ANGLE_MAX, Math.max(CSP_SPOT_ANGLE_MIN_RADIANS, spot * CSP_SPOT_DEGREES_TO_RADIANS) * 0.5);
  const sharpness = Math.min(CSP_SPOT_SHARPNESS_MAX, Math.max(0, Number(spotSharpness) || 0));
  const outerCos = Math.cos(halfAngle), innerCos = Math.cos(sharpness * halfAngle), inverseWidth = 1 / (outerCos - innerCos);
  const packedDirection = source.map((value) => value / magnitude * inverseWidth);
  return Object.freeze({ enabled: true, direction: Object.freeze(packedDirection), start: outerCos * inverseWidth, outerCos, innerCos, inverseWidth, halfAngle, sharpness });
}

export function cspSpotConeFactor(direction, toLight, spotDegrees, spotSharpness = 0.8) {
  const packed = cspSpotConePacking(direction, spotDegrees, spotSharpness);
  if (!packed.enabled) return 1;
  const source = Array.isArray(toLight) ? toLight.slice(0, 3).map(Number) : [];
  const magnitude = source.length === 3 && source.every(Number.isFinite) ? Math.hypot(...source) : 0;
  if (!(magnitude > 1e-8)) return 0;
  const packedAlignment = packed.direction.reduce((sum, value, index) => sum + value * (-source[index] / magnitude), 0);
  return Math.max(0, Math.min(1, packed.start - packedAlignment));
}

export function cspSpotEdgePacking(up, edge, sharpness) {
  const source = Array.isArray(up) ? up.slice(0, 3).map(Number) : [], offsets = Array.isArray(edge) ? edge.slice(0, 3).map(Number) : [];
  const amount = Math.max(0, Number(sharpness) || 0), magnitude = source.length === 3 && source.every(Number.isFinite) ? Math.hypot(...source) : 0;
  if (!(amount > 0) || !(magnitude > 1e-8) || offsets.length < 3 || offsets.some((value) => !Number.isFinite(value))) return Object.freeze({ enabled: false, up: Object.freeze([0, 0, 0]), offsets: Object.freeze([1, 1, 1]), sharpness: 0 });
  return Object.freeze({ enabled: true, up: Object.freeze(source.map((value) => value / magnitude * amount)), offsets: Object.freeze(offsets.map((value) => value * amount)), sharpness: amount });
}

export function cspSpotEdgeFactors(up, edge, sharpness, toLight) {
  const packed = cspSpotEdgePacking(up, edge, sharpness);
  if (!packed.enabled) return [1, 1, 1];
  const source = Array.isArray(toLight) ? toLight.slice(0, 3).map(Number) : [], magnitude = source.length === 3 && source.every(Number.isFinite) ? Math.hypot(...source) : 0;
  if (!(magnitude > 1e-8)) return [0, 0, 0];
  const projected = packed.up.reduce((sum, value, index) => sum + value * (-source[index] / magnitude), 0);
  return packed.offsets.map((value) => Math.max(0, Math.min(1, value - projected)));
}

export function cspSecondarySpotPacking(range, skip) {
  const far = Math.max(0, Number(range) || 0), fraction = Math.max(0, Math.min(1, Number(skip) || 0));
  if (!(far > 0) || !(fraction > 0)) return Object.freeze({ enabled: false, rangeInverse: 0, trimStart: 0, trimLengthInverse: 0, skip: fraction });
  return Object.freeze({ enabled: true, rangeInverse: 1 / far, trimStart: 0, trimLengthInverse: 1 / (far * fraction), skip: fraction });
}

export function cspSecondarySpotAttenuation(distance, range, skip) {
  const value = Math.max(0, Number(distance) || 0), packed = cspSecondarySpotPacking(range, skip);
  if (!packed.enabled) return 0;
  const shaped = Math.max(0, Math.min(1, value * packed.trimLengthInverse - packed.trimStart) - Math.min(1, value * packed.rangeInverse));
  return shaped * shaped;
}

const RAW_STOCK_WEATHER = Object.freeze([
  ["1_heavy_fog","Heavy Fog",2,[164,164,164,3.5],[164,164,164,4],[164,164,164,3],[164,164,164,3.5],[229.5,168.3,86.7,0],[170,170,160,0],[160,150,140,10],[140,150,155,10.8],[4.25,4.25,4.25],1,2000,0,.69,.3],
  ["2_light_fog","Light Fog",1.8,[150,150,150,5.25],[150,150,150,5],[100,130,150,3.5],[100,130,150,5.5],[200.5,110.3,46.7,9],[170,165,160,8],[130,120,105,10],[130,135,140,10],[4.15,4.15,4.15],.85,2700,0,.69,.3],
  ["3_clear","Clear",3.4,[255,138,34,1.9],[150,170,220,3.5],[30,73,167,2.8],[30,73,167,3],[229.5,140,70,40],[170,160,140,20],[124,124,124,18],[105,105,105,11],[1.8,2.37,3.42],.85,9000,.9,.5,.7],
  ["4_mid_clear","Mid Clear",3.4,[255,138,34,1.9],[150,170,220,3.5],[30,73,167,2.8],[30,73,167,3],[229.5,140,70,40],[170,160,140,20],[124,124,124,18],[105,105,105,11],[1.8,2.37,3.42],.85,9000,.9,.5,.7],
  ["5_light_clouds","Light Clouds",1.8,[255,138,34,5.5],[140,170,200,5.5],[100,133,187,6],[80,133,200,6.5],[229.5,168.3,86.7,6],[170,170,160,6],[135,120,118,11],[124,134,145,15],[1.5,2.25,3.5],.8,12000,1,.7,3.05],
  ["6_mid_clouds","Mid Clouds",2.8,[150,150,150,1.5],[150,150,150,1.5],[145,140,140,3.5],[120,150,180,3.5],[0,0,0,0],[0,0,0,0],[140,140,140,9],[140,140,140,11],[1.45,1.55,1.65],.8,12000,.4,.85,1.5],
  ["7_heavy_clouds","Heavy Clouds",2,[140,140,140,2],[140,140,140,2],[145,140,142,4],[140,140,140,4],[0,0,0,0],[0,0,0,0],[140,130,120,6],[140,140,140,8],[1.1,1.1,1.1],.8,12000,.95,.55,.2]
]);

export const STOCK_WEATHER_PRESETS = Object.freeze(RAW_STOCK_WEATHER.map((entry) => Object.freeze(createPreset(...entry))));
export const KS_EDITOR_DEFAULT_WEATHER = STOCK_WEATHER_PRESETS.find((preset) => preset.id === "5_light_clouds");

export function parseKsWeatherLighting(colorCurvesText, weatherText, source = "weather") {
  const curves = parseIni(colorCurvesText), weather = parseIni(weatherText);
  const version = scalar(curves, "HEADER", "VERSION", 0);
  if (version !== 3) throw new Error(`${source}: unsupported colorCurves.ini version ${version}`);
  const curve = (section, key) => colorCurve(vector(curves, section, key, 4));
  return Object.freeze({
    id: source,
    name: text(weather, "LAUNCHER", "NAME", source),
    source,
    version,
    angleGamma: scalar(curves, "HEADER", "ANGLE_GAMMA", 1),
    hdrOffMult: scalar(curves, "HEADER", "HDR_OFF_MULT", 1),
    horizonLow: curve("HORIZON", "LOW"), horizonHigh: curve("HORIZON", "HIGH"),
    skyLow: curve("SKY", "LOW"), skyHigh: curve("SKY", "HIGH"),
    sunLow: curve("SUN", "LOW"), sunHigh: curve("SUN", "HIGH"),
    ambientLow: curve("AMBIENT", "LOW"), ambientHigh: curve("AMBIENT", "HIGH"),
    fogColor: vector(weather, "FOG", "COLOR", 3),
    fogBlend: scalar(weather, "FOG", "BLEND", 1),
    fogDistance: Math.max(1, scalar(weather, "FOG", "DISTANCE", 1)),
    cloudCover: scalar(weather, "CLOUDS", "COVER", 0),
    cloudCutoff: scalar(weather, "CLOUDS", "CUTOFF", 0),
    cloudColor: scalar(weather, "CLOUDS", "COLOR", 0)
  });
}

export function evaluateKsLighting(preset, sunDirection) {
  const direction = normalize(sunDirection || [0, 1, 0]);
  const sunHeight = clamp(direction[1], 0, 1);
  const angleMix = Math.pow(1 - sunHeight, Math.max(0.001, preset.angleGamma));
  return {
    preset,
    sunDirection: direction,
    sunHeight,
    angleMix,
    horizonColor: mix(preset.horizonHigh, preset.horizonLow, angleMix),
    skyColor: mix(preset.skyHigh, preset.skyLow, angleMix),
    sunColor: mix(preset.sunHigh, preset.sunLow, angleMix),
    ambientColor: mix(preset.ambientHigh, preset.ambientLow, angleMix),
    fogColor: [...preset.fogColor], fogBlend: preset.fogBlend, fogDistance: preset.fogDistance
  };
}

export function sunDirectionFromAngles(headingDegrees, heightDegrees) {
  const heading = Number(headingDegrees) * Math.PI / 180, height = clamp(Number(heightDegrees), 0, 90) * Math.PI / 180, horizontal = Math.cos(height);
  return normalize([Math.sin(heading) * horizontal, Math.sin(height), Math.cos(heading) * horizontal]);
}

function createPreset(id,name,angleGamma,horizonLow,horizonHigh,skyLow,skyHigh,sunLow,sunHigh,ambientLow,ambientHigh,fogColor,fogBlend,fogDistance,cloudCover,cloudCutoff,cloudColor) {
  return {id,name,source:`Assetto Corsa SDK · ${id}`,version:3,angleGamma,hdrOffMult:1,horizonLow:colorCurve(horizonLow),horizonHigh:colorCurve(horizonHigh),skyLow:colorCurve(skyLow),skyHigh:colorCurve(skyHigh),sunLow:colorCurve(sunLow),sunHigh:colorCurve(sunHigh),ambientLow:colorCurve(ambientLow),ambientHigh:colorCurve(ambientHigh),fogColor:[...fogColor],fogBlend,fogDistance,cloudCover,cloudCutoff,cloudColor};
}

function colorCurve(value) { return value.slice(0, 3).map((component) => component * value[3] / 255); }
function mix(high, low, amount) { return high.map((value, index) => value + (low[index] - value) * amount); }
function normalize(value) { const length = Math.hypot(...value) || 1; return value.map((component) => component / length); }
function clamp(value, min, max) { return Math.max(min, Math.min(max, Number(value) || 0)); }

function parseIni(source) {
  const sections = new Map(); let current = "";
  for (const raw of String(source || "").replace(/\r/g, "").split("\n")) {
    const line = raw.split(";")[0].trim(); if (!line) continue;
    const header = /^\[([^\]]+)\]$/.exec(line); if (header) { current = header[1].trim().toUpperCase(); if (!sections.has(current)) sections.set(current, new Map()); continue; }
    const equals = line.indexOf("="); if (equals < 0 || !current) continue;
    sections.get(current).set(line.slice(0, equals).trim().toUpperCase(), line.slice(equals + 1).trim());
  }
  return sections;
}
function raw(ini, section, key) { return ini.get(section)?.get(key); }
function scalar(ini, section, key, fallback) { const value = Number.parseFloat(raw(ini, section, key)); return Number.isFinite(value) ? value : fallback; }
function vector(ini, section, key, size) { const values = String(raw(ini, section, key) || "").split(",").map(Number); if (values.length < size || values.slice(0, size).some((value) => !Number.isFinite(value))) throw new Error(`Missing or invalid [${section}] ${key}`); return values.slice(0, size); }
function text(ini, section, key, fallback) { return String(raw(ini, section, key) || fallback).trim(); }
