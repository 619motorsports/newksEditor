import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { AnimationClip, BufferGeometry, Float32BufferAttribute, Group, Mesh, MeshPhongMaterial, NumberKeyframeTrack, QuaternionKeyframeTrack, Texture, VectorKeyframeTrack } from "three";
import { decodeDdsRgba, inspectDds } from "../src/dds.js";
import { convertFbxAnimations, convertFbxScene, inspectFbxHeader, parseFbx, parseFbxWithTextures, resolveFbxTextures } from "../src/fbx-import.js";
import { parseKsAnimation, serializeKsAnimation } from "../src/ksanim.js";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { serializeKn5 } from "../src/kn5-write.js";

const sdk = "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk";

test("recognizes binary and ASCII FBX headers", () => {
  const binary = new Uint8Array(27);
  binary.set(new TextEncoder().encode("Kaydara FBX Binary  \0\x1a\0"));
  new DataView(binary.buffer).setUint32(23, 7400, true);
  assert.deepEqual(inspectFbxHeader(binary), { format: "binary", version: 7400 });
  assert.deepEqual(inspectFbxHeader(new TextEncoder().encode("; FBX 7.4.0 project file\nFBXVersion: 7400\n")), { format: "ascii", version: 7400 });
  assert.throws(() => inspectFbxHeader(new Uint8Array(32)), /recognized FBX header/);
});

test("samples FBX clips into native 100-frame local-transform tracks", () => {
  const scene = new Group(), pivot = new Group(), mesh = new Mesh(triangleGeometry(3), new MeshPhongMaterial());
  scene.name = "DRIVERROOT"; scene.userData.originalName = "DRIVER:ROOT";
  pivot.name = "PIVOT"; mesh.name = "BODY"; pivot.add(mesh); scene.add(pivot);
  scene.animations = [new AnimationClip("Open", 2, [
    new VectorKeyframeTrack("PIVOT.position", [0, 2], [0, 0, 0, 10, 0, 0]),
    new QuaternionKeyframeTrack("PIVOT.quaternion", [0, 2], [0, 0, 0, 1, 0, 0, 1, 0]),
    new NumberKeyframeTrack("BODY.visible", [0, 2], [1, 0])
  ])];
  const animations = convertFbxAnimations(scene, "door.fbx"), animation = animations[0];
  assert.deepEqual([animation.name, animation.version, animation.duration, animation.frameCount, animation.sourceTrackCount], ["Open", 2, 2, 100, 3]);
  assert.deepEqual(animation.tracks.map((track) => track.name), ["DRIVER:ROOT", "PIVOT", "BODY"]);
  assert.equal(animation.tracks[1].frames[50].position[0], 5);
  assert.equal(animation.tracks[0].animated, false);
  assert.equal(animation.tracks[1].animated, true);
  assert.equal(animation.tracks[2].animated, false);
  assert.deepEqual(pivot.position.toArray(), [0, 0, 0]);
  const reparsed = parseKsAnimation(serializeKsAnimation(animation));
  assert.deepEqual([reparsed.version, reparsed.tracks.length, reparsed.frameCount], [2, 3, 100]);
  assert.equal(convertFbxScene(scene).root.children[0].name, "DRIVER:ROOT");
});

function triangleGeometry(vertexCount) {
  const geometry = new BufferGeometry(), positions = new Float32Array(vertexCount * 3), normals = new Float32Array(vertexCount * 3), uvs = new Float32Array(vertexCount * 2);
  for (let index = 0; index < vertexCount; index++) { positions[index * 3] = index % 3 === 1 ? 1 : 0; positions[index * 3 + 1] = index % 3 === 2 ? 1 : 0; normals[index * 3 + 2] = 1; }
  geometry.setAttribute("position", new Float32BufferAttribute(positions, 3)); geometry.setAttribute("normal", new Float32BufferAttribute(normals, 3)); geometry.setAttribute("uv", new Float32BufferAttribute(uvs, 2));
  return geometry;
}

function sourceFile(name, path, bytes) {
  return { name, webkitRelativePath: path, size: bytes.byteLength, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) };
}

