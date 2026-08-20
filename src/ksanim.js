const decoder = new TextDecoder("utf-8", { fatal: true });
const encoder = new TextEncoder();

export class KsAnimationError extends Error {
  constructor(message, offset) {
    super(`${message} (at 0x${offset.toString(16)})`);
    this.name = "KsAnimationError";
    this.offset = offset;
  }
}

class Reader {
  constructor(input) {
    this.bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
    this.offset = 0;
  }
  need(size, label) {
    if (size < 0 || this.offset + size > this.bytes.byteLength) throw new KsAnimationError(`Unexpected end of file while reading ${label}`, this.offset);
  }
  u32(label) { this.need(4, label); const value = this.view.getUint32(this.offset, true); this.offset += 4; return value; }
  f32(label) { this.need(4, label); const value = this.view.getFloat32(this.offset, true); this.offset += 4; return value; }
  string(label) {
    const length = this.u32(`${label} length`);
    if (length > 1024 * 1024) throw new KsAnimationError(`Invalid ${label} length ${length}`, this.offset - 4);
    this.need(length, label);
    let value;
    try { value = decoder.decode(this.bytes.subarray(this.offset, this.offset + length)); }
    catch { throw new KsAnimationError(`Invalid UTF-8 in ${label}`, this.offset); }
    this.offset += length;
    return value;
  }
  floats(count, label) { return Array.from({ length: count }, () => this.f32(label)); }
}

function normalizedQuaternion(value) {
  const length = Math.hypot(...value);
  return length > Number.EPSILON ? value.map((component) => component / length) : [0, 0, 0, 1];
}

function matrixQuaternion(matrix, scale) {
  const m = [...matrix];
  for (let row = 0; row < 3; row++) {
    const divisor = Math.abs(scale[row]) > Number.EPSILON ? scale[row] : 1;
    for (let column = 0; column < 3; column++) m[row * 4 + column] /= divisor;
  }
  const trace = m[0] + m[5] + m[10];
  let x, y, z, w;
  if (trace > 0) {
    const s = Math.sqrt(trace + 1) * 2;
    w = s / 4; x = (m[6] - m[9]) / s; y = (m[8] - m[2]) / s; z = (m[1] - m[4]) / s;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const s = Math.sqrt(1 + m[0] - m[5] - m[10]) * 2;
    w = (m[6] - m[9]) / s; x = s / 4; y = (m[4] + m[1]) / s; z = (m[8] + m[2]) / s;
  } else if (m[5] > m[10]) {
    const s = Math.sqrt(1 + m[5] - m[0] - m[10]) * 2;
    w = (m[8] - m[2]) / s; x = (m[4] + m[1]) / s; y = s / 4; z = (m[9] + m[6]) / s;
  } else {
    const s = Math.sqrt(1 + m[10] - m[0] - m[5]) * 2;
    w = (m[1] - m[4]) / s; x = (m[8] + m[2]) / s; y = (m[9] + m[6]) / s; z = s / 4;
  }
  return normalizedQuaternion([x, y, z, w]);
}

export function decomposeKsAnimationMatrix(matrix) {
  const scale = [Math.hypot(matrix[0], matrix[1], matrix[2]), Math.hypot(matrix[4], matrix[5], matrix[6]), Math.hypot(matrix[8], matrix[9], matrix[10])];
  return { quaternion: matrixQuaternion(matrix, scale), position: matrix.slice(12, 15), scale };
}

