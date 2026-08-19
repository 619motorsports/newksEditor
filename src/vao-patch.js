import { walkNodes } from "./kn5.js";

const decoder = new TextDecoder("utf-8");
const ZIP_LOCAL = 0x04034b50, ZIP_CENTRAL = 0x02014b50, ZIP_END = 0x06054b50;
const PATCH_ENTRIES = ["Patch_v5.data", "Patch_v4.data", "Patch_v3.data", "Patch.data"];
export const CSP_VAO_BIND_DISTANCE_SQUARED = 0.01;

export async function parseVaoPatch(input, source = "VAO patch") {
  const bytes = asBytes(input), archive = readZipDirectory(bytes, source);
  const patchEntry = PATCH_ENTRIES.map((name) => archive.get(name.toLowerCase())).find(Boolean);
  if (!patchEntry) throw new Error(`${source}: archive has no supported Patch_v1/v3/v4/v5 data`);
  const version = patchEntry.name === "Patch.data" ? 1 : Number(/_v(\d+)/i.exec(patchEntry.name)?.[1]);
  const [payload, configBytes] = await Promise.all([
    extractZipEntry(bytes, patchEntry, source),
    archive.has("config.ini") ? extractZipEntry(bytes, archive.get("config.ini"), source) : new Uint8Array()
  ]);
  const configText = decoder.decode(configBytes), lighting = parseLightingConfig(configText);
  const parsed = parseVaoData(payload, { version, lighting, source });
  const extra = archive.get("extrasamples.data"), trees = archive.get("treesamples.data");
  return Object.freeze({
    ...parsed,
    source,
    version,
    entry: patchEntry.name,
    configText,
    lighting,
    archiveEntries: [...archive.values()].map(({ name, uncompressedSize }) => ({ name, size: uncompressedSize })),
    extraSamples: extra ? { entry: extra.name, bytes: extra.uncompressedSize, version: 2 } : parsed.embeddedExtraSamples,
    treeSamples: trees ? { entry: trees.name, bytes: trees.uncompressedSize } : null
  });
}

export function parseVaoData(input, options = {}) {
  const bytes = asBytes(input), view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = Number(options.version || 4), source = options.source || `Patch_v${version}.data`;
  const lighting = { opacity: .85, brightness: 1.1, gamma: 1, ...(options.lighting || {}) };
  const records = [];
  let offset = 0, embeddedExtraSamples = null;
  while (offset < bytes.length) {
    const recordOffset = offset;
    need(bytes, offset, 4, source, "record name length");
    const nameLength = view.getUint32(offset, true); offset += 4;
    if (!nameLength || nameLength > 4096) throw new Error(`${source}: invalid record name length ${nameLength} at 0x${recordOffset.toString(16)}`);
    need(bytes, offset, nameLength + 20, source, "record header");
    let name = decoder.decode(bytes.subarray(offset, offset + nameLength)); offset += nameLength;
    const type = view.getUint32(offset, true); offset += 4;
    const firstVertex = [view.getFloat32(offset, true), view.getFloat32(offset + 4, true), view.getFloat32(offset + 8, true)]; offset += 12;
    const vertexCount = view.getUint32(offset, true); offset += 4;
    if (name === "@@__EXTRA_AO@" || type === 4) {
      embeddedExtraSamples = { entry: name, version: 1, samples: vertexCount, bytes: bytes.length - offset };
      break;
    }
    const alternate = name.startsWith("@@__ALT@:");
    if (alternate) name = name.slice(9);
    if (vertexCount > 10_000_000) throw new Error(`${source}: invalid vertex count ${vertexCount} for ${name}`);
    const components = type === 0 || type === 2 ? 3 : type === 1 || type === 3 ? 1 : 0;
    if (!components) throw new Error(`${source}: unsupported record type ${type} for ${name}`);
    const componentBytes = version >= 4 ? 1 : 2, payloadBytes = vertexCount * components * componentBytes;
    need(bytes, offset, payloadBytes, source, `record ${name}`);
    let values = null;
    if (type !== 2) {
      values = new Uint8Array(vertexCount);
      for (let vertex = 0; vertex < vertexCount; vertex++) {
        let sample;
        if (version >= 4) {
          if (components === 1) sample = bytes[offset + vertex];
          else sample = Math.trunc((bytes[offset + vertex * 3] + bytes[offset + vertex * 3 + 1] + bytes[offset + vertex * 3 + 2]) / 3);
          if (version === 4) sample = Math.trunc(Math.sqrt(sample / 255) * 255);
        } else {
          let sum = 0;
          for (let component = 0; component < components; component++) sum += halfToFloat(view.getUint16(offset + (vertex * components + component) * 2, true));
          sample = legacyVaoByte(sum / components, lighting);
        }
        values[vertex] = Math.max(0, Math.min(255, sample));
      }
    }
    records.push(Object.freeze({ name, type, channel: type === 3 ? "secondary" : type === 2 ? "normal" : "primary", alternate, firstVertex, vertexCount, values, offset: recordOffset }));
    offset += payloadBytes;
  }
  return { records: Object.freeze(records), recordCount: records.length, byteLength: bytes.length, bytesRead: embeddedExtraSamples ? bytes.length : offset, embeddedExtraSamples };
}

