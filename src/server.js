import { createReadStream, existsSync, statSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("../public/", import.meta.url));
const sourceRoot = fileURLToPath(new URL("./", import.meta.url));
const port = Number(process.env.APEX_EDITOR_PORT || 4173);
const mime = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8", ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml"
};

createServer((request, response) => {
  const url = new URL(request.url, `http://${request.headers.host}`);
  let path;
  if (url.pathname === "/src/kn5.js") path = join(sourceRoot, "kn5.js");
  else if (url.pathname === "/src/csp-config.js") path = join(sourceRoot, "csp-config.js");
  else if (url.pathname === "/src/csp-occlusion.js") path = join(sourceRoot, "csp-occlusion.js");
  else if (url.pathname === "/src/custom-emissive.js") path = join(sourceRoot, "custom-emissive.js");
  else if (url.pathname === "/src/editor-project.js") path = join(sourceRoot, "editor-project.js");
  else if (url.pathname === "/src/dds.js") path = join(sourceRoot, "dds.js");
  else if (url.pathname === "/src/asset-files.js") path = join(sourceRoot, "asset-files.js");
  else if (url.pathname === "/src/kn5-workspace.js") path = join(sourceRoot, "kn5-workspace.js");
  else if (url.pathname === "/src/ksanim.js") path = join(sourceRoot, "ksanim.js");
  else if (url.pathname === "/src/kn5-bake.js") path = join(sourceRoot, "kn5-bake.js");
  else if (url.pathname === "/src/kn5-write.js") path = join(sourceRoot, "kn5-write.js");
  else if (url.pathname === "/src/track-validation.js") path = join(sourceRoot, "track-validation.js");
  else if (url.pathname === "/src/acd.js") path = join(sourceRoot, "acd.js");
  else if (url.pathname === "/src/car-validation.js") path = join(sourceRoot, "car-validation.js");
  else if (url.pathname === "/src/knh.js") path = join(sourceRoot, "knh.js");
  else if (url.pathname === "/src/driver-workspace.js") path = join(sourceRoot, "driver-workspace.js");
  else if (url.pathname === "/src/skinning.js") path = join(sourceRoot, "skinning.js");
  else if (url.pathname === "/src/track-cameras.js") path = join(sourceRoot, "track-cameras.js");
  else if (url.pathname === "/src/grass-fx.js") path = join(sourceRoot, "grass-fx.js");
  else if (url.pathname === "/src/csp-noise.js") path = join(sourceRoot, "csp-noise.js");
  else if (url.pathname === "/src/csp-wind.js") path = join(sourceRoot, "csp-wind.js");
  else if (url.pathname === "/src/rain-fx.js") path = join(sourceRoot, "rain-fx.js");
  else if (url.pathname === "/src/shadows.js") path = join(sourceRoot, "shadows.js");
  else if (url.pathname === "/src/lighting.js") path = join(sourceRoot, "lighting.js");
  else if (url.pathname === "/src/vao-patch.js") path = join(sourceRoot, "vao-patch.js");
  else if (url.pathname === "/src/seasons.js") path = join(sourceRoot, "seasons.js");
  else if (url.pathname === "/src/shader-profiles.js") path = join(sourceRoot, "shader-profiles.js");
  else if (url.pathname === "/src/reflections.js") path = join(sourceRoot, "reflections.js");
  else {
    const relative = url.pathname === "/" ? "index.html" : decodeURIComponent(url.pathname.slice(1));
    path = join(root, normalize(relative));
    if (!path.startsWith(root)) path = "";
  }
  if (!path || !existsSync(path) || !statSync(path).isFile()) {
    response.writeHead(404, { "content-type": "text/plain" });
    response.end("Not found");
    return;
  }
  response.writeHead(200, { "content-type": mime[extname(path)] || "application/octet-stream", "cache-control": "no-store" });
  createReadStream(path).pipe(response);
}).listen(port, "127.0.0.1", () => {
  console.log(`Apex Editor is running at http://127.0.0.1:${port}`);
});
