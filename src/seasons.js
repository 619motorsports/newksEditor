const clamp01 = (value) => Math.max(0, Math.min(1, Number(value) || 0));

export const CSP_SEASONAL_PROPERTIES = Object.freeze({
  autumn: "seasonautumn",
  winter: "seasonwinter",
  summer: "seasonsummer"
});

export function adjustCspSeasonColor(color, normal = [0, 1, 0], variation = 0.5, autumn = 0, winter = 0) {
  let result = color.slice(0, 3).map(clamp01);
  const autumnAmount = Math.max(0, Number(autumn) || 0);
  const winterAmount = Math.max(0, Number(winter) || 0);
  if (autumnAmount + winterAmount <= 0) return result;

  const greenMask = (result[1] * 2 - result[0] - result[2]) * 20;
  const leaf = clamp01(winterAmount * clamp01(normal[1]) + greenMask);
  if (autumnAmount > 0) {
    const spatialVariation = Number.isFinite(Number(variation)) ? Number(variation) : 0.5;
    const target = [
      clamp01(result[0] * 0.2 + result[1] * 1.6),
      clamp01(result[1] * (1.3 - spatialVariation * spatialVariation) + result[0] * 0.2),
      clamp01(result[2] - 0.4 * result[1])
    ];
    const factor = autumnAmount * leaf;
    result = result.map((component, index) => component + (target[index] - component) * factor);
  }

  const luminance = result[0] * 0.2126 + result[1] * 0.7152 + result[2] * 0.0722;
  const coldBoost = clamp01(winterAmount * 2 - 1) * 0.4;
  const winterTarget = [1, 1.1, 1.2].map((component) => clamp01(luminance * (component * 1.6 + coldBoost) * 0.85));
  const winterFactor = clamp01(winterAmount * 2) * leaf;
  return result.map((component, index) => component + (winterTarget[index] - component) * winterFactor);
}

export function analyzeCspSeasonalOverrides(evaluation) {
  const status = { affectedMeshes: 0, autumnMeshes: 0, winterMeshes: 0, legacySummerMeshes: 0, peakAutumn: 0, peakWinter: 0, peakSummer: 0 };
  for (const override of evaluation?.nodeOverrides?.values?.() || []) {
    const autumn = scalar(override.properties.get(CSP_SEASONAL_PROPERTIES.autumn));
    const winter = scalar(override.properties.get(CSP_SEASONAL_PROPERTIES.winter));
    const summer = scalar(override.properties.get(CSP_SEASONAL_PROPERTIES.summer));
    if (autumn > 0 || winter > 0) status.affectedMeshes++;
    if (autumn > 0) status.autumnMeshes++;
    if (winter > 0) status.winterMeshes++;
    if (summer > 0) status.legacySummerMeshes++;
    status.peakAutumn = Math.max(status.peakAutumn, autumn);
    status.peakWinter = Math.max(status.peakWinter, winter);
    status.peakSummer = Math.max(status.peakSummer, summer);
  }
  return status;
}

function scalar(value) {
  if (Array.isArray(value)) return Math.max(0, Number(value[0]) || 0);
  return Math.max(0, Number(value) || 0);
}
