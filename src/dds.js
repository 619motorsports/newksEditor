const DDS_MAGIC = 0x20534444;
const DDPF_FOURCC = 0x4;
const DDPF_LUMINANCE = 0x20000;

function bytesOf(input) {
  return input instanceof Uint8Array ? input : new Uint8Array(input);
}

function ascii(bytes, offset, length) {
  return String.fromCharCode(...bytes.subarray(offset, offset + length)).replaceAll("\0", "");
}

const legacyFormats = new Map([
  ["DXT1", { format: "BC1", blockBytes: 8, cpuFallback: "bc1" }],
  ["DXT2", { format: "BC2_PREMULT", blockBytes: 16, cpuFallback: "bc2", premultiplied: true }],
  ["DXT3", { format: "BC2", blockBytes: 16, cpuFallback: "bc2" }],
  ["DXT4", { format: "BC3_PREMULT", blockBytes: 16, cpuFallback: "bc3", premultiplied: true }],
  ["DXT5", { format: "BC3", blockBytes: 16, cpuFallback: "bc3" }],
  ["ATI1", { format: "BC4_UNORM", blockBytes: 8, cpu: "bc4" }],
  ["BC4U", { format: "BC4_UNORM", blockBytes: 8, cpu: "bc4" }],
  ["BC4S", { format: "BC4_SNORM", blockBytes: 8, cpu: "bc4", signed: true }],
  ["ATI2", { format: "BC5_UNORM", blockBytes: 16, cpu: "bc5" }],
  ["BC5U", { format: "BC5_UNORM", blockBytes: 16, cpu: "bc5" }],
  ["BC5S", { format: "BC5_SNORM", blockBytes: 16, cpu: "bc5", signed: true }]
]);

// D3DFORMAT values stored in the legacy FOURCC field by D3DXSaveTextureToFile.
const legacyFloatFormats = new Map([
  [111, { format: "R16F", compressed: false, float: true, channels: 1, componentBytes: 2, bitsPerPixel: 16 }],
  [112, { format: "RG16F", compressed: false, float: true, channels: 2, componentBytes: 2, bitsPerPixel: 32 }],
  [113, { format: "RGBA16F", compressed: false, float: true, channels: 4, componentBytes: 2, bitsPerPixel: 64 }],
  [114, { format: "R32F", compressed: false, float: true, channels: 1, componentBytes: 4, bitsPerPixel: 32 }],
  [115, { format: "RG32F", compressed: false, float: true, channels: 2, componentBytes: 4, bitsPerPixel: 64 }],
  [116, { format: "RGBA32F", compressed: false, float: true, channels: 4, componentBytes: 4, bitsPerPixel: 128 }]
]);

const dxgiFormats = new Map([
  [28, { format: "RGBA8", compressed: false, bitsPerPixel: 32, masks: [0xff, 0xff00, 0xff0000, 0xff000000] }],
  [29, { format: "RGBA8_SRGB", compressed: false, bitsPerPixel: 32, masks: [0xff, 0xff00, 0xff0000, 0xff000000] }],
  [61, { format: "R8", compressed: false, bitsPerPixel: 8, masks: [0xff, 0, 0, 0], luminance: true }],
  [71, { format: "BC1", compressed: true, blockBytes: 8, cpuFallback: "bc1" }], [72, { format: "BC1_SRGB", compressed: true, blockBytes: 8, cpuFallback: "bc1" }],
  [74, { format: "BC2", compressed: true, blockBytes: 16, cpuFallback: "bc2" }], [75, { format: "BC2_SRGB", compressed: true, blockBytes: 16, cpuFallback: "bc2" }],
  [77, { format: "BC3", compressed: true, blockBytes: 16, cpuFallback: "bc3" }], [78, { format: "BC3_SRGB", compressed: true, blockBytes: 16, cpuFallback: "bc3" }],
  [80, { format: "BC4_UNORM", compressed: true, blockBytes: 8, cpu: "bc4" }], [81, { format: "BC4_SNORM", compressed: true, blockBytes: 8, cpu: "bc4", signed: true }],
  [83, { format: "BC5_UNORM", compressed: true, blockBytes: 16, cpu: "bc5" }], [84, { format: "BC5_SNORM", compressed: true, blockBytes: 16, cpu: "bc5", signed: true }],
  [87, { format: "BGRA8", compressed: false, bitsPerPixel: 32, masks: [0xff0000, 0xff00, 0xff, 0xff000000] }],
  [91, { format: "BGRA8_SRGB", compressed: false, bitsPerPixel: 32, masks: [0xff0000, 0xff00, 0xff, 0xff000000] }],
  [95, { format: "BC6H_UF16", compressed: true, blockBytes: 16 }], [96, { format: "BC6H_SF16", compressed: true, blockBytes: 16 }],
  [98, { format: "BC7", compressed: true, blockBytes: 16 }], [99, { format: "BC7_SRGB", compressed: true, blockBytes: 16 }]
]);

