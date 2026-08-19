import assert from "node:assert/strict";
import test from "node:test";
import { KS_EDITOR_CUBEMAP, WEBGL_CUBEMAP_FACES, portableReflectionEnvironment, reflectionBlurFromExponent, reflectionFresnel, reflectionSmoothMinimum, selectReflectionCaptureItems } from "../src/reflections.js";

test("uses the native ksEditor cubemap budget and all six orthogonal faces", () => {
  assert.deepEqual(KS_EDITOR_CUBEMAP, { size: 512, facesPerFrame: 1, nearPlane: 0.01, farPlane: 500, fovDegrees: 90, shaderSlot: 10 });
  assert.deepEqual(WEBGL_CUBEMAP_FACES.map((face) => face.direction), [[-1,0,0],[1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]]);
  for (const face of WEBGL_CUBEMAP_FACES) {
    assert.equal(Math.hypot(...face.direction), 1);
    assert.equal(Math.hypot(...face.up), 1);
    assert.equal(face.direction.reduce((sum, value, index) => sum + value * face.up[index], 0), 0);
  }
});

test("selects an explicit ksEditor-style subtree and prefers tagged showroom geometry", () => {
  const carRoot = { name: "CAR" }, showroomRoot = { name: "SHOWROOM" };
  const car = { transparent: false, item: { node: { name: "BODY" }, reflectionAncestors: [carRoot] } };
  const showroom = { transparent: false, item: { node: { name: "FLOOR" }, reflectionAncestors: [showroomRoot], workspaceAuxiliary: "reflectionEnvironment", workspaceFile: "hangar.kn5" } };
  const glass = { transparent: true, item: { node: { name: "GLASS" }, reflectionAncestors: [showroomRoot], workspaceAuxiliary: "reflectionEnvironment" } };
  assert.deepEqual(selectReflectionCaptureItems([car, showroom, glass]).items, [showroom, glass]);
  const explicit = selectReflectionCaptureItems([car, showroom, glass], { explicitRoot: carRoot });
  assert.deepEqual(explicit.items, [car]);
  assert.equal(explicit.mode, "explicit");
  assert.equal(explicit.rootName, "CAR");
});

test("keeps a lone car on fallback but captures full track scenes", () => {
  const car = { transparent: false, item: { node: { name: "BODY" }, reflectionAncestors: [] } };
  assert.equal(selectReflectionCaptureItems([car], { boundsRadius: 3 }).mode, "fallback");
  assert.deepEqual(selectReflectionCaptureItems([car], { workspaceKind: "track" }).items, [car]);
  assert.equal(selectReflectionCaptureItems([car], { workspaceKind: "track", isolated: true }).reason, "Isolated mesh preview");
});

test("uses the native smooth Fresnel cap and reflection multiplier", () => {
  assert.equal(reflectionSmoothMinimum(0, 0), 0);
  assert.ok(reflectionFresnel(0.8, 0.04, 3, 0.6, 0.5) > reflectionFresnel(0.2, 0.04, 3, 0.6, 0.5));
  assert.ok(reflectionFresnel(1, 0.1, 1, 0.6, 1) <= 0.6);
  assert.equal(reflectionFresnel(1, 0.1, 1, 0.6, 0), 0);
});

test("derives the native squared reflection mip from specular exponent", () => {
  assert.deepEqual(reflectionBlurFromExponent(255), { base: 0, mip: 0 });
  assert.deepEqual(reflectionBlurFromExponent(0), { base: 6, mip: 6 });
  const medium = reflectionBlurFromExponent(127.5);
  assert.equal(medium.base, 3);
  assert.equal(medium.mip, 1.5);
});

test("portable reflection fallback separates zenith, horizon, and ground", () => {
  const lighting = { skyColor: [3, 4, 5], horizonColor: [1, 1.5, 2], fogColor: [.5, .6, .7], fogBlend: 0 };
  const zenith = portableReflectionEnvironment([0, 1, 0], 0, lighting);
  const horizon = portableReflectionEnvironment([1, 0, 0], 0, lighting);
  const ground = portableReflectionEnvironment([0, -1, 0], 0, lighting);
  assert.ok(zenith[2] > horizon[2]);
  assert.ok(horizon[0] > ground[0]);
  assert.deepEqual(ground, [0.006, 0.0132, 0.0096]);
});
