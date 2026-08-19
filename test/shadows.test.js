import assert from "node:assert/strict";
import test from "node:test";
import { computeDirectionalProbeShadowCascades, computeDirectionalShadowCascades, computeLocalLightShadow, cspExponentialShadowParams, cspLocalShadowFilter, resolveCspExponentialShadow, CSP_LOCAL_SHADOW_ATLAS_SIZE, CSP_LOCAL_SHADOW_CELL_SIZE, CSP_LOCAL_SHADOW_DEFAULT_EXP_FACTOR, CSP_LOCAL_SHADOW_FILTERS, CSP_LOCAL_SHADOW_LIMIT, CSP_LOCAL_SHADOW_SAMPLES, KS_SHADOW_BIASES, KS_SHADOW_MAP_SIZE, KS_SHADOW_SPLITS, shadowCasterEnabled } from "../src/shadows.js";

test("uses the native ksEditor shadow-map constants", () => {
  assert.equal(KS_SHADOW_MAP_SIZE, 2048);
  assert.deepEqual(KS_SHADOW_SPLITS, [2, 12, 50]);
  assert.deepEqual(KS_SHADOW_BIASES, [0.000002, 0.000015, 0.0003]);
});

test("packs CSP radial exponential-shadow constants and automatic boost", () => {
  assert.equal(CSP_LOCAL_SHADOW_DEFAULT_EXP_FACTOR, 20);
  const narrow = cspExponentialShadowParams({ range: 100, shadowRangeAuthored: false, shadowSpot: 108 });
  assert.equal(narrow.maxRange, 30);
  assert.equal(narrow.expFactor, 20);
  assert.equal(Number(narrow.rangeInvExpFactor.toFixed(6)), 0.666667);
  assert.equal(Number(narrow.biasMult.toFixed(6)), 0.2);
  assert.equal(narrow.boost, 10);
  assert.equal(narrow.thicknessFixAdd, -9);
  const wide = cspExponentialShadowParams({ range: 200, shadowRangeAuthored: false, shadowSpot: 162, shadowBoost: 4 }, { vehicleAttached: true });
  assert.equal(wide.maxRange, 120);
  assert.equal(wide.boost, 4);
  assert.equal(wide.thicknessFixAdd, -3);
  assert.equal(cspExponentialShadowParams({ range: 10, shadowClipSphere: 0 }).clipSphere, 0);
});

test("resolves the public CSP one-sample ESM equation", () => {
  const params = cspExponentialShadowParams({ range: 30, shadowRange: 30, shadowExpFactor: 20, shadowBoost: 4, shadowSpot: 120 });
  const receiverDistance = 12;
  const matchingOccluder = Math.exp(receiverDistance * params.rangeInvExpFactor);
  assert.equal(resolveCspExponentialShadow(matchingOccluder, receiverDistance, 0, params), 1);
  assert.equal(resolveCspExponentialShadow(Math.exp(5 * params.rangeInvExpFactor), receiverDistance, 0, params), 0);
  assert.ok(resolveCspExponentialShadow(matchingOccluder, receiverDistance + 0.1, 1, params) > 0.99);
});

test("selects the native separable local-shadow Gaussian variants", () => {
  assert.deepEqual(CSP_LOCAL_SHADOW_FILTERS.standard.weights, [0.4490798, 0.0509202]);
  assert.deepEqual(CSP_LOCAL_SHADOW_FILTERS.extra.offsets, [0.6443417, 2.3788476, 4.2911105, 6.2166071]);
  assert.equal(CSP_LOCAL_SHADOW_FILTERS.standard.weights.reduce((sum, value) => sum + value * 2, 0), 1);
  assert.equal(Number(CSP_LOCAL_SHADOW_FILTERS.extra.weights.reduce((sum, value) => sum + value * 2, 0).toFixed(7)), 1);
  assert.equal(cspLocalShadowFilter({ section: "LIGHT_0" }).mode, 0);
  assert.equal(cspLocalShadowFilter({ section: "LIGHT_0", shadowExtraBlur: true }).mode, 1);
  assert.equal(cspLocalShadowFilter({ section: "LIGHT_HEADLIGHT_0", shadowExtraBlur: true }).mode, 2);
  assert.equal(cspLocalShadowFilter({ section: "LIGHT_EXTRA_APEX_SELFLIGHT_4", source: "<built-in:SelfLight_Headlights>" }).mode, 2);
});

