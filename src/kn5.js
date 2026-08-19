const decoder = new TextDecoder("utf-8");
const kn5EncryptionMarker = new TextEncoder().encode("__AC_SHADERS_PATCH_KN5ENC_v1__");

export class Kn5Error extends Error {
  constructor(message, offset) {
    super(`${message} (at 0x${offset.toString(16)})`);
    this.name = "Kn5Error";
    this.offset = offset;
  }
}

class Reader {
  constructor(buffer) {
    this.buffer = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
    this.view = new DataView(this.buffer.buffer, this.buffer.byteOffset, this.buffer.byteLength);
    this.offset = 0;
  }

  need(size, label = "data") {
    if (size < 0 || this.offset + size > this.buffer.byteLength) {
      throw new Kn5Error(`Unexpected end of file while reading ${label}`, this.offset);
    }
  }

  u8(label) { this.need(1, label); return this.view.getUint8(this.offset++); }
  u16(label) { this.need(2, label); const v = this.view.getUint16(this.offset, true); this.offset += 2; return v; }
  u32(label) { this.need(4, label); const v = this.view.getUint32(this.offset, true); this.offset += 4; return v; }
  f32(label) { this.need(4, label); const v = this.view.getFloat32(this.offset, true); this.offset += 4; return v; }
  bytes(size, label) { this.need(size, label); const v = this.buffer.subarray(this.offset, this.offset + size); this.offset += size; return v; }
  string(label = "string") {
    const size = this.u32(`${label} length`);
    if (size > 16 * 1024 * 1024) throw new Kn5Error(`Invalid ${label} length ${size}`, this.offset - 4);
    return decoder.decode(this.bytes(size, label));
  }
  floats(count, label) { return Array.from({ length: count }, () => this.f32(label)); }
}

function saneCount(reader, count, minimumSize, label) {
  const remaining = reader.buffer.byteLength - reader.offset;
  if (count > 10_000_000 || count * minimumSize > remaining) {
    throw new Kn5Error(`Invalid ${label} count ${count}`, reader.offset - 4);
  }
  return count;
}

function readMaterial(reader) {
  const name = reader.string("material name");
  const shader = reader.string("shader name");
  const alphaBlend = Boolean(reader.u8("alpha-blend flag"));
  const alphaToCoverage = Boolean(reader.u8("alpha-to-coverage flag"));
  const blendMode = alphaToCoverage ? 2 : alphaBlend ? 1 : 0;
  const material = {
    name,
    shader,
    blendMode,
    blendFlags: { alphaBlend, alphaToCoverage },
    serializedBlendMode: blendMode,
    depthMode: reader.u32("depth mode"),
    properties: [],
    resources: []
  };
  const propertyCount = saneCount(reader, reader.u32("material property count"), 44, "material property");
  for (let i = 0; i < propertyCount; i++) {
    material.properties.push({
      name: reader.string("property name"),
      value: reader.f32("property value"),
      value2: reader.floats(2, "property vec2 value"),
      value3: reader.floats(3, "property vec3 value"),
      value4: reader.floats(4, "property vec4 value")
    });
  }
  const resourceCount = saneCount(reader, reader.u32("material resource count"), 8, "material resource");
  for (let i = 0; i < resourceCount; i++) {
    material.resources.push({
      slot: reader.string("resource slot"),
      textureId: reader.u32("resource texture ID"),
      texture: reader.string("texture name")
    });
  }
  return material;
}

function readMesh(reader, node) {
  node.castShadows = Boolean(reader.u8("cast-shadows flag"));
  node.visible = Boolean(reader.u8("visibility flag"));
  node.transparent = Boolean(reader.u8("transparent flag"));
  const vertexCount = saneCount(reader, reader.u32("vertex count"), 44, "vertex");
  const stride = 11;
  const data = new Float32Array(vertexCount * stride);
  for (let i = 0; i < data.length; i++) data[i] = reader.f32("vertex data");
  node.vertices = data;
  node.vertexStride = stride;
  const indexCount = saneCount(reader, reader.u32("index count"), 2, "index");
  const indices = new Uint16Array(indexCount);
  for (let i = 0; i < indexCount; i++) indices[i] = reader.u16("index");
  node.indices = indices;
  node.materialId = reader.u32("material ID");
  node.layer = reader.u32("layer");
  node.lodIn = reader.f32("LOD in");
  node.lodOut = reader.f32("LOD out");
  node.bounds = reader.floats(4, "bounding sphere");
  node.renderable = Boolean(reader.u8("renderable flag"));
}

function readSkinnedMesh(reader, node) {
  node.castShadows = Boolean(reader.u8("cast-shadows flag"));
  node.visible = Boolean(reader.u8("visibility flag"));
  node.transparent = Boolean(reader.u8("transparent flag"));
  const boneCount = saneCount(reader, reader.u32("bone count"), 68, "bone");
  node.bones = Array.from({ length: boneCount }, () => ({
    name: reader.string("bone name"),
    transform: reader.floats(16, "bone transform")
  }));
  const vertexCount = saneCount(reader, reader.u32("skinned vertex count"), 76, "skinned vertex");
  node.vertexStride = 19;
  node.vertices = new Float32Array(vertexCount * node.vertexStride);
  for (let i = 0; i < node.vertices.length; i++) node.vertices[i] = reader.f32("skinned vertex data");
  const indexCount = saneCount(reader, reader.u32("index count"), 2, "index");
  node.indices = new Uint16Array(indexCount);
  for (let i = 0; i < indexCount; i++) node.indices[i] = reader.u16("index");
  node.materialId = reader.u32("material ID");
  node.layer = reader.u32("layer");
  node.lodIn = reader.f32("LOD in");
  node.lodOut = reader.f32("LOD out");
  node.renderable = true;
}

