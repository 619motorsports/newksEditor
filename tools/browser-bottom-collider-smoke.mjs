import { createHash } from "node:crypto";

function option(name, fallback = "") {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const assetsPath = option("assets");
const modelPath = option("model");
const port = Number(option("port", "9222"));
const appPort = Number(option("app-port", "4173"));
if (!assetsPath || !modelPath) throw new Error("Usage: node tools/browser-bottom-collider-smoke.mjs --assets CAR_FOLDER --model CAR.kn5 [--port 9222] [--app-port PORT]");

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
  const state = await evaluate("({status:document.querySelector('#status')?.textContent,files:document.querySelector('#asset-folder')?.files.length,bottom:window.__apexBottomColliders,scene:window.__apexRenderer?.sceneStatus})");
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
  await waitFor("document.querySelector('#asset-folder').files.length>0&&window.__apexBottomColliders?.colliders?.length===1&&window.__apexRenderer?.externalTextureStatus.pending===0");
  await setFile("#file", modelPath);
  await waitFor("document.querySelector('#status').textContent.includes('KN5 v')");
  await waitFor("window.__apexRenderer?.textureStatus.pending===0&&window.__apexRenderer?.sceneStatus.bottomColliderBoxes===1");
}

async function setVector(key, value) {
  const edited = await evaluate(`(()=>{const input=document.querySelector('[data-edit-bottom-collider-vector=${JSON.stringify(key)}][data-bottom-collider-position="0"]');if(!input)return false;input.value=${JSON.stringify(value)};input.dispatchEvent(new Event('change',{bubbles:true}));return true;})()`);
  if (!edited) throw new Error(`Bottom-collider ${key} input was not available`);
}

await command("Runtime.enable");
await command("Log.enable");
await command("Page.enable");
await load(true);
const overlayEnabled = await evaluate("(()=>{const button=document.querySelector('#collider-overlay');if(!button||button.hidden||button.disabled)return false;button.click();return button.classList.contains('active')})()");
if (!overlayEnabled) throw new Error("The collision overlay was not available");
await waitFor("window.__apexRenderer?.sceneStatus.colliderVisible===true");
const states = { initial: await capture() };

await setVector("centre", "0.2, -0.35, 0.8");
await setVector("size", "2.1, 0.12, 3.8");
const groundEdited = await evaluate("(()=>{const select=document.querySelector('[data-edit-bottom-collider-ground][data-bottom-collider-position=\"0\"]');if(!select)return false;select.value='false';select.dispatchEvent(new Event('change',{bubbles:true}));return true})()");
if (!groundEdited) throw new Error("The bottom-collider ground control was not available");
await waitFor("window.__apexBottomColliderAuthoring?.applied===3&&window.__apexBottomColliders?.colliders?.[0]?.groundEnabled===false");
states.edited = await capture();

await evaluate("document.querySelector('#undo').click()");
await waitFor("window.__apexBottomColliderAuthoring?.applied===2&&window.__apexBottomColliders?.colliders?.[0]?.groundEnabled===true");
states.undone = await capture();
await evaluate("document.querySelector('#redo').click()");
await waitFor("window.__apexBottomColliderAuthoring?.applied===3&&window.__apexBottomColliders?.colliders?.[0]?.groundEnabled===false");
states.redone = await capture();

const exported = await evaluate("(()=>{document.querySelector('[data-export-bottom-colliders]').click();return window.__apexLastBottomColliderExport||null})()");
if (!exported || !exported.text.includes("CENTRE=0.2, -0.35, 0.8") || !exported.text.includes("GROUND_ENABLE=0")) throw new Error("The exported colliders.ini file did not contain the edits");
const beforeReload = await evaluate("({project:JSON.parse(localStorage.getItem(Object.keys(localStorage).find(key=>key.startsWith('apex-editor:project:')))),cspDisabled:document.querySelector('#export-csp').disabled,authoring:window.__apexBottomColliderAuthoring,scene:window.__apexRenderer.sceneStatus,box:window.__apexBottomColliders.colliders[0]})");

await load(false);
await waitFor("window.__apexBottomColliderAuthoring?.applied===3&&window.__apexBottomColliders?.colliders?.[0]?.groundEnabled===false");
const recovered = await evaluate("({cspDisabled:document.querySelector('#export-csp').disabled,authoring:window.__apexBottomColliderAuthoring,scene:window.__apexRenderer.sceneStatus,box:window.__apexBottomColliders.colliders[0]})");
states.recovered = await capture();
if (states.initial.hash === states.edited.hash) throw new Error("The collision overlay did not change after editing the bottom box");
if (Object.values(states).some((state) => state.glError !== 0)) throw new Error("The production WebGL context reported an error");
if (!beforeReload.cspDisabled || !recovered.cspDisabled) throw new Error("A bottom-collider-only edit enabled CSP export");

console.log(JSON.stringify({ states, beforeReload, recovered, exported, errors }, null, 2));
socket.close();
