// Ported from bcdec commit 93628fe5627102fe5187b7eeb99122dec6612c36.
// bcdec is Copyright (c) 2022 Sergii Kudlai and is used under the MIT license.
// See THIRD_PARTY_NOTICES.md for the complete license text.

const PARTITION_DATA = "gAABAQAAAQEAAAEBAAABgYAAAAEAAAABAAAAAQAAAIGAAQEBAAEBAQABAQEAAQGBgAAAAQAAAQEAAAEBAAEBgYAAAAAAAAABAAAAAQAAAYGAAAEBAAEBAQABAQEBAQGBgAAAAQAAAQEAAQEBAQEBgYAAAAAAAAABAAABAQABAYGAAAAAAAAAAAAAAAEAAAGBgAABAQABAQEBAQEBAQEBgYAAAAAAAAABAAEBAQEBAYGAAAAAAAAAAAAAAAEAAQGBgAAAAQABAQEBAQEBAQEBgYAAAAAAAAAAAQEBAQEBAYGAAAAAAQEBAQEBAQEBAQGBgAAAAAAAAAAAAAAAAQEBgYAAAAABAAAAAQEBAAEBAYGAAYEBAAAAAQAAAAAAAAAAgAAAAAAAAACBAAAAAQEBAIABgQEAAAEBAAAAAQAAAACAAIEBAAAAAQAAAAAAAAAAgAAAAAEAAACBAQAAAQEBAIAAAAAAAAAAgQAAAAEBAACAAQEBAAABAQAAAQEAAACBgACBAQAAAAEAAAABAAAAAIAAAAABAAAAgQAAAAEBAACAAYEAAAEBAAABAQAAAQEAgACBAQABAQAAAQEAAQEAAIAAAAEAAQEBgQEBAAEAAACAAAAAAQEBAYEBAQEAAAAAgAGBAQAAAAEBAAAAAQEBAIAAgQEBAAABAQAAAQEBAACAAQABAAEAAQABAAEAAQCBgAAAAAEBAQEAAAAAAQEBgYABAAEBAIEAAAEAAQEAAQCAAAEBAAABAYEBAAABAQAAgACBAQEBAAAAAAEBAQEAAIABAAEAAQABgQABAAEAAQCAAQEAAQAAAQABAQABAACBgAEAAQEAAQABAAEAAAEAgYABgQEAAAEBAQEAAAEBAQCAAAABAAABAYEBAAABAAAAgACBAQAAAQAAAQAAAQEAAIAAgQEBAAEBAQEAAQEBAACAAYEAAQAAAQEAAAEAAQEAgAABAQEBAAABAQAAAAABgYABAQAAAQEAAQAAAQEAAIGAAAAAAAGBAAABAQAAAAAAgAEAAAEBgQAAAQAAAAAAAIAAgQAAAQEBAAABAAAAAACAAAAAAACBAAABAQEAAAEAgAAAAAABAACBAQEAAAEAAIABAQABAQAAAQAAAQAAAYGAAAEBAAEBAAEBAAABAACBgAGBAAAAAQEBAAABAQEAAIAAgQEBAAABAQEAAAABAQCAAQEAAQEAAAEBAAABAACBgAEBAAAAAQEAAAEBAQAAgYABAQEBAQEAAQAAAAAAAIGAAAABAQAAAAEBAQAAAQGBgAAAAAEBAQEAAAEBAAABgYAAgQEAAAEBAQEBAQAAAACAAIEAAAABAAEBAQABAQEAgAEAAAABAAAAAQEBAAEBgYAAAYEAAAEBAAICAQICAoKAAACBAAABAYICAQECAgIBgAAAAAIAAAGCAgEBAgIBgYACAoIAAAICAAABAQABAYGAAAAAAAAAAIEBAgIBAQKCgAABgQAAAQEAAAICAAACgoAAAoIAAAICAQEBAQEBAYGAAAEBAAABAYICAQECAgGBgAAAAAAAAACBAQEBAgICgoAAAAABAQEBgQEBAQICAoKAAAAAAQGBAQICAgICAgKCgAABAgAAgQIAAAECAAABgoABAQIAAYECAAEBAgABAYKAAQICAIECAgABAgIAAQKCgAABgQABAQIBAQICAQICgoAAAYECAAABggIAAAICAgCAAACBAAABAQABAQIBAQKCgAEBgQAAAQGCAAABAgIAAIAAAAABAQICgQECAgEBAoKAAAKCAAACAgAAAgIBAQGBgAEBgQABAQEAAgICAAICgoAAAIEAAAABggICAQICAgGAAAAAAACBAQABAgIAAQKCgAAAAAEBAACCAoEAAgIBAIABAoIAgQICAAABAQAAAACAAAECAAABAoEBAgICAgKCgAEBAAECggGBAgIBAAEBAIAAAAAAAYEAAQKCAQECAgGAAAICAQEAAoEBAAIAAAKCgAEBAACBAQACAAACAgICgoAAAQEAAQICAAGCAgAAAYGAAAAAAgAAAIICAQECAgKBgAAAAAAAAAKBAQICAQICgoACAoIAAAICAAABAgAAAYGAAAGBAAABAgAAAgIAAgKCgAECAACBAgAAAYIAAAECAIAAAAABAYEBAgKCAgAAAACAAQIAAQIAAYIAgQIAAQIAgAECAAIAAQKBggABAAECAIAAAQECAgAAAQGCAgAAAYGAAAEBAQGCAgICAAAAAAGBgAEAgQABAAECAgICAgICgoAAAAAAAAAAggECAQIBAoGAAAICAYECAgAAAgIBAQKCgAACggAAAQEAAAICAAABgYACAgABAoIBAAICAAECAoGAAQABAgKCAgICAgIAAQCBgAAAAAIBAgGCAQIBAgECgYABAIEAAQABAAEAAQICAoKAAgKCAAEBAQACAgIAAQGBgAAAAgGBAQIAAAACAQEBgoAAAAACgQECAgEBAgIBAYKAAgICAIEBAQABAQEAAgKCgAAAAgEBAQKBAQECAAAAgoABAQAAgQEAAAEBAAICAoKAAAAAAAAAAAIBgQICAQGCgAEBAACBAQACAgICAgICgoAAAgIAAAEBAACBAQAAAoKAAAICAQECAoEBAgIAAAKCgAAAAAAAAAAAAAAAAoEBgoAAAIIAAAABAAAAAgAAAIGAAgICAQICAgACAgKBAgKCgAEAgQICAgICAgICAgICgoABAYECAAEBggIAAQICAgA=";

