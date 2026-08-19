function saturate(value) {
  return Math.max(0, Math.min(1, Number(value) || 0));
}

/** Values loaded by the shipped ksEditor from cfg/video.ini. */
export const KS_EDITOR_CUBEMAP = Object.freeze({
  size: 512,
  facesPerFrame: 1,
  nearPlane: 0.01,
  farPlane: 500,
  fovDegrees: 90,
  shaderSlot: 10
});

/**
 * Native CubeMapRenderer camera directions converted from DirectX render-target
 * orientation to WebGL cubemap faces. Update order remains -X,+X,+Y,-Y,+Z,-Z.
 */
export const WEBGL_CUBEMAP_FACES = Object.freeze([
  Object.freeze({ target: "negative-x", direction: Object.freeze([-1, 0, 0]), up: Object.freeze([0, -1, 0]) }),
  Object.freeze({ target: "positive-x", direction: Object.freeze([1, 0, 0]), up: Object.freeze([0, -1, 0]) }),
  Object.freeze({ target: "positive-y", direction: Object.freeze([0, 1, 0]), up: Object.freeze([0, 0, 1]) }),
  Object.freeze({ target: "negative-y", direction: Object.freeze([0, -1, 0]), up: Object.freeze([0, 0, -1]) }),
  Object.freeze({ target: "positive-z", direction: Object.freeze([0, 0, 1]), up: Object.freeze([0, -1, 0]) }),
  Object.freeze({ target: "negative-z", direction: Object.freeze([0, 0, -1]), up: Object.freeze([0, -1, 0]) })
]);

/**
 * Selects the visible subtree supplied to ksEditor's cubemap renderer. An explicit
 * root mirrors addCubeMapNode(); a tagged showroom is preferred in automatic mode.
 */
export function selectReflectionCaptureItems(renderItems, { explicitRoot = null, workspaceKind = "", boundsRadius = 0, isolated = false } = {}) {
  const visible = renderItems || [];
  if (isolated) return { items: [], mode: "disabled", rootName: "", reason: "Isolated mesh preview" };
  if (explicitRoot) {
    const items = visible.filter(({ item }) => item?.node === explicitRoot || item?.reflectionAncestors?.includes(explicitRoot));
    return { items, mode: "explicit", rootName: String(explicitRoot.name || "Selected subtree"), reason: items.length ? "" : "Selected reflection subtree has no visible geometry" };
  }
  const environment = visible.filter(({ item }) => item?.workspaceAuxiliary === "reflectionEnvironment");
  if (environment.length) {
    const rootName = environment[0].item.workspaceFile || "Reflection environment";
    return { items: environment, mode: "environment", rootName, reason: "" };
  }
  if (workspaceKind === "track" || Number(boundsRadius) > 20) return { items: visible, mode: "scene", rootName: "Whole scene", reason: visible.length ? "" : "Scene has no visible geometry" };
  return { items: [], mode: "fallback", rootName: "", reason: "No separate environment geometry" };
}

/** Kunos/CSP smooth cap used after adding the constant and angle Fresnel terms. */
export function reflectionSmoothMinimum(a, b) {
  const first = Number(a) || 0, second = Number(b) || 0;
  const h = saturate(second - Math.abs(first - second));
  return Math.min(first, second) - h * h / Math.max(0.00001, second * 3) * (1 - second);
}

export function reflectionFresnel(facing, fresnelC, fresnelExponent, fresnelMaximum, reflectionMultiplier = 1) {
  const input = saturate(facing);
  const angle = Math.pow(input, Math.max(0.01, Number(fresnelExponent) || 0));
  return Math.max(0, reflectionSmoothMinimum(angle + Math.max(0, Number(fresnelC) || 0), Math.max(0, Number(fresnelMaximum) || 0))) * Math.max(0, Number(reflectionMultiplier) || 0);
}

/** Native reflection mip heuristic before any shader-specific blur multiplier. */
export function reflectionBlurFromExponent(specularExponent) {
  const base = saturate(1 - Math.max(0, Number(specularExponent) || 0) / 255);
  return { base: base * 6, mip: base * base * 6 };
}

/** CPU reference for the portable sky/fog/ground fallback mirrored in WebGL. */
export function portableReflectionEnvironment(direction, blur, lighting) {
  const length = Math.hypot(...direction) || 1, d = direction.map((value) => value / length), y = d[1];
  const upperY = saturate(y), horizonWeight = (1 - upperY) ** 2;
  const upper = lighting.skyColor.map((value, index) => (value * (1 - horizonWeight) + lighting.horizonColor[index] * horizonWeight) * saturate(0.75 + y * 2));
  const fogMix = saturate(lighting.fogBlend) * 0.35, fogged = upper.map((value, index) => value * (1 - fogMix) + lighting.fogColor[index] * fogMix);
  const groundEdge = 0.025 + (0.24 - 0.025) * saturate(blur / 6), edgeT = saturate((y + groundEdge) / (groundEdge * 2));
  const smoothEdge = edgeT * edgeT * (3 - 2 * edgeT), ground = [0.05, 0.11, 0.08];
  return fogged.map((value, index) => (ground[index] * (1 - smoothEdge) + value * smoothEdge) * 0.12);
}