function readNode(reader, depth = 0) {
  if (depth > 1024) throw new Kn5Error("Scene hierarchy is too deep", reader.offset);
  const type = reader.u32("node type");
  if (type < 1 || type > 3) throw new Kn5Error(`Unsupported node type ${type}`, reader.offset - 4);
  const node = {
    type,
    kind: ["", "node", "mesh", "skinnedMesh"][type],
    name: reader.string("node name"),
    children: [],
    active: false
  };
  const childCount = saneCount(reader, reader.u32("child count"), 9, "child");
  node.active = Boolean(reader.u8("active flag"));
  if (type === 1) node.transform = reader.floats(16, "node transform");
  else if (type === 2) readMesh(reader, node);
  else readSkinnedMesh(reader, node);
  for (let i = 0; i < childCount; i++) node.children.push(readNode(reader, depth + 1));
  return node;
}

function markerAt(bytes, offset) {
  if (offset < 0 || offset + kn5EncryptionMarker.length > bytes.length) return false;
  for (let index = 0; index < kn5EncryptionMarker.length; index++) if (bytes[offset + index] !== kn5EncryptionMarker[index]) return false;
  return true;
}

export function inspectKn5Encryption(input, payloadOffset = 0) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input), view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let markerOffset = -1;
  for (let offset = bytes.length - kn5EncryptionMarker.length - 8; offset >= Math.max(payloadOffset, bytes.length - 256); offset--) if (markerAt(bytes, offset)) { markerOffset = offset; break; }
  if (markerOffset < 4 || view.getUint32(markerOffset - 4, true) !== kn5EncryptionMarker.length) return null;
  const recordsEnd = markerOffset - 4, protectedTextures = new Set(), protectedMeshes = new Set();
  let offset = payloadOffset, recordCount = 0;
  try {
    while (offset < recordsEnd) {
      if (offset + 8 > recordsEnd) throw new Error("truncated record header");
      const nameLength = view.getUint32(offset, true); offset += 4;
      if (!nameLength || nameLength > 4096 || offset + nameLength + 4 > recordsEnd) throw new Error(`invalid record name length ${nameLength}`);
      const name = decoder.decode(bytes.subarray(offset, offset + nameLength)); offset += nameLength;
      const size = view.getUint32(offset, true); offset += 4;
      if (offset + size > recordsEnd) throw new Error(`record ${name} exceeds payload`);
      const texture = name.match(/^tex\.(.*)\.d$/i), mesh = name.match(/^ver\.(.*)\.x$/i);
      if (texture) protectedTextures.add(texture[1]);
      if (mesh) protectedMeshes.add(mesh[1]);
      offset += size; recordCount++;
      if (recordCount > 1_000_000) throw new Error("too many encryption records");
    }
    if (offset !== recordsEnd) throw new Error("records do not end at footer");
    return {
      format: "CSP_KN5ENC_v1", valid: true, payloadOffset, payloadBytes: recordsEnd - payloadOffset,
      recordCount, protectedTextures: [...protectedTextures], protectedMeshes: [...protectedMeshes],
      footer: [view.getUint32(markerOffset + kn5EncryptionMarker.length, true), view.getUint32(markerOffset + kn5EncryptionMarker.length + 4, true)]
    };
  } catch (error) {
    return { format: "CSP_KN5ENC_v1", valid: false, payloadOffset, payloadBytes: recordsEnd - payloadOffset, recordCount, protectedTextures: [...protectedTextures], protectedMeshes: [...protectedMeshes], error: error.message };
  }
}

export function parseKn5(input, options = {}) {
  const reader = new Reader(input);
  const magic = decoder.decode(reader.bytes(6, "magic"));
  if (magic !== "sc6969") throw new Kn5Error(`Not a KN5 file (magic is ${JSON.stringify(magic)})`, 0);
  const version = reader.u32("version");
  if (version < 5 || version > 6) throw new Kn5Error(`Unsupported KN5 version ${version}`, 6);
  const source = version >= 6 ? reader.u32("source marker") : 0;
  const textureCount = saneCount(reader, reader.u32("texture count"), 12, "texture");
  const textures = [];
  for (let i = 0; i < textureCount; i++) {
    const active = Boolean(reader.u32("texture active flag"));
    const name = reader.string("texture name");
    const size = reader.u32("texture byte size");
    const data = reader.bytes(size, `texture ${name}`);
    textures.push({ active, name, size, data: options.metadataOnly ? undefined : data });
  }
  const materialCount = saneCount(reader, reader.u32("material count"), 15, "material");
  const materials = Array.from({ length: materialCount }, () => readMaterial(reader));
  const root = readNode(reader);
  const encryption = reader.offset < reader.buffer.byteLength ? inspectKn5Encryption(reader.buffer, reader.offset) : null;
  return { magic, version, source, textures, materials, root, bytesRead: reader.offset, byteLength: reader.buffer.byteLength, encryption };
}

export function walkNodes(root) {
  const result = [];
  const visit = (node, parent = null) => {
    result.push({ node, parent });
    for (const child of node.children) visit(child, node);
  };
  visit(root);
  return result;
}

export function computeKn5Visibility(root) {
  const result = new WeakMap();
  const visit = (node, parentActive) => {
    const branchActive = Boolean(parentActive && node.active);
    const meshVisible = node.kind !== "mesh" && node.kind !== "skinnedMesh" || Boolean(node.visible && node.renderable);
    result.set(node, branchActive && meshVisible);
    for (const child of node.children || []) visit(child, branchActive);
  };
  visit(root, true);
  return result;
}

export function propertyValue(material, name, fallback = 0) {
  return material?.properties.find((p) => p.name === name)?.value ?? fallback;
}
