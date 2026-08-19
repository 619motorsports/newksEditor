import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { decodeDdsRgba, inspectDds } from "../src/dds.js";
import { parseKn5 } from "../src/kn5.js";

const abarthFixture = "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_abarth500_assetto_corse/abarth500_assetto_corse.kn5";
const hangarFixture = "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/showroom/Hangar/hangar.kn5";

function rawDds(width, height, bits, masks, pixels, flags = 0x40, pitch = width * bits / 8) {
  const bytes = new Uint8Array(128 + pixels.length), view = new DataView(bytes.buffer);
  bytes.set([68, 68, 83, 32]); view.setUint32(4, 124, true); view.setUint32(8, 0x100f, true);
  view.setUint32(12, height, true); view.setUint32(16, width, true); view.setUint32(20, pitch, true); view.setUint32(28, 1, true);
  view.setUint32(76, 32, true); view.setUint32(80, flags, true); view.setUint32(88, bits, true);
  masks.forEach((mask, index) => view.setUint32(92 + index * 4, mask, true)); bytes.set(pixels, 128);
  return bytes;
}

function dx10Dds(width, height, dxgi, pixels) {
  const bytes = new Uint8Array(148 + pixels.length), view = new DataView(bytes.buffer);
  bytes.set([68, 68, 83, 32]); view.setUint32(4, 124, true); view.setUint32(12, height, true); view.setUint32(16, width, true); view.setUint32(28, 1, true);
  view.setUint32(76, 32, true); view.setUint32(80, 4, true); bytes.set([68, 88, 49, 48], 84); view.setUint32(128, dxgi, true); bytes.set(pixels, 148);
  return bytes;
}

function legacyBcDds(fourCC,block){const bytes=new Uint8Array(128+block.length),view=new DataView(bytes.buffer);bytes.set([68,68,83,32]);view.setUint32(4,124,true);view.setUint32(12,4,true);view.setUint32(16,4,true);view.setUint32(28,1,true);view.setUint32(76,32,true);view.setUint32(80,4,true);bytes.set([...fourCC].map((char)=>char.charCodeAt(0)),84);bytes.set(block,128);return bytes;}
function legacyFloatDds(width,height,format,pixels){const bytes=new Uint8Array(128+pixels.byteLength),view=new DataView(bytes.buffer);bytes.set([68,68,83,32]);view.setUint32(4,124,true);view.setUint32(12,height,true);view.setUint32(16,width,true);view.setUint32(20,width*16,true);view.setUint32(28,1,true);view.setUint32(76,32,true);view.setUint32(80,4,true);view.setUint32(84,format,true);bytes.set(new Uint8Array(pixels.buffer,pixels.byteOffset,pixels.byteLength),128);return bytes;}

test("decodes legacy 24-bit BGR DDS pixels through channel masks", () => {
  const bytes = rawDds(2, 1, 24, [0xff0000, 0xff00, 0xff, 0], [30, 20, 10, 60, 50, 40]);
  const descriptor = inspectDds(bytes), [level] = decodeDdsRgba(bytes, descriptor);
  assert.equal(descriptor.format, "RAW_24");
  assert.deepEqual([...level.pixels], [10, 20, 30, 255, 40, 50, 60, 255]);
});

test("decodes luminance-alpha DDS pixels", () => {
  const bytes = rawDds(2, 1, 16, [0xff, 0, 0, 0xff00], [32, 64, 128, 255], 0x20001);
  assert.deepEqual([...decodeDdsRgba(bytes)[0].pixels], [32, 32, 32, 64, 128, 128, 128, 255]);
});

test("decodes RGB565 DDS pixels", () => {
  const bytes = rawDds(2, 1, 16, [0xf800, 0x07e0, 0x001f, 0], [0x00, 0xf8, 0xe0, 0x07]);
  assert.deepEqual([...decodeDdsRgba(bytes)[0].pixels], [255, 0, 0, 255, 0, 255, 0, 255]);
});

