import { createReadStream, existsSync, statSync } from "node:fs";
import { createServer } from "node:http";
import { extname, isAbsolute, join, normalize, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const publicRoot = fileURLToPath(new URL("../public/", import.meta.url));
const sourceRoot = fileURLToPath(new URL("./", import.meta.url));
const threeBuildRoot = fileURLToPath(new URL("../node_modules/three/build/", import.meta.url));
const developmentThreeAddonsRoot = fileURLToPath(new URL("../node_modules/three/examples/jsm/", import.meta.url));
const packagedThreeAddonsRoot = process.resourcesPath ? join(process.resourcesPath, "three-addons") : "";
const threeAddonsRoot = packagedThreeAddonsRoot && existsSync(packagedThreeAddonsRoot) ? packagedThreeAddonsRoot : developmentThreeAddonsRoot;
const sourceFiles = new Set([
  "acd.js", "asset-files.js", "brake-disc-shader.js", "car-validation.js", "csp-config.js",
  "cockpit-preview.js", "csp-noise.js", "csp-occlusion.js", "csp-wind.js", "custom-emissive.js",
  "dds.js", "driver-workspace.js", "dynamic-track.js", "editor-project.js", "fbx-import.js", "file-identity.js", "geometry-authoring.js", "grass-fx.js",
  "kn5-bake.js", "kn5-workspace.js", "kn5-write.js", "kn5.js", "knh.js",
  "ksanim.js", "lighting.js", "node-authoring.js", "rain-fx.js", "reflections.js", "rim-blur.js", "scene-diagnostics.js", "seasons.js",
  "shader-profiles.js", "shadows.js", "skin-metadata.js", "skinning.js", "track-cameras.js",
  "surface-authoring.js", "track-validation.js", "tyre-shader.js", "vao-patch.js", "workspace-authoring.js"
]);
const threeBuildFiles = new Set(["three.core.js", "three.module.js"]);
const threeAddonFiles = new Set(["curves/NURBSCurve.js", "curves/NURBSUtils.js", "libs/fflate.module.js", "loaders/FBXLoader.js"]);
const mime = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".wasm": "application/wasm"
};
const contentSecurityPolicy = [
  "default-src 'self'",
  "script-src 'self' 'sha256-tEXe25dtif2LwpNJCTLnEVIg3DbNBDfAjN7yOdhyZdA='",
  "style-src 'self'",
  "img-src 'self' blob: data:",
  "connect-src 'self' blob: data:",
  "worker-src 'self' blob:",
  "object-src 'none'",
  "base-uri 'none'",
  "frame-ancestors 'none'"
].join("; ");

function safePublicPath(pathname) {
  let relativeName;
  try { relativeName = pathname === "/" ? "index.html" : decodeURIComponent(pathname.slice(1)); }
  catch { return ""; }
  if (!relativeName || relativeName.includes("\0")) return "";
  const path = resolve(publicRoot, normalize(relativeName));
  const fromRoot = relative(resolve(publicRoot), path);
  return !fromRoot.startsWith("..") && !isAbsolute(fromRoot) ? path : "";
}

function routePath(pathname) {
  if (pathname.startsWith("/vendor/three-addons/")) {
    const name = pathname.slice(21);
    return threeAddonFiles.has(name) ? join(threeAddonsRoot, name) : "";
  }
  if (pathname.startsWith("/vendor/")) {
    const name = pathname.slice(8);
    return threeBuildFiles.has(name) ? join(threeBuildRoot, name) : "";
  }
  if (pathname.startsWith("/src/")) {
    const name = pathname.slice(5);
    return sourceFiles.has(name) ? join(sourceRoot, name) : "";
  }
  return safePublicPath(pathname);
}

function writeHeaders(response, status, type = "text/plain; charset=utf-8") {
  response.writeHead(status, {
    "cache-control": "no-store",
    "content-security-policy": contentSecurityPolicy,
    "content-type": type,
    "cross-origin-resource-policy": "same-origin",
    "referrer-policy": "no-referrer",
    "x-content-type-options": "nosniff",
    "x-frame-options": "DENY"
  });
}

export function createApexServer() {
  return createServer((request, response) => {
    if (request.method !== "GET" && request.method !== "HEAD") {
      writeHeaders(response, 405);
      response.end("Method not allowed");
      return;
    }
    let pathname;
    try { pathname = new URL(request.url || "/", "http://127.0.0.1").pathname; }
    catch { pathname = ""; }
    const path = routePath(pathname);
    if (!path || !existsSync(path) || !statSync(path).isFile()) {
      writeHeaders(response, 404);
      response.end("Not found");
      return;
    }
    writeHeaders(response, 200, mime[extname(path)] || "application/octet-stream");
    if (request.method === "HEAD") response.end();
    else createReadStream(path).pipe(response);
  });
}

export async function startApexServer(options = {}) {
  const host = options.host || "127.0.0.1";
  const port = options.port === undefined ? Number(process.env.APEX_EDITOR_PORT || 4173) : Number(options.port);
  const server = createApexServer();
  await new Promise((resolveListening, reject) => {
    server.once("error", reject);
    server.listen(port, host, () => {
      server.off("error", reject);
      resolveListening();
    });
  });
  const address = server.address();
  const actualPort = typeof address === "object" && address ? address.port : port;
  server.apexUrl = `http://${host}:${actualPort}`;
  if (options.log !== false) console.log(`Apex Editor is running at ${server.apexUrl}`);
  return server;
}

const invokedPath = process.argv[1] ? resolve(process.argv[1]) : "";
if (invokedPath && invokedPath === resolve(fileURLToPath(import.meta.url))) {
  startApexServer().catch((error) => {
    console.error(`Could not start Apex Editor: ${error.message}`);
    process.exitCode = 1;
  });
}
