import { SURFACE_EDIT_KEYS } from "./surface-authoring.js";
import { normalizeFileIdentity } from "./file-identity.js";
import { normalizeCarLodFileName } from "./kn5-workspace.js";

export const PROJECT_FORMAT = "apex-editor-project";
export const PROJECT_VERSION = 1;
const WORKSPACE_FILE_EDIT_KEYS = ["name", "position", "rotation", "lodIn", "lodOut", "probability", "multiplicity", "posMode", "positionCenter", "positionRange", "velMode", "velocityBase", "velocityRange", "playWav"];

function safeScalar(value) {
  if (typeof value === "number") return Number.isFinite(value) ? value : 0;
  if (typeof value === "string") return value.slice(0, 4096);
  if (Array.isArray(value)) return value.slice(0, 16).map((item) => Number(item)).filter(Number.isFinite);
  return null;
}

function safeRecord(value, mapper) {
  const output = Object.create(null);
  if (!value || typeof value !== "object" || Array.isArray(value)) return output;
  for (const [key, item] of Object.entries(value)) {
    if (!key || key === "__proto__" || key === "constructor" || key === "prototype") continue;
    const mapped = mapper(item, key.slice(0, 512));
    if (mapped !== null && mapped !== undefined) output[key.slice(0, 512)] = mapped;
  }
  return output;
}

function cleanEdit(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const state = {};
  for (const key of ["shader", "blendMode", "depthMode", "cullMode"]) {
    const item = value[key];
    if (typeof item === "string" && item.trim()) state[key] = item.trim().slice(0, 512);
  }
  const properties = safeRecord(value.properties, (item) => safeScalar(item));
  const resources = safeRecord(value.resources, (item) => {
    if (!item || typeof item !== "object" || Array.isArray(item)) return null;
    const output = {};
    if (typeof item.texture === "string" && item.texture.trim()) output.texture = item.texture.trim().slice(0, 2048);
    if (typeof item.file === "string" && item.file.trim()) output.file = item.file.trim().slice(0, 2048);
    const color = safeScalar(item.color);
    if (Array.isArray(color)) output.color = color;
    return Object.keys(output).length ? output : null;
  });
  if (!Object.keys(state).length && !Object.keys(properties).length && !Object.keys(resources).length) return null;
  return { ...state, properties, resources };
}

function cleanMeshEdit(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const output = {};
  for (const key of ["isTransparent", "castShadows"]) if (typeof value[key] === "boolean") output[key] = value[key];
  for (const key of ["layer", "lodIn", "lodOut"]) if (value[key] !== null && value[key] !== "" && Number.isFinite(Number(value[key]))) output[key] = Number(value[key]);
  return Object.keys(output).length ? output : null;
}

function cleanNodeEdit(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const output = {};
  if (typeof value.name === "string" && value.name.trim()) output.name = value.name.trim().slice(0, 1024);
  if (typeof value.active === "boolean") output.active = value.active;
  if (Array.isArray(value.transform) && value.transform.length === 16) {
    const transform = value.transform.map(Number);
    if (transform.every(Number.isFinite)) output.transform = transform;
  }
  return Object.keys(output).length ? output : null;
}

function cleanGeometryEdit(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const output = {};
  if (Array.isArray(value.transform) && value.transform.length === 16) {
    const transform = value.transform.map(Number);
    if (transform.every(Number.isFinite)) output.transform = transform;
  }
  for (const key of ["removeDegenerate", "reverseWinding", "recalculateNormals"]) if (value[key] === true) output[key] = true;
  return Object.keys(output).length ? output : null;
}