export function bindVaoPatch(model, patch) {
  const meshes = walkNodes(model.root).map(({ node }) => node).filter((node) => node.kind === "mesh" || node.kind === "skinnedMesh");
  const byName = new Map();
  for (const node of meshes) { const list = byName.get(node.name) || []; list.push(node); byName.set(node.name, list); }
  const bindings = new Map(), unmatched = [], alternate = [];
  let matchedRecords = 0, normalRecords = 0;
  for (const record of patch?.records || []) {
    if (record.channel === "normal") { normalRecords++; continue; }
    if (record.alternate) { alternate.push(record); continue; }
    const candidates = byName.get(record.name) || [];
    const node = candidates.find((candidate) => {
      if (candidate.vertices.length / candidate.vertexStride !== record.vertexCount) return false;
      const dx = candidate.vertices[0] - record.firstVertex[0], dy = candidate.vertices[1] - record.firstVertex[1], dz = candidate.vertices[2] - record.firstVertex[2];
      return dx * dx + dy * dy + dz * dz < CSP_VAO_BIND_DISTANCE_SQUARED;
    });
    if (!node) { unmatched.push(record); continue; }
    const binding = bindings.get(node) || { node, primary: null, secondary: null, records: [] };
    binding[record.channel] = record.values; binding.records.push(record); bindings.set(node, binding); matchedRecords++;
  }
  let vertices = 0, sum = 0, minimum = 255, maximum = 0, primaryMeshes = 0, secondaryMeshes = 0;
  for (const binding of bindings.values()) {
    if (binding.primary) { primaryMeshes++;vertices += binding.primary.length;for (const value of binding.primary) { sum += value;minimum = Math.min(minimum, value);maximum = Math.max(maximum, value); } }
    if (binding.secondary) secondaryMeshes++;
  }
  return { bindings, patchRecords: patch?.recordCount || patch?.records?.length || 0, matchedRecords, unmatchedRecords: unmatched.length, alternateRecords: alternate.length, normalRecords, matchedMeshes: bindings.size, primaryMeshes, secondaryMeshes, vertices, minimum: vertices ? minimum : 255, maximum: vertices ? maximum : 255, mean: vertices ? sum / vertices : 255, unmatched: unmatched.slice(0, 32), alternate: alternate.slice(0, 32) };
}

function parseLightingConfig(text) {
  let section = ""; const values = new Map();
  for (const raw of String(text || "").replace(/\r/g, "").split("\n")) {
    const line = raw.split(";")[0].trim(); if (!line) continue;
    const header = /^\[([^\]]+)\]$/.exec(line); if (header) { section = header[1].trim().toUpperCase(); continue; }
    const equals = line.indexOf("="); if (equals < 0 || section !== "LIGHTING") continue;
    values.set(line.slice(0, equals).trim().toUpperCase(), Number.parseFloat(line.slice(equals + 1)));
  }
  const value = (key, fallback) => Number.isFinite(values.get(key)) ? values.get(key) : fallback;
  return Object.freeze({ opacity: value("OPACITY", .85), brightness: value("BRIGHTNESS", 1.1), gamma: value("GAMMA", 1) });
}

function legacyVaoByte(value, lighting) {
  const adjusted = Math.max(0, Math.min(1, (lighting.opacity * value + 1 - lighting.opacity) * lighting.brightness));
  const encoded = Math.trunc(Math.pow(adjusted, lighting.gamma) * 255);
  return Math.trunc(Math.sqrt((encoded & 255) / 255) * 255);
}

