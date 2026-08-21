import { createHash } from "node:crypto";

function option(name, fallback = "") {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const assetsPath = option("assets");
const modelPath = option("model");
const port = Number(option("port", "9222"));
const appPort = Number(option("app-port", "4173"));
if (!assetsPath || !modelPath) throw new Error("Usage: node tools/browser-car-damage-smoke.mjs --assets CAR_FOLDER --model CAR.kn5 [--port 9222] [--app-port PORT]");

const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = pages.find((item) => item.type === "page");
if (!page) throw new Error(`No Chrome page target on port ${port}`);
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });

let nextId = 0;
const pending = new Map(), errors = [];
socket.onmessage = ({ data }) => {
  const message = JSON.parse(data);
  if (message.id) {
    const waiter = pending.get(message.id);
    pending.delete(message.id);
    if (message.error) waiter.reject(new Error(message.error.message));
    else waiter.resolve(message.result);
  } else if (message.method === "Runtime.exceptionThrown") errors.push(message.params.exceptionDetails.exception?.description || message.params.exceptionDetails.text);
  else if (message.method === "Log.entryAdded" && message.params.entry.level === "error") errors.push(message.params.entry.text);
};

function command(method, params = {}) {
  const id = ++nextId;
  socket.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}

async function evaluate(expression) {
  const result = await command("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  return result.result.value;
}

async function waitFor(expression, timeout = 120000) {
  const started = Date.now();
  while (Date.now() - started < timeout) {
    if (await evaluate(expression)) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  const state = await evaluate("({status:document.querySelector('#status')?.textContent,damage:window.__apexCarDamage,authoring:window.__apexCarDamageAuthoring,scene:window.__apexRenderer?.sceneStatus})");
  throw new Error(`Timed out: ${expression}\nState: ${JSON.stringify(state)}${errors.length ? `\n${errors.join("\n")}` : ""}`);
}

async function setFile(selector, path) {
  const { root } = await command("DOM.getDocument", { depth: -1 });
  const { nodeId } = await command("DOM.querySelector", { nodeId: root.nodeId, selector });
  await command("DOM.setFileInputFiles", { nodeId, files: [path] });
  await evaluate(`document.querySelector(${JSON.stringify(selector)}).dispatchEvent(new Event('change',{bubbles:true}))`);
}

async function capture() {
  await evaluate("new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))");
  const clip = await evaluate("(()=>{const r=document.querySelector('#view').getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,scale:1}})()");
  const { data } = await command("Page.captureScreenshot", { format: "png", fromSurface: true, clip });
  return {
    hash: createHash("sha256").update(data).digest("hex").slice(0, 16),
    bytes: Buffer.from(data, "base64").length,
    glError: await evaluate("document.querySelector('#view').getContext('webgl2').getError()")
  };
}

async function load(clear = false) {
  await command("Page.navigate", { url: `http://127.0.0.1:${appPort}` });
  await waitFor("window.__apexAppReady===true");
  if (clear) await evaluate("Object.keys(localStorage).filter(key=>key.startsWith('apex-editor:project:')).forEach(key=>localStorage.removeItem(key))");
  await setFile("#asset-folder", assetsPath);
  await waitFor("document.querySelector('#asset-folder').files.length>0&&window.__apexCarDamage?.visualObjects?.length===2&&window.__apexRenderer?.externalTextureStatus.pending===0");
  await setFile("#file", modelPath);
  await waitFor("document.querySelector('#status').textContent.includes('KN5 v')");
  await waitFor("window.__apexRenderer?.textureStatus.pending===0&&window.__apexCarDamageAuthoring?.assetMatch===true");
}

async function setControl(attribute, key, section, value) {
  const changed = await evaluate(`(()=>{const input=document.querySelector('[${attribute}=${JSON.stringify(key)}][data-damage-section=${JSON.stringify(section)}]');if(!input)return false;input.value=${JSON.stringify(value)};input.dispatchEvent(new Event('change',{bubbles:true}));return true})()`);
  if (!changed) throw new Error(`${section} ${key} control was not available`);
}

await command("Runtime.enable");
await command("Log.enable");
await command("Page.enable");
await load(true);
const states = { initial: await capture() };

await setControl("data-edit-damage-number", "maxSpeed", "SCRATCHES", "-1");
await waitFor("document.querySelector('[data-edit-damage-number=maxSpeed][data-damage-section=SCRATCHES]').classList.contains('invalid')&&window.__apexCarDamageAuthoring.edits===0");
await setControl("data-edit-damage-number", "maxSpeed", "SCRATCHES", "24");
await setControl("data-edit-damage-number", "initialLevel", "DAMAGE", "35");
await setControl("data-edit-damage-vector", "staticRotationAxis", "VISUAL_OBJECT_0", "0, 1, 0");
await setControl("data-edit-damage-text", "damageZone", "VISUAL_OBJECT_0", "FRONT_CENTER");
await setControl("data-edit-damage-boolean", "enabled", "OSCILLATIONS", "false");
await waitFor("window.__apexCarDamageAuthoring?.applied===5&&window.__apexCarDamage.oscillations.enabled===false&&window.__apexCarDamage.damage.initialLevel===35");
states.edited = await capture();

await evaluate("document.querySelector('#undo').click()");
await waitFor("window.__apexCarDamageAuthoring?.applied===4&&window.__apexCarDamage.oscillations.enabled===true");
await evaluate("document.querySelector('#redo').click()");
await waitFor("window.__apexCarDamageAuthoring?.applied===5&&window.__apexCarDamage.oscillations.enabled===false");

const exported = await evaluate("(()=>{document.querySelector('[data-export-damage]').click();return window.__apexLastCarDamageExport||null})()");
if (!exported || !exported.text.includes("MAX_SPEED=24") || !exported.text.includes("INITIAL_LEVEL=35") || !exported.text.includes("STATIC_ROTATION_AXIS=0, 1, 0") || !exported.text.includes("DAMAGE_ZONE=FRONT_CENTER") || !exported.text.includes("ENABLED=0")) throw new Error("The exported damage.ini file did not contain the edits");
const beforeReload = await evaluate("({project:JSON.parse(localStorage.getItem(Object.keys(localStorage).find(key=>key.startsWith('apex-editor:project:')))),cspDisabled:document.querySelector('#export-csp').disabled,authoring:window.__apexCarDamageAuthoring})");

await load(false);
await waitFor("window.__apexCarDamageAuthoring?.applied===5&&window.__apexCarDamage.oscillations.enabled===false&&window.__apexCarDamage.damage.initialLevel===35");
const recovered = await evaluate("({cspDisabled:document.querySelector('#export-csp').disabled,authoring:window.__apexCarDamageAuthoring,damage:window.__apexCarDamage})");
states.recovered = await capture();
if (Object.values(states).some((state) => state.glError !== 0)) throw new Error("The production WebGL context reported an error");
if (!beforeReload.cspDisabled || !recovered.cspDisabled) throw new Error("A damage-only edit enabled CSP export");
if (!beforeReload.project.damageAsset?.sha256 || beforeReload.project.damageEdits.VISUAL_OBJECT_0.damageZone !== "FRONT_CENTER") throw new Error("The project did not store the bound damage edits");
if (errors.length) throw new Error(`The browser reported errors:\n${errors.join("\n")}`);

console.log(JSON.stringify({ states, beforeReload, recovered, exported, errors }, null, 2));
socket.close();
