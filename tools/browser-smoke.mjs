import { createHash } from "node:crypto";
import { writeFile } from "node:fs/promises";

function option(name, fallback = "") {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const modelPath = option("model");
const layoutName = option("workspace", option("layout"));
const lodChoice = option("lod", "auto");
const skinName = option("skin");
const animationName = option("animation");
const rpmValue = option("rpm");
const compareRpmValue = option("compare-rpm");
const configPath = option("config");
const vaoPath = option("vao");
const meshName = option("mesh");
const screenshotPath = option("screenshot");
const assetsPath = option("assets");
const cspAssetsPath = option("csp-assets");
const driverPath = option("driver");
const reflectionEnvironmentPath = option("reflection-environment");
const reflectionRootName = option("reflection-root");
const driverCockpit = process.argv.includes("--driver-cockpit");
const trackCameraName = option("track-camera");
const trackCameraPosition = option("track-camera-position");
const playTrackCamera = process.argv.includes("--play-track-camera");
const port = Number(option("port", "9222"));
const appPort = Number(option("app-port", "4173"));
const isolate = !process.argv.includes("--assembled");
const colliderOverlay = process.argv.includes("--collider");
const surfaceOverlay = process.argv.includes("--surface-overlay");
const grassFx = process.argv.includes("--grass-fx");
const rainFx = process.argv.includes("--rain-fx");
const shadows = process.argv.includes("--shadows");
const reflectionCompare = process.argv.includes("--reflection-compare");
const lighting = process.argv.includes("--lighting");
const clouds = process.argv.includes("--clouds");
const seasons = process.argv.includes("--seasons");
const showHidden = process.argv.includes("--show-hidden");
const disableBptc = process.argv.includes("--disable-bptc");
const requireSharedGeometry = process.argv.includes("--require-shared-geometry");
const requireDynamicShadowRefresh = process.argv.includes("--require-dynamic-shadow-refresh");
const weatherName = option("weather");
const sunHeading = option("sun-heading");
const sunHeight = option("sun-height");
const compareSunHeight = Number(option("compare-sun-height", "10"));
const manualExposure = option("manual-exposure");
const dynamicAdvance = option("dynamic-advance");
const rainWetness = Number(option("rain-wetness", "1"));
const tyreBlur = option("tyre-blur");
const tyreDirt = option("tyre-dirt");
const compareTyreBlur = option("compare-tyre-blur");
const compareTyreDirt = option("compare-tyre-dirt");
const tyrePreview = tyreBlur !== "" || tyreDirt !== "";
const brakeTemperature = option("brake-temperature");
const brakeFrontGlow = option("brake-front-glow");
const brakeRearGlow = option("brake-rear-glow");
const brakeBlur = option("brake-blur");
const compareBrakeTemperature = option("compare-brake-temperature");
const compareBrakeBlur = option("compare-brake-blur");
const brakePreview = brakeTemperature !== "" || brakeFrontGlow !== "" || brakeRearGlow !== "" || brakeBlur !== "";
const blurredRims = process.argv.includes("--blurred-rims");
const cockpitResolution = process.argv.includes("--cockpit-resolution");
const damagePreview = process.argv.includes("--damage-preview");
const yearProgress = Number(option("year-progress", "0.5"));
const compareYearProgress = Number(option("compare-year-progress", "0"));
const verbose = process.argv.includes("--verbose");
const trace = (message) => { if (verbose) console.error(`[browser-smoke] ${message}`); };
const inputChanges = process.argv.flatMap((argument, index) => argument === "--input" ? [process.argv[index + 1]] : []).map((entry) => {
  const separator = entry.indexOf("=");
  return [entry.slice(0, separator).toUpperCase(), Number(entry.slice(separator + 1))];
});
const conditionChanges = process.argv.flatMap((argument, index) => argument === "--condition" ? [process.argv[index + 1]] : []).map((entry) => {
  const separator = entry.indexOf("=");
  return [entry.slice(0, separator).toUpperCase(), Number(entry.slice(separator + 1))];
});
const animationPositions = process.argv.flatMap((argument, index) => argument === "--animation-position" ? [Number(process.argv[index + 1])] : []);
if ((!modelPath && !layoutName) || !meshName || (layoutName && !assetsPath)) throw new Error("Usage: node tools/browser-smoke.mjs (--model FILE.kn5 | --workspace MANIFEST.ini --assets FOLDER) --mesh NAME [--assembled --show-hidden] [--rpm 1000 --compare-rpm 6000] [--dynamic-advance 2] [--require-shared-geometry] [--require-dynamic-shadow-refresh] [--csp-assets assettocorsa/extension/textures --clouds] [--reflection-environment SHOWROOM.kn5 --reflection-root NODE] [--seasons --year-progress 0.5 --compare-year-progress 0] [--vao FILE.vao-patch] [--lighting --weather PRESET --sun-heading 40 --sun-height 55 --compare-sun-height 10 --manual-exposure 0.35] [--shadows] [--reflection-compare] [--surface-overlay] [--grass-fx] [--rain-fx --rain-wetness 1] [--tyre-blur 0 --tyre-dirt 0 --compare-tyre-blur 1 --compare-tyre-dirt 1] [--brake-temperature 160 --brake-front-glow 64 --brake-rear-glow 24 --brake-blur 0 --compare-brake-temperature 10 --compare-brake-blur 1] [--blurred-rims] [--cockpit-resolution] [--damage-preview] [--track-camera LABEL --track-camera-position 0.5 --play-track-camera] [--driver FILE.kn5 --driver-cockpit] [--skin NAME] [--animation NAME --animation-position 0.5] [--lod auto|INDEX] [--config FILE.ini] [--input NAME=VALUE] [--port 9222] [--app-port 4173]");
if (requireDynamicShadowRefresh && dynamicAdvance === "") throw new Error("--require-dynamic-shadow-refresh requires --dynamic-advance");

const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = pages.find((item) => item.type === "page");
if (!page) throw new Error(`No Chrome page target on port ${port}`);
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let nextId = 0, captureErrors = false;
const pending = new Map(), errors = [];
socket.onmessage = ({ data }) => {
  const message = JSON.parse(data);
  if (message.id) {
    const waiter = pending.get(message.id); pending.delete(message.id);
    if (message.error) waiter.reject(new Error(message.error.message)); else waiter.resolve(message.result);
  } else if (captureErrors && message.method === "Runtime.exceptionThrown") errors.push(message.params.exceptionDetails.exception?.description || message.params.exceptionDetails.text);
  else if (captureErrors && message.method === "Log.entryAdded" && message.params.entry.level === "error") errors.push(message.params.entry.text);
};
function command(method, params = {}) {
  const id = ++nextId;
  socket.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}
async function evaluate(expression) {
  const result = await command("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.text);
  return result.result.value;
}
async function waitFor(expression, timeout = 30000) {
  const started = Date.now();
  while (Date.now() - started < timeout) {
    if (await evaluate(expression)) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out: ${expression}${errors.length ? `\nBrowser errors:\n${errors.join("\n")}` : ""}`);
}
async function setFile(selector, path) {
  const { root } = await command("DOM.getDocument", { depth: -1 });
  const { nodeId } = await command("DOM.querySelector", { nodeId: root.nodeId, selector });
  await command("DOM.setFileInputFiles", { nodeId, files: Array.isArray(path) ? path : [path] });
  await evaluate(`document.querySelector(${JSON.stringify(selector)}).dispatchEvent(new Event('change',{bubbles:true}))`);
}
async function screenshotState(path = "") {
  await evaluate(`new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))`);
  const clip = await evaluate(`(()=>{const r=document.querySelector('#view').getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,scale:1}})()`);
  const { data } = await command("Page.captureScreenshot", { format: "png", fromSurface: true, clip });
  const bytes = Buffer.from(data, "base64");
  if (path) await writeFile(path, bytes);
  return {
    hash: createHash("sha256").update(data).digest("hex").slice(0, 16),
    bytes: bytes.length,
    glError: await evaluate(`document.querySelector('#view').getContext('webgl2').getError()`)
  };
}
async function setInput(name, value) {
  return evaluate(`(()=>{const e=window.__apexInputs?.[${JSON.stringify(name)}];if(!e)return false;e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));return true;})()`);
}
async function setCondition(name, value) {
  return evaluate(`(()=>{const e=window.__apexConditions?.[${JSON.stringify(name)}];if(!e)return false;e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));return true;})()`);
}

await command("Runtime.enable"); await command("Log.enable"); await command("Page.enable"); await command("Network.enable"); await command("Network.setCacheDisabled", { cacheDisabled: true });
if(disableBptc)await command("Page.addScriptToEvaluateOnNewDocument",{source:`(()=>{const original=WebGL2RenderingContext.prototype.getExtension;WebGL2RenderingContext.prototype.getExtension=function(name){return name==="EXT_texture_compression_bptc"?null:original.call(this,name);};})();`});
trace("connected");
await command("Page.navigate", { url: `http://127.0.0.1:${appPort}` });
captureErrors = true;
await waitFor(`document.readyState==='complete'`);
await waitFor(`window.__apexAppReady===true`);
trace("app loaded");
if (assetsPath) {
  await setFile("#asset-folder", assetsPath);
  await waitFor(`document.querySelector('#asset-folder')?.files.length>0`);
  trace("asset folder selected");
}
if(cspAssetsPath){await setFile("#csp-texture-folder",cspAssetsPath);await waitFor(`document.querySelector('#csp-texture-folder')?.files.length>0`);trace("shared CSP texture folder selected");}
if (layoutName) {
  await waitFor(`[...document.querySelector('#layout-select').options].some(option=>option.textContent.endsWith(${JSON.stringify(layoutName)})||option.value.endsWith(${JSON.stringify(layoutName)}))`);
  const opened = await evaluate(`(()=>{const select=document.querySelector('#layout-select'),option=[...select.options].find(option=>option.textContent.endsWith(${JSON.stringify(layoutName)})||option.value.endsWith(${JSON.stringify(layoutName)}));if(!option)return false;select.value=option.value;document.querySelector('#open-layout').click();return true;})()`);
  if (!opened) throw new Error(`Layout manifest was not discovered: ${layoutName}`);
  await waitFor(`document.querySelector('#status').textContent.includes('KN5 files')||document.querySelector('#status').textContent.includes('KN5 v')`, 120000);
  trace("workspace loaded");
  if (driverPath) {
    await setFile("#driver-model-file", driverPath);
    await waitFor(`document.querySelector('#status').textContent.includes('5 KN5 files')`, 120000);
    trace("shared driver loaded");
    if(driverCockpit){const enabled=await evaluate(`(()=>{const button=document.querySelector('#driver-cockpit-view');if(!button||button.hidden||button.disabled)return false;if(!button.classList.contains('active'))button.click();return true;})()`);if(!enabled)throw new Error("Cockpit driver mode was not available");await waitFor(`window.__apexRenderer?.sceneStatus.driverCockpitMode===true`);trace("cockpit driver mode enabled");}
  }
  if (lodChoice !== "auto") {
    const selectedLod = await evaluate(`(()=>{const select=document.querySelector('#lod-preview'),option=[...select.options].find(option=>option.value===${JSON.stringify(lodChoice)});if(!option)return false;select.value=option.value;select.dispatchEvent(new Event('change',{bubbles:true}));return true;})()`);
    if (!selectedLod) throw new Error(`Car LOD was not available: ${lodChoice}`);
  }
} else {
  await setFile("#file", modelPath);
  await waitFor(`document.querySelector('#status').textContent.includes('KN5 v')`);
}
if(reflectionEnvironmentPath){await setFile("#reflection-environment-file",reflectionEnvironmentPath);await waitFor(`window.__apexRenderer?.sceneStatus.reflections.selectionMode==='environment'`,120000);trace("reflection environment loaded");}
if(reflectionRootName){const selectedRoot=await evaluate(`(()=>{const select=document.querySelector('#reflection-root'),option=[...select.options].find(option=>option.textContent===${JSON.stringify(reflectionRootName)}||option.textContent.endsWith(${JSON.stringify(reflectionRootName)}));if(!option)return false;select.value=option.value;select.dispatchEvent(new Event('change',{bubbles:true}));return true;})()`);if(!selectedRoot)throw new Error(`Reflection root was not available: ${reflectionRootName}`);await waitFor(`window.__apexRenderer?.sceneStatus.reflections.selectionMode==='explicit'`);trace("explicit reflection root selected");}
if (colliderOverlay) {
  const enabled = await evaluate(`(()=>{const button=document.querySelector('#collider-overlay');if(!button||button.hidden||button.disabled)return false;if(!button.classList.contains('active'))button.click();return true;})()`);
  if (!enabled) throw new Error("Collider overlay was not available");
  await waitFor(`window.__apexRenderer?.sceneStatus.colliderVisible===true`);
}
if (skinName) {
  trace(`selecting skin ${skinName}`);
  const selectedSkin = await evaluate(`(()=>{const select=document.querySelector('#skin-select'),option=[...select.options].find(option=>option.value===${JSON.stringify(skinName)});if(!option)return false;select.value=option.value;select.dispatchEvent(new Event('change',{bubbles:true}));return true;})()`);
  if (!selectedSkin) throw new Error(`Skin was not discovered: ${skinName}`);
  await waitFor(`window.__apexRenderer?.skinTextureStatus.name===${JSON.stringify(skinName)}&&window.__apexRenderer?.skinTextureStatus.pending===0`, 120000);
  trace("skin loaded");
}
if (animationName) {
  trace(`selecting animation ${animationName}`);
  const selectedAnimation = await evaluate(`(()=>{const select=document.querySelector('#animation-select'),option=[...select.options].find(option=>option.textContent.endsWith(${JSON.stringify(animationName)})||option.value.endsWith(${JSON.stringify(animationName)}));if(!option)return false;select.value=option.value;select.dispatchEvent(new Event('change',{bubbles:true}));return true;})()`);
  if (!selectedAnimation) throw new Error(`Animation was not discovered: ${animationName}`);
  await waitFor(`window.__apexRenderer?.animationStatus.name.endsWith(${JSON.stringify(animationName)})`, 120000);
  trace("animation loaded");
}
if(rpmValue!==""){const selectedRpm=await evaluate(`(()=>{const input=document.querySelector('#analog-rpm');if(!input||input.closest('label')?.hidden)return false;input.value=${JSON.stringify(rpmValue)};input.dispatchEvent(new Event('input',{bubbles:true}));return true;})()`);if(!selectedRpm)throw new Error("Analog RPM preview was not available");await waitFor(`window.__apexRenderer?.analogInstrumentStatus?.rpm===${JSON.stringify(Number(rpmValue))}&&window.__apexRenderer?.analogInstrumentStatus?.applied===true`);trace("analog RPM preview selected");}
await waitFor(`window.__apexRenderer?.textureStatus.pending===0`);
if(requireSharedGeometry){await waitFor(`window.__apexRenderer?.sceneStatus.sharedGeometryInstances>0`);const sharing=await evaluate(`({renderItems:window.__apexRenderer.sceneStatus.total,gpuGeometries:window.__apexRenderer.sceneStatus.gpuGeometries,sharedGeometryInstances:window.__apexRenderer.sceneStatus.sharedGeometryInstances})`);if(sharing.gpuGeometries>=sharing.renderItems)throw new Error(`GPU geometry was not shared: ${JSON.stringify(sharing)}`);trace(`shared GPU geometry ${JSON.stringify(sharing)}`);}
if (configPath) {
  await setFile("#csp-file", configPath);
  await waitFor(`document.querySelector('#pipeline').textContent.includes('CSP')`);
  await waitFor(`window.__apexRenderer?.externalTextureStatus.pending===0`);
}
if(vaoPath){await setFile("#vao-file",vaoPath);await waitFor(`window.__apexRenderer?.vaoStatus?.matchedMeshes>0`,120000);trace("VAO patch bound");}
if(seasons){const available=await evaluate(`(()=>{const e=document.querySelector('#year-progress');if(!e||e.closest('label')?.hidden)return false;e.value=${JSON.stringify(yearProgress)};e.dispatchEvent(new Event('input',{bubbles:true}));return true;})()`);if(!available)throw new Error("YEAR_PROGRESS seasonal control was not available");await waitFor(`window.__apexRenderer?.seasonalStatus?.yearProgress===${JSON.stringify(yearProgress)}`);trace("seasonal material preview selected");}
if (weatherName) {
  const selectedWeather = await evaluate(`(()=>{const select=document.querySelector('#weather-select'),option=[...select.options].find(option=>option.value===${JSON.stringify(weatherName)}||option.textContent===${JSON.stringify(weatherName)});if(!option)return false;select.value=option.value;select.dispatchEvent(new Event('change',{bubbles:true}));return true;})()`);
  if (!selectedWeather) throw new Error(`Weather preset was not available: ${weatherName}`);
}
for (const [selector, value] of [["#sun-heading", sunHeading], ["#sun-height", sunHeight]]) {
  if (value === "") continue;
  await evaluate(`(()=>{const input=document.querySelector(${JSON.stringify(selector)});input.value=${JSON.stringify(value)};input.dispatchEvent(new Event('input',{bubbles:true}));})()`);
}
if (manualExposure !== "") {
  await evaluate(`(()=>{const toggle=document.querySelector('#auto-exposure');if(toggle.classList.contains('active'))toggle.click();const input=document.querySelector('#exposure');input.value=${JSON.stringify(manualExposure)};input.dispatchEvent(new Event('input',{bubbles:true}));})()`);
  await waitFor(`window.__apexRenderer?.lightingStatus?.autoExposure===false`);
}
if(grassFx){await waitFor(`window.__apexRenderer?.grassStatus.instanceCount>0&&window.__apexRenderer?.grassStatus.weatherLit===true${cspAssetsPath?"&&window.__apexRenderer?.grassStatus.atlasReady===true":""}`,120000);trace("GrassFX instances generated");}
if(rainFx){await waitFor(`window.__apexRenderer?.rainStatus.matchedMeshes>0`,120000);await evaluate(`(()=>{const e=document.querySelector('#rain-wetness');e.value=${JSON.stringify(rainWetness)};e.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.rainStatus.wetness===${JSON.stringify(rainWetness)}`);if(grassFx&&rainWetness<0)await waitFor(`window.__apexRenderer?.grassStatus.snowAmount===${JSON.stringify(Math.min(1,-rainWetness))}`);trace("RainFX preview enabled");}
if(tyrePreview){await waitFor(`window.__apexRenderer?.tyreStatus.completeMaterials>0&&window.__apexRenderer?.tyreStatus.exactMeshes>0`,120000);await evaluate(`(()=>{for(const [selector,value] of [["#tyre-blur",${JSON.stringify(tyreBlur)}],["#tyre-dirt",${JSON.stringify(tyreDirt)}]]){if(value==="")continue;const e=document.querySelector(selector);e.value=value;e.dispatchEvent(new Event('input',{bubbles:true}));}})()`);await waitFor(`window.__apexRenderer?.tyreStatus.blurLevel===${JSON.stringify(Number(tyreBlur || 0))}&&window.__apexRenderer?.tyreStatus.dirtyLevel===${JSON.stringify(Number(tyreDirt || 0))}`);trace("stock tyre state selected");}
if(brakePreview){await waitFor(`window.__apexRenderer?.brakeDiscStatus.completeMaterials>0&&window.__apexRenderer?.brakeDiscStatus.exactMeshes>0`,120000);await evaluate(`(()=>{for(const [selector,value] of [["#brake-temperature",${JSON.stringify(brakeTemperature)}],["#brake-front-glow",${JSON.stringify(brakeFrontGlow)}],["#brake-rear-glow",${JSON.stringify(brakeRearGlow)}],["#brake-blur",${JSON.stringify(brakeBlur)}]]){if(value==="")continue;const e=document.querySelector(selector);e.value=value;e.dispatchEvent(new Event('input',{bubbles:true}));}})()`);await waitFor(`window.__apexRenderer?.brakeDiscStatus.temperature===${JSON.stringify(Number(brakeTemperature || 10))}&&window.__apexRenderer?.brakeDiscStatus.blurLevel===${JSON.stringify(Number(brakeBlur || 0))}`);trace("stock brake-disc state selected");}
if(blurredRims){await waitFor(`window.__apexRenderer?.rimBlurStatus.available&&window.__apexRenderer?.rimBlurStatus.blurredNodes>0`,120000);const enabled=await evaluate(`window.__apexRenderer.setBlurredRimsVisible(true)`);if(!enabled)throw new Error("Native blurred-rim preview was not available");await waitFor(`window.__apexRenderer?.rimBlurStatus.mode==='blurred'`);trace("native blurred rims selected");}
if(cockpitResolution){await waitFor(`window.__apexRenderer?.cockpitPreviewStatus.available`,120000);const enabled=await evaluate(`window.__apexRenderer.setCockpitHighVisible(false)`);if(!enabled)throw new Error("Native cockpit-resolution preview was not available");await waitFor(`window.__apexRenderer?.cockpitPreviewStatus.selection==='low'`);trace("native low-resolution cockpit selected");}
if(damagePreview){await waitFor(`window.__apexRenderer?.damagePreviewStatus.available&&window.__apexRenderer?.damagePreviewStatus.exactMaterials>0`,120000);const enabled=await evaluate(`window.__apexRenderer.setDamageBrokenVisible(true)`);if(!enabled)throw new Error("Native damage preview was not available");await waitFor(`window.__apexRenderer?.damagePreviewStatus.selection==='broken'`);trace("native broken damage state selected");}
const selected = await evaluate(`(()=>{
  window.__apexInputs=Object.fromEntries([...document.querySelectorAll('[data-input]')].map(e=>[e.dataset.input,e]));
  window.__apexConditions=Object.fromEntries([...document.querySelectorAll('[data-condition]')].map(e=>[e.dataset.condition,e]));
  const search=document.querySelector('#search');search.value=${JSON.stringify(meshName)};search.dispatchEvent(new Event('input',{bubbles:true}));
  const row=[...document.querySelectorAll('.node-row.mesh,.node-row.skinnedMesh')].find(e=>e.querySelector('.node-name')?.textContent===${JSON.stringify(meshName)});
  row?.click();if(${JSON.stringify(isolate)}&&!document.querySelector('#isolate').classList.contains('active'))document.querySelector('#isolate').click();document.querySelector('#frame').click();
  return {found:Boolean(row),inspector:document.querySelector('#inspector').innerText.slice(0,1000)};
})()`);
if(surfaceOverlay){const enabled=await evaluate(`(()=>{const button=document.querySelector('#surface-overlay');if(!button||button.hidden||button.disabled)return false;if(!button.classList.contains('active'))button.click();return true;})()`);if(!enabled)throw new Error("Surface physics overlay was not available");await waitFor(`window.__apexRenderer?.sceneStatus.surfaceOverlay===true`);trace("surface physics overlay enabled");}
if(trackCameraName){const selectedCamera=await evaluate(`(()=>{const select=document.querySelector('#track-camera-select'),option=[...select.options].find(option=>option.textContent===${JSON.stringify(trackCameraName)}||option.textContent.endsWith(${JSON.stringify(trackCameraName)}));if(!option)return false;select.value=option.value;select.dispatchEvent(new Event('change',{bubbles:true}));return true;})()`);if(!selectedCamera)throw new Error(`Track camera was not discovered: ${trackCameraName}`);await waitFor(`window.__apexRenderer?.sceneStatus.trackCamera!==null`);if(trackCameraPosition!==""){const moved=await evaluate(`(()=>{const input=document.querySelector('#track-camera-position');if(!input||input.closest('label')?.hidden)return false;input.value=${JSON.stringify(trackCameraPosition)};input.dispatchEvent(new Event('input',{bubbles:true}));return true;})()`);if(!moved)throw new Error("Selected track camera has no spline position control");await waitFor(`window.__apexRenderer?.sceneStatus.trackCamera?.splinePosition===${JSON.stringify(Number(trackCameraPosition))}`);}if(playTrackCamera){const started=await evaluate(`(()=>{const button=document.querySelector('#track-camera-play');if(!button||button.hidden||button.disabled)return false;button.click();return button.classList.contains('active');})()`);if(!started)throw new Error("Track camera spline playback was not available");const initial=trackCameraPosition===""?0:Number(trackCameraPosition);await waitFor(`window.__apexRenderer?.sceneStatus.trackCamera?.splinePosition>${JSON.stringify(initial+.005)}`);}trace("track camera selected");}
if(lighting)await waitFor(`window.__apexRenderer?.lightingStatus?.name&&window.__apexRenderer?.lightingStatus?.height===Number(document.querySelector('#sun-height').value)`,120000);
if(clouds)await waitFor(`window.__apexRenderer?.cloudStatus?.readyTextures===7&&window.__apexRenderer?.cloudStatus?.drawn>0`,120000);
if(shadows)await waitFor(`window.__apexRenderer?.shadowStatus.enabled&&window.__apexRenderer?.shadowStatus.casters>0`,120000);
if(grassFx&&shadows)await waitFor(`window.__apexRenderer?.shadowStatus.grassInstances>0&&window.__apexRenderer?.grassStatus.castsShadows===true&&window.__apexRenderer?.grassStatus.receivesShadows===true`,120000);
if(reflectionCompare&&shadows)await waitFor(`window.__apexRenderer?.sceneStatus.reflections.directionalShadows===true&&window.__apexRenderer?.sceneStatus.reflections.shadowCasters>0`,120000);
if(reflectionCompare&&grassFx)await waitFor(`window.__apexRenderer?.sceneStatus.reflections.selectionMode!=='scene'||(window.__apexRenderer?.sceneStatus.reflections.grassFx===true&&window.__apexRenderer?.sceneStatus.reflections.grassInstances>0${shadows?"&&window.__apexRenderer?.sceneStatus.reflections.grassCastsShadows===true&&window.__apexRenderer?.sceneStatus.reflections.grassReceivesShadows===true":""})`,120000);
if(dynamicAdvance!==""){
  await waitFor(`window.__apexRenderer?.dynamicTrackStatus.active===true`,120000);
  let localShadowUpdatesBefore=null;
  if(requireDynamicShadowRefresh){
    await waitFor(`window.__apexRenderer?.dynamicTrackStatus.movingInstances>0&&window.__apexRenderer?.grassStatus.localShadowLights>0&&window.__apexRenderer?.grassStatus.localShadowCasters>0&&window.__apexRenderer?.grassStatus.localShadowUpdates>0`,120000);
    localShadowUpdatesBefore=await evaluate(`window.__apexRenderer.grassStatus.localShadowUpdates`);
  }
  await evaluate(`window.__apexRenderer.advanceDynamicTrack(${JSON.stringify(Number(dynamicAdvance))})`);
  if(requireDynamicShadowRefresh){
    const localShadowUpdatesAfter=await evaluate(`window.__apexRenderer.grassStatus.localShadowUpdates`);
    if(!(localShadowUpdatesAfter>localShadowUpdatesBefore))throw new Error(`Dynamic caster movement did not refresh the local-shadow atlas (${localShadowUpdatesBefore} -> ${localShadowUpdatesAfter})`);
    trace(`local-shadow atlas refreshed ${localShadowUpdatesBefore} -> ${localShadowUpdatesAfter}`);
  }
  trace(`dynamic track advanced ${dynamicAdvance} seconds`);
}
const sceneBeforeShowHidden = await evaluate(`window.__apexRenderer?.sceneStatus`);
const states = { [surfaceOverlay ? "surface-overlay" : grassFx ? "grass-fx" : rainFx ? "rain-fx" : damagePreview ? "damage=broken" : cockpitResolution ? "cockpit=low" : blurredRims ? "rims=blurred" : brakePreview ? `brake=${Number(brakeTemperature || 10)},${Number(brakeBlur || 0)}` : tyrePreview ? `tyre=${Number(tyreBlur || 0)},${Number(tyreDirt || 0)}` : seasons ? `year-progress=${yearProgress}` : vaoPath ? "vao" : reflectionCompare ? "reflections" : clouds ? "clouds" : lighting ? "lighting" : shadows ? "shadows" : animationName ? "animation=0" : rpmValue!=="" ? `rpm=${rpmValue}` : dynamicAdvance!=="" ? `dynamic=${dynamicAdvance}` : "off"]: await screenshotState(screenshotPath) };
if(showHidden){const enabled=await evaluate(`(()=>{const button=document.querySelector('#show-hidden');if(!button||button.disabled)return false;if(!button.classList.contains('active'))button.click();return true;})()`);if(!enabled)throw new Error("Show hidden mode was not available");await waitFor(`window.__apexRenderer?.sceneStatus.showHidden===true`);const sceneAfterShowHidden=await evaluate(`window.__apexRenderer?.sceneStatus`);if(sceneAfterShowHidden.gameVisible!==sceneBeforeShowHidden.gameVisible||sceneAfterShowHidden.gameHidden!==sceneBeforeShowHidden.gameHidden)throw new Error("Show hidden changed the parsed KN5 visibility counts");if(sceneAfterShowHidden.previewVisible<sceneBeforeShowHidden.previewVisible)throw new Error("Show hidden removed preview meshes");states["hidden-shown"]=await screenshotState();trace("hidden KN5 meshes shown");}
if(seasons){await evaluate(`(()=>{const e=document.querySelector('#year-progress');e.value=${JSON.stringify(compareYearProgress)};e.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.seasonalStatus?.yearProgress===${JSON.stringify(compareYearProgress)}`);states[`year-progress=${compareYearProgress}`]=await screenshotState();}
if(shadows){await evaluate(`document.querySelector('#sun-shadows').click()`);await waitFor(reflectionCompare?`window.__apexRenderer?.shadowStatus.enabled===false&&window.__apexRenderer?.sceneStatus.reflections.directionalShadows===false`:`window.__apexRenderer?.shadowStatus.enabled===false`);states["shadows-off"]=await screenshotState();await evaluate(`document.querySelector('#sun-shadows').click()`);await waitFor(reflectionCompare?`window.__apexRenderer?.shadowStatus.enabled===true&&window.__apexRenderer?.sceneStatus.reflections.directionalShadows===true`:`window.__apexRenderer?.shadowStatus.enabled===true`);}
if(reflectionCompare){await evaluate(`document.querySelector('#scene-reflections').click()`);await waitFor(`window.__apexRenderer?.sceneStatus.reflections.enabled===false`);states["reflections-off"]=await screenshotState();await evaluate(`document.querySelector('#scene-reflections').click()`);await waitFor(`window.__apexRenderer?.sceneStatus.reflections.enabled===true`);}
if(grassFx){await evaluate(`document.querySelector('#grass-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.grassStatus.visible===false`);states["grass-fx-off"]=await screenshotState();await evaluate(`document.querySelector('#grass-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.grassStatus.visible===true`);}
if(rainFx){await evaluate(`document.querySelector('#rain-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.rainStatus.visible===false`);states["rain-fx-off"]=await screenshotState();await evaluate(`document.querySelector('#rain-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.rainStatus.visible===true`);}
if(tyrePreview&&(compareTyreBlur!==""||compareTyreDirt!=="")){const nextBlur=Number(compareTyreBlur===""?tyreBlur||0:compareTyreBlur),nextDirt=Number(compareTyreDirt===""?tyreDirt||0:compareTyreDirt);await evaluate(`(()=>{for(const [selector,value] of [["#tyre-blur",${JSON.stringify(nextBlur)}],["#tyre-dirt",${JSON.stringify(nextDirt)}]]){const e=document.querySelector(selector);e.value=String(value);e.dispatchEvent(new Event('input',{bubbles:true}));}})()`);await waitFor(`window.__apexRenderer?.tyreStatus.blurLevel===${JSON.stringify(nextBlur)}&&window.__apexRenderer?.tyreStatus.dirtyLevel===${JSON.stringify(nextDirt)}`);states[`tyre=${nextBlur},${nextDirt}`]=await screenshotState();}
if(brakePreview&&(compareBrakeTemperature!==""||compareBrakeBlur!=="")){const nextTemperature=Number(compareBrakeTemperature===""?brakeTemperature||10:compareBrakeTemperature),nextBlur=Number(compareBrakeBlur===""?brakeBlur||0:compareBrakeBlur);await evaluate(`(()=>{for(const [selector,value] of [["#brake-temperature",${JSON.stringify(nextTemperature)}],["#brake-blur",${JSON.stringify(nextBlur)}]]){const e=document.querySelector(selector);e.value=String(value);e.dispatchEvent(new Event('input',{bubbles:true}));}})()`);await waitFor(`window.__apexRenderer?.brakeDiscStatus.temperature===${JSON.stringify(nextTemperature)}&&window.__apexRenderer?.brakeDiscStatus.blurLevel===${JSON.stringify(nextBlur)}`);states[`brake=${nextTemperature},${nextBlur}`]=await screenshotState();}
if(blurredRims){await evaluate(`window.__apexRenderer.setBlurredRimsVisible(false)`);await waitFor(`window.__apexRenderer?.rimBlurStatus.mode==='regular'`);states["rims=regular"]=await screenshotState();await evaluate(`window.__apexRenderer.setBlurredRimsVisible(true)`);await waitFor(`window.__apexRenderer?.rimBlurStatus.mode==='blurred'`);}
if(cockpitResolution){await evaluate(`window.__apexRenderer.setCockpitHighVisible(true)`);await waitFor(`window.__apexRenderer?.cockpitPreviewStatus.selection==='high'`);states["cockpit=high"]=await screenshotState();await evaluate(`window.__apexRenderer.setCockpitHighVisible(false)`);await waitFor(`window.__apexRenderer?.cockpitPreviewStatus.selection==='low'`);}
if(damagePreview){await evaluate(`window.__apexRenderer.setDamageBrokenVisible(false)`);await waitFor(`window.__apexRenderer?.damagePreviewStatus.selection==='intact'`);states["damage=intact"]=await screenshotState();await evaluate(`window.__apexRenderer.setDamageBrokenVisible(true)`);await waitFor(`window.__apexRenderer?.damagePreviewStatus.selection==='broken'`);}
if(vaoPath){await evaluate(`document.querySelector('#vao-toggle').click()`);await waitFor(`window.__apexRenderer?.vaoStatus.enabled===false`);states["vao-off"]=await screenshotState();await evaluate(`document.querySelector('#vao-toggle').click()`);await waitFor(`window.__apexRenderer?.vaoStatus.enabled===true`);}
if(lighting){const originalSunHeight=await evaluate(`document.querySelector('#sun-height').value`);await evaluate(`(()=>{const input=document.querySelector('#sun-height');input.value=${JSON.stringify(compareSunHeight)};input.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.lightingStatus?.height===${JSON.stringify(compareSunHeight)}`);states[`sun-height=${compareSunHeight}`]=await screenshotState();await evaluate(`(()=>{const input=document.querySelector('#sun-height');input.value=${JSON.stringify(originalSunHeight)};input.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.lightingStatus?.height===${JSON.stringify(Number(originalSunHeight))}`);}
if(compareRpmValue!==""){await evaluate(`(()=>{const input=document.querySelector('#analog-rpm');input.value=${JSON.stringify(compareRpmValue)};input.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.analogInstrumentStatus?.rpm===${JSON.stringify(Number(compareRpmValue))}`);states[`rpm=${compareRpmValue}`]=await screenshotState();}
if(clouds){const originalWeather=await evaluate(`document.querySelector('#weather-select').value`);await evaluate(`(()=>{const select=document.querySelector('#weather-select');select.value='3_clear';select.dispatchEvent(new Event('change',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.cloudStatus?.configured===0&&window.__apexRenderer?.cloudStatus?.drawn===0`);states["clouds-clear"]=await screenshotState();await evaluate(`(()=>{const select=document.querySelector('#weather-select');select.value=${JSON.stringify(originalWeather)};select.dispatchEvent(new Event('change',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.cloudStatus?.drawn>0`);}
for (const value of animationPositions) {
  await evaluate(`(()=>{const e=document.querySelector('#animation-position');e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));})()`);
  states[`animation=${value}`] = await screenshotState();
}
for (const [name, value] of inputChanges) {
  const present = await setInput(name, value);
  states[`${name}=${value}`] = present ? await screenshotState() : null;
  if (present) await setInput(name, 0);
}
for (const [name, value] of conditionChanges) {
  const present = await setCondition(name, value);
  if (present && grassFx && ["SMOOTH_SUN_B", "NIGHT_SMOOTH"].includes(name) && value > 0) {
    trace(await evaluate(`JSON.stringify({lights:window.__apexCsp?.lights?.length??0,active:window.__apexCsp?.lights?.filter(light=>Math.max(...light.color.map(Math.abs))>1e-6).length??0,shadowed:window.__apexCsp?.lights?.filter(light=>light.castsShadows).length??0,activeShadowed:window.__apexCsp?.lights?.filter(light=>light.castsShadows&&Math.max(...light.color.map(Math.abs))>1e-6).length??0,sample:window.__apexCsp?.lights?.find(light=>light.castsShadows)??null})`));
    await waitFor(`window.__apexRenderer?.grassStatus.localLights>0&&window.__apexRenderer?.grassStatus.localShadowLights>0`, 120000);
  }
  if (present) {
    states[`${name}=${value}`] = {
      ...await screenshotState(),
      grassLocalLights: grassFx ? await evaluate(`window.__apexRenderer?.grassStatus.localLights??0`) : null,
      grassLocalShadowLights: grassFx ? await evaluate(`window.__apexRenderer?.grassStatus.localShadowLights??0`) : null,
      grassLocalShadowSamples: grassFx ? await evaluate(`window.__apexRenderer?.grassStatus.localShadowSamples??0`) : null,
      grassLocalShadowAtlasMode: grassFx ? await evaluate(`window.__apexRenderer?.grassStatus.localShadowAtlasMode??""`) : null,
    };
  } else states[`${name}=${value}`] = null;
  if (present) await setCondition(name, 0);
}
const summary = await evaluate(`({pipeline:document.querySelector('#pipeline').textContent,status:document.querySelector('#status').textContent,textures:window.__apexRenderer?.textureStatus,fbx:window.__apexFbx?{sourceName:window.__apexFbx.sourceName,format:window.__apexFbx.format,version:window.__apexFbx.version,textureSummary:window.__apexFbx.textureSummary,references:window.__apexFbx.textureReferences.map(reference=>({material:reference.material,slot:reference.slot,source:reference.source,status:reference.status,matchedBy:reference.matchedBy,path:reference.path,format:reference.format,output:reference.output})),warnings:window.__apexFbx.warnings}:null,skinTextures:window.__apexRenderer?.skinTextureStatus,externalTextures:window.__apexRenderer?.externalTextureStatus,animation:window.__apexRenderer?.animationStatus,analogInstruments:window.__apexRenderer?.analogInstrumentStatus,dynamicTrack:window.__apexRenderer?.dynamicTrackStatus,packedData:window.__apexPackedData?{source:window.__apexPackedData.source,assetName:window.__apexPackedData.assetName,entries:window.__apexPackedData.entries.length,warnings:window.__apexPackedData.warnings}:null,carHierarchyAudit:window.__apexCarHierarchyAudit,colliderAudit:window.__apexColliderAudit,bottomColliders:window.__apexBottomColliders,driverAudit:window.__apexDriverAudit,trackCameras:window.__apexTrackCameras?{sets:window.__apexTrackCameras.length,cameras:window.__apexTrackCameras.reduce((sum,set)=>sum+set.cameras.length,0),warnings:window.__apexTrackCameras.reduce((sum,set)=>sum+set.warnings.length,0)}:null,trackAudit:window.__apexTrackAudit,grass:window.__apexRenderer?.grassStatus,rain:window.__apexRenderer?.rainStatus,clouds:window.__apexRenderer?.cloudStatus,tyres:window.__apexRenderer?.tyreStatus,rims:window.__apexRenderer?.rimBlurStatus,cockpit:window.__apexRenderer?.cockpitPreviewStatus,damage:window.__apexRenderer?.damagePreviewStatus,brakes:window.__apexRenderer?.brakeDiscStatus,shadows:window.__apexRenderer?.shadowStatus,lighting:window.__apexRenderer?.lightingStatus,shaderProfiles:window.__apexRenderer?.shaderProfileStatus,vao:window.__apexRenderer?.vaoStatus,seasons:window.__apexRenderer?.seasonalStatus,scene:window.__apexRenderer?.sceneStatus,workspaceLod:window.__apexRenderer?.workspaceLodStatus})`);
summary.brakes=await evaluate(`window.__apexRenderer?.brakeDiscStatus`);
summary.rims=await evaluate(`window.__apexRenderer?.rimBlurStatus`);
summary.cockpit=await evaluate(`window.__apexRenderer?.cockpitPreviewStatus`);
console.log(JSON.stringify({ bptcAvailable:await evaluate(`Boolean(document.querySelector('#view').getContext('webgl2').getExtension('EXT_texture_compression_bptc'))`), summary, sceneBeforeShowHidden, selected, states, errors }, null, 2));
socket.close();
const failedStates=Object.entries(states).filter(([,state])=>state?.glError);if(errors.length||failedStates.length)throw new Error([errors.length?`Browser errors: ${errors.join("; ")}`:"",failedStates.length?`WebGL errors: ${failedStates.map(([name,state])=>`${name}=${state.glError}`).join(", ")}`:""].filter(Boolean).join("\n"));