test("builds the bounded static-editor CSP local-shadow projection", () => {
  assert.equal(CSP_LOCAL_SHADOW_ATLAS_SIZE, 1024);
  assert.equal(CSP_LOCAL_SHADOW_CELL_SIZE, 512);
  assert.equal(CSP_LOCAL_SHADOW_LIMIT, 4);
  assert.equal(CSP_LOCAL_SHADOW_SAMPLES, 4);
  const shadow = computeLocalLightShadow({ position: [2, 5, 3], direction: [0, -1, 0], spot: 120, range: 20, shadowSpot: 150, shadowRange: 45, shadowClipPlane: 0.07 });
  assert.equal(shadow.spot, 150);
  assert.equal(shadow.near, 0.07);
  assert.equal(shadow.far, 45);
  assert.ok(shadow.matrix.every(Number.isFinite));
  const point = [2, 4, 3], matrix = shadow.matrix;
  const clip = [matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12], matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13]];
  assert.ok(Math.abs(clip[0]) < 1e-9);
  assert.ok(Math.abs(clip[1]) < 1e-9);
});

test("builds three finite directional cascades at native split distances", () => {
  const result = computeDirectionalShadowCascades({ eye: [4, 3, 7], target: [0, 1, 0], fovRadians: Math.PI / 4, aspect: 16 / 9, near: 0.01, far: 100, sceneRadius: 20 });
  assert.deepEqual(result.splits, [2, 12, 50]);
  assert.equal(result.cascades.length, 3);
  assert.deepEqual(result.cascades.map((cascade) => cascade.near), [0.01, 2, 12]);
  for (const cascade of result.cascades) {
    assert.equal(cascade.matrix.length, 16);
    assert.ok(cascade.matrix.every(Number.isFinite));
    assert.ok(cascade.radius > 0);
    assert.ok(cascade.texelWorldSize > 0);
  }
});

test("builds direction-independent cubemap probe cascades around one eye", () => {
  const eye = [4, 3, 7], result = computeDirectionalProbeShadowCascades({ eye, sunDirection: [.2, .9, .3], sceneRadius: 80 });
  assert.equal(result.coverage, "probe-cube");
  assert.deepEqual(result.splits, [2, 12, 50]);
  assert.deepEqual(result.cascades.map((cascade) => cascade.near), [.001, 2, 12]);
  for (const cascade of result.cascades) {
    assert.deepEqual(cascade.center, eye);
    assert.equal(cascade.matrix.length, 16);
    assert.ok(cascade.matrix.every(Number.isFinite));
    assert.ok(cascade.texelWorldSize > 0);
    for(const x of [-cascade.radius,cascade.radius])for(const y of [-cascade.radius,cascade.radius])for(const z of [-cascade.radius,cascade.radius]){
      const point=[eye[0]+x,eye[1]+y,eye[2]+z],matrix=cascade.matrix,clip=[matrix[0]*point[0]+matrix[4]*point[1]+matrix[8]*point[2]+matrix[12],matrix[1]*point[0]+matrix[5]*point[1]+matrix[9]*point[2]+matrix[13],matrix[2]*point[0]+matrix[6]*point[1]+matrix[10]*point[2]+matrix[14]];
      assert.ok(clip.every((value)=>Math.abs(value)<=1.000001),`cascade ${cascade.index} does not contain cube corner ${clip}`);
    }
  }
});

test("honors native and CSP cast-shadow flags", () => {
  assert.equal(shadowCasterEnabled({ castShadows: true }), true);
  assert.equal(shadowCasterEnabled({ castShadows: false }), false);
  assert.equal(shadowCasterEnabled({ castShadows: false }, { castShadows: true }), true);
  assert.equal(shadowCasterEnabled({ castShadows: true }, { castShadows: false }), false);
});
