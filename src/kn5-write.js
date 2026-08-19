const encoder = new TextEncoder();

export class Kn5WriteError extends Error {
  constructor(message) { super(message); this.name = "Kn5WriteError"; }
}

class Writer {
  constructor(capacity = 1024) { this.bytes = new Uint8Array(Math.max(1024, capacity)); this.view = new DataView(this.bytes.buffer); this.offset = 0; }
  reserve(size) {
    if (this.offset + size <= this.bytes.length) return;
    let capacity = this.bytes.length;
    while (capacity < this.offset + size) capacity *= 2;
    const next = new Uint8Array(capacity); next.set(this.bytes); this.bytes = next; this.view = new DataView(next.buffer);
  }
  u8(value) { this.reserve(1); this.view.setUint8(this.offset, Number(value) || 0); this.offset++; }
  u16(value) { this.reserve(2); this.view.setUint16(this.offset, Number(value) || 0, true); this.offset += 2; }
  u32(value) { this.reserve(4); this.view.setUint32(this.offset, Number(value) || 0, true); this.offset += 4; }
  f32(value) { this.reserve(4); this.view.setFloat32(this.offset, Number(value), true); this.offset += 4; }
  raw(value) { const source=value instanceof Uint8Array?value:new Uint8Array(value);this.reserve(source.byteLength);this.bytes.set(source,this.offset);this.offset+=source.byteLength; }
  string(value) { const bytes=encoder.encode(String(value??""));this.u32(bytes.byteLength);this.raw(bytes); }
  floats(values, count, label) { if(!values||values.length!==count)throw new Kn5WriteError(`${label} must contain ${count} floats`);for(const value of values)this.f32(value); }
  finish() { return this.bytes.slice(0,this.offset); }
}

function integer(value, minimum, maximum, label) {
  if(!Number.isInteger(value)||value<minimum||value>maximum)throw new Kn5WriteError(`${label} must be an integer from ${minimum} to ${maximum}`);
  return value;
}

function writeMaterial(writer, material) {
  writer.string(material?.name);writer.string(material?.shader);
  const blendMode=integer(material?.blendMode??0,0,2,"Material blend mode"),preserveFlags=material?.blendFlags&&blendMode===material.serializedBlendMode;
  const alphaBlend=preserveFlags?Boolean(material.blendFlags.alphaBlend):blendMode===1;
  const alphaToCoverage=preserveFlags?Boolean(material.blendFlags.alphaToCoverage):blendMode===2;
  writer.u8(alphaBlend);writer.u8(alphaToCoverage);writer.u32(integer(material?.depthMode??0,0,0xffffffff,"Material depth mode"));
  const properties=material?.properties||[];writer.u32(properties.length);
  for(const property of properties){writer.string(property.name);writer.f32(property.value);writer.floats(property.value2,2,`${property.name} value2`);writer.floats(property.value3,3,`${property.name} value3`);writer.floats(property.value4,4,`${property.name} value4`);}
  const resources=material?.resources||[];writer.u32(resources.length);
  for(const resource of resources){writer.string(resource.slot);writer.u32(integer(resource.textureId??0,0,0xffffffff,`${resource.slot} texture ID`));writer.string(resource.texture);}
}

function writeVertices(writer, node, stride, label) {
  if(!node.vertices||node.vertices.length%stride)throw new Kn5WriteError(`${label} vertex data is not divisible by stride ${stride}`);
  writer.u32(node.vertices.length/stride);for(const value of node.vertices)writer.f32(value);
  const indices=node.indices||[];writer.u32(indices.length);for(const value of indices)writer.u16(integer(Number(value),0,0xffff,`${label} index`));
}

function writeNode(writer, node, depth = 0) {
  if(depth>1024)throw new Kn5WriteError("Scene hierarchy is too deep");
  const type=integer(node?.type??({node:1,mesh:2,skinnedMesh:3}[node?.kind]),1,3,"Node type"),children=node.children||[];
  writer.u32(type);writer.string(node.name);writer.u32(children.length);writer.u8(Boolean(node.active));
  if(type===1)writer.floats(node.transform,16,`${node.name} transform`);
  else if(type===2){writer.u8(Boolean(node.castShadows));writer.u8(Boolean(node.visible));writer.u8(Boolean(node.transparent));writeVertices(writer,node,11,node.name);writer.u32(integer(node.materialId,0,0xffffffff,`${node.name} material ID`));writer.u32(integer(node.layer,0,0xffffffff,`${node.name} layer`));writer.f32(node.lodIn);writer.f32(node.lodOut);writer.floats(node.bounds,4,`${node.name} bounds`);writer.u8(Boolean(node.renderable));}
  else{writer.u8(Boolean(node.castShadows));writer.u8(Boolean(node.visible));writer.u8(Boolean(node.transparent));const bones=node.bones||[];writer.u32(bones.length);for(const bone of bones){writer.string(bone.name);writer.floats(bone.transform,16,`${bone.name} bone transform`);}writeVertices(writer,node,19,node.name);writer.u32(integer(node.materialId,0,0xffffffff,`${node.name} material ID`));writer.u32(integer(node.layer,0,0xffffffff,`${node.name} layer`));writer.f32(node.lodIn);writer.f32(node.lodOut);}
  for(const child of children)writeNode(writer,child,depth+1);
}

export function serializeKn5(model) {
  if(!model||model.magic!=="sc6969")throw new Kn5WriteError("Model is not parsed KN5 data");
  if(model.encryption||model.workspace?.protectedFiles?.length)throw new Kn5WriteError("CSP-protected KN5 payloads cannot be rewritten without their original encrypted bytes");
  const version=integer(model.version,5,6,"KN5 version"),writer=new Writer(model.bytesRead||1024);writer.raw(encoder.encode("sc6969"));writer.u32(version);if(version>=6)writer.u32(integer(model.source??0,0,0xffffffff,"KN5 source marker"));
  const textures=model.textures||[];writer.u32(textures.length);for(const texture of textures){if(!texture.data)throw new Kn5WriteError(`Texture ${texture.name} has no data; parse without metadataOnly before writing`);const data=texture.data instanceof Uint8Array?texture.data:new Uint8Array(texture.data);writer.u32(Boolean(texture.active));writer.string(texture.name);writer.u32(data.byteLength);writer.raw(data);}
  const materials=model.materials||[];writer.u32(materials.length);for(const material of materials)writeMaterial(writer,material);writeNode(writer,model.root);return writer.finish();
}