export function inspectDds(input) {
  const bytes = bytesOf(input);
  if (bytes.byteLength < 128) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) !== DDS_MAGIC || view.getUint32(4, true) !== 124 || view.getUint32(76, true) !== 32) return null;
  const width = view.getUint32(16, true), height = view.getUint32(12, true);
  if (!width || !height || width > 32768 || height > 32768) return null;
  const mipCount = Math.max(1, view.getUint32(28, true));
  const pixelFlags = view.getUint32(80, true), fourCCValue=view.getUint32(84,true),fourCC = ascii(bytes, 84, 4);
  if (pixelFlags & DDPF_FOURCC) {
    if (fourCC === "DX10") {
      if (bytes.byteLength < 148) return null;
      const dxgi = view.getUint32(128, true), known = dxgiFormats.get(dxgi);
      return known ? { pitch: view.getUint32(20, true), luminance: false, ...known, width, height, mipCount, dataOffset: 148, dxgi } : { width, height, mipCount, dataOffset: 148, compressed: true, format: `DXGI_${dxgi}`, dxgi };
    }
    const floatFormat=legacyFloatFormats.get(fourCCValue);if(floatFormat)return {...floatFormat,width,height,mipCount,dataOffset:128,pitch:view.getUint32(20,true),fourCCValue};
    const known = legacyFormats.get(fourCC);
    return known ? { ...known, width, height, mipCount, dataOffset: 128, compressed: true, fourCC } : { width, height, mipCount, dataOffset: 128, compressed: true, format: `FOURCC_${fourCC || "0"}`, fourCC };
  }
  const bitsPerPixel = view.getUint32(88, true);
  if (![8, 16, 24, 32].includes(bitsPerPixel)) return null;
  return {
    width, height, mipCount, dataOffset: 128, compressed: false, format: `RAW_${bitsPerPixel}`,
    bitsPerPixel, pitch: view.getUint32(20, true), luminance: Boolean(pixelFlags & DDPF_LUMINANCE),
    masks: [view.getUint32(92, true), view.getUint32(96, true), view.getUint32(100, true), view.getUint32(104, true)]
  };
}

function maskByte(pixel, mask) {
  if (!mask) return 0;
  let shift = 0;
  while (shift < 32 && ((mask >>> shift) & 1) === 0) shift++;
  const maximum = mask >>> shift;
  const value = (pixel & mask) >>> shift;
  return maximum ? Math.round(value * 255 / maximum) : 0;
}

