import { createHash } from "node:crypto";

function option(name, fallback = "") {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const modelPath = option("model"), configPath = option("config"), meshName = option("mesh"), nodeName = option("node"), propertyName = option("property"), propertyValue = option("value"), resourceName = option("resource"), resourceValue = option("resource-value"), meshField = option("mesh-field"), meshValue = option("mesh-value"), nodeField = option("node-field"), nodeValue = option("node-value");
const port = Number(option("port", "9222")), reset = process.argv.includes("--reset"), exportKn5 = process.argv.includes("--export-kn5");
const editKind = nodeField ? "node" : meshField ? "mesh" : resourceName ? "resource" : "property", editName = nodeField || meshField || resourceName || propertyName, editValue = nodeField ? nodeValue : meshField ? meshValue : resourceName ? resourceValue : propertyValue;
const selectionName = nodeName || meshName, selectionNeedsMesh = !nodeName;
if (!modelPath || !configPath || !selectionName || !editName || !editValue || (editKind !== "node" && !meshName)) throw new Error("Usage: node tools/browser-authoring-smoke.mjs --model FILE.kn5 --config FILE.ini (--mesh MESH | --node NODE) (--property NAME --value VALUE | --resource SLOT --resource-value VALUE | --mesh-field NAME --mesh-value VALUE | --node-field name|active|position|rotation|scale --node-value VALUE) [--reset] [--port 9222]");

const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(), page = pages.find((item) => item.type === "page");
if (!page) throw new Error(`No Chrome page target on port ${port}`);
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let nextId = 0;
const pending = new Map(), errors = [];
socket.onmessage = ({ data }) => {
  const message = JSON.parse(data);
  if (message.id) {
    const waiter = pending.get(message.id); pending.delete(message.id);
    if (message.error) waiter.reject(new Error(message.error.message)); else waiter.resolve(message.result);
  } else if (message.method === "Runtime.exceptionThrown") errors.push(message.params.exceptionDetails.text);
  else if (message.method === "Log.entryAdded" && message.params.entry.level === "error") errors.push(message.params.entry.text);
};
function command(method, params = {}) {
  const id = ++nextId; socket.send(JSON.stringify({ id, method, params }));
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
  throw new Error(`Timed out: ${expression}`);
}
async function setFile(selector, path) {
  const { root } = await command("DOM.getDocument", { depth: -1 });
  const { nodeId } = await command("DOM.querySelector", { nodeId: root.nodeId, selector });
  await command("DOM.setFileInputFiles", { nodeId, files: [path] });
}
async function capture() {
  const clip = await evaluate(`(()=>{const r=document.querySelector('#view').getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,scale:1}})()`);
  const { data } = await command("Page.captureScreenshot", { format: "png", fromSurface: true, clip });
  return { hash: createHash("sha256").update(data).digest("hex").slice(0, 16), bytes: Buffer.from(data, "base64").length, glError: await evaluate(`document.querySelector('#view').getContext('webgl2').getError()`) };
}
async function loadAndSelect(clear = false, selectedName = selectionName) {
  await command("Page.navigate", { url: "http://127.0.0.1:4173" });
  await waitFor(`document.readyState==='complete'`);
  if (clear) await evaluate(`Object.keys(localStorage).filter(key=>key.startsWith('apex-editor:project:')).forEach(key=>localStorage.removeItem(key))`);
  await setFile("#file", modelPath); await waitFor(`document.querySelector('#status').textContent.includes('KN5 v')`);
  await waitFor(`window.__apexRenderer?.textureStatus.pending===0`);
  await setFile("#csp-file", configPath); await waitFor(`document.querySelector('#pipeline').textContent.includes('CSP')`);
  const found = await evaluate(`(()=>{const search=document.querySelector('#search');search.value=${JSON.stringify(selectedName)};search.dispatchEvent(new Event('input',{bubbles:true}));const row=[...document.querySelectorAll('.node-row')].find(e=>${selectionNeedsMesh ? "e.querySelector('.node-icon')?.textContent==='◆'&&" : ""}e.querySelector('.node-name')?.textContent===${JSON.stringify(selectedName)});row?.click();if(!document.querySelector('#isolate').disabled&&!document.querySelector('#isolate').classList.contains('active'))document.querySelector('#isolate').click();document.querySelector('#frame').click();return Boolean(row)})()`);
  if (!found) throw new Error(`Node not found: ${selectedName}`);
}

await command("Runtime.enable"); await command("Log.enable"); await command("Page.enable");
await loadAndSelect(reset);
const states = { initial: await capture() };
const meshBoolean = editKind === "mesh" && ["isTransparent", "castShadows"].includes(editName), nodeAttribute = editName === "name" ? "data-edit-node-name" : editName === "active" ? "data-edit-node-active" : "data-edit-node-transform", attribute = editKind === "node" ? nodeAttribute : editKind === "resource" ? "data-edit-resource" : editKind === "mesh" ? (meshBoolean ? "data-edit-mesh-boolean" : "data-edit-mesh-number") : "data-edit-property", datasetKey = editKind === "node" ? (editName === "name" || editName === "active" ? "" : "editNodeTransform") : editKind === "resource" ? "editResource" : editKind === "mesh" ? (meshBoolean ? "editMeshBoolean" : "editMeshNumber") : "editProperty";
const edited = await evaluate(`(()=>{const input=[...document.querySelectorAll('[${attribute}]')].find(e=>${datasetKey ? `e.dataset.${datasetKey}.toLowerCase()===${JSON.stringify(editName.toLowerCase())}` : "true"});if(!input)return false;input.value=${JSON.stringify(editValue)};input.dispatchEvent(new Event('change',{bubbles:true}));return true})()`);
if (!edited) throw new Error(`Editable ${editKind} not found: ${editName}`);
await waitFor(`document.querySelector('#status').textContent.includes('authored edit')`);
states.edited = await capture();
const authored = await evaluate(`(async()=>{const key=Object.keys(localStorage).find(key=>key.startsWith('apex-editor:project:'));const project=key?JSON.parse(localStorage.getItem(key)):null;const module=await import('/src/editor-project.js');return {key,project,csp:project?module.serializeEditorCsp(project):''}})()`);
await evaluate(`document.querySelector('#undo').click()`); await waitFor(`document.querySelector('#undo').disabled`); states.undone = await capture();
await evaluate(`document.querySelector('#redo').click()`); await waitFor(`!document.querySelector('#undo').disabled`); states.redone = await capture();
await loadAndSelect(false, editKind === "node" && editName === "name" ? editValue : selectionName); await waitFor(`document.querySelector('#status').textContent.includes('authored edit')`); states.recovered = await capture();
await loadAndSelect(true);
await evaluate(`(()=>{const input=document.querySelector('#project-file'),transfer=new DataTransfer();transfer.items.add(new File([${JSON.stringify(JSON.stringify(authored.project))}],"roundtrip.apex.json",{type:"application/json"}));input.files=transfer.files;input.dispatchEvent(new Event('change',{bubbles:true}));})()`);
await waitFor(`document.querySelector('#status').textContent.includes('authored edit')`); states.reopened = await capture();
if(exportKn5){await evaluate(`document.querySelector('#export-kn5').click()`);await waitFor(`Boolean(window.__apexLastKn5Export)`);}
const ui = await evaluate(`({status:document.querySelector('#status').textContent,undoDisabled:document.querySelector('#undo').disabled,redoDisabled:document.querySelector('#redo').disabled,exportLabel:document.querySelector('#export-csp').textContent,authoredInputs:document.querySelectorAll('[data-edit-property].authored,[data-edit-resource].authored,[data-edit-mesh-number].authored,[data-edit-mesh-boolean].authored,[data-edit-node-name].authored,[data-edit-node-active].authored,[data-edit-node-transform].authored').length,nodeName:document.querySelector('[data-edit-node-name]')?.placeholder})`);
const kn5Export=await evaluate(`window.__apexLastKn5Export||null`);
const materialEditCount = Object.values(authored.project?.materialEdits||{}).reduce((n,e)=>n+Object.keys(e.properties||{}).length+Object.keys(e.resources||{}).length+[e.shader,e.blendMode,e.depthMode,e.cullMode].filter(Boolean).length,0);
const meshEditCount = Object.values(authored.project?.meshEdits||{}).reduce((n,e)=>n+["isTransparent","castShadows","layer","lodIn","lodOut"].filter((key)=>e[key]!==undefined).length,0);
const nodeEditCount = Object.values(authored.project?.nodeEdits||{}).reduce((n,e)=>n+["name","active","transform"].filter((key)=>e[key]!==undefined).length,0);
console.log(JSON.stringify({ edit: { kind: editKind, name: editName, value: editValue }, states, authored: { key: authored.key, editCount: materialEditCount + meshEditCount + nodeEditCount, csp: authored.csp }, kn5Export, ui, errors }, null, 2));
socket.close();
