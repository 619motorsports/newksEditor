export const KS_SHADOW_MAP_SIZE = 2048;
export const KS_SHADOW_SPLITS = Object.freeze([2, 12, 50]);
export const KS_SHADOW_BIASES = Object.freeze([0.000002, 0.000015, 0.0003]);
export const KS_SUN_DIRECTION = Object.freeze([0.35, 0.82, 0.42]);
export const CSP_LOCAL_SHADOW_ATLAS_SIZE = 1024;
export const CSP_LOCAL_SHADOW_CELL_SIZE = 512;
export const CSP_LOCAL_SHADOW_LIMIT = 4;
export const CSP_LOCAL_SHADOW_SAMPLES = 4;
export const CSP_LOCAL_SHADOW_DEFAULT_EXP_FACTOR = 20;
export const CSP_LOCAL_SHADOW_DEFAULT_CLIP_PLANE = 0.5;
export const CSP_LOCAL_SHADOW_DEFAULT_CLIP_SPHERE = 0.5;
export const CSP_LOCAL_SHADOW_TRACK_RANGE_LIMIT = 30;
export const CSP_LOCAL_SHADOW_CAR_RANGE_LIMIT = 120;
export const CSP_LOCAL_SHADOW_WORLD_BIAS = 0.3;
export const CSP_LOCAL_SHADOW_FILTERS = Object.freeze({
  standard: Object.freeze({ mode: 0, kernel: 7, weights: Object.freeze([0.4490798, 0.0509202]), offsets: Object.freeze([0.5380487, 2.0627797]) }),
  extra: Object.freeze({ mode: 1, kernel: 15, weights: Object.freeze([0.2496147, 0.1924633, 0.0514763, 0.0064457]), offsets: Object.freeze([0.6443417, 2.3788476, 4.2911105, 6.2166071]) }),
  headlight: Object.freeze({ mode: 2, kernel: 7, weights: Object.freeze([0.4490798, 0.0509202]), offsets: Object.freeze([0.5380487, 2.0627797]), valueAware: true })
});

export function cspLocalShadowFilter(light) {
  const headlight = /^LIGHT_HEADLIGHT(?:_|$)/i.test(String(light?.section || "")) || /SELFLIGHT_HEADLIGHTS/i.test(String(light?.source || ""));
  return headlight ? CSP_LOCAL_SHADOW_FILTERS.headlight : light?.shadowExtraBlur ? CSP_LOCAL_SHADOW_FILTERS.extra : CSP_LOCAL_SHADOW_FILTERS.standard;
}

export function shadowCasterEnabled(node, override) {
  if (override?.castShadows !== null && override?.castShadows !== undefined) return Boolean(override.castShadows);
  return node?.castShadows !== false;
}

export function computeDirectionalShadowCascades({
  eye,
  target,
  up = [0, 1, 0],
  fovRadians,
  aspect,
  near,
  far,
  sunDirection = KS_SUN_DIRECTION,
  splits = KS_SHADOW_SPLITS,
  mapSize = KS_SHADOW_MAP_SIZE,
  sceneRadius = 1
}) {
  const forward = normalize(sub(target, eye));
  let right = normalize(cross(forward, up));
  if (length(right) < 0.5) right = normalize(cross(forward, [0, 0, 1]));
  const cameraUp = normalize(cross(right, forward));
  const lightDirection = normalize(sunDirection);
  const safeNear = Math.max(0.001, Number(near) || 0.001);
  const safeFar = Math.max(safeNear + 0.001, Number(far) || splits.at(-1));
  const tanHalfFov = Math.tan(Math.max(0.001, fovRadians) / 2);
  const depthPadding = Math.max(50, Math.min(500, Math.max(1, sceneRadius) * 2));
  const cascades = [];
  let previous = safeNear;

  for (let index = 0; index < splits.length; index++) {
    const split = Math.max(previous + 0.001, Math.min(safeFar, Number(splits[index]) || safeFar));
    const corners = frustumCorners(eye, forward, right, cameraUp, previous, split, tanHalfFov, aspect);
    const center = average(corners);
    const radius = Math.max(0.01, ...corners.map((point) => length(sub(point, center))));
    const lightEye = add(center, scale(lightDirection, radius + depthPadding));
    const lightView = lookAt(lightEye, center, Math.abs(lightDirection[1]) > 0.98 ? [0, 0, 1] : [0, 1, 0]);
    const lightCorners = corners.map((point) => transformPoint(lightView, point));
    const min = [0, 1, 2].map((axis) => Math.min(...lightCorners.map((point) => point[axis])));
    const max = [0, 1, 2].map((axis) => Math.max(...lightCorners.map((point) => point[axis])));
    const extent = Math.max(max[0] - min[0], max[1] - min[1]) * 1.04;
    const texel = extent / Math.max(1, mapSize);
    const centerX = Math.round(((min[0] + max[0]) * 0.5) / texel) * texel;
    const centerY = Math.round(((min[1] + max[1]) * 0.5) / texel) * texel;
    const half = extent * 0.5;
    const lightProjection = orthographic(centerX - half, centerX + half, centerY - half, centerY + half, -max[2] - depthPadding, -min[2] + depthPadding);
    cascades.push({
      index,
      near: previous,
      far: split,
      matrix: multiply(lightProjection, lightView),
      center,
      radius,
      texelWorldSize: texel
    });
    previous = split;
  }

  return { cascades, splits: cascades.map((cascade) => cascade.far), forward, lightDirection, mapSize };
}