function rawLevels(bytes, descriptor) {
  const levels = [];
  const bytesPerPixel = descriptor.bitsPerPixel / 8;
  let offset = descriptor.dataOffset, width = descriptor.width, height = descriptor.height;
  for (let level = 0; level < descriptor.mipCount; level++) {
    const minimumPitch = width * bytesPerPixel;
    const rowPitch = level === 0 && descriptor.pitch >= minimumPitch && descriptor.pitch <= minimumPitch + 16 ? descriptor.pitch : minimumPitch;
    const size = rowPitch * height;
    if (offset + size > bytes.byteLength) throw new Error(`DDS mip ${level} exceeds texture data`);
    const pixels = new Uint8Array(width * height * 4), [rMask, gMask, bMask, aMask] = descriptor.masks;
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const source = offset + y * rowPitch + x * bytesPerPixel;
      let packed = 0;
      for (let byte = 0; byte < bytesPerPixel; byte++) packed += bytes[source + byte] * 2 ** (byte * 8);
      packed >>>= 0;
      const target = (y * width + x) * 4, red = maskByte(packed, rMask);
      pixels[target] = red;
      pixels[target + 1] = descriptor.luminance || !gMask ? red : maskByte(packed, gMask);
      pixels[target + 2] = descriptor.luminance || !bMask ? red : maskByte(packed, bMask);
      pixels[target + 3] = aMask ? maskByte(packed, aMask) : 255;
    }
    levels.push({ width, height, pixels });
    offset += size; width = Math.max(1, width >> 1); height = Math.max(1, height >> 1);
  }
  return levels;
}

function bcPalette(data, offset, signed) {
  const convert = (value) => signed ? Math.max(-127, value > 127 ? value - 256 : value) : value;
  const a = convert(data[offset]), b = convert(data[offset + 1]), values = [a, b];
  if (a > b) for (let index = 1; index <= 6; index++) values.push(((7 - index) * a + index * b) / 7);
  else {
    for (let index = 1; index <= 4; index++) values.push(((5 - index) * a + index * b) / 5);
    values.push(signed ? -127 : 0, signed ? 127 : 255);
  }
  return values.map((value) => signed ? Math.round((Math.max(-127, Math.min(127, value)) / 127 * .5 + .5) * 255) : Math.round(value));
}

function bcIndex(data, offset, pixel) {
  const bit = pixel * 3, byte = offset + 2 + (bit >> 3), shift = bit & 7;
  return ((data[byte] | (data[byte + 1] || 0) << 8) >> shift) & 7;
}

function bcLevels(bytes, descriptor) {
  const levels = [];
  let offset = descriptor.dataOffset, width = descriptor.width, height = descriptor.height;
  for (let level = 0; level < descriptor.mipCount; level++) {
    const blocksWide = Math.max(1, Math.ceil(width / 4)), blocksHigh = Math.max(1, Math.ceil(height / 4)), size = blocksWide * blocksHigh * descriptor.blockBytes;
    if (offset + size > bytes.byteLength) throw new Error(`DDS mip ${level} exceeds texture data`);
    const pixels = new Uint8Array(width * height * 4);
    for (let blockY = 0; blockY < blocksHigh; blockY++) for (let blockX = 0; blockX < blocksWide; blockX++) {
      const block = offset + (blockY * blocksWide + blockX) * descriptor.blockBytes;
      const red = bcPalette(bytes, block, descriptor.signed), green = descriptor.cpu === "bc5" ? bcPalette(bytes, block + 8, descriptor.signed) : red;
      for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) {
        const px = blockX * 4 + x, py = blockY * 4 + y;
        if (px >= width || py >= height) continue;
        const pixel = y * 4 + x, r = red[bcIndex(bytes, block, pixel)], g = green[bcIndex(bytes, block + (descriptor.cpu === "bc5" ? 8 : 0), pixel)], target = (py * width + px) * 4;
        pixels[target] = r; pixels[target + 1] = g;
        if (descriptor.cpu === "bc5") {
          const nx = r / 127.5 - 1, ny = g / 127.5 - 1;
          pixels[target + 2] = Math.round((Math.sqrt(Math.max(0, 1 - nx * nx - ny * ny)) * .5 + .5) * 255);
        } else pixels[target + 2] = r;
        pixels[target + 3] = 255;
      }
    }
    levels.push({ width, height, pixels });
    offset += size; width = Math.max(1, width >> 1); height = Math.max(1, height >> 1);
  }
  return levels;
}