function cleanWorkspaceFileEdit(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const output = {};
  if (typeof value.name === "string") {
    try { output.name = normalizeCarLodFileName(value.name); }
    catch { /* Invalid project file names are dropped. */ }
  }
  for (const key of ["position", "rotation", "positionCenter", "positionRange", "velocityBase", "velocityRange"]) {
    if (!Array.isArray(value[key]) || value[key].length !== 3) continue;
    const vector = value[key].map(Number);
    if (vector.every(Number.isFinite)) output[key] = vector;
  }
  if (Array.isArray(value.multiplicity) && value.multiplicity.length === 2) {
    const multiplicity = value.multiplicity.map(Number);
    if (multiplicity.every(Number.isFinite)) output.multiplicity = multiplicity;
  }
  for (const key of ["lodIn", "lodOut", "probability"]) if (value[key] !== null && value[key] !== undefined && value[key] !== "" && Number.isFinite(Number(value[key]))) output[key] = Number(value[key]);
  for (const key of ["posMode", "velMode"]) if (typeof value[key] === "string" && value[key].trim()) output[key] = value[key].trim().toUpperCase().slice(0, 128);
  if (value.playWav === null) output.playWav = null;
  else if (typeof value.playWav === "string") output.playWav = value.playWav.trim().slice(0, 1024) || null;
  return Object.keys(output).length ? output : null;
}

function cleanWorkspaceEdits(value) {
  const output = { files: Object.create(null) };
  if (!value || typeof value !== "object" || Array.isArray(value)) return output;
  output.files = safeRecord(value.files, (item) => cleanWorkspaceFileEdit(item));
  for (const key of ["cockpitHrDistance", "driverHrDistance"]) if (value[key] !== null && value[key] !== undefined && value[key] !== "" && Number.isFinite(Number(value[key]))) output[key] = Number(value[key]);
  return output;
}

function cleanSurfaceEdit(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const output = {};
  for (const key of ["friction", "damping", "dirtAdditive", "blackFlagTime", "sinHeight", "sinLength", "vibrationGain", "vibrationLength", "wavPitch"]) {
    if (value[key] !== null && value[key] !== undefined && value[key] !== "" && Number.isFinite(Number(value[key]))) output[key] = Number(value[key]);
  }
  for (const key of ["isValidTrack", "isPitlane"]) if (typeof value[key] === "boolean") output[key] = value[key];
  if (typeof value.key === "string" && value.key.trim()) output.key = value.key.trim().toUpperCase().slice(0, 128);
  for (const key of ["wav", "ffEffect"]) {
    if (value[key] === null) output[key] = null;
    else if (typeof value[key] === "string") output[key] = value[key].trim().slice(0, 1024) || null;
  }
  return Object.keys(output).length ? output : null;
}

export function createEditorProject(asset = {}) {
  return {
    format: PROJECT_FORMAT,
    version: PROJECT_VERSION,
    asset: {
      name: String(asset.name || "").slice(0, 1024),
      size: Math.max(0, Number(asset.size) || 0),
      kn5Version: Math.max(0, Number(asset.kn5Version) || 0)
    },
    colliderAsset: null,
    materialEdits: Object.create(null),
    meshEdits: Object.create(null),
    nodeEdits: Object.create(null),
    geometryEdits: Object.create(null),
    colliderEdits: Object.create(null),
    workspaceEdits: { files: Object.create(null) },
    surfaceEdits: Object.create(null)
  };
}

export function normalizeEditorProject(value) {
  if (!value || typeof value !== "object" || value.format !== PROJECT_FORMAT) throw new Error("Not an Apex Editor project");
  if (Number(value.version) !== PROJECT_VERSION) throw new Error(`Unsupported Apex Editor project version ${value.version}`);
  const project = createEditorProject(value.asset || {});
  project.materialEdits = safeRecord(value.materialEdits, (item) => cleanEdit(item));
  project.meshEdits = safeRecord(value.meshEdits, (item) => cleanMeshEdit(item));
  project.nodeEdits = safeRecord(value.nodeEdits, (item) => cleanNodeEdit(item));
  project.geometryEdits = safeRecord(value.geometryEdits, (item) => cleanGeometryEdit(item));
  project.colliderEdits = safeRecord(value.colliderEdits, (item) => cleanGeometryEdit(item));
  project.colliderAsset = Object.keys(project.colliderEdits).length ? normalizeFileIdentity(value.colliderAsset) : null;
  project.workspaceEdits = cleanWorkspaceEdits(value.workspaceEdits);
  project.surfaceEdits = safeRecord(value.surfaceEdits, (item) => cleanSurfaceEdit(item));
  return project;
}