/**
 * Builds direction-independent cascades for a cubemap probe. Each cascade covers a
 * cube around the probe instead of one camera frustum, so the same three shadow maps
 * remain valid while the renderer visits all six cubemap faces.
 */
export function computeDirectionalProbeShadowCascades({
  eye,
  sunDirection = KS_SUN_DIRECTION,
  splits = KS_SHADOW_SPLITS,
  mapSize = KS_SHADOW_MAP_SIZE,
  sceneRadius = 1
}) {
  const center = eye.map((value) => Number(value) || 0);
  const lightDirection = normalize(sunDirection);
  const depthPadding = Math.max(50, Math.min(500, Math.max(1, sceneRadius) * 2));
  const cascades = [];
  let previous = 0.001;

  for (let index = 0; index < splits.length; index++) {
    const radius = Math.max(previous + 0.001, Number(splits[index]) || previous + 1);
    const corners = [];
    for (const x of [-radius, radius]) for (const y of [-radius, radius]) for (const z of [-radius, radius]) corners.push(add(center, [x, y, z]));
    const lightEye = add(center, scale(lightDirection, radius + depthPadding));
    const lightView = lookAt(lightEye, center, Math.abs(lightDirection[1]) > 0.98 ? [0, 0, 1] : [0, 1, 0]);
    const lightCorners = corners.map((point) => transformPoint(lightView, point));
    const min = [0, 1, 2].map((axis) => Math.min(...lightCorners.map((point) => point[axis])));
    const max = [0, 1, 2].map((axis) => Math.max(...lightCorners.map((point) => point[axis])));
    const extent = Math.max(max[0] - min[0], max[1] - min[1]) * 1.04;
    const texel = extent / Math.max(1, mapSize);
    const centerX = Math.round(((min[0] + max[0]) * 0.5) / texel) * texel;
    const centerY = Math.round(((min[1] + max[1]) * 0.5) / texel) * texel;
    const half = extent * 0.5;
    const lightProjection = orthographic(centerX - half, centerX + half, centerY - half, centerY + half, -max[2] - depthPadding, -min[2] + depthPadding);
    cascades.push({ index, near: previous, far: radius, matrix: multiply(lightProjection, lightView), center: [...center], radius, texelWorldSize: texel });
    previous = radius;
  }

  return { cascades, splits: cascades.map((cascade) => cascade.far), lightDirection, mapSize, coverage: "probe-cube" };
}

export function computeLocalLightShadow(light) {
  const esm = cspExponentialShadowParams(light);
  const position = [0, 1, 2].map((axis) => Number(light?.position?.[axis]) || 0);
  const sourceDirection = [0, 1, 2].map((axis) => Number(light?.direction?.[axis]) || 0);
  const direction = length(sourceDirection) > 1e-8 ? normalize(sourceDirection) : [0, -1, 0];
  const spot = Math.max(1, Math.min(175, Number(light?.shadowSpot) || Number(light?.spot) || 90));
  const near = esm.clipPlane;
  const far = Math.max(near + 0.001, esm.maxRange);
  const target = add(position, direction);
  const up = Math.abs(direction[1]) > 0.98 ? [0, 0, 1] : [0, 1, 0];
  return { matrix: multiply(perspective(spot * Math.PI / 180, 1, near, far), lookAt(position, target, up)), position, direction, spot, near, far, ...esm };
}

/**
 * Recreates CSP's radial exponential-shadow constants. The native shadow pass
 * writes distance/maxRange and resolves it as exp(expFactor * depth). The public
 * receiver multiplies that value by the inverse exponential for the receiver.
 */