const onePixelPng = new Uint8Array(Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M/wHwAF/gL+XkWPWQAAAABJRU5ErkJggg==", "base64"));

test("splits material groups and meshes that exceed the KN5 index limit", () => {
  const scene = new Group(), materials = [new MeshPhongMaterial({ name: "A" }), new MeshPhongMaterial({ name: "B" })], grouped = new Mesh(triangleGeometry(6), materials);
  grouped.name = "Grouped"; grouped.geometry.addGroup(0, 3, 0); grouped.geometry.addGroup(3, 3, 1); scene.add(grouped);
  const large = new Mesh(triangleGeometry(65538), materials[0]); large.name = "Large"; scene.add(large);
  const model = convertFbxScene(scene), meshes = walkNodes(model.root).map(({ node }) => node).filter((node) => node.kind === "mesh");
  assert.deepEqual(meshes.slice(0, 2).map((mesh) => [mesh.name, mesh.materialId]), [["Grouped_SUB0", 0], ["Grouped_SUB1", 1]]);
  assert.deepEqual(meshes.slice(2).map((mesh) => mesh.vertices.length / mesh.vertexStride), [65535, 3]);
  assert.ok(meshes.every((mesh) => Math.max(...mesh.indices.subarray(Math.max(0, mesh.indices.length - 4))) <= 0xffff));
});

test("resolves FBX diffuse textures after a source folder is selected", async () => {
  const scene = new Group(), material = new MeshPhongMaterial({ name: "Paint" }), map = new Texture();
  map.name = "Paint map"; map.userData.apexFbxSource = "textures\\paint.png"; material.map = map;
  const mesh = new Mesh(triangleGeometry(3), material); mesh.name = "Body"; scene.add(mesh);
  const model = convertFbxScene(scene);
  assert.equal(model.fbx.textureSummary.missing, 1);
  assert.equal(model.materials[0].resources[0].textureId, 0);
  assert.match(model.textures[0].name, /^APEX_FBX_/);
  assert.ok(inspectDds(model.textures[0].data));

  await resolveFbxTextures(model, [sourceFile("paint.png", "source/textures/paint.png", onePixelPng)]);
  assert.deepEqual(model.fbx.textureSummary, { referenced: 1, resolved: 1, embedded: 0, missing: 0, ambiguous: 0, unsupported: 0, error: 0 });
  assert.equal(model.textures[0].name, "paint.png");
  assert.equal(model.materials[0].resources[0].texture, "paint.png");
  assert.deepEqual(model.textures[0].data, onePixelPng);

  const reparsed = parseKn5(serializeKn5(model));
  assert.equal(reparsed.materials[0].resources[0].texture, "paint.png");
  assert.deepEqual(reparsed.textures[0].data, onePixelPng);

  await resolveFbxTextures(model, [sourceFile("paint.png", "source/a/paint.png", onePixelPng), sourceFile("paint.png", "source/b/paint.png", onePixelPng)]);
  assert.equal(model.fbx.textureSummary.ambiguous, 1);
  assert.match(model.textures[0].name, /^APEX_FBX_/);
  assert.ok(inspectDds(model.textures[0].data));
});

test("preserves a static FBX normal map with the stock normal shader", async () => {
  const scene = new Group(), material = new MeshPhongMaterial({ name: "Paint" }), diffuse = new Texture(), normal = new Texture();
  diffuse.name = "Paint diffuse"; diffuse.userData.apexFbxSource = "textures\\paint.png"; material.map = diffuse;
  normal.name = "Paint normal"; normal.userData.apexFbxSource = "textures\\paint_n.png"; material.normalMap = normal;
  const mesh = new Mesh(triangleGeometry(3), material); mesh.name = "Body"; scene.add(mesh);
  const model = convertFbxScene(scene), converted = model.materials[0];
  assert.equal(converted.shader, "ksPerPixelNM");
  assert.deepEqual(converted.resources.map((resource) => [resource.slot, resource.textureId]), [["txDiffuse", 0], ["txNormal", 1]]);
  assert.deepEqual(model.fbx.mapSummary, { preserved: 2, folded: 0, ignored: 0 });
  assert.deepEqual(model.fbx.textureSummary, { referenced: 2, resolved: 0, embedded: 0, missing: 2, ambiguous: 0, unsupported: 0, error: 0 });
  const normalFallback = decodeDdsRgba(model.textures[1].data, inspectDds(model.textures[1].data));
  assert.deepEqual([...normalFallback[0].pixels], [128, 128, 255, 255]);

  await resolveFbxTextures(model, [sourceFile("paint.png", "source/textures/paint.png", onePixelPng), sourceFile("paint_n.png", "source/textures/paint_n.png", onePixelPng)]);
  assert.equal(model.fbx.textureSummary.resolved, 2);
  assert.deepEqual(converted.resources.map((resource) => resource.texture), ["paint.png", "paint_n.png"]);
  const reparsed = parseKn5(serializeKn5(model));
  assert.equal(reparsed.materials[0].shader, "ksPerPixelNM");
  assert.deepEqual(reparsed.materials[0].resources.map((resource) => [resource.slot, resource.texture]), [["txDiffuse", "paint.png"], ["txNormal", "paint_n.png"]]);
});

test("reports FBX maps that have no safe stock KN5 binding", () => {
  const scene = new Group(), material = new MeshPhongMaterial({ name: "Driver" });
  material.normalMap = Object.assign(new Texture(), { name: "driver_n.png" });
  material.bumpMap = Object.assign(new Texture(), { name: "driver_height.png" });
  material.emissiveMap = Object.assign(new Texture(), { name: "driver_emissive.png" });
  material.alphaMap = Object.assign(new Texture(), { name: "driver_alpha.png" });
  const mesh = new Mesh(triangleGeometry(3), material); mesh.name = "Driver"; mesh.isSkinnedMesh = true; scene.add(mesh);
  const model = convertFbxScene(scene);
  assert.equal(model.materials[0].shader, "ksSkinnedMesh");
  assert.deepEqual(model.fbx.mapSummary, { preserved: 0, folded: 0, ignored: 4 });
  assert.equal(model.fbx.materialMapWarnings.length, 4);
  assert.ok(model.fbx.warnings.some((warning) => warning.includes("stock skinned normal shader")));
  assert.ok(model.fbx.warnings.some((warning) => warning.includes("height map must be converted")));
  assert.ok(model.fbx.warnings.some((warning) => warning.includes("txDiffuse alpha")));
});

test("preserves a supported embedded FBX diffuse image", () => {
  const scene = new Group(), material = new MeshPhongMaterial({ name: "EmbeddedPaint" }), map = new Texture();
  map.name = "Embedded map";
  map.userData.apexFbxSource = "blob:embedded";
  map.userData.apexFbxResolution = { source: "base_color_texture", status: "embedded", embedded: true, name: "base_color_texture.png", format: "png", data: onePixelPng };
  material.map = map;
  const mesh = new Mesh(triangleGeometry(3), material); mesh.name = "EmbeddedCube"; scene.add(mesh);
  const model = convertFbxScene(scene), reparsed = parseKn5(serializeKn5(model));
  assert.deepEqual(model.fbx.textureSummary, { referenced: 1, resolved: 0, embedded: 1, missing: 0, ambiguous: 0, unsupported: 0, error: 0 });
  assert.equal(model.textures[0].name, "base_color_texture.png");
  assert.deepEqual(model.textures[0].data, onePixelPng);
  assert.equal(reparsed.materials[0].resources[0].texture, "base_color_texture.png");
  assert.deepEqual(reparsed.textures[0].data, onePixelPng);
});

test("generated FBX material colors are valid one-pixel DDS textures", async (t) => {
  let bytes;
  try { bytes = await readFile(`${sdk}/editor/content/objects3D/sphere.FBX`); }
  catch { t.skip("Assetto Corsa SDK sphere fixture is not installed"); return; }
  const model = parseFbx(bytes, "sphere.FBX"), descriptor = inspectDds(model.textures[0].data), levels = decodeDdsRgba(model.textures[0].data, descriptor);
  assert.equal(descriptor.width, 1); assert.equal(descriptor.height, 1); assert.equal(levels[0].pixels.length, 4);
});

test("imports and round-trips the official SDK sphere FBX", async (t) => {
  let bytes;
  try { bytes = await readFile(`${sdk}/editor/content/objects3D/sphere.FBX`); }
  catch { t.skip("Assetto Corsa SDK sphere fixture is not installed"); return; }
  const model = parseFbx(bytes, "sphere.FBX"), meshes = walkNodes(model.root).map(({ node }) => node).filter((node) => node.kind === "mesh");
  assert.equal(model.fbx.version, 7200); assert.equal(model.fbx.format, "binary"); assert.equal(meshes.length, 1); assert.equal(meshes[0].indices.length / 3, 224);
  assert.ok(meshes[0].vertices[7] <= 0); assert.equal(meshes[0].castShadows, true);
  const reparsed = parseKn5(serializeKn5(model)), outputMesh = walkNodes(reparsed.root).map(({ node }) => node).find((node) => node.kind === "mesh");
  assert.equal(outputMesh.indices.length, meshes[0].indices.length); assert.equal(reparsed.materials[0].shader, "ksPerPixel"); assert.equal(reparsed.textures.length, 1);
});

test("imports static and skinned geometry from the official GT40 FBX", async (t) => {
  let bytes;
  try { bytes = await readFile(`${sdk}/dev/car_pipeline_2.0rev/Scene templates/GT40_animated_suspension_example_fbx.FBX`); }
  catch { t.skip("Assetto Corsa SDK GT40 fixture is not installed"); return; }
  const model = parseFbx(bytes, "GT40_animated_suspension_example_fbx.FBX"), meshes = walkNodes(model.root).map(({ node }) => node).filter((node) => node.kind === "mesh" || node.kind === "skinnedMesh");
  assert.equal(model.fbx.version, 7300); assert.equal(model.fbx.animations.length, 1); assert.equal(meshes.length, 42); assert.equal(meshes.filter((node) => node.kind === "skinnedMesh").length, 4);
  assert.deepEqual([model.fbx.animations[0].sourceTrackCount, model.fbx.animations[0].tracks.length, model.fbx.animations[0].frameCount], [104, 112, 100]);
  assert.ok(model.fbx.animations[0].tracks.some((track) => track.animated));
  const exportedAnimation = parseKsAnimation(serializeKsAnimation(model.fbx.animations[0]));
  assert.deepEqual([exportedAnimation.version, exportedAnimation.tracks.length, exportedAnimation.frameCount], [2, 112, 100]);
  assert.equal(meshes.reduce((sum, node) => sum + node.indices.length / 3, 0), 16514);
  const reparsed = parseKn5(serializeKn5(model)), reparsedMeshes = walkNodes(reparsed.root).map(({ node }) => node).filter((node) => node.kind === "mesh" || node.kind === "skinnedMesh");
  assert.equal(reparsedMeshes.length, 42); assert.equal(reparsedMeshes.filter((node) => node.kind === "skinnedMesh").length, 4);
});

test("maps a selected DDS source texture in the official GT40 FBX", async (t) => {
  let fbxBytes, greyBytes;
  try {
    fbxBytes = await readFile(`${sdk}/dev/car_pipeline_2.0rev/Scene templates/GT40_animated_suspension_example_fbx.FBX`);
    greyBytes = await readFile("/mnt/D/SteamLibrary/steamapps/common/assettocorsa/content/cars/COT_suspension_testing_platform/unp_roy_cot_ford.kn5/texture/Grey.dds");
  } catch { t.skip("The SDK GT40 FBX or a matching installed Grey.dds is not available"); return; }
  const grey = new Uint8Array(greyBytes.buffer, greyBytes.byteOffset, greyBytes.byteLength);
  const model = await parseFbxWithTextures(fbxBytes, "GT40.FBX", [sourceFile("Grey.dds", "source/texture/Grey.dds", grey)]);
  assert.deepEqual(model.fbx.textureSummary, { referenced: 2, resolved: 1, embedded: 0, missing: 1, ambiguous: 0, unsupported: 0, error: 0 });
  const spring = model.materials.find((material) => material.name === "SPRING"), texture = model.textures.find((entry) => entry.name === "Grey.dds");
  assert.equal(spring.resources[0].texture, "Grey.dds");
  assert.equal(spring.resources[0].textureId, 0);
  assert.deepEqual(texture.data, grey);
  const reparsed = parseKn5(serializeKn5(model));
  assert.equal(reparsed.materials.find((material) => material.name === "SPRING").resources[0].texture, "Grey.dds");
});

test("preserves normal-map connections from an installed production FBX", async (t) => {
  const source = "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/content/cars/619_nextgen_mustang/nextgen_commonnewhires.fbx";
  let bytes;
  try { bytes = await readFile(source); }
  catch { t.skip("The production normal-mapped FBX fixture is not installed"); return; }
  const model = parseFbx(bytes, "nextgen_commonnewhires.fbx"), normalMaterials = model.materials.filter((material) => material.shader === "ksPerPixelNM");
  assert.deepEqual(normalMaterials.map((material) => material.name), ["MI_DriverSeat_NG", "MI_Flaps_NG", "MI_Cockpit_parts_NG", "MI_SteeringWheel_NG", "LightShader"]);
  assert.ok(normalMaterials.every((material) => material.resources.map((resource) => resource.slot).join(",") === "txDiffuse,txNormal"));
  assert.deepEqual(model.fbx.mapSummary, { preserved: 23, folded: 0, ignored: 0 });
  assert.equal(model.textures.length, 27);
});
