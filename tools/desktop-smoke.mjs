import { writeFile } from "node:fs/promises";

function option(name, fallback = "") {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const port = Number(option("port", "9230"));
const screenshotPath = option("screenshot");
const modelPath = option("model");
const meshName = option("mesh");
const assetsPath = option("assets");
const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const target = targets.find((entry) => entry.type === "page");
if (!target) throw new Error(`No packaged Apex Editor page target is available on port ${port}`);

const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let nextId = 0;
const pending = new Map();
const errors = [];
socket.onmessage = ({ data }) => {
  const message = JSON.parse(data);
  if (message.id) {
    const waiter = pending.get(message.id);
    pending.delete(message.id);
    if (message.error) waiter.reject(new Error(message.error.message));
    else waiter.resolve(message.result);
  } else if (message.method === "Runtime.exceptionThrown") {
    errors.push(message.params.exceptionDetails.exception?.description || message.params.exceptionDetails.text);
  } else if (message.method === "Log.entryAdded" && message.params.entry.level === "error") {
    errors.push(message.params.entry.text);
  }
};
function command(method, params = {}) {
  const id = ++nextId;
  socket.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}
async function evaluate(expression) {
  const result = await command("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.text);
  return result.result.value;
}
async function waitFor(expression, timeout = 30000) {
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
  if (!nodeId) throw new Error(`Desktop file input was not found: ${selector}`);
  await command("DOM.setFileInputFiles", { nodeId, files: [path] });
  await evaluate(`document.querySelector(${JSON.stringify(selector)}).dispatchEvent(new Event('change',{bubbles:true}))`);
}

await command("Runtime.enable");
await command("Log.enable");
await command("Page.enable");
await command("DOM.enable");
await command("Page.reload", { ignoreCache: true });
await waitFor("window.__apexAppReady===true");
if (assetsPath) {
  await setFile("#asset-folder", assetsPath);
  await waitFor("document.querySelector('#asset-folder')?.files.length>0", 120000);
  await waitFor("window.__apexRenderer?.externalTextureStatus.pending===0", 120000);
}
if (modelPath) {
  await setFile("#file", modelPath);
  await waitFor("document.querySelector('#status').textContent.includes('KN5 v')", 120000);
  await waitFor("window.__apexRenderer?.textureStatus.pending===0", 120000);
  if (meshName) {
    const selected = await evaluate(`(()=>{const row=[...document.querySelectorAll('.node-row.mesh,.node-row.skinnedMesh')].find(entry=>entry.querySelector('.node-name')?.textContent===${JSON.stringify(meshName)});row?.click();document.querySelector('#frame').click();return Boolean(row);})()`);
    if (!selected) throw new Error(`Packaged scene did not contain mesh ${meshName}`);
  }
}
await evaluate("new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))");
const state = await evaluate(`(async()=>({
  title:document.title,
  url:location.href,
  appReady:window.__apexAppReady===true,
  nodeProcess:typeof process,
  nodeRequire:typeof require,
  openedExternal:window.open('https://example.com'),
  status:document.querySelector('#status')?.textContent||'',
  gpu:document.querySelector('#gpu')?.textContent||'',
  glError:document.querySelector('#view')?.getContext('webgl2')?.getError()??null,
  textures:window.__apexRenderer?.textureStatus||null,
  clouds:window.__apexRenderer?.cloudStatus||null,
  fbx:window.__apexFbx?{sourceName:window.__apexFbx.sourceName,format:window.__apexFbx.format,version:window.__apexFbx.version,textureSummary:window.__apexFbx.textureSummary,references:window.__apexFbx.textureReferences.map(reference=>({material:reference.material,slot:reference.slot,source:reference.source,status:reference.status,matchedBy:reference.matchedBy,path:reference.path,format:reference.format,output:reference.output})),warnings:window.__apexFbx.warnings}:null,
  scene:window.__apexRenderer?.sceneStatus||null,
  contentSecurityPolicy:(await fetch('/')).headers.get('content-security-policy')||''
}))()`);
if (state.title !== "Apex Editor" || !state.appReady) throw new Error("Packaged renderer did not initialize Apex Editor");
if (state.nodeProcess !== "undefined" || state.nodeRequire !== "undefined") throw new Error("Node APIs leaked into the sandboxed renderer");
if (state.openedExternal !== null) throw new Error("The desktop shell allowed an external popup");
if (!state.contentSecurityPolicy.includes("default-src 'self'")) throw new Error("Desktop response is missing its content security policy");
if (state.glError !== 0) throw new Error(`Packaged WebGL renderer returned error ${state.glError}`);
if (screenshotPath) {
  const { data } = await command("Page.captureScreenshot", { format: "png", fromSurface: true });
  await writeFile(screenshotPath, Buffer.from(data, "base64"));
}
if (errors.length) throw new Error(`Packaged renderer errors:\n${errors.join("\n")}`);
console.log(JSON.stringify({ ...state, openedExternal: null, errors }, null, 2));
socket.close();
