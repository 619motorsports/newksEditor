import assert from "node:assert/strict";
import { readdir, readFile } from "node:fs/promises";
import test from "node:test";
import {
  auditMaterialShaderProfiles,
  parseStockShaderContainerHeader,
  resolveMaterialRenderProfile,
  stockShaderProfile
} from "../src/shader-profiles.js";
import { assettoPath } from "./fixture-paths.js";

function container(alphaTested = false, layout = 0) {
  const bytes = new Uint8Array(26), view = new DataView(bytes.buffer);
  bytes[0] = 2; bytes[1] = Number(alphaTested); view.setUint32(2, layout, true); view.setUint32(6, 4, true);
  bytes.set([68, 88, 66, 67], 10); view.setUint32(14, 4, true); bytes.set([68, 88, 66, 67], 18); view.setUint32(22, 0, true);
  return bytes;
}

test("decodes stock shader container flags and payload lengths", () => {
  assert.deepEqual(parseStockShaderContainerHeader(container(true, 1)), {
    version: 2, alphaTested: true, vertexLayout: "skinned", vertexBytes: 4, pixelBytes: 4, geometryBytes: 0
  });
  assert.throws(() => parseStockShaderContainerHeader(container(false, 9)), /vertex layout 9/);
  assert.throws(() => parseStockShaderContainerHeader(new Uint8Array(5)), /truncated/);
});

test("applies shader-package A2C defaults before serialized KN5 blend overrides", () => {
  const packageOnly = resolveMaterialRenderProfile({ shader: "ksPerPixelAT", blendMode: 0, depthMode: 0 });
  assert.equal(packageOnly.alphaToCoverage, true);
  assert.equal(packageOnly.effectiveBlendMode, 2);
  assert.equal(packageOnly.blendSource, "shader-package");
  assert.equal(packageOnly.transparent, false);
  assert.equal(packageOnly.blendEnabled, false);
  assert.equal(packageOnly.cull, "back");

  const alphaOverride = resolveMaterialRenderProfile({ shader: "ksPerPixelAT", blendMode: 1, depthMode: 0 });
  assert.equal(alphaOverride.alphaToCoverage, false);
  assert.equal(alphaOverride.effectiveBlendMode, 1);
  assert.equal(alphaOverride.blendSource, "kn5");
  assert.equal(alphaOverride.transparent, true);
  assert.equal(alphaOverride.blendEnabled, true);

  const explicitCoverage = resolveMaterialRenderProfile({ shader: "ksPerPixel", blendMode: 2, depthMode: 0 });
  assert.equal(explicitCoverage.alphaToCoverage, true);
  assert.equal(explicitCoverage.blendSource, "kn5");

  const sortedOpaque = resolveMaterialRenderProfile({ shader: "ksPerPixel", blendMode: 0, depthMode: 0 }, { transparent: true });
  assert.equal(sortedOpaque.transparent, true);
  assert.equal(sortedOpaque.blendEnabled, false);
  assert.equal(sortedOpaque.depthWrite, false);
});

test("resolves CSP blend, depth, transparency, and authoritative cull overrides", () => {
  const profile = resolveMaterialRenderProfile(
    { shader: "ksTree", blendMode: 0, depthMode: 0 },
    { transparent: false },
    { blendMode: "ALPHA_BLEND", depthMode: "READ_ONLY", cullMode: "BACK", isTransparent: null }
  );
  assert.equal(profile.alphaToCoverage, false);
  assert.equal(profile.shadowAlphaTested, true);
  assert.equal(profile.transparent, true);
  assert.equal(profile.blend, "alpha");
  assert.equal(profile.depthTest, true);
  assert.equal(profile.depthWrite, false);
  assert.equal(profile.cull, "back");
  assert.equal(profile.cullSource, "override");

  const explicitAlpha = resolveMaterialRenderProfile(
    { shader: "extensionCustom", blendMode: 0, depthMode: 0 }, {},
    { blendMode: "ALPHA_TEST", cullMode: "DOUBLE_SIDED" }
  );
  assert.equal(explicitAlpha.alphaToCoverage, true);
  assert.equal(explicitAlpha.blendSource, "override");
  assert.equal(explicitAlpha.cull, "none");
});

test("audits package defaults, serialized modes, and cutout shadow selection", () => {
  const audit = auditMaterialShaderProfiles([
    { shader: "ksTree", blendMode: 2 },
    { shader: "ksPerPixelAT", blendMode: 0 },
    { shader: "ksPerPixel", blendMode: 1 },
    { shader: "customShader", blendMode: 0 }
  ]);
  assert.deepEqual(audit, {
    materials: 4, knownStock: 3, alphaBlend: 1, alphaToCoverage: 2, shadowCutout: 3,
    packageDefaults: 1, serializedOverrides: 2, windscreens: 0, reflectionGlass: 0, refractive: 0, unknownShaders: ["customShader"]
  });
  assert.equal(stockShaderProfile("KSTREE").name, "ksTree");
  assert.equal(stockShaderProfile("unknown"), null);
});

test("classifies stock and CSP glass paths without conflating windscreens and reflections", () => {
  const windscreen = resolveMaterialRenderProfile({ shader: "ksWindscreen", blendMode: 1 }, { transparent: true });
  assert.equal(windscreen.glassMode, "windscreen");
  assert.equal(windscreen.reflectionAlpha, false);
  assert.equal(windscreen.refractive, false);

  const glass = resolveMaterialRenderProfile({ shader: "ksPerPixelReflection", blendMode: 1 }, { transparent: true });
  assert.equal(glass.glassMode, "reflection");
  assert.equal(glass.reflectionAlpha, true);

  const broken = resolveMaterialRenderProfile({ shader: "ksBrokenGlass", blendMode: 1 }, { transparent: true });
  assert.equal(broken.glassMode, "broken glass · CSP refraction");
  assert.equal(broken.brokenGlass, true);
  assert.equal(broken.reflectionAlpha, true);
  assert.equal(broken.refractive, true);

  const configured = resolveMaterialRenderProfile(
    { shader: "ksPerPixel", blendMode: 1 }, { transparent: true },
    { shader: "smGlass", properties: new Map([["extrefraction", 0.03]]) }
  );
  assert.equal(configured.glassMode, "refractive");
  assert.equal(configured.refractive, true);
});

test("matches every non-empty installed ksEditor shader package header", async (t) => {
  const root = assettoPath("sdk/editor/system/shaders");
  let names;
  try { names = (await readdir(root)).filter((name) => name.endsWith(".shader")); }
  catch { t.skip("Installed ksEditor shader fixtures are unavailable"); return; }
  let checked = 0;
  for (const file of names) {
    const bytes = await readFile(`${root}/${file}`);
    if (!bytes.length) continue;
    const name = file.slice(0, -7), header = parseStockShaderContainerHeader(bytes), profile = stockShaderProfile(name);
    assert.ok(profile, `${name} is missing from the stock catalog`);
    assert.equal(profile.alphaTested, header.alphaTested, `${name} alpha-test flag`);
    assert.equal(profile.vertexLayout, header.vertexLayout, `${name} vertex layout`);
    checked++;
  }
  assert.equal(checked, 79);
});
