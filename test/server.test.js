import assert from "node:assert/strict";
import test from "node:test";
import { startApexServer } from "../src/server.js";

async function withServer(run) {
  const server = await startApexServer({ port: 0, log: false });
  try { await run(server.apexUrl); }
  finally { await new Promise((resolve) => server.close(resolve)); }
}

test("serves the desktop application and its explicitly allowed modules", async () => {
  await withServer(async (url) => {
    const page = await fetch(url);
    assert.equal(page.status, 200);
    assert.match(page.headers.get("content-security-policy"), /default-src 'self'/);
    assert.match(page.headers.get("content-security-policy"), /connect-src 'self' blob: data:/);
    assert.match(await page.text(), /Apex Editor/);
    const module = await fetch(`${url}/src/kn5.js`);
    assert.equal(module.status, 200);
    assert.match(module.headers.get("content-type"), /text\/javascript/);
    assert.match(await module.text(), /parseKn5/);
    const importer = await fetch(`${url}/src/fbx-import.js`);
    assert.equal(importer.status, 200);
    assert.match(await importer.text(), /parseFbx/);
    const surfaceAuthoring = await fetch(`${url}/src/surface-authoring.js`);
    assert.equal(surfaceAuthoring.status, 200);
    assert.match(await surfaceAuthoring.text(), /applySurfaceEdits/);
    const bottomColliderAuthoring = await fetch(`${url}/src/bottom-collider-authoring.js`);
    assert.equal(bottomColliderAuthoring.status, 200);
    assert.match(await bottomColliderAuthoring.text(), /applyBottomColliderEdits/);
    const bc7Decoder = await fetch(`${url}/src/bc7-decoder.js`);
    assert.equal(bc7Decoder.status, 200);
    assert.match(await bc7Decoder.text(), /decodeBc7Block/);
    const customEmissiveUv = await fetch(`${url}/src/custom-emissive-uv.js`);
    assert.equal(customEmissiveUv.status, 200);
    assert.match(await customEmissiveUv.text(), /customEmissiveUv/);
    const skinMetadata = await fetch(`${url}/src/skin-metadata.js`);
    assert.equal(skinMetadata.status, 200);
    assert.match(await skinMetadata.text(), /parseSkinMetadata/);
    const dynamicTrack = await fetch(`${url}/src/dynamic-track.js`);
    assert.equal(dynamicTrack.status, 200);
    assert.match(await dynamicTrack.text(), /sampleDynamicTrackObjects/);
    const sceneDiagnostics = await fetch(`${url}/src/scene-diagnostics.js`);
    assert.equal(sceneDiagnostics.status, 200);
    assert.match(await sceneDiagnostics.text(), /analyzeScene/);
    const fileIdentity = await fetch(`${url}/src/file-identity.js`);
    assert.equal(fileIdentity.status, 200);
    assert.match(await fileIdentity.text(), /createFileIdentity/);
    const tyreShader = await fetch(`${url}/src/tyre-shader.js`);
    assert.equal(tyreShader.status, 200);
    assert.match(await tyreShader.text(), /stockTyreTexel/);
    const brakeDiscShader = await fetch(`${url}/src/brake-disc-shader.js`);
    assert.equal(brakeDiscShader.status, 200);
    assert.match(await brakeDiscShader.text(), /brakeDiscGlowTarget/);
    const rimBlur = await fetch(`${url}/src/rim-blur.js`);
    assert.equal(rimBlur.status, 200);
    assert.match(await rimBlur.text(), /nativeRimBlurToggle/);
    const cockpitPreview = await fetch(`${url}/src/cockpit-preview.js`);
    assert.equal(cockpitPreview.status, 200);
    assert.match(await cockpitPreview.text(), /nativeCockpitToggle/);
    const analogInstruments = await fetch(`${url}/src/analog-instruments.js`);
    assert.equal(analogInstruments.status, 200);
    assert.match(await analogInstruments.text(), /parseAnalogInstrumentsIni/);
    const clouds = await fetch(`${url}/src/clouds.js`);
    assert.equal(clouds.status, 200);
    assert.match(await clouds.text(), /buildKsCloudBillboards/);
    const carDamage = await fetch(`${url}/src/car-damage.js`);
    assert.equal(carDamage.status, 200);
    assert.match(await carDamage.text(), /parseCarDamageIni/);
    assert.equal((await fetch(`${url}/vendor/three.module.js`)).status, 200);
    assert.equal((await fetch(`${url}/vendor/three-addons/loaders/FBXLoader.js`)).status, 200);
  });
});

test("rejects traversal, unknown source modules, and state-changing methods", async () => {
  await withServer(async (url) => {
    assert.equal((await fetch(`${url}/src/server.js`)).status, 404);
    assert.equal((await fetch(`${url}/vendor/three-addons/loaders/GLTFLoader.js`)).status, 404);
    assert.equal((await fetch(`${url}/%2e%2e/package.json`)).status, 404);
    assert.equal((await fetch(url, { method: "POST" })).status, 405);
    const head = await fetch(`${url}/app.css`, { method: "HEAD" });
    assert.equal(head.status, 200);
    assert.equal(await head.text(), "");
  });
});