function rgb565(value){const red=(value>>11)&31,green=(value>>5)&63,blue=value&31;return [Math.round(red*255/31),Math.round(green*255/63),Math.round(blue*255/31)];}
function mixRgb(a,b,aWeight,divisor){return a.map((value,index)=>Math.round((value*aWeight+b[index]*(divisor-aWeight))/divisor));}

function bcColorLevels(bytes,descriptor){
  const levels=[];let offset=descriptor.dataOffset,width=descriptor.width,height=descriptor.height;
  for(let level=0;level<descriptor.mipCount;level++){
    const blocksWide=Math.max(1,Math.ceil(width/4)),blocksHigh=Math.max(1,Math.ceil(height/4)),size=blocksWide*blocksHigh*descriptor.blockBytes;
    if(offset+size>bytes.byteLength)throw new Error(`DDS mip ${level} exceeds texture data`);
    const pixels=new Uint8Array(width*height*4);
    for(let blockY=0;blockY<blocksHigh;blockY++)for(let blockX=0;blockX<blocksWide;blockX++){
      const block=offset+(blockY*blocksWide+blockX)*descriptor.blockBytes,colorOffset=block+(descriptor.cpuFallback==="bc1"?0:8),c0=bytes[colorOffset]|bytes[colorOffset+1]<<8,c1=bytes[colorOffset+2]|bytes[colorOffset+3]<<8,p0=rgb565(c0),p1=rgb565(c1),fourColor=descriptor.cpuFallback!=="bc1"||c0>c1,palette=[p0,p1,fourColor?mixRgb(p0,p1,2,3):mixRgb(p0,p1,1,2),fourColor?mixRgb(p0,p1,1,3):[0,0,0]],indices=(bytes[colorOffset+4]|bytes[colorOffset+5]<<8|bytes[colorOffset+6]<<16|bytes[colorOffset+7]<<24)>>>0,alphaPalette=descriptor.cpuFallback==="bc3"?bcPalette(bytes,block,false):null;
      for(let y=0;y<4;y++)for(let x=0;x<4;x++){
        const px=blockX*4+x,py=blockY*4+y;if(px>=width||py>=height)continue;const pixel=y*4+x,colorIndex=indices>>>(pixel*2)&3,color=palette[colorIndex],alpha=descriptor.cpuFallback==="bc1"?fourColor||colorIndex!==3?255:0:descriptor.cpuFallback==="bc2"?((bytes[block+(pixel>>1)]>>((pixel&1)*4))&15)*17:alphaPalette[bcIndex(bytes,block,pixel)],target=(py*width+px)*4;
        if(descriptor.premultiplied&&alpha>0&&alpha<255){pixels[target]=Math.min(255,Math.round(color[0]*255/alpha));pixels[target+1]=Math.min(255,Math.round(color[1]*255/alpha));pixels[target+2]=Math.min(255,Math.round(color[2]*255/alpha));}else{pixels[target]=color[0];pixels[target+1]=color[1];pixels[target+2]=color[2];}pixels[target+3]=alpha;
      }
    }
    levels.push({width,height,pixels});offset+=size;width=Math.max(1,width>>1);height=Math.max(1,height>>1);
  }
  return levels;
}

export function decodeDdsRgba(input, descriptor = inspectDds(input)) {
  if (!descriptor) throw new Error("Not a supported DDS header");
  const bytes = bytesOf(input);
  if (!descriptor.compressed) return rawLevels(bytes, descriptor);
  if (descriptor.cpu === "bc4" || descriptor.cpu === "bc5") return bcLevels(bytes, descriptor);
  if (descriptor.cpuFallback) return bcColorLevels(bytes,descriptor);
  throw new Error(`DDS ${descriptor.format} should use a GPU compressed-texture path`);
}
