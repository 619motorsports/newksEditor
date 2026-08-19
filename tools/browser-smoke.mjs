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
const seasons = process.argv.includes("--seasons");
const showHidden = process.argv.includes("--show-hidden");
const weatherName = option("weather");
const sunHeading = option("sun-heading");
const sunHeight = option("sun-height");
const compareSunHeight = Number(option("compare-sun-height", "10"));
const manualExposure = option("manual-exposure");
const rainWetness = Number(option("rain-wetness", "1"));
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
if ((!modelPath && !layoutName) || !meshName || (layoutName && !assetsPath)) throw new Error("Usage: node tools/browser-smoke.mjs (--model FILE.kn5 | --workspace MANIFEST.ini --assets FOLDER) --mesh NAME [--assembled --show-hidden] [--csp-assets assettocorsa/extension/textures] [--reflection-environment SHOWROOM.kn5 --reflection-root NODE] [--seasons --year-progress 0.5 --compare-year-progress 0] [--vao FILE.vao-patch] [--lighting --weather PRESET --sun-heading 40 --sun-height 55 --compare-sun-height 10 --manual-exposure 0.35] [--shadows] [--reflection-compare] [--surface-overlay] [--grass-fx] [--rain-fx --rain-wetness 1] [--track-camera LABEL --track-camera-position 0.5 --play-track-camera] [--driver FILE.kn5 --driver-cockpit] [--skin NAME] [--animation NAME --animation-position 0.5] [--lod auto|INDEX] [--config FILE.ini] [--input NAME=VALUE] [--port 9222] [--app-port 4173]");

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
trace("connected");
await command("Page.navigate", { url: `http://127.0.0.1:${appPort}` });
captureErrors = true;
await waitFor(`document.readyState==='complete'`);
await waitFor(`window.__apexAppReady===true`);
trace("app loaded");
if (assetsPath) {
  await setFile("#asset-folder", assetsPath);
  await waitFor(`window.__apexRenderer?.externalTextureStatus.selected>0`);
  trace("asset folder selected");
}
if(cspAssetsPath){await setFile("#csp-texture-folder",cspAssetsPath);await waitFor(`window.__apexRenderer?.externalTextureStatus.selected>0`);trace("shared CSP texture folder selected");}
if (layoutName) {
  const opened = await evaluate(`(()=>{const select=document.querySelector('#layout-select'),option=[...select.options].find(option=>option.textContent.endsWith(${JSON.stringify(layoutName)})||option.value.endsWith(${JSON.stringify(layoutName)}));if(!option)return false;select.value=option.value;document.querySelector('#open-layout').click();return true;})()`);
  if (!opened) throw new Error(`Layout manifest was not discovered: ${layoutName}`);
  await waitFor(`document.querySelector('#status').textContent.includes('KN5 files')`, 120000);
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
await waitFor(`window.__apexRenderer?.textureStatus.pending===0`);
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
if(rainFx){await waitFor(`window.__apexRenderer?.rainStatus.matchedMeshes>0`,120000);await evaluate(`(()=>{const e=document.querySelector('#rain-wetness');e.value=${JSON.stringify(rainWetness)};e.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.rainStatus.wetness===${JSON.stringify(rainWetness)}`);trace("RainFX preview enabled");}
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
if(shadows)await waitFor(`window.__apexRenderer?.shadowStatus.enabled&&window.__apexRenderer?.shadowStatus.casters>0`,120000);
if(grassFx&&shadows)await waitFor(`window.__apexRenderer?.shadowStatus.grassInstances>0&&window.__apexRenderer?.grassStatus.castsShadows===true&&window.__apexRenderer?.grassStatus.receivesShadows===true`,120000);
if(reflectionCompare&&shadows)await waitFor(`window.__apexRenderer?.sceneStatus.reflections.directionalShadows===true&&window.__apexRenderer?.sceneStatus.reflections.shadowCasters>0`,120000);
if(reflectionCompare&&grassFx)await waitFor(`window.__apexRenderer?.sceneStatus.reflections.selectionMode!=='scene'||(window.__apexRenderer?.sceneStatus.reflections.grassFx===true&&window.__apexRenderer?.sceneStatus.reflections.grassInstances>0${shadows?"&&window.__apexRenderer?.sceneStatus.reflections.grassCastsShadows===true&&window.__apexRenderer?.sceneStatus.reflections.grassReceivesShadows===true":""})`,120000);
const sceneBeforeShowHidden = await evaluate(`window.__apexRenderer?.sceneStatus`);
const states = { [surfaceOverlay ? "surface-overlay" : grassFx ? "grass-fx" : rainFx ? "rain-fx" : seasons ? `year-progress=${yearProgress}` : vaoPath ? "vao" : reflectionCompare ? "reflections" : lighting ? "lighting" : shadows ? "shadows" : animationName ? "animation=0" : "off"]: await screenshotState(screenshotPath) };
if(showHidden){const enabled=await evaluate(`(()=>{const button=document.querySelector('#show-hidden');if(!button||button.disabled)return false;if(!button.classList.contains('active'))button.click();return true;})()`);if(!enabled)throw new Error("Show hidden mode was not available");await waitFor(`window.__apexRenderer?.sceneStatus.showHidden===true`);const sceneAfterShowHidden=await evaluate(`window.__apexRenderer?.sceneStatus`);if(sceneAfterShowHidden.gameVisible!==sceneBeforeShowHidden.gameVisible||sceneAfterShowHidden.gameHidden!==sceneBeforeShowHidden.gameHidden)throw new Error("Show hidden changed the parsed KN5 visibility counts");if(sceneAfterShowHidden.previewVisible<sceneBeforeShowHidden.previewVisible)throw new Error("Show hidden removed preview meshes");states["hidden-shown"]=await screenshotState();trace("hidden KN5 meshes shown");}
if(seasons){await evaluate(`(()=>{const e=document.querySelector('#year-progress');e.value=${JSON.stringify(compareYearProgress)};e.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.seasonalStatus?.yearProgress===${JSON.stringify(compareYearProgress)}`);states[`year-progress=${compareYearProgress}`]=await screenshotState();}
if(shadows){await evaluate(`document.querySelector('#sun-shadows').click()`);await waitFor(reflectionCompare?`window.__apexRenderer?.shadowStatus.enabled===false&&window.__apexRenderer?.sceneStatus.reflections.directionalShadows===false`:`window.__apexRenderer?.shadowStatus.enabled===false`);states["shadows-off"]=await screenshotState();await evaluate(`document.querySelector('#sun-shadows').click()`);await waitFor(reflectionCompare?`window.__apexRenderer?.shadowStatus.enabled===true&&window.__apexRenderer?.sceneStatus.reflections.directionalShadows===true`:`window.__apexRenderer?.shadowStatus.enabled===true`);}
if(reflectionCompare){await evaluate(`document.querySelector('#scene-reflections').click()`);await waitFor(`window.__apexRenderer?.sceneStatus.reflections.enabled===false`);states["reflections-off"]=await screenshotState();await evaluate(`document.querySelector('#scene-reflections').click()`);await waitFor(`window.__apexRenderer?.sceneStatus.reflections.enabled===true`);}
if(grassFx){await evaluate(`document.querySelector('#grass-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.grassStatus.visible===false`);states["grass-fx-off"]=await screenshotState();await evaluate(`document.querySelector('#grass-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.grassStatus.visible===true`);}
if(rainFx){await evaluate(`document.querySelector('#rain-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.rainStatus.visible===false`);states["rain-fx-off"]=await screenshotState();await evaluate(`document.querySelector('#rain-fx-toggle').click()`);await waitFor(`window.__apexRenderer?.rainStatus.visible===true`);}
if(vaoPath){await evaluate(`document.querySelector('#vao-toggle').click()`);await waitFor(`window.__apexRenderer?.vaoStatus.enabled===false`);states["vao-off"]=await screenshotState();await evaluate(`document.querySelector('#vao-toggle').click()`);await waitFor(`window.__apexRenderer?.vaoStatus.enabled===true`);}
if(lighting){const originalSunHeight=await evaluate(`document.querySelector('#sun-height').value`);await evaluate(`(()=>{const input=document.querySelector('#sun-height');input.value=${JSON.stringify(compareSunHeight)};input.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.lightingStatus?.height===${JSON.stringify(compareSunHeight)}`);states[`sun-height=${compareSunHeight}`]=await screenshotState();await evaluate(`(()=>{const input=document.querySelector('#sun-height');input.value=${JSON.stringify(originalSunHeight)};input.dispatchEvent(new Event('input',{bubbles:true}));})()`);await waitFor(`window.__apexRenderer?.lightingStatus?.height===${JSON.stringify(Number(originalSunHeight))}`);}
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
    };
  } else states[`${name}=${value}`] = null;
  if (present) await setCondition(name, 0);
}
const summary = await evaluate(`({pipeline:document.querySelector('#pipeline').textContent,status:document.querySelector('#status').textContent,textures:window.__apexRenderer?.textureStatus,skinTextures:window.__apexRenderer?.skinTextureStatus,externalTextures:window.__apexRenderer?.externalTextureStatus,animation:window.__apexRenderer?.animationStatus,packedData:window.__apexPackedData?{source:window.__apexPackedData.source,assetName:window.__apexPackedData.assetName,entries:window.__apexPackedData.entries.length,warnings:window.__apexPackedData.warnings}:null,carHierarchyAudit:window.__apexCarHierarchyAudit,colliderAudit:window.__apexColliderAudit,bottomColliders:window.__apexBottomColliders,driverAudit:window.__apexDriverAudit,trackCameras:window.__apexTrackCameras?{sets:window.__apexTrackCameras.length,cameras:window.__apexTrackCameras.reduce((sum,set)=>sum+set.cameras.length,0),warnings:window.__apexTrackCameras.reduce((sum,set)=>sum+set.warnings.length,0)}:null,trackAudit:window.__apexTrackAudit,grass:window.__apexRenderer?.grassStatus,rain:window.__apexRenderer?.rainStatus,shadows:window.__apexRenderer?.shadowStatus,lighting:window.__apexRenderer?.lightingStatus,shaderProfiles:window.__apexRenderer?.shaderProfileStatus,vao:window.__apexRenderer?.vaoStatus,seasons:window.__apexRenderer?.seasonalStatus,scene:window.__apexRenderer?.sceneStatus,workspaceLod:window.__apexRenderer?.workspaceLodStatus})`);
console.log(JSON.stringify({ summary, sceneBeforeShowHidden, selected, states, errors }, null, 2));
socket.close();