export function cloneEditorProject(project) {
  return normalizeEditorProject(JSON.parse(JSON.stringify(project)));
}

export function parseEditorValue(text) {
  const source = String(text).trim();
  if (!source) throw new Error("Enter a number or comma-separated vector");
  const values = source.split(",").map((part) => Number(part.trim()));
  if (values.some((value) => !Number.isFinite(value))) throw new Error("Every component must be a finite number");
  return values.length === 1 ? values[0] : values;
}

export function formatEditorValue(value) {
  const format = (item) => Number.isFinite(Number(item)) ? Number(Number(item).toFixed(6)).toString() : "0";
  return Array.isArray(value) ? value.map(format).join(", ") : format(value);
}

export function editorProjectEditCount(project) {
  const materials = Object.values(project?.materialEdits || {}).reduce((count, edit) => count + Object.keys(edit.properties || {}).length + Object.keys(edit.resources || {}).length + ["shader", "blendMode", "depthMode", "cullMode"].filter((key) => edit[key]).length, 0);
  const meshes = Object.values(project?.meshEdits || {}).reduce((count, edit) => count + ["isTransparent", "castShadows", "layer", "lodIn", "lodOut"].filter((key) => edit[key] !== undefined).length, 0);
  const nodes = Object.values(project?.nodeEdits || {}).reduce((count, edit) => count + ["name", "active", "transform"].filter((key) => edit[key] !== undefined).length, 0);
  const geometry = Object.values(project?.geometryEdits || {}).reduce((count, edit) => count + ["transform", "removeDegenerate", "reverseWinding", "recalculateNormals"].filter((key) => edit[key] !== undefined).length, 0);
  const colliders = Object.values(project?.colliderEdits || {}).reduce((count, edit) => count + ["transform", "removeDegenerate", "reverseWinding", "recalculateNormals"].filter((key) => edit[key] !== undefined).length, 0);
  const workspaceFiles = Object.values(project?.workspaceEdits?.files || {}).reduce((count, edit) => count + WORKSPACE_FILE_EDIT_KEYS.filter((key) => edit[key] !== undefined).length, 0);
  const workspace = workspaceFiles + ["cockpitHrDistance", "driverHrDistance"].filter((key) => project?.workspaceEdits?.[key] !== undefined).length;
  const surfaces = Object.values(project?.surfaceEdits || {}).reduce((count, edit) => count + SURFACE_EDIT_KEYS.filter((key) => edit?.[key] !== undefined).length, 0);
  return materials + meshes + nodes + geometry + colliders + workspace + surfaces;
}

export function editorProjectCspEditCount(project) {
  const total = editorProjectEditCount(project);
  const nodes = Object.values(project?.nodeEdits || {}).reduce((count, edit) => count + ["name", "active", "transform"].filter((key) => edit[key] !== undefined).length, 0);
  const geometry = Object.values(project?.geometryEdits || {}).reduce((count, edit) => count + ["transform", "removeDegenerate", "reverseWinding", "recalculateNormals"].filter((key) => edit[key] !== undefined).length, 0);
  const colliders = Object.values(project?.colliderEdits || {}).reduce((count, edit) => count + ["transform", "removeDegenerate", "reverseWinding", "recalculateNormals"].filter((key) => edit[key] !== undefined).length, 0);
  const workspaceFiles = Object.values(project?.workspaceEdits?.files || {}).reduce((count, edit) => count + WORKSPACE_FILE_EDIT_KEYS.filter((key) => edit[key] !== undefined).length, 0);
  const workspace = workspaceFiles + ["cockpitHrDistance", "driverHrDistance"].filter((key) => project?.workspaceEdits?.[key] !== undefined).length;
  const surfaces = Object.values(project?.surfaceEdits || {}).reduce((count, edit) => count + SURFACE_EDIT_KEYS.filter((key) => edit?.[key] !== undefined).length, 0);
  return total - nodes - geometry - colliders - workspace - surfaces;
}

