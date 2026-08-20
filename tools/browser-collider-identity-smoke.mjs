import { createHash } from "node:crypto";
import { writeFile } from "node:fs/promises";

function option(name, fallback = "") {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const port = Number(option("port", "9231"));
const modelPath = option("model");
const matchingAssetsPath = option("matching-assets");
const differentCarAssetsPath = option("different-car-assets");
const replacementAssetsPath = option("replacement-assets");
const screenshotPath = option("screenshot");
if (!modelPath || !matchingAssetsPath || !differentCarAssetsPath || !replacementAssetsPath) {
  throw new Error("Usage: node tools/browser-collider-identity-smoke.mjs --model car.kn5 --matching-assets CAR_A --different-car-assets CAR_C --replacement-assets CAR_B [--port 9231] [--screenshot FILE.png]");
}

const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const target = targets.find((entry) => entry.type === "page");
if (!target) throw new Error(`No Apex Editor page target is available on port ${port}`);
const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let nextId = 0;
const pending = new Map(), errors = [];
socket.onmessage = ({ data }) => {
  const message = JSON.parse(data);
  if (message.id) {
    const waiter = pending.get(message.id); pending.delete(message.id);
    if (message.error) waiter.reject(new Error(message.error.message)); else waiter.resolve(message.result);
  } else if (message.method === "Runtime.exceptionThrown") errors.push(message.params.exceptionDetails.exception?.description || message.params.exceptionDetails.text);
  else if (message.method === "Log.entryAdded" && message.params.entry.level === "error") errors.push(message.params.entry.text);
};
function command(method, params = {}) {
  const id = ++nextId; socket.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}
async function evaluate(expression) {
  const result = await command("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  return result.result.value;
}
async function waitFor(expression, timeout = 120000) {
  const started = Date.now();
  while (Date.now() - started < timeout) {
    if (await evaluate(expression)) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${expression}`);
}
async function setFile(selector, path) {
  const { root } = await command("DOM.getDocument", { depth: -1 });
  const { nodeId } = await command("DOM.querySelector", { nodeId: root.nodeId, selector });
  if (!nodeId) throw new Error(`File input was not found: ${selector}`);
  await command("DOM.setFileInputFiles", { nodeId, files: [path] });
}
async function authoringState() {
  return evaluate(`(()=>{const key=Object.keys(localStorage).find(value=>value.startsWith('apex-editor:project:'));const project=key?JSON.parse(localStorage.getItem(key)):null;return {authoring:window.__apexColliderAuthoring,project,warning:document.querySelector('.validation-warning')?.textContent||'',editControls:document.querySelectorAll('[data-edit-collider-transform],[data-edit-collider-operation]').length,resetStale:Boolean(document.querySelector('[data-reset-colliders]')),exportAvailable:Boolean(document.querySelector('[data-export-collider]')),glError:document.querySelector('#view').getContext('webgl2').getError(),status:document.querySelector('#status').textContent}})()`);
}

await command("Runtime.enable"); await command("Log.enable"); await command("Page.enable"); await command("DOM.enable");
await command("Page.reload", { ignoreCache: true });
await waitFor("window.__apexAppReady===true");
await evaluate("Object.keys(localStorage).filter(key=>key.startsWith('apex-editor:project:')).forEach(key=>localStorage.removeItem(key))");
await setFile("#asset-folder", matchingAssetsPath);
await setFile("#file", modelPath);
await waitFor("window.__apexColliderAuthoring?.assetMatch===true && document.querySelector('[data-edit-collider-transform]')");
const edited = await evaluate(`(()=>{const input=document.querySelector('[data-edit-collider-transform="position"]');if(!input)return false;input.value='0.25, 0, 0';input.dispatchEvent(new Event('change',{bubbles:true}));return true})()`);
if (!edited) throw new Error("Collider position control was not available");
await waitFor("window.__apexColliderAuthoring?.applied===1 && Boolean(JSON.parse(localStorage.getItem(Object.keys(localStorage).find(key=>key.startsWith('apex-editor:project:')))).colliderAsset?.sha256)");
const matching = await authoringState();

await setFile("#asset-folder", differentCarAssetsPath);
await waitFor("window.__apexColliderAuthoring?.assetMatch===false");
const differentCar = await authoringState();

await setFile("#asset-folder", replacementAssetsPath);
await waitFor("window.__apexColliderAuthoring?.assetMatch===true && window.__apexColliderAuthoring?.colliderMatch===false && document.querySelector('[data-reset-colliders]')");
const replacement = await authoringState();
await evaluate("document.querySelector('[data-reset-colliders]').click()");
await waitFor("window.__apexColliderAuthoring?.colliderMatch===true && window.__apexColliderAuthoring?.edits===0 && document.querySelector('[data-edit-collider-transform]')");
const reset = await authoringState();

let screenshot = null;
if (screenshotPath) {
  const { data } = await command("Page.captureScreenshot", { format: "png", fromSurface: true });
  const bytes = Buffer.from(data, "base64");
  await writeFile(screenshotPath, bytes);
  screenshot = { path: screenshotPath, bytes: bytes.byteLength, sha256: createHash("sha256").update(bytes).digest("hex").slice(0, 16) };
}
for (const [name, state] of Object.entries({ matching, differentCar, replacement, reset })) {
  if (state.glError !== 0) throw new Error(`${name} WebGL state returned ${state.glError}`);
}
if (matching.authoring?.applied !== 1 || !matching.project?.colliderAsset?.sha256) throw new Error("The first collider edit did not persist its source identity");
if (differentCar.authoring?.applied !== 0 || differentCar.authoring?.assetMatch !== false) throw new Error("Collider edits crossed into a different car folder");
if (replacement.authoring?.applied !== 0 || replacement.authoring?.colliderMatch !== false || replacement.editControls !== 0 || replacement.exportAvailable) throw new Error("Collider edits crossed into a replacement file");
if (reset.authoring?.edits !== 0 || reset.project?.colliderAsset !== null || !reset.exportAvailable) throw new Error("Reset did not clear the stale collider binding");
if (errors.length) throw new Error(`Renderer errors:\n${errors.join("\n")}`);
console.log(JSON.stringify({ matching, differentCar, replacement, reset, screenshot, errors }, null, 2));
socket.close();