function decodeBase64(value) {
  const decoded = globalThis.atob(value), bytes = new Uint8Array(decoded.length);
  for (let index = 0; index < decoded.length; index++) bytes[index] = decoded.charCodeAt(index);
  return bytes;
}

const PARTITIONS = decodeBase64(PARTITION_DATA);
if (PARTITIONS.length !== 2048) throw new Error("BC7 partition table is invalid");
const ACTUAL_BITS = [[4, 6, 5, 7, 5, 7, 7, 5], [0, 0, 0, 0, 6, 8, 7, 5]];
const WEIGHTS = {
  2: [0, 21, 43, 64],
  3: [0, 9, 18, 27, 37, 46, 55, 64],
  4: [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]
};
const MODES_WITH_P_BITS = 0b11001011;
const DECODER_SCRATCH = {
  position: 0,
  endpoints: new Uint16Array(24),
  indices: new Uint8Array(16)
};

function bytesOf(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError("BC7 input must be an ArrayBuffer or typed array");
}

function readBits(bytes, inputOffset, scratch, count) {
  if (scratch.position + count > 128) throw new Error("BC7 block exceeds 128 bits");
  let value = 0;
  for (let bit = 0; bit < count; bit++, scratch.position++) {
    value |= ((bytes[inputOffset + (scratch.position >> 3)] >> (scratch.position & 7)) & 1) << bit;
  }
  return value;
}

function partitionSetAt(numPartitions, partition, row, column) {
  return numPartitions === 1
    ? (row | column ? 0 : 128)
    : PARTITIONS[((numPartitions - 2) * 64 + partition) * 16 + row * 4 + column];
}

function interpolate(first, second, weights, index) {
  const weight = weights[index];
  return (first * (64 - weight) + second * weight + 32) >> 6;
}

