export function customEmissiveAtlasSize(resolution, maximum = 1024, normalizedSize = 512) {
  const sourceWidth = Math.max(1, Number(resolution?.[0]) || 1);
  const sourceHeight = Math.max(1, Number(resolution?.[1]) || sourceWidth);
  const normalized = Math.max(sourceWidth, sourceHeight) <= 4;
  const scale = normalized
    ? normalizedSize / Math.max(sourceWidth, sourceHeight)
    : Math.min(1, maximum / sourceWidth, maximum / sourceHeight);
  return {
    sourceWidth,
    sourceHeight,
    width: Math.max(1, Math.round(sourceWidth * scale)),
    height: Math.max(1, Math.round(sourceHeight * scale)),
    normalized
  };
}

/** CSP uses this exponent when GAMMA_FIX is not active. Apex uses the same gamma-space material path. */
export const CSP_BOUNCEBACK_EXPONENT = 80;

function saturate(value) {
  return Math.max(0, Math.min(1, Number(value) || 0));
}

function dot3(a, b) {
  return [0, 1, 2].reduce((sum, index) => sum + (Number(a?.[index]) || 0) * (Number(b?.[index]) || 0), 0);
}

/** CPU reference for CSP's CustomEmissive_BounceBack material multiplier. */
export function customEmissiveBounceMultiplier(emissiveMap, diffuse, diffuseAlpha, rule) {
  const intensity = Number(rule?.intensity) || 0;
  const channelMask = saturate([0, 1, 2, 3].reduce((sum, index) => sum + (Number(emissiveMap?.[index]) || 0) * (Number(rule?.mask?.[index]) || 0), 0));
  const alphaMask = intensity < 0 ? 1 - saturate(diffuseAlpha) : 1;
  return [0, 1, 2].map((index) => Math.abs(intensity) * channelMask * alphaMask * 2 * Math.max(0, Number(diffuse?.[index]) || 0));
}

/** CPU reference for CSP's narrow view-to-light bounce-back lobe. */
export function customEmissiveBounceLobe(toCamera, toLight, localLight = false) {
  const cosine = saturate(dot3(toCamera, toLight) * (localLight ? -1 : 1));
  return cosine ** CSP_BOUNCEBACK_EXPONENT;
}
