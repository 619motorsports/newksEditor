export const SURFACE_EDIT_KEYS = [
  "key", "friction", "damping", "dirtAdditive", "blackFlagTime",
  "isValidTrack", "isPitlane", "sinHeight", "sinLength",
  "vibrationGain", "vibrationLength", "wav", "wavPitch", "ffEffect"
];

const NUMBER_KEYS = [
  "friction", "damping", "dirtAdditive", "blackFlagTime", "sinHeight",
  "sinLength", "vibrationGain", "vibrationLength", "wavPitch"
];
const BOOLEAN_KEYS = ["isValidTrack", "isPitlane"];
const TEXT_KEYS = ["key", "wav", "ffEffect"];

function cloneSurface(surface) {
  return { ...surface };
}

export function captureSurfaceBaseline(config) {
  return config?.surfaces ? config.surfaces.map(cloneSurface) : null;
}

export function surfaceEditCount(edits) {
  return Object.values(edits || {}).reduce((count, edit) => count + SURFACE_EDIT_KEYS.filter((key) => edit?.[key] !== undefined).length, 0);
}

export function applySurfaceEdits(config, edits = {}, baseline) {
  if (!config || !baseline) return 0;
  config.surfaces = baseline.map(cloneSurface);
  for (const [key, edit] of Object.entries(edits || {})) {
    const position = Number(key), surface = config.surfaces[position];
    if (!Number.isInteger(position) || !surface || !edit) continue;
    for (const field of NUMBER_KEYS) if (edit[field] !== undefined) surface[field] = edit[field];
    for (const field of BOOLEAN_KEYS) if (edit[field] !== undefined) surface[field] = edit[field];
    for (const field of TEXT_KEYS) if (edit[field] !== undefined) surface[field] = edit[field] ?? "";
  }
  return surfaceEditCount(edits);
}