export function ksAnimationMatrix(frame) {
  const [x, y, z, w] = normalizedQuaternion(frame.quaternion), [sx, sy, sz] = frame.scale, [px, py, pz] = frame.position;
  return [
    (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0,
    (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0,
    (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0,
    px, py, pz, 1
  ];
}

function frameChanged(first, candidate) {
  return first.quaternion.some((value, index) => !Object.is(value, candidate.quaternion[index])) || first.position.some((value, index) => !Object.is(value, candidate.position[index])) || first.scale.some((value, index) => !Object.is(value, candidate.scale[index]));
}

function rawFramesChanged(bytes, start, count, frameSize) {
  for (let frame = 1; frame < count; frame++) for (let offset = 0; offset < frameSize; offset++) if (bytes[start + offset] !== bytes[start + frame * frameSize + offset]) return true;
  return false;
}

export function parseKsAnimation(input, source = "animation.ksanim") {
  const reader = new Reader(input), version = reader.u32("version");
  if (version !== 1 && version !== 2) throw new KsAnimationError(`Unsupported KSANIM version ${version}`, 0);
  const trackCount = reader.u32("track count");
  if (trackCount > 1_000_000) throw new KsAnimationError(`Invalid track count ${trackCount}`, 4);
  const tracks = [], warnings = [];
  let frameCount = null;
  for (let trackIndex = 0; trackIndex < trackCount; trackIndex++) {
    const name = reader.string("track name"), countOffset = reader.offset, count = reader.u32("frame count"), frameSize = version === 1 ? 64 : 40;
    if (count > 10_000_000 || count * frameSize > reader.bytes.byteLength - reader.offset) throw new KsAnimationError(`Invalid frame count ${count} for ${name}`, countOffset);
    const frames = [], framesOffset = reader.offset;
    for (let frameIndex = 0; frameIndex < count; frameIndex++) {
      if (version === 1) frames.push(decomposeKsAnimationMatrix(reader.floats(16, `${name} matrix frame`)));
      else frames.push({ quaternion: reader.floats(4, `${name} quaternion frame`), position: reader.floats(3, `${name} position frame`), scale: reader.floats(3, `${name} scale frame`) });
    }
    if (frameCount === null) frameCount = count;
    else if (count !== frameCount) warnings.push(`${source}: ${name} has ${count} frames; the first track has ${frameCount}`);
    tracks.push({ name, frames, animated: frames.length > 1 && (version === 2 ? rawFramesChanged(reader.bytes, framesOffset, count, frameSize) : frames.slice(1).some((frame) => frameChanged(frames[0], frame))) });
  }
  if (reader.offset !== reader.bytes.byteLength) throw new KsAnimationError(`${reader.bytes.byteLength - reader.offset} trailing bytes after KSANIM data`, reader.offset);
  return { source, version, tracks, frameCount: frameCount || 0, bytesRead: reader.offset, byteLength: reader.bytes.byteLength, warnings };
}

function finiteFrameValues(frame, trackName, frameIndex) {
  const groups = [["quaternion", 4], ["position", 3], ["scale", 3]], values = [];
  for (const [name, count] of groups) {
    const data = frame?.[name];
    if ((!Array.isArray(data) && !ArrayBuffer.isView(data)) || data.length !== count) {
      throw new TypeError(`${trackName} frame ${frameIndex} must contain ${count} ${name} values`);
    }
    for (const value of data) {
      if (!Number.isFinite(value)) throw new TypeError(`${trackName} frame ${frameIndex} contains a non-finite ${name} value`);
      values.push(value);
    }
  }
  return values;
}

/** Serialize animation tracks with the KSANIM v2 quatpos layout used by ksEditor. */
export function serializeKsAnimation(animation) {
  const tracks = animation?.tracks;
  if (!Array.isArray(tracks)) throw new TypeError("KSANIM tracks must be an array");
  if (tracks.length > 1_000_000) throw new RangeError(`KSANIM has too many tracks: ${tracks.length}`);
  const prepared = [];
  let byteLength = 8;
  for (let trackIndex = 0; trackIndex < tracks.length; trackIndex++) {
    const track = tracks[trackIndex], name = String(track?.name || ""), nameBytes = encoder.encode(name), frames = track?.frames;
    if (!name) throw new TypeError(`KSANIM track ${trackIndex} has no name`);
    if (nameBytes.byteLength > 1024 * 1024) throw new RangeError(`KSANIM track name is too long: ${name}`);
    if (!Array.isArray(frames)) throw new TypeError(`${name} frames must be an array`);
    if (frames.length > 10_000_000) throw new RangeError(`${name} has too many frames: ${frames.length}`);
    const values = frames.map((frame, frameIndex) => finiteFrameValues(frame, name, frameIndex));
    byteLength += 8 + nameBytes.byteLength + frames.length * 40;
    if (!Number.isSafeInteger(byteLength) || byteLength > 0xffffffff) throw new RangeError("KSANIM output is too large");
    prepared.push({ nameBytes, values });
  }
  const bytes = new Uint8Array(byteLength), view = new DataView(bytes.buffer);
  let offset = 0;
  const u32 = (value) => { view.setUint32(offset, value, true); offset += 4; };
  const f32 = (value) => { view.setFloat32(offset, value, true); offset += 4; };
  u32(2); u32(prepared.length);
  for (const track of prepared) {
    u32(track.nameBytes.byteLength); bytes.set(track.nameBytes, offset); offset += track.nameBytes.byteLength;
    u32(track.values.length);
    for (const frame of track.values) for (const value of frame) f32(value);
  }
  return bytes;
}

function slerpQuaternion(from, to, amount) {
  const source=normalizedQuaternion(from);let target = normalizedQuaternion(to), dot = source.reduce((sum, value, index) => sum + value * target[index], 0);
  if (dot < 0) { target = target.map((value) => -value); dot = -dot; }
  if (dot > 0.9995) return normalizedQuaternion(source.map((value, index) => value + (target[index] - value) * amount));
  const angle = Math.acos(Math.max(-1, Math.min(1, dot))), sine = Math.sin(angle);
  const a = Math.sin((1 - amount) * angle) / sine, b = Math.sin(amount * angle) / sine;
  return source.map((value, index) => value * a + target[index] * b);
}

export function sampleKsAnimationTrack(track, position) {
  if (!track?.frames?.length) return null;
  const value = Math.max(0, Math.min(1, Number(position) || 0)), count = track.frames.length;
  if (count === 1 || value >= (count - 1) / count) return ksAnimationMatrix(track.frames[count - 1]);
  const scaled = count * value, frameIndex = Math.floor(scaled), amount = scaled - frameIndex, from = track.frames[frameIndex], to = track.frames[frameIndex + 1];
  const interpolate = (a, b) => a.map((component, index) => component + (b[index] - component) * amount);
  return ksAnimationMatrix({ quaternion: slerpQuaternion(from.quaternion, to.quaternion, amount), position: interpolate(from.position, to.position), scale: interpolate(from.scale, to.scale) });
}

export function sampleKsAnimation(animation, position) {
  const result = new Map();
  for (const track of animation?.tracks || []) if (track.animated) result.set(track.name, sampleKsAnimationTrack(track, position));
  return result;
}

/** KN5 animation tracks replace transforms on null nodes. Mesh records have no local transform. */
export function animationTransformForNode(node, transforms) {
  return node?.kind === "node" ? transforms?.get(node.name) : undefined;
}