export function cspExponentialShadowParams(light, { vehicleAttached = Boolean(light?.vehicleAttached) } = {}) {
  const sourceRange = Math.max(0.001, Number(light?.range) || 10);
  const rangeLimit = vehicleAttached ? CSP_LOCAL_SHADOW_CAR_RANGE_LIMIT : CSP_LOCAL_SHADOW_TRACK_RANGE_LIMIT;
  const authoredRange = light?.shadowRangeAuthored !== false && Number(light?.shadowRange) > 0;
  const maxRange = authoredRange ? Number(light.shadowRange) : Math.min(sourceRange, rangeLimit);
  const expFactor = Math.max(0.001, Math.min(80, Number(light?.shadowExpFactor) || CSP_LOCAL_SHADOW_DEFAULT_EXP_FACTOR));
  const spotRadians = Math.max(0, Number(light?.shadowSpot ?? light?.spot) || 0) * Math.PI / 180;
  const automaticBoost = 10 - 7.5 * clamp((spotRadians - Math.PI * 0.6) / (Math.PI * 0.3), 0, 1);
  const authoredBoost = Number(light?.shadowBoost);
  const boost = authoredBoost > 0 ? authoredBoost : automaticBoost;
  const clipSphere = Number(light?.shadowClipSphere);
  return {
    maxRange,
    rangeInv: 1 / maxRange,
    rangeInvExpFactor: expFactor / maxRange,
    expFactor,
    biasMult: expFactor * CSP_LOCAL_SHADOW_WORLD_BIAS / maxRange,
    thicknessFixMult: boost,
    thicknessFixAdd: 1 - boost,
    boost,
    clipPlane: Math.max(0.001, Number(light?.shadowClipPlane) || CSP_LOCAL_SHADOW_DEFAULT_CLIP_PLANE),
    clipSphere: Math.max(0, Number.isFinite(clipSphere) ? clipSphere : CSP_LOCAL_SHADOW_DEFAULT_CLIP_SPHERE),
    extraBlur: Boolean(light?.shadowExtraBlur)
  };
}

/** Exact public LightsFX exponential receiver equation. */
export function resolveCspExponentialShadow(occluder, receiverDistance, biasFactor, params) {
  const receiver = Math.exp(params.biasMult * biasFactor - Math.min(receiverDistance, params.maxRange) * params.rangeInvExpFactor);
  return clamp(occluder * receiver * params.thicknessFixMult + params.thicknessFixAdd, 0, 1);
}

function frustumCorners(eye, forward, right, up, near, far, tanHalfFov, aspect) {
  const result = [];
  for (const depth of [near, far]) {
    const halfHeight = depth * tanHalfFov;
    const halfWidth = halfHeight * Math.max(0.001, aspect);
    const center = add(eye, scale(forward, depth));
    for (const vertical of [-1, 1]) for (const horizontal of [-1, 1]) {
      result.push(add(add(center, scale(right, halfWidth * horizontal)), scale(up, halfHeight * vertical)));
    }
  }
  return result;
}

function orthographic(left, right, bottom, top, near, far) {
  const lr = 1 / (left - right), bt = 1 / (bottom - top), nf = 1 / (near - far);
  return [-2 * lr, 0, 0, 0, 0, -2 * bt, 0, 0, 0, 0, 2 * nf, 0, (left + right) * lr, (top + bottom) * bt, (far + near) * nf, 1];
}

function perspective(fov, aspect, near, far) {
  const f = 1 / Math.tan(fov / 2), nf = 1 / (near - far);
  return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
}

function lookAt(eye, target, up) {
  const z = normalize(sub(eye, target)), x = normalize(cross(up, z)), y = cross(z, x);
  return [x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1];
}

function multiply(a, b) {
  const output = new Array(16);
  for (let column = 0; column < 4; column++) for (let row = 0; row < 4; row++) {
    output[column * 4 + row] = 0;
    for (let index = 0; index < 4; index++) output[column * 4 + row] += a[index * 4 + row] * b[column * 4 + index];
  }
  return output;
}

function transformPoint(matrix, point) {
  const x = point[0], y = point[1], z = point[2];
  return [matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12], matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13], matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]];
}

function average(points) { return points.reduce((sum, point) => add(sum, point), [0, 0, 0]).map((value) => value / points.length); }
function add(a, b) { return a.map((value, index) => value + b[index]); }
function sub(a, b) { return a.map((value, index) => value - b[index]); }
function scale(value, factor) { return value.map((component) => component * factor); }
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function cross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
function length(value) { return Math.hypot(...value); }
function normalize(value) { const size = length(value) || 1; return value.map((component) => component / size); }
function clamp(value, minimum, maximum) { return Math.max(minimum, Math.min(maximum, value)); }