function halfToFloat(value) {
  const sign = value & 0x8000 ? -1 : 1, exponent = value >> 10 & 0x1f, fraction = value & 0x3ff;
  if (exponent === 0) return sign * Math.pow(2, -14) * (fraction / 1024);
  if (exponent === 31) return fraction ? NaN : sign * Infinity;
  return sign * Math.pow(2, exponent - 15) * (1 + fraction / 1024);
}

function readZipDirectory(bytes, source) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let end = -1;
  for (let offset = bytes.length - 22, minimum = Math.max(0, bytes.length - 65557); offset >= minimum; offset--) if (view.getUint32(offset, true) === ZIP_END) { end = offset; break; }
  if (end < 0) throw new Error(`${source}: ZIP end record is missing`);
  const count = view.getUint16(end + 10, true), centralSize = view.getUint32(end + 12, true), centralOffset = view.getUint32(end + 16, true);
  if (centralOffset + centralSize > bytes.length) throw new Error(`${source}: ZIP directory exceeds the archive`);
  const entries = new Map(); let offset = centralOffset;
  for (let index = 0; index < count; index++) {
    need(bytes, offset, 46, source, "ZIP central header");
    if (view.getUint32(offset, true) !== ZIP_CENTRAL) throw new Error(`${source}: invalid ZIP central header at 0x${offset.toString(16)}`);
    const flags = view.getUint16(offset + 8, true), method = view.getUint16(offset + 10, true), crc = view.getUint32(offset + 16, true), compressedSize = view.getUint32(offset + 20, true), uncompressedSize = view.getUint32(offset + 24, true), nameLength = view.getUint16(offset + 28, true), extraLength = view.getUint16(offset + 30, true), commentLength = view.getUint16(offset + 32, true), localOffset = view.getUint32(offset + 42, true);
    need(bytes, offset + 46, nameLength + extraLength + commentLength, source, "ZIP central entry");
    const name = decoder.decode(bytes.subarray(offset + 46, offset + 46 + nameLength));
    if (flags & 1) throw new Error(`${source}: encrypted ZIP entry ${name} is unsupported`);
    if (method !== 0 && method !== 8) throw new Error(`${source}: ZIP method ${method} for ${name} is unsupported`);
    if (uncompressedSize > 512 * 1024 * 1024) throw new Error(`${source}: ZIP entry ${name} is too large`);
    entries.set(name.toLowerCase(), { name, flags, method, crc, compressedSize, uncompressedSize, localOffset });
    offset += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

async function extractZipEntry(bytes, entry, source) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), offset = entry.localOffset;
  need(bytes, offset, 30, source, `ZIP local header ${entry.name}`);
  if (view.getUint32(offset, true) !== ZIP_LOCAL) throw new Error(`${source}: invalid ZIP local header for ${entry.name}`);
  const nameLength = view.getUint16(offset + 26, true), extraLength = view.getUint16(offset + 28, true), start = offset + 30 + nameLength + extraLength;
  need(bytes, start, entry.compressedSize, source, `ZIP data ${entry.name}`);
  const compressed = bytes.slice(start, start + entry.compressedSize);
  let output;
  if (entry.method === 0) output = compressed;
  else {
    const stream = new Blob([compressed]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    output = new Uint8Array(await new Response(stream).arrayBuffer());
  }
  if (output.length !== entry.uncompressedSize) throw new Error(`${source}: ${entry.name} expanded to ${output.length} bytes, expected ${entry.uncompressedSize}`);
  if (crc32(output) !== entry.crc) throw new Error(`${source}: ${entry.name} CRC-32 does not match`);
  return output;
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) { crc ^= byte; for (let bit = 0; bit < 8; bit++) crc = crc >>> 1 ^ (crc & 1 ? 0xedb88320 : 0); }
  return (crc ^ 0xffffffff) >>> 0;
}

function asBytes(input) { return input instanceof Uint8Array ? input : new Uint8Array(input); }
function need(bytes, offset, size, source, label) { if (size < 0 || offset < 0 || offset + size > bytes.length) throw new Error(`${source}: truncated ${label} at 0x${offset.toString(16)}`); }