export function decodeBc7Block(input, inputOffset = 0, output = new Uint8Array(64), outputOffset = 0, outputPitch = 16) {
  const bytes = bytesOf(input);
  if (!Number.isSafeInteger(inputOffset) || inputOffset < 0 || inputOffset + 16 > bytes.byteLength) throw new RangeError("BC7 block needs 16 input bytes");
  if (!(output instanceof Uint8Array)) throw new TypeError("BC7 output must be a Uint8Array");
  if (!Number.isSafeInteger(outputOffset) || outputOffset < 0 || !Number.isSafeInteger(outputPitch) || outputPitch < 16 || outputOffset + outputPitch * 3 + 16 > output.byteLength) throw new RangeError("BC7 output cannot hold a 4x4 RGBA block");
  if (bytes[inputOffset] === 0) throw new Error("BC7 block has no valid mode");

  const scratch = DECODER_SCRATCH, endpoints = scratch.endpoints, indices = scratch.indices;
  scratch.position = 0; endpoints.fill(0);
  let mode = 0;
  while (mode < 8 && readBits(bytes, inputOffset, scratch, 1) === 0) mode++;
  if (mode === 8) throw new Error("BC7 block has no valid mode");

  let partition = 0, numPartitions = 1, rotation = 0, indexSelectionBit = 0;
  if (mode === 0 || mode === 1 || mode === 2 || mode === 3 || mode === 7) {
    numPartitions = mode === 0 || mode === 2 ? 3 : 2;
    partition = readBits(bytes, inputOffset, scratch, mode === 0 ? 4 : 6);
  }
  const numEndpoints = numPartitions * 2;
  if (mode === 4 || mode === 5) {
    rotation = readBits(bytes, inputOffset, scratch, 2);
    if (mode === 4) indexSelectionBit = readBits(bytes, inputOffset, scratch, 1);
  }

  for (let component = 0; component < 3; component++) {
    for (let endpoint = 0; endpoint < numEndpoints; endpoint++) endpoints[endpoint * 4 + component] = readBits(bytes, inputOffset, scratch, ACTUAL_BITS[0][mode]);
  }
  if (ACTUAL_BITS[1][mode]) {
    for (let endpoint = 0; endpoint < numEndpoints; endpoint++) endpoints[endpoint * 4 + 3] = readBits(bytes, inputOffset, scratch, ACTUAL_BITS[1][mode]);
  }

  if (mode === 0 || mode === 1 || mode === 3 || mode === 6 || mode === 7) {
    for (let endpoint = 0; endpoint < numEndpoints; endpoint++) {
      for (let component = 0; component < 4; component++) endpoints[endpoint * 4 + component] <<= 1;
    }
    if (mode === 1) {
      const first = readBits(bytes, inputOffset, scratch, 1), second = readBits(bytes, inputOffset, scratch, 1);
      for (let component = 0; component < 3; component++) {
        endpoints[component] |= first; endpoints[4 + component] |= first;
        endpoints[8 + component] |= second; endpoints[12 + component] |= second;
      }
    } else if (MODES_WITH_P_BITS & (1 << mode)) {
      for (let endpoint = 0; endpoint < numEndpoints; endpoint++) {
        const pBit = readBits(bytes, inputOffset, scratch, 1);
        for (let component = 0; component < 4; component++) endpoints[endpoint * 4 + component] |= pBit;
      }
    }
  }

  for (let endpoint = 0; endpoint < numEndpoints; endpoint++) {
    const colorBits = ACTUAL_BITS[0][mode] + ((MODES_WITH_P_BITS >> mode) & 1);
    for (let component = 0; component < 3; component++) {
      const target = endpoint * 4 + component;
      endpoints[target] <<= 8 - colorBits;
      endpoints[target] |= endpoints[target] >> colorBits;
    }
    const alphaBits = ACTUAL_BITS[1][mode] + ((MODES_WITH_P_BITS >> mode) & 1);
    const alpha = endpoint * 4 + 3;
    endpoints[alpha] <<= 8 - alphaBits;
    endpoints[alpha] |= endpoints[alpha] >> alphaBits;
  }
  if (!ACTUAL_BITS[1][mode]) {
    for (let endpoint = 0; endpoint < numEndpoints; endpoint++) endpoints[endpoint * 4 + 3] = 255;
  }

  const primaryBits = mode === 0 || mode === 1 ? 3 : mode === 6 ? 4 : 2;
  const secondaryBits = mode === 4 ? 3 : mode === 5 ? 2 : 0;
  const primaryWeights = WEIGHTS[primaryBits], secondaryWeights = WEIGHTS[secondaryBits];

  for (let row = 0; row < 4; row++) for (let column = 0; column < 4; column++) {
    const partitionSet = partitionSetAt(numPartitions, partition, row, column);
    indices[row * 4 + column] = readBits(bytes, inputOffset, scratch, primaryBits - (partitionSet & 0x80 ? 1 : 0));
  }

  for (let row = 0; row < 4; row++) for (let column = 0; column < 4; column++) {
    const subset = partitionSetAt(numPartitions, partition, row, column) & 3, first = subset * 8, second = first + 4, index = indices[row * 4 + column];
    let red, green, blue, alpha;
    if (!secondaryBits) {
      red = interpolate(endpoints[first], endpoints[second], primaryWeights, index);
      green = interpolate(endpoints[first + 1], endpoints[second + 1], primaryWeights, index);
      blue = interpolate(endpoints[first + 2], endpoints[second + 2], primaryWeights, index);
      alpha = interpolate(endpoints[first + 3], endpoints[second + 3], primaryWeights, index);
    } else {
      const index2 = readBits(bytes, inputOffset, scratch, secondaryBits - (row | column ? 0 : 1));
      const colorWeights = indexSelectionBit ? secondaryWeights : primaryWeights, colorIndex = indexSelectionBit ? index2 : index;
      const alphaWeights = indexSelectionBit ? primaryWeights : secondaryWeights, alphaIndex = indexSelectionBit ? index : index2;
      red = interpolate(endpoints[first], endpoints[second], colorWeights, colorIndex);
      green = interpolate(endpoints[first + 1], endpoints[second + 1], colorWeights, colorIndex);
      blue = interpolate(endpoints[first + 2], endpoints[second + 2], colorWeights, colorIndex);
      alpha = interpolate(endpoints[first + 3], endpoints[second + 3], alphaWeights, alphaIndex);
    }
    if (rotation === 1) { const swap = alpha; alpha = red; red = swap; }
    else if (rotation === 2) { const swap = alpha; alpha = green; green = swap; }
    else if (rotation === 3) { const swap = alpha; alpha = blue; blue = swap; }
    const target = outputOffset + row * outputPitch + column * 4;
    output[target] = red; output[target + 1] = green; output[target + 2] = blue; output[target + 3] = alpha;
  }
  return output;
}