function quoteListItem(value) {
  const text = String(value);
  return /[,;\s]/.test(text) ? `"${text.replaceAll('"', "'")}"` : text;
}

function iniValue(value) {
  if (Array.isArray(value)) return value.map((item) => formatEditorValue(item)).join(", ");
  return typeof value === "number" ? formatEditorValue(value) : String(value);
}

export function serializeEditorCsp(project) {
  const sections = [];
  const edits = Object.entries(project?.materialEdits || {}).filter(([, edit]) => cleanEdit(edit)).sort(([a], [b]) => a.localeCompare(b));
  for (let index = 0; index < edits.length; index++) {
    const [material, raw] = edits[index], edit = cleanEdit(raw);
    const lines = [`[SHADER_REPLACEMENT_APEX_EDITOR_${String(index).padStart(3, "0")}]`, `MATERIALS = ${quoteListItem(material)}`];
    if (edit.shader) lines.push(`SHADER = ${edit.shader}`);
    if (edit.blendMode) lines.push(`BLEND_MODE = ${edit.blendMode}`);
    if (edit.depthMode) lines.push(`DEPTH_MODE = ${edit.depthMode}`);
    if (edit.cullMode) lines.push(`CULL_MODE = ${edit.cullMode}`);
    let propertyIndex = 0;
    for (const [name, value] of Object.entries(edit.properties).sort(([a], [b]) => a.localeCompare(b))) lines.push(`PROP_${propertyIndex++} = ${name}, ${iniValue(value)}`);
    let resourceIndex = 0;
    for (const [slot, resource] of Object.entries(edit.resources).sort(([a], [b]) => a.localeCompare(b))) {
      lines.push(`RESOURCE_${resourceIndex} = ${slot}`);
      if (resource.texture) lines.push(`RESOURCE_TEXTURE_${resourceIndex} = ${resource.texture}`);
      else if (resource.file) lines.push(`RESOURCE_FILE_${resourceIndex} = ${resource.file}`);
      else if (resource.color) lines.push(`RESOURCE_COLOR_${resourceIndex} = ${iniValue(resource.color)}`);
      resourceIndex++;
    }
    sections.push(lines.join("\n"));
  }
  const meshEdits = Object.entries(project?.meshEdits || {}).filter(([, edit]) => cleanMeshEdit(edit)).sort(([a], [b]) => a.localeCompare(b));
  for (let index = 0; index < meshEdits.length; index++) {
    const [mesh, raw] = meshEdits[index], edit = cleanMeshEdit(raw), lines = [`[MESH_ADJUSTMENT_APEX_EDITOR_${String(index).padStart(3, "0")}]`, `MESHES = ${quoteListItem(mesh)}`];
    if (edit.isTransparent !== undefined) lines.push(`IS_TRANSPARENT = ${edit.isTransparent ? 1 : 0}`);
    if (edit.layer !== undefined) lines.push(`LAYER = ${formatEditorValue(edit.layer)}`);
    if (edit.lodIn !== undefined) lines.push(`LOD_IN = ${formatEditorValue(edit.lodIn)}`);
    if (edit.lodOut !== undefined) lines.push(`LOD_OUT = ${formatEditorValue(edit.lodOut)}`);
    if (edit.castShadows !== undefined) lines.push(`CAST_SHADOWS = ${edit.castShadows ? 1 : 0}`);
    sections.push(lines.join("\n"));
  }
  return `; Generated by Apex Editor. Source KN5 is not modified.\n${sections.length ? `\n${sections.join("\n\n")}\n` : ""}`;
}

export function serializeEditorProject(project) {
  const normalized = normalizeEditorProject(project);
  return `${JSON.stringify(normalized, null, 2)}\n`;
}
