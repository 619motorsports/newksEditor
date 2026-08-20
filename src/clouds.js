export const KS_CLOUD_MAX_COUNT = 512;
export const KS_CLOUD_TEXTURE_PATHS = Object.freeze(Array.from({ length: 7 }, (_, index) => `content/texture/clouds/cloud${index + 1}C.dds`));

const DEG_TO_RAD = Math.PI / 180;
const KS_RAND_SCALE = 1 / 32767;

function finite(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function bounded(value, fallback, minimum, maximum) {
  return Math.max(minimum, Math.min(maximum, finite(value, fallback)));
}

export function normalizeKsCloudSettings(value = {}) {
  return Object.freeze({
    width: bounded(value.cloudWidth, 4, 0, 1000),
    height: bounded(value.cloudHeight, 2, 0, 1000),
    radius: bounded(value.cloudRadius, 4, 0, 1000),
    count: Math.floor(bounded(value.cloudNumber, 100, 0, KS_CLOUD_MAX_COUNT)),
    baseSpeed: bounded(value.cloudBaseSpeed, 0.01, 0, 1)
  });
}

/** Return the Visual C++ rand() sequence that the installed ksEditor uses. */
export function createKsCloudRandom(seed = 1) {
  let state = Number(seed) >>> 0;
  return () => {
    state = (Math.imul(state, 214013) + 2531011) >>> 0;
    return ((state >>> 16) & 0x7fff) * KS_RAND_SCALE;
  };
}

/**
 * Build the recovered stock cloud layout with a local deterministic random seed.
 * Native ksEditor uses the same formulas with the process-global rand() state.
 */
export function buildKsCloudBillboards(value, { worldDetail = 5, textureCount = KS_CLOUD_TEXTURE_PATHS.length, seed = 1 } = {}) {
  const settings = normalizeKsCloudSettings(value);
  const detail = bounded(worldDetail, 5, 0, 5);
  const count = Math.min(KS_CLOUD_MAX_COUNT, Math.floor(settings.count * detail * 0.2));
  const textures = Math.max(0, Math.min(KS_CLOUD_TEXTURE_PATHS.length, Math.floor(finite(textureCount, KS_CLOUD_TEXTURE_PATHS.length))));
  if (!count || !textures || !settings.width || !settings.height) return Object.freeze([]);
  const random = createKsCloudRandom(seed), clouds = [];
  for (let index = 0; index < count; index++) {
    const phi = (((random() - 0.5) * 10) + 360 / count) * DEG_TO_RAD * index;
    const band = (((random() - 0.5) * 5) + 15) * DEG_TO_RAD * ((index + 1) % 5);
    const theta = (((random() - 0.5) * 30) + 20) * DEG_TO_RAD + band;
    const radius = (1 - Math.cos(theta)) * 4 + settings.radius;
    const speed = settings.baseSpeed === 0 ? 0 : Math.max(0.0005, Math.min(1, random() * settings.baseSpeed));
    const texture = Math.min(textures - 1, Math.floor(random() * textures));
    const ring = radius * Math.sin(phi);
    clouds.push(Object.freeze({
      index,
      phi,
      theta,
      radius,
      speed,
      texture,
      position: Object.freeze([ring * Math.cos(theta), radius * Math.cos(phi), -ring * Math.sin(theta)])
    }));
  }
  return Object.freeze(clouds);
}

/** Group only adjacent clouds so alpha blending keeps construction order. */
export function buildKsCloudTextureRuns(clouds = []) {
  const runs = [];
  for (const cloud of Array.isArray(clouds) ? clouds : []) {
    const texture = Math.floor(finite(cloud?.texture, -1));
    if (texture < 0 || texture >= KS_CLOUD_TEXTURE_PATHS.length) continue;
    const previous = runs.at(-1);
    if (previous?.texture === texture) previous.clouds.push(cloud);
    else runs.push({ texture, clouds: [cloud] });
  }
  return Object.freeze(runs.map((run) => Object.freeze({ texture: run.texture, clouds: Object.freeze(run.clouds) })));
}

/** Apply each generated speed as a bounded vertical-axis motion preview. */
export function ksCloudPositionAtTime(cloud, elapsedSeconds = 0) {
  const position = Array.isArray(cloud?.position) ? cloud.position : [];
  const x = finite(position[0], 0), y = finite(position[1], 0), z = finite(position[2], 0);
  const angle = (bounded(cloud?.speed, 0, 0, 1) * Math.max(0, finite(elapsedSeconds, 0))) % (Math.PI * 2);
  const cosine = Math.cos(angle), sine = Math.sin(angle);
  return Object.freeze([x * cosine - z * sine, y, x * sine + z * cosine]);
}

/** Identify cloud changes that require all six reflection faces to be recaptured. */
export function ksCloudCaptureSignature(value = {}, readyTextures = []) {
  const settings = normalizeKsCloudSettings(value);
  const ready = Array.from({ length: KS_CLOUD_TEXTURE_PATHS.length }, (_, index) => Boolean(readyTextures?.[index]) ? "1" : "0").join("");
  return [value?.id || "", settings.width, settings.height, settings.radius, settings.count, settings.baseSpeed,
    bounded(value?.cloudCover, 0, 0, 1), bounded(value?.cloudCutoff, 0, 0, 1), bounded(value?.cloudColor, 0, 0, 1000), ready].join(":");
}

export function ksCloudShaderSample({ textureRed = 0, textureAlpha = 1, lightDirection = [0, -1, 0], lightColor = [1, 1, 1], ambientColor = [0, 0, 0], fogDistance = 12000, cloudCover = 1, cloudCutoff = 0.7, cloudColor = 1 } = {}) {
  const fog = Math.max(0, Math.min(1, 900 / Math.max(Number.EPSILON, finite(fogDistance, 12000))));
  const lightDot = -(finite(lightDirection?.[1], -1));
  const value = lightDot * (1 - finite(textureRed, 0)) * finite(cloudColor, 1);
  const cutoff = finite(cloudCutoff, 0), ambientRed = finite(ambientColor?.[0], 0);
  const rgb = Array.from({ length: 3 }, (_, channel) => {
    const base = 0.5 * (finite(lightColor?.[channel], 0) + finite(ambientColor?.[channel], 0)) * (1 - cutoff);
    return base + (value + (ambientRed * 0.75 - value) * fog) * cutoff;
  });
  return Object.freeze({ rgb: Object.freeze(rgb), alpha: Math.max(0, Math.min(1, finite(cloudCover, 0) * finite(textureAlpha, 0))), fog });
}
