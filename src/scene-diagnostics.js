const IDENTITY = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

function multiply(a, b) {
  const output = new Array(16);
  for (let column = 0; column < 4; column++) for (let row = 0; row < 4; row++) {
    let value = 0;
    for (let index = 0; index < 4; index++) value += a[index * 4 + row] * b[column * 4 + index];
    output[column * 4 + row] = value;
  }
  return output;
}

function transformPoint(matrix, x, y, z) {
  return [
    matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
    matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
    matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]
  ];
}

function emptyBounds() {
  return { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity], vertices: 0 };
}

function addPoint(bounds, point) {
  for (let axis = 0; axis < 3; axis++) {
    bounds.min[axis] = Math.min(bounds.min[axis], point[axis]);
    bounds.max[axis] = Math.max(bounds.max[axis], point[axis]);
  }
  bounds.vertices++;
}

function finishBounds(bounds) {
  if (!bounds.vertices) return null;
  const size = bounds.min.map((value, axis) => bounds.max[axis] - value);
  const center = bounds.min.map((value, axis) => (value + bounds.max[axis]) / 2);
  return {
    min: [...bounds.min],
    max: [...bounds.max],
    size,
    center,
    radius: Math.hypot(...size) / 2,
    centerDistance: Math.hypot(...center)
  };
}

function fileRecord(name, source = null, index = null) {
  return {
    name, index,
    auxiliary: source?.auxiliary || null,
    sourceBytes: Math.max(0, Number(source?.size) || 0),
    materials: Math.max(0, Number(source?.materials) || 0),
    textures: Math.max(0, Number(source?.textures) || 0),
    textureBytes: Math.max(0, Number(source?.textureBytes) || 0),
    nodes: 0,
    meshes: 0,
    visibleMeshes: 0,
    staticMeshes: 0,
    skinnedMeshes: 0,
    vertices: 0,
    triangles: 0,
    geometryBytes: 0,
    boundsAccumulator: emptyBounds()
  };
}

function finishFile(record) {
  const { boundsAccumulator, ...output } = record;
  return { ...output, bounds: finishBounds(boundsAccumulator) };
}

