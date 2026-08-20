const RAND_MAX = 0x7fff;
const RAND_SCALE = Math.fround(1 / RAND_MAX);
const IDENTITY = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

function finite(value, fallback = 0) {
  const rounded = Math.fround(Number(value)), fallbackRounded = Math.fround(Number(fallback));
  return Number.isFinite(rounded) ? rounded : Number.isFinite(fallbackRounded) ? fallbackRounded : 0;
}

function isFiniteFloat32(value) {
  return Number.isFinite(Math.fround(Number(value)));
}

function vector(value, fallback = [0, 0, 0], warnings = null, label = "Vector") {
  if (value === undefined || value === null) return fallback.map((item) => finite(item));
  const input = Array.from(value || [], Number), rounded = input.map(Math.fround);
  if (rounded.length === 3 && rounded.every(Number.isFinite)) return rounded;
  warnings?.push(`${label} must contain three finite float32 values; the preview uses its fallback`);
  return fallback.map((item) => finite(item));
}

function randomUnit(random) {
  return Math.fround(random.nextInt() * RAND_SCALE);
}

function randomRange(random, minimum, maximum) {
  const span = finite(maximum - minimum);
  return finite(finite(randomUnit(random) * span) + minimum, minimum);
}

function randomCenteredAxis(random, center, range) {
  const minimum = finite(-range);
  return finite(randomRange(random, minimum, range) + center, center);
}

/** Preserve a loaded KN5 root basis while replacing its native translation fields. */
export function dynamicTrackRootTransform(rootTransform, position) {
  const output = Array.from({ length: 16 }, (_, index) => finite(rootTransform?.[index], IDENTITY[index]));
  const translation = vector(position);
  output[12] = translation[0]; output[13] = translation[1]; output[14] = translation[2];
  return output;
}

function normalizeSampledMultiplicity(value, warnings, label) {
  if (!Number.isFinite(value) || value <= 0) {
    if (!Number.isFinite(value)) warnings.push(`${label} produced a non-finite multiplicity; the preview uses 0 instances`);
    return 0;
  }
  if (value > 0xffffffff) {
    warnings.push(`${label} exceeded the unsigned 32-bit instance count; the preview uses 4294967295 instances`);
    return 0xffffffff;
  }
  return Math.trunc(value);
}

export function createMsvcRandom(seed = 1) {
  let state = Number(seed) >>> 0;
  return {
    nextInt() {
      state = (Math.imul(state, 0x343fd) + 0x269ec3) >>> 0;
      return (state >>> 16) & RAND_MAX;
    },
    skip(count) {
      let remaining = BigInt(Math.max(0, Math.trunc(Number(count) || 0)));
      let multiplier = 0x343fdn, increment = 0x269ec3n;
      let accumulatedMultiplier = 1n, accumulatedIncrement = 0n;
      const mask = 0xffffffffn;
      while (remaining) {
        if (remaining & 1n) {
          accumulatedIncrement = (multiplier * accumulatedIncrement + increment) & mask;
          accumulatedMultiplier = (multiplier * accumulatedMultiplier) & mask;
        }
        increment = (multiplier * increment + increment) & mask;
        multiplier = (multiplier * multiplier) & mask;
        remaining >>= 1n;
      }
      state = Number((accumulatedMultiplier * BigInt(state) + accumulatedIncrement) & mask);
    },
    get state() { return state; }
  };
}