test("recognizes legacy BC5 and reconstructs a positive normal Z channel", () => {
  const bytes = new Uint8Array(128 + 16), view = new DataView(bytes.buffer);
  bytes.set([68, 68, 83, 32]); view.setUint32(4, 124, true); view.setUint32(12, 4, true); view.setUint32(16, 4, true); view.setUint32(28, 1, true); view.setUint32(76, 32, true); view.setUint32(80, 4, true); bytes.set([65, 84, 73, 50], 84);
  bytes.set([128, 128], 128); bytes.set([128, 128], 136);
  const descriptor = inspectDds(bytes), pixels = decodeDdsRgba(bytes, descriptor)[0].pixels;
  assert.equal(descriptor.format, "BC5_UNORM");
  assert.deepEqual([...pixels.slice(0, 4)], [128, 128, 255, 255]);
});

test("software-decodes BC1 colors and transparent palette entries",()=>{const opaque=legacyBcDds("DXT1",[0x00,0xf8,0xe0,0x07,0xe4,0,0,0]),transparent=legacyBcDds("DXT1",[0,0,0xff,0xff,3,0,0,0]);const pixels=decodeDdsRgba(opaque)[0].pixels;assert.deepEqual([...pixels.slice(0,16)],[255,0,0,255,0,255,0,255,170,85,0,255,85,170,0,255]);assert.equal(decodeDdsRgba(transparent)[0].pixels[3],0);});

test("software-decodes BC2 and BC3 alpha blocks",()=>{const color=[0x00,0xf8,0xe0,0x07,0,0,0,0],bc2=legacyBcDds("DXT3",[0x0f,0,0,0,0,0,0,0,...color]),bc3=legacyBcDds("DXT5",[255,0,1,0,0,0,0,0,...color]);assert.deepEqual([...decodeDdsRgba(bc2)[0].pixels.slice(3,8)],[255,255,0,0,0]);assert.equal(decodeDdsRgba(bc3)[0].pixels[3],0);});

test("decodes DX10 RGBA8 and recognizes BC7 GPU blocks", () => {
  const rgba = dx10Dds(1, 1, 28, [10, 20, 30, 40]), bc7 = dx10Dds(4, 4, 98, new Uint8Array(16));
  assert.equal(inspectDds(rgba).format, "RGBA8");
  assert.deepEqual([...decodeDdsRgba(rgba)[0].pixels], [10, 20, 30, 40]);
  const descriptor = inspectDds(bc7);
  assert.equal(descriptor.format, "BC7");
  assert.equal(descriptor.blockBytes, 16);
});

test("recognizes legacy D3D9 float panorama textures",()=>{const bytes=legacyFloatDds(2,1,116,new Float32Array([.1,.2,.3,1,.4,.5,.6,1])),descriptor=inspectDds(bytes);assert.equal(descriptor.format,"RGBA32F");assert.equal(descriptor.compressed,false);assert.equal(descriptor.float,true);assert.equal(descriptor.channels,4);assert.equal(descriptor.bitsPerPixel,128);});

test("recognizes the installed Hangar HDR panorama",async(t)=>{let data;try{data=await readFile(hangarFixture);}catch{t.skip("Assetto Corsa showroom fixture is not installed");return;}const model=parseKn5(data),texture=model.textures.find((entry)=>entry.name==="Old_Hangar4.dds"),descriptor=inspectDds(texture?.data);assert.ok(texture);assert.equal(descriptor.format,"RGBA32F");assert.equal(descriptor.width,6000);assert.equal(descriptor.height,3000);assert.equal(texture.size,128+6000*3000*16);});

test("decodes a real Kunos raw DDS and identifies its embedded PNGs", async (t) => {
  let data;
  try { data = await readFile(abarthFixture); } catch { t.skip("Assetto Corsa fixture is not installed"); return; }
  const model = parseKn5(data), raw = model.textures.find((texture) => texture.name.toLowerCase() === "500abarth_racing_mechanics.dds"), png = model.textures.find((texture) => texture.name === "500_Abarth_Racing_GRID_White_D.png");
  assert.ok(raw); assert.ok(png);
  const descriptor = inspectDds(raw.data), [level] = decodeDdsRgba(raw.data, descriptor);
  assert.equal(descriptor.format, "RAW_24");
  assert.equal(level.pixels.length, descriptor.width * descriptor.height * 4);
  assert.ok(new Set(level.pixels.filter((_, index) => index % 4 !== 3)).size > 32);
  assert.deepEqual([...png.data.slice(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
});