/** Compute exact parsed-scene inventory and transformed geometry bounds. */
export function analyzeScene(model, options = {}) {
  if (!model?.root || !Array.isArray(model.materials) || !Array.isArray(model.textures)) throw new TypeError("A parsed KN5 model is required");
  const workspaceFiles = model.workspace?.files || [];
  const excludeAuxiliary = Boolean(options.excludeAuxiliary);
  const includedWorkspaceFiles = excludeAuxiliary ? workspaceFiles.filter((file) => !file.auxiliary) : workspaceFiles;
  const fallbackName = String(options.sourceName || model.workspace?.name || model.root.name || "Scene");
  const files = new Map();
  if (!model.workspace) files.set("source", fileRecord(fallbackName, {
    size: model.byteLength,
    materials: model.materials.length,
    textures: model.textures.length,
    textureBytes: model.textures.reduce((sum, texture) => sum + Math.max(0, Number(texture.size) || 0), 0)
  }));
  else for (let index = 0; index < workspaceFiles.length; index++) if (!excludeAuxiliary || !workspaceFiles[index].auxiliary) files.set(index, fileRecord(workspaceFiles[index].name, workspaceFiles[index], index));

  const bounds = emptyBounds(), visibleBounds = emptyBounds(), materialIds = new Set(), largestMeshes = [];
  const totals = {
    nodes: 0, meshes: 0, visibleMeshes: 0, staticMeshes: 0, skinnedMeshes: 0,
    vertices: 0, triangles: 0, geometryBytes: 0, emptyMeshes: 0, invalidMaterialMeshes: 0,
    maxDepth: 0, materials: model.workspace ? includedWorkspaceFiles.reduce((sum, file) => sum + Math.max(0, Number(file.materials) || 0), 0) : model.materials.length,
    textures: model.workspace ? includedWorkspaceFiles.reduce((sum, file) => sum + Math.max(0, Number(file.textures) || 0), 0) : model.textures.length,
    effectiveTextures: model.workspace && excludeAuxiliary ? model.textures.filter((texture) => !Number.isInteger(texture.workspaceFileIndex) || !workspaceFiles[texture.workspaceFileIndex]?.auxiliary).length : model.textures.length,
    textureBytes: model.workspace ? includedWorkspaceFiles.reduce((sum, file) => sum + Math.max(0, Number(file.textureBytes) || 0), 0) : model.textures.reduce((sum, texture) => sum + Math.max(0, Number(texture.size) || 0), 0),
    sourceBytes: model.workspace ? includedWorkspaceFiles.reduce((sum, file) => sum + Math.max(0, Number(file.size) || 0), 0) : Math.max(0, Number(model.byteLength) || 0)
  };

  const visit = (node, parentWorld, parentActive, inheritedFileKey, path, depth, synthetic = false) => {
    const world = Array.isArray(node.transform) || ArrayBuffer.isView(node.transform) ? multiply(parentWorld, node.transform) : parentWorld;
    const active = Boolean(parentActive && node.active);
    let fileKey = Number.isInteger(node.workspaceFileIndex) ? node.workspaceFileIndex : inheritedFileKey;
    if (fileKey === null || fileKey === undefined) fileKey = "source";
    let file = files.get(fileKey);
    if (!file) {
      const source = Number.isInteger(fileKey) ? workspaceFiles[fileKey] : null, fileName = String(node.workspaceFile || source?.name || fallbackName);
      file = fileRecord(fileName, source, Number.isInteger(fileKey) ? fileKey : null); files.set(fileKey, file);
    }
    if (excludeAuxiliary && file.auxiliary) return;
    if (!synthetic) { totals.nodes++; file.nodes++; totals.maxDepth = Math.max(totals.maxDepth, depth); }
    const mesh = node.kind === "mesh" || node.kind === "skinnedMesh";
    if (mesh) {
      const stride = Math.max(1, Number(node.vertexStride) || 1), vertexValues = node.vertices?.length || 0;
      const vertexCount = Math.floor(vertexValues / stride), indexCount = node.indices?.length || 0, triangles = Math.floor(indexCount / 3);
      const geometryBytes = (Number(node.vertices?.byteLength) || vertexValues * 4) + (Number(node.indices?.byteLength) || indexCount * 2);
      const visible = Boolean(active && node.visible && node.renderable);
      totals.meshes++; totals.visibleMeshes += Number(visible); totals.vertices += vertexCount; totals.triangles += triangles; totals.geometryBytes += geometryBytes;
      totals.staticMeshes += Number(node.kind === "mesh"); totals.skinnedMeshes += Number(node.kind === "skinnedMesh");
      totals.emptyMeshes += Number(!vertexCount || !triangles);
      const validMaterial = Number.isInteger(node.materialId) && node.materialId >= 0 && node.materialId < model.materials.length;
      totals.invalidMaterialMeshes += Number(!validMaterial);
      if (validMaterial) materialIds.add(node.materialId);
      file.meshes++; file.visibleMeshes += Number(visible); file.vertices += vertexCount; file.triangles += triangles; file.geometryBytes += geometryBytes;
      file.staticMeshes += Number(node.kind === "mesh"); file.skinnedMeshes += Number(node.kind === "skinnedMesh");
      let furthestDistance = 0;
      for (let offset = 0; offset + 2 < vertexValues; offset += stride) {
        const point = transformPoint(world, node.vertices[offset], node.vertices[offset + 1], node.vertices[offset + 2]);
        if (point.every(Number.isFinite)) {
          addPoint(bounds, point); addPoint(file.boundsAccumulator, point); if (visible) addPoint(visibleBounds, point);
          furthestDistance = Math.max(furthestDistance, Math.hypot(...point));
        }
      }
      largestMeshes.push({
        name: String(node.name || "Unnamed mesh"), path, file: file.name, fileIndex: file.index, kind: node.kind,
        vertices: vertexCount, triangles, geometryBytes, furthestDistance,
        material: validMaterial ? String(model.materials[node.materialId]?.name || `Material ${node.materialId}`) : "Invalid material"
      });
    }
    for (let index = 0; index < (node.children || []).length; index++) {
      const child = node.children[index];
      const childSynthetic = Boolean(model.workspace && (node === model.root || Number.isInteger(child.workspaceFileIndex)));
      visit(child, world, active, fileKey, `${path}/${index}:${child.name || child.kind}`, synthetic ? depth : depth + 1, childSynthetic);
    }
  };
  visit(model.root, IDENTITY, true, model.workspace ? null : "source", model.root.name || "root", 0, Boolean(model.workspace));
  largestMeshes.sort((a, b) => b.triangles - a.triangles || b.vertices - a.vertices || a.file.localeCompare(b.file) || a.name.localeCompare(b.name));
  const finishedFiles = [...files.values()].map(finishFile).filter((file) => (!excludeAuxiliary || !file.auxiliary) && (file.nodes || file.meshes || file.sourceBytes)).sort((a, b) => b.triangles - a.triangles || a.name.localeCompare(b.name));
  return {
    totals: { ...totals, usedMaterials: materialIds.size },
    bounds: finishBounds(bounds),
    visibleBounds: finishBounds(visibleBounds),
    files: finishedFiles,
    largestMeshes
  };
}