export function sampleDynamicTrackObjects(files, seed = 1, options = {}) {
  const random = createMsvcRandom(seed);
  const maximumPreviewInstances = Math.max(1, Math.trunc(Number(options.maximumPreviewInstances) || 256));
  const serverTime = finite(options.serverTime);
  const results = [];
  const warnings = [];
  let nativeInstances = 0;

  for (let fileIndex = 0; fileIndex < (files || []).length; fileIndex++) {
    const dynamic = files[fileIndex]?.dynamic;
    if (!dynamic) continue;

    const label = files[fileIndex].name || dynamic.section || `DYNAMIC_OBJECT_${dynamic.index ?? fileIndex}`;
    const probability = Math.trunc(Number(dynamic.probability) || 0);
    const probabilitySample = Math.fround(Math.fround(randomUnit(random) * 100));
    const accepted = probabilitySample < probability;
    let sampledMultiplicity = 0;
    if (accepted) {
      const minimumInput = dynamic.multiplicity?.[0] ?? 1, maximumInput = dynamic.multiplicity?.[1] ?? 1;
      const validMultiplicity = isFiniteFloat32(minimumInput) && isFiniteFloat32(maximumInput) && isFiniteFloat32(Number(maximumInput) + 1);
      let sampled;
      if (validMultiplicity) sampled = randomRange(random, finite(minimumInput, 1), finite(Number(maximumInput) + 1, 2));
      else { randomUnit(random); sampled = NaN; }
      sampledMultiplicity = normalizeSampledMultiplicity(sampled, warnings, label);
    }

    const available = Math.max(0, maximumPreviewInstances - nativeInstances);
    const previewMultiplicity = Math.min(sampledMultiplicity, available);
    if (sampledMultiplicity > previewMultiplicity) warnings.push(
      `${files[fileIndex].name || dynamic.section || `DYNAMIC_OBJECT_${dynamic.index ?? fileIndex}`} sampled ${sampledMultiplicity} instances; the preview limit is ${maximumPreviewInstances}`
    );

    const positionCenter = vector(dynamic.positionCenter, [0, 0, 0], warnings, `${label} RND_POS_CENTER`);
    const positionRange = vector(dynamic.positionRange, [0, 0, 0], warnings, `${label} RND_POS_RANGE`);
    const velocityBase = vector(dynamic.velocityBase, [0, 0, 0], warnings, `${label} RND_VEL_BASE`);
    const velocityRange = vector(dynamic.velocityRange, [0, 0, 0], warnings, `${label} RND_VEL_RANGE`);
    const randomPosition = String(dynamic.posMode || "") === "RANDOM";
    const randomVelocity = String(dynamic.velMode || "") === "RANDOM";
    const instances = [];

    for (let instanceIndex = 0; instanceIndex < previewMultiplicity; instanceIndex++) {
      const position = randomPosition ? [
        0, 0, randomCenteredAxis(random, positionCenter[2], positionRange[2])
      ] : [0, 0, 0];
      if (randomPosition) {
        position[1] = randomCenteredAxis(random, positionCenter[1], positionRange[1]);
        position[0] = randomCenteredAxis(random, positionCenter[0], positionRange[0]);
      }
      const velocity = randomVelocity ? [
        0, 0, randomCenteredAxis(random, velocityBase[2], velocityRange[2])
      ] : [0, 0, 0];
      if (randomVelocity) {
        velocity[1] = randomCenteredAxis(random, velocityBase[1], velocityRange[1]);
        velocity[0] = randomCenteredAxis(random, velocityBase[0], velocityRange[0]);
      }
      for (let axis = 0; axis < 3; axis++) position[axis] = finite(position[axis] + finite(serverTime * velocity[axis]), position[axis]);
      instances.push({ fileIndex, instanceIndex, position, velocity });
    }
    const callsPerOmittedInstance = (randomPosition ? 3 : 0) + (randomVelocity ? 3 : 0);
    random.skip((sampledMultiplicity - previewMultiplicity) * callsPerOmittedInstance);

    nativeInstances += sampledMultiplicity;
    results.push({ fileIndex, index: dynamic.index, probabilitySample, accepted, sampledMultiplicity, previewMultiplicity, instances });
  }

  return {
    seed: Number(seed) >>> 0,
    serverTime,
    elapsed: 0,
    nativeInstances,
    previewInstances: results.reduce((sum, result) => sum + result.instances.length, 0),
    results,
    warnings
  };
}

export function advanceDynamicTrackObjects(state, deltaTime) {
  if (!state) return null;
  const delta = finite(Math.max(0, Number(deltaTime) || 0));
  state.elapsed += delta;
  for (const result of state.results) for (const instance of result.instances) {
    for (let axis = 0; axis < 3; axis++) instance.position[axis] = finite(instance.position[axis] + finite(delta * instance.velocity[axis]), instance.position[axis]);
  }
  return state;
}
