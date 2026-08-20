import { computeKn5Visibility, parseKn5, propertyValue, walkNodes } from "/src/kn5.js";
import { evaluateCspConfig, expandCspMaterialTemplates, parseCspIni } from "/src/csp-config.js";
import { customEmissiveAtlasSize } from "/src/custom-emissive.js";
import { cloneEditorProject, createEditorProject, editorProjectCspEditCount, editorProjectEditCount, formatEditorValue, normalizeEditorProject, parseEditorValue, serializeEditorCsp, serializeEditorProject } from "/src/editor-project.js";
import { decodeDdsRgba, inspectDds } from "/src/dds.js";
import { createAssetFileIndex, discoverAssetAnimations, discoverAssetSkins, externalResourcePaths, matchSkinTextures, normalizeAssetPath, resolveAssetFile } from "/src/asset-files.js";
import { applyGeometryEdits, captureStaticGeometryBaselines, staticGeometryMetrics } from "/src/geometry-authoring.js";
import { carLodDistance, carLodVisible, mergeKn5Models, normalizeCarLodFileName, parseCarLodsIni, parseModelsIni, serializeCarLodsIni, serializeModelsIni } from "/src/kn5-workspace.js";
import { applyWorkspaceEdits, captureWorkspaceBaseline, workspaceEditCount as countWorkspaceEdits } from "/src/workspace-authoring.js";
import { animationTransformForNode, parseKsAnimation, sampleKsAnimation, serializeKsAnimation } from "/src/ksanim.js";
import { bakeEditorProjectIntoKn5 } from "/src/kn5-bake.js";
import { serializeKn5 } from "/src/kn5-write.js";
import { auditTrackModel, parseSurfacesIni, resolveTrackSurface, serializeSurfacesIni } from "/src/track-validation.js";
import { applySurfaceEdits, captureSurfaceBaseline, surfaceEditCount as countSurfaceEdits } from "/src/surface-authoring.js";
import { findAcdEntry, parseAcd } from "/src/acd.js";
import { auditCarCollider, auditCarHierarchy, parseBottomCollidersIni } from "/src/car-validation.js";
import { parseKnh } from "/src/knh.js";
import { applyDriverBasePose, auditDriverHiddenObjects, auditDriverRig, findDriverModelAsset, parseDriver3dIni } from "/src/driver-workspace.js";
import { skinMeshVertices } from "/src/skinning.js";
import { parseCameraSplineCsv, parseTrackCamerasIni, rotateCameraSpline, sampleCameraSpline } from "/src/track-cameras.js";
import { buildGrassInstances, evaluateGrassFx, grassTangentSpaceNormal, GRASS_FX_DEFAULT_TEXTURE, GRASS_FX_INSTANCE_STRIDE } from "/src/grass-fx.js";
import { evaluateRainFx } from "/src/rain-fx.js";
import { computeDirectionalProbeShadowCascades, computeDirectionalShadowCascades, computeLocalLightShadow, cspLocalShadowFilter, CSP_LOCAL_SHADOW_ATLAS_SIZE, CSP_LOCAL_SHADOW_CELL_SIZE, CSP_LOCAL_SHADOW_LIMIT, CSP_LOCAL_SHADOW_SAMPLES, KS_SHADOW_BIASES, KS_SHADOW_MAP_SIZE, KS_SHADOW_SPLITS, shadowCasterEnabled } from "/src/shadows.js";
import { cspLightDistanceFade, cspLightReceiverVisible, cspLineClosestPoint, cspSecondarySpotPacking, cspSpotConePacking, cspSpotEdgePacking, evaluateKsLighting, ksEditorAutoExposure, ksEditorBloomCompositeScale, ksEditorBloomGaussianKernel, KS_EDITOR_DEFAULT_WEATHER, KS_EDITOR_EXPOSURE, KS_EDITOR_GLARE, KS_EDITOR_TONEMAP, STOCK_WEATHER_PRESETS, sunDirectionFromAngles } from "/src/lighting.js";
import { cspTrackOccluded } from "/src/csp-occlusion.js";
import { bindVaoPatch, parseVaoPatch } from "/src/vao-patch.js";
import { adjustCspSeasonColor, analyzeCspSeasonalOverrides } from "/src/seasons.js";
import { auditMaterialShaderProfiles, resolveMaterialRenderProfile } from "/src/shader-profiles.js";
import { KS_EDITOR_CUBEMAP, WEBGL_CUBEMAP_FACES, reflectionBlurFromExponent, selectReflectionCaptureItems } from "/src/reflections.js";
import { createCspWindParticles, CSP_WIND_MAP_FORMAT, CSP_WIND_MAP_SIZE, CSP_WIND_PARTICLE_COUNT, updateCspWindParticles } from "/src/csp-wind.js";
import { collectFbxAnimations, parseFbxWithTextures, resolveFbxModelTextures } from "/src/fbx-import.js";
import { applyNodeEdits, composeNodeTransform, decomposeNodeTransform, nodePathEntries } from "/src/node-authoring.js";

const $ = (selector) => document.querySelector(selector);
const fileInput = $("#file");
const canvas = $("#view");
const tree = $("#tree");
const inspector = $("#inspector");
const status = $("#status");
const loading = $("#loading");
let sceneVisibility = new WeakMap();
const renderer = createRenderer(canvas);
window.__apexRenderer = renderer;
if (renderer) renderer.onExternalTextureStatus = updateExternalTextureUi;
if (renderer) renderer.onWorkspaceLodStatus = updateWorkspaceLodUi;
if (renderer) renderer.onSkinTextureStatus = updateSkinTextureUi;
if (renderer) renderer.onAnimationStatus = updateAnimationUi;
if (renderer) renderer.onTrackCameraChange = () => { stopTrackCameraPlayback();$("#track-camera-select").value="";$("#track-camera-position-control").hidden=true;$("#track-camera-play").hidden=true;$("#track-camera-play").disabled=true;if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles); };
let model = null;
let selectedNode = null;
let inspectedMaterial = null;
let modelFile = null;
let modelSummary = null;
let cspConfig = null;
let cspEvaluation = null;
let grassFxEvaluation = null;
let rainFxEvaluation = null;
let cspFileName = "";
let activeCspConfig = null;
let editorProject = null;
let authoredConfig = null;
let undoStack = [];
let redoStack = [];
const cspContext = { conditions: {}, inputs: {} };
let assetFolderName = "";
let assetFolderFiles = [];
let assetFileIndex = createAssetFileIndex([]);
let cspTextureFolderName = "";
let cspTextureFolderFiles = [];
let assetSkins = [];
let assetSkinName = "";
let assetAnimations = [];
let importedFbxAnimations = [];
let activeAnimation = null;
let activeAnimationName = "";
let activeFbxAnimationIndex = -1;
let animationPosition = 0;
let assetSurfaceEntries = [];
let trackSurfaceConfig = null;
let trackSurfaceBaseline = null;
let trackSurfaceFileName = "";
let trackAudit = null;
let trackSurfaceBindings = new Map();
let trackCameraSets = [];
let trackCameraChoices = [];
let trackCameraErrors = [];
let trackCameraPosition = 0;
let trackCameraPlayFrame = 0;
let trackCameraPlayLast = 0;
let packedDataArchive = null;
let packedDataFileName = "";
let packedDataError = "";
let assetLayoutEntries = [];
let carColliderModel = null;
let carColliderAudit = null;
let carColliderFileName = "";
let carColliderError = "";
let bottomColliderConfig = null;
let bottomColliderFileName = "";
let carHierarchyAudit = null;
let driverConfig = null;
let driverPose = null;
let driverAudit = null;
let driverConfigFileName = "";
let driverPoseFileName = "";
let driverDataError = "";
let sharedDriverFile = null;
let sharedDriverPoseResult = null;
let sharedDriverHideAudit = null;
let driverCockpitView = false;
let driverAutoSelected = false;
let driverAssetResolution = null;
let vaoPatch = null;
let vaoBinding = null;
let vaoFileName = "";
let vaoError = "";
let loadedModelDescriptors = [];
let loadedWorkspaceOptions = {};
let reflectionRootChoices = [];
let nodePathByNode = new WeakMap();
let nodeByPath = new Map();
let nodeBaselines = new WeakMap();
let geometryBaselines = new Map();
let workspaceBaseline = null;

$("#gpu").textContent = renderer ? renderer.description : "WebGL 2 is unavailable";
document.querySelectorAll("[data-file-mirror]").forEach((input) => input.addEventListener("change", () => load([...input.files])));
fileInput.addEventListener("change", () => load([...fileInput.files]));
$("#csp-file").addEventListener("change", (event) => loadCspConfig(event.target.files[0]));
$("#vao-file").addEventListener("change", (event) => loadVaoPatch(event.target.files[0]));
$("#reflection-environment-file").addEventListener("change",async(event)=>{const file=event.target.files[0];event.target.value="";if(file)await loadReflectionEnvironment(file);});
$("#reflection-root").addEventListener("change",(event)=>{const choice=reflectionRootChoices[Number(event.target.value)];renderer?.setReflectionCaptureRoot(choice?.node||null);if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#asset-folder").addEventListener("change", async (event) => {
  const files = [...event.target.files];
  assetFolderFiles = files;
  sharedDriverFile=null;sharedDriverPoseResult=null;sharedDriverHideAudit=null;driverCockpitView=false;driverAutoSelected=false;driverAssetResolution=null;
  assetFolderName = files[0]?.webkitRelativePath?.split("/")[0] || (files.length ? `${files.length} selected files` : "");
  assetFileIndex = createAssetFileIndex(files);
  await configurePackedData();
  await configureCarCollider();
  configureLayoutChoices();
  configureSkinChoices();
  configureAnimationChoices();
  await configureDriverData();
  await configureTrackDataChoices();
  const importedFbxModels=[...new Set(loadedModelDescriptors.map((descriptor)=>descriptor.model).filter((entry)=>entry?.fbx))];
  if(importedFbxModels.length){
    await resolveFbxModelTextures(importedFbxModels,assetFileIndex);
    if(model?.workspace)await load(loadedModelDescriptors,loadedWorkspaceOptions);
    else{model=importedFbxModels[0];window.__apexFbx=model.fbx;renderer?.setModel(model);status.textContent=modelStatusText();}
  }
  await Promise.all([renderer?.setExternalFiles([...assetFolderFiles,...cspTextureFolderFiles]), renderer?.setSkinFiles([], "")]);
  refreshCarColliderAudit();
  updateExternalTextureUi();
  if (modelFile) status.textContent = modelStatusText();
  if (model && !selectedNode) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
});
$("#csp-texture-folder").addEventListener("change",async(event)=>{cspTextureFolderFiles=[...event.target.files];cspTextureFolderName=cspTextureFolderFiles[0]?.webkitRelativePath?.split("/")[0]||(cspTextureFolderFiles.length?`${cspTextureFolderFiles.length} selected files`:"");await renderer?.setExternalFiles([...assetFolderFiles,...cspTextureFolderFiles]);updateExternalTextureUi();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#driver-model-file").addEventListener("change",async(event)=>{const file=event.target.files[0];if(!file)return;const expected=`${driverConfig?.model.name||""}.kn5`;if(expected!==".kn5"&&file.name.toLowerCase()!==expected.toLowerCase()){status.textContent=`Driver config expects ${expected}, not ${file.name}`;event.target.value="";return;}sharedDriverFile=file;sharedDriverHideAudit=null;driverCockpitView=false;driverAutoSelected=false;configureDriverModelButton();const selected=assetLayoutEntries.find(({entry})=>entry.path===$("#layout-select").value);if(selected?.kind==="carLods")await loadSelectedCarLods(selected.entry);});
$("#open-layout").addEventListener("click", loadSelectedLayout);
$("#skin-select").addEventListener("change", async (event) => {
  const skin = assetSkins.find((candidate) => candidate.name === event.target.value);
  assetSkinName = skin?.name || "";
  status.textContent = assetSkinName ? `Loading skin ${assetSkinName}…` : modelStatusText();
  await renderer?.setSkinFiles(skin?.files || [], assetSkinName);
  if (modelFile) status.textContent = modelStatusText();
  if (model && !selectedNode) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
});
$("#animation-select").addEventListener("change", loadSelectedAnimation);
$("#export-ksanim").addEventListener("click", exportFbxAnimation);
$("#animation-position").addEventListener("input", (event) => {
  animationPosition = Number(event.target.value);
  $("#animation-position-output").value = animationPosition.toFixed(3);
  renderer?.setAnimation(activeAnimation, animationPosition, activeAnimationName);
  if (popupAnimationSelected() && activeCspConfig) applyCspConfig();
  if (modelFile) status.textContent = modelStatusText();
});
$("#lod-preview").addEventListener("change", (event) => renderer?.setWorkspaceLod(event.target.value === "auto" ? null : Number(event.target.value)));
$("#track-camera-select").addEventListener("change",()=>{stopTrackCameraPlayback();setTrackCameraPosition(0);applyTrackCameraPreview();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#track-camera-position").addEventListener("input",(event)=>{stopTrackCameraPlayback();setTrackCameraPosition(Number(event.target.value)||0);applyTrackCameraPreview();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#track-camera-play").addEventListener("click",()=>{if(trackCameraPlayFrame){stopTrackCameraPlayback();return;}const choice=trackCameraChoices.find((item)=>item.key===$("#track-camera-select").value);if(!choice?.camera.splineData)return;if(trackCameraPosition>=1)setTrackCameraPosition(0);trackCameraPlayLast=performance.now();$("#track-camera-play").textContent="Pause spline";$("#track-camera-play").classList.add("active");trackCameraPlayFrame=requestAnimationFrame(playTrackCameraSpline);});
$("#collider-overlay").addEventListener("click",(event)=>{if(!renderer||!carColliderModel)return;renderer.colliderVisible=!renderer.colliderVisible;event.target.classList.toggle("active",renderer.colliderVisible);renderer.draw();});
$("#surface-overlay").addEventListener("click",(event)=>{if(!renderer||!trackSurfaceBindings.size)return;renderer.surfaceOverlay=!renderer.surfaceOverlay;event.target.classList.toggle("active",renderer.surfaceOverlay);renderer.draw();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#grass-fx-toggle").addEventListener("click",(event)=>{if(!renderer||!grassFxEvaluation)return;renderer.grassVisible=!renderer.grassVisible;event.target.classList.toggle("active",renderer.grassVisible);renderer.draw();});
$("#rain-fx-toggle").addEventListener("click",(event)=>{if(!renderer||!rainFxEvaluation)return;renderer.rainVisible=!renderer.rainVisible;event.target.classList.toggle("active",renderer.rainVisible);renderer.draw();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#rain-wetness").addEventListener("input",(event)=>{const value=Number(event.target.value);$("#rain-wetness-output").value=value.toFixed(2);if(renderer){renderer.rainWetness=value;renderer.draw();}if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#sun-shadows").addEventListener("click",(event)=>{if(!renderer)return;renderer.shadowsEnabled=!renderer.shadowsEnabled;event.target.classList.toggle("active",renderer.shadowsEnabled);renderer.draw();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#scene-reflections").addEventListener("click",(event)=>{if(!renderer)return;renderer.reflectionsEnabled=!renderer.reflectionsEnabled;event.target.classList.toggle("active",renderer.reflectionsEnabled);renderer.draw();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#vao-toggle").addEventListener("click",(event)=>{if(!renderer||!vaoBinding?.matchedMeshes)return;renderer.vaoEnabled=!renderer.vaoEnabled;event.target.classList.toggle("active",renderer.vaoEnabled);renderer.draw();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#year-progress").addEventListener("input",(event)=>{const value=Number(event.target.value);cspContext.inputs.YEAR_PROGRESS=value;$("#year-progress-output").value=value.toFixed(3);applyCspConfig();});
const weatherSelect=$("#weather-select");weatherSelect.innerHTML=STOCK_WEATHER_PRESETS.map((preset)=>`<option value="${preset.id}"${preset===KS_EDITOR_DEFAULT_WEATHER?" selected":""}>${preset.name}</option>`).join("");
weatherSelect.addEventListener("change",()=>{if(!renderer)return;renderer.weatherPreset=STOCK_WEATHER_PRESETS.find((preset)=>preset.id===weatherSelect.value)||KS_EDITOR_DEFAULT_WEATHER;renderer.draw();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
function updateSunLighting(){const heading=Number($("#sun-heading").value),height=Number($("#sun-height").value);$("#sun-heading-output").value=`${heading}°`;$("#sun-height-output").value=`${height}°`;if(renderer){renderer.sunHeading=heading;renderer.sunHeight=height;renderer.draw();}if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);}
$("#sun-heading").addEventListener("input",updateSunLighting);$("#sun-height").addEventListener("input",updateSunLighting);
function updateWind(){const heading=Number($("#wind-heading").value),speed=Number($("#wind-speed").value);$("#wind-heading-output").value=`${heading}°`;$("#wind-speed-output").value=`${speed.toFixed(1)} m/s`;if(renderer){renderer.windHeading=heading;renderer.windSpeed=speed;renderer.draw();}if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);}
$("#wind-heading").addEventListener("input",updateWind);$("#wind-speed").addEventListener("input",updateWind);
$("#auto-exposure").addEventListener("click",(event)=>{if(!renderer)return;renderer.autoExposure=!renderer.autoExposure;event.target.classList.toggle("active",renderer.autoExposure);$("#exposure").disabled=renderer.autoExposure;renderer.draw();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#exposure").addEventListener("input",(event)=>{const value=Number(event.target.value);$("#exposure-output").value=value.toFixed(2);if(renderer){renderer.exposure=value;renderer.draw();}if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#driver-cockpit-view").addEventListener("click",(event)=>{if(!renderer||!sharedDriverHideAudit?.matched)return;driverCockpitView=!driverCockpitView;event.target.classList.toggle("active",driverCockpitView);event.target.textContent=driverCockpitView?"Cockpit driver · hidden":"Cockpit driver";renderer.setDriverCockpitMode(driverCockpitView,driverConfig?.hideObjects||[]);if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);});
$("#frame").addEventListener("click", () => renderer?.frame(selectedNode));
$("#isolate").addEventListener("click", (event) => {
  if (!renderer || !selectedNode) return;
  renderer.isolate = !renderer.isolate;
  event.target.classList.toggle("active", renderer.isolate);
  renderer.draw();
});
$("#show-hidden").addEventListener("click", (event) => {
  if (!renderer) return;
  renderer.setShowHidden(!renderer.showHidden);
  event.target.classList.toggle("active", renderer.showHidden);
  event.target.textContent = renderer.showHidden ? "Hidden shown" : "Show hidden";
  renderer.draw();
  if (model && !selectedNode) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
});
$("#wireframe").addEventListener("click", (event) => {
  if (!renderer) return;
  renderer.wireframe = !renderer.wireframe;
  event.target.classList.toggle("active", renderer.wireframe);
  renderer.draw();
});
$("#search").addEventListener("input", (event) => renderTree(event.target.value));
$("#undo").addEventListener("click", undoEditorChange);
$("#redo").addEventListener("click", redoEditorChange);
$("#save-project").addEventListener("click", saveEditorProject);
$("#export-csp").addEventListener("click", exportEditorCsp);
$("#export-kn5").addEventListener("click", exportBakedKn5);
$("#open-project").addEventListener("click", () => $("#project-file").click());
$("#project-file").addEventListener("change", (event) => loadEditorProject(event.target.files[0]));
window.addEventListener("keydown", (event) => {
  if (!(event.ctrlKey || event.metaKey) || event.altKey || /INPUT|TEXTAREA|SELECT/.test(event.target.tagName)) return;
  if (event.key.toLowerCase() === "z") { event.preventDefault(); event.shiftKey ? redoEditorChange() : undoEditorChange(); }
  else if (event.key.toLowerCase() === "y") { event.preventDefault(); redoEditorChange(); }
  else if (event.key.toLowerCase() === "s" && editorProject) { event.preventDefault(); saveEditorProject(); }
});
window.__apexAppReady=true;

async function load(files, workspaceOptions = {}) {
  const descriptors = (Array.isArray(files) ? files : [files]).filter(Boolean).map((entry) => entry.file ? entry : { file: entry });
  if (!descriptors.length) return;
  const displayName = workspaceOptions.name || (descriptors.length === 1 ? descriptors[0].file.name : `${descriptors[0].file.name.replace(/\.(?:kn5|fbx)$/i, "")} workspace`);
  loading.hidden = false;
  status.textContent = `Loading ${displayName}…`;
  await new Promise((resolve) => requestAnimationFrame(resolve));
  try {
    const parsedByFile = new Map(), entries = [];
    for (let index = 0; index < descriptors.length; index++) {
      const descriptor = descriptors[index], file = descriptor.file;
      status.textContent = `Loading ${file.name} · ${index + 1}/${descriptors.length}…`;
      await new Promise((resolve) => requestAnimationFrame(resolve));
      let parsed = descriptor.model || parsedByFile.get(file);
      if (!parsed) { const bytes=await file.arrayBuffer();parsed=/\.fbx$/i.test(file.name)?await parseFbxWithTextures(bytes,file.webkitRelativePath||file.name,assetFileIndex):parseKn5(bytes);if(descriptor.driverPose){sharedDriverPoseResult=applyDriverBasePose(parsed,descriptor.driverPose);parsed=sharedDriverPoseResult.model;}if(descriptor.auxiliary==="driver")sharedDriverHideAudit=auditDriverHiddenObjects(parsed,driverConfig?.hideObjects||[]);parsedByFile.set(file, parsed); }
      entries.push({ name: descriptor.name || file.name, size: file.size, model: parsed, position: descriptor.position, rotation: descriptor.rotation, lod: descriptor.lod, manifestIndex: descriptor.manifestIndex, auxiliary:descriptor.auxiliary, dynamic: descriptor.dynamic });
    }
    const forceWorkspace = workspaceOptions.forceWorkspace || entries.length > 1 || entries.some((entry) => [...(entry.position || []), ...(entry.rotation || [])].some((value) => Number(value) !== 0));
    model = forceWorkspace ? mergeKn5Models(entries, { name: displayName, kind: workspaceOptions.kind, manifest: workspaceOptions.manifest, warnings: workspaceOptions.warnings, cockpitHrDistance: workspaceOptions.cockpitHrDistance, driverHrDistance: workspaceOptions.driverHrDistance }) : entries[0].model;
    loadedModelDescriptors=descriptors.map((descriptor,index)=>({...descriptor,model:entries[index].model}));loadedWorkspaceOptions={...workspaceOptions,name:displayName};
    const totalSize = descriptors.reduce((sum, descriptor) => sum + (Number(descriptor.file.size) || 0), 0);
    modelFile = forceWorkspace ? { name: displayName, size: totalSize, workspaceKey: descriptors.map((descriptor, index) => `${descriptor.file.webkitRelativePath || descriptor.file.name}:${descriptor.file.size}:${entries[index].position || ""}:${entries[index].rotation || ""}:${entries[index].lod ? `${entries[index].lod.index},${entries[index].lod.in},${entries[index].lod.out}` : ""}`).join("|") } : descriptors[0].file;
    selectedNode = null;
    inspectedMaterial = null;
    if (renderer) { renderer.isolate = false; renderer.surfaceOverlay = false; renderer.showHidden = false; }
    $("#isolate").disabled = true; $("#isolate").classList.remove("active"); $("#frame").textContent = "Frame scene";
    $("#show-hidden").disabled = !renderer; $("#show-hidden").classList.remove("active"); $("#show-hidden").textContent = "Show hidden";
    initializeNodeAuthoring(model.root);
    geometryBaselines = captureStaticGeometryBaselines(model.root);
    initializeWorkspaceAuthoring();
    initializeEditorProject(modelFile);
    applyProjectSceneEdits();
    const nodes = walkNodes(model.root);
    const meshes = nodes.filter(({ node }) => node.kind === "mesh" || node.kind === "skinnedMesh");
    sceneVisibility = computeKn5Visibility(model.root);
    const previewMeshes = meshes.filter(({ node }) => sceneVisibility.get(node));
    const triangles = meshes.reduce((sum, { node }) => sum + node.indices.length / 3, 0);
    modelSummary = { nodes, meshes, previewMeshes, triangles };window.__apexFbx=model.fbx||null;
    trackAudit = model.workspace?.kind === "track" ? auditTrackModel(model, trackSurfaceConfig) : null;trackSurfaceBindings=trackAudit?new Map(meshes.map(({node})=>[node,resolveTrackSurface(node.name,trackSurfaceConfig)])):new Map();window.__apexTrackAudit=trackAudit;carHierarchyAudit=model.workspace?.kind==="carLods"?auditCarHierarchy(model):null;window.__apexCarHierarchyAudit=carHierarchyAudit;refreshCarColliderAudit();
    $("#welcome").hidden = true;
    $("#stats").hidden = false;
    $("#stats").innerHTML = `${previewMeshes.length.toLocaleString()} renderable / ${meshes.length.toLocaleString()} meshes<br>${Math.round(triangles).toLocaleString()} triangle${Math.round(triangles) === 1 ? "" : "s"}<br>${model.materials.length} materials${model.workspace ? `<br>${model.workspace.files.length} KN5 files` : ""}`;
    $("#node-count").textContent = nodes.length;
    $("#search").disabled = false;
    $("#frame").disabled = !renderer;
    $("#wireframe").disabled = !renderer;
    configureAnimationChoices();
    status.textContent = modelStatusText();
    renderTree();
    renderer?.setModel(model);configureReflectionCaptureChoices();applyVaoPatch();renderer?.setSurfaceBindings(trackSurfaceBindings);configureSurfaceOverlay();
    renderer?.setDriverCockpitMode(driverCockpitView,driverConfig?.hideObjects||[]);
    configureLodPreview();
    configureDriverViewButton();
    applyCspConfig();
    renderModelInspector(modelFile, nodes, triangles);
  } catch (error) {
    console.error(error);
    status.textContent = `Could not open ${displayName}`;
    inspector.innerHTML = `<div class="empty"><strong>Model read error</strong><br>${escapeHtml(error.message)}</div>`;
  } finally { loading.hidden = true; }
}

async function loadReflectionEnvironment(file){
  if(!loadedModelDescriptors.length){status.textContent="Open a car or track before adding a reflection environment";return;}
  const base=loadedModelDescriptors.filter((descriptor)=>descriptor.auxiliary!=="reflectionEnvironment"),currentKind=loadedWorkspaceOptions.kind||(model?.workspace?.manifest?model.workspace.kind:"carEnvironment"),baseName=loadedWorkspaceOptions.name||modelFile?.name?.replace(/\.kn5$/i,"")||"Scene",name=base.length===1?`${baseName.replace(/\.kn5$/i,"")} + ${file.name.replace(/\.kn5$/i,"")}`:baseName;
  await load([...base,{file,name:file.name,auxiliary:"reflectionEnvironment"}],{...loadedWorkspaceOptions,name,kind:currentKind,forceWorkspace:true});
}

function configureReflectionCaptureChoices(){
  const select=$("#reflection-root"),button=$("#reflection-environment-button");if(!model){select.hidden=true;button.hidden=true;return;}
  const environment=model.workspace?.files.find((file)=>file.auxiliary==="reflectionEnvironment"),choices=[{node:null,label:environment?`Automatic · ${environment.name}`:model.workspace?.kind==="track"?"Automatic · whole track":"Automatic · procedural fallback"}],visit=(node,path=[])=>{const next=[...path,node.name].filter(Boolean);if(node!==model.root&&node.kind==="node")choices.push({node,label:next.slice(-3).join(" › ")});for(const child of node.children||[])visit(child,next);};
  visit(model.root);reflectionRootChoices=choices;select.replaceChildren(...choices.map((choice,index)=>new Option(index?`Reflections · ${choice.label}`:`Reflections · ${choice.label}`,String(index))));select.value="0";select.hidden=false;select.title="Choose the single scene subtree supplied to ksEditor-style cubemap capture";button.hidden=false;button.firstChild.nodeValue=environment?`Showroom: ${environment.name}`:"Open showroom";button.classList.toggle("active",Boolean(environment));renderer?.setReflectionCaptureRoot(null);
}

function configureLayoutChoices() {
  const manifests = assetFileIndex.entries.map((entry) => {
    const basename = entry.path.split("/").at(-1), parent = entry.relativePath.split("/").at(-2) || "";
    if (/^models(?:_[^/]*)?\.ini$/i.test(basename)) return { entry, kind: "track" };
    if (/^lods\.ini$/i.test(basename) && /^data(?:[-_].*)?$/i.test(parent)) return { entry, kind: "carLods" };
    return null;
  }).filter(Boolean);
  const packedLods = virtualAcdAssetEntry("lods.ini");
  if (packedLods && !manifests.some(({ entry, kind }) => kind === "carLods" && /^data\/lods\.ini$/i.test(entry.relativePath))) manifests.push({ entry: packedLods, kind: "carLods" });
  manifests.sort((a, b) => a.entry.relativePath.localeCompare(b.entry.relativePath));
  assetLayoutEntries = manifests;
  const select = $("#layout-select"), button = $("#open-layout");
  select.replaceChildren(...manifests.map(({ entry, kind }) => { const option = new Option(`${kind === "carLods" ? "Car LODs" : "Track"} · ${entry.relativePath}`, entry.path); option.dataset.kind = kind; return option; }));
  select.hidden = !manifests.length; button.hidden = !manifests.length; button.disabled = !manifests.length;
  button.textContent = manifests.length === 1 ? `Open ${manifests[0].kind === "carLods" ? "car LODs" : manifests[0].entry.path.split("/").at(-1)}` : "Open workspace";
}

function configureSkinChoices() {
  assetSkins = discoverAssetSkins(assetFileIndex);
  assetSkinName = "";
  const select = $("#skin-select");
  select.replaceChildren(new Option("Embedded textures", ""), ...assetSkins.map((skin) => new Option(`Skin · ${skin.name}`, skin.name)));
  select.hidden = !assetSkins.length;
  select.value = "";
  select.title = assetSkins.length ? `${assetSkins.length} skin folders discovered` : "No skin folders discovered";
}

function configureAnimationChoices() {
  assetAnimations = discoverAssetAnimations(assetFileIndex);
  activeAnimation = null; activeAnimationName = ""; activeFbxAnimationIndex = -1; animationPosition = 0;
  const select = $("#animation-select"), control = $("#animation-position-control"), slider = $("#animation-position");
  importedFbxAnimations = collectFbxAnimations(loadedModelDescriptors.map((descriptor) => descriptor.model));
  select.replaceChildren(new Option("Bind pose", ""), ...importedFbxAnimations.map((entry, index) => new Option(`FBX · ${entry.sourceName} · ${entry.clip.name}`, `fbx:${index}`)), ...assetAnimations.map((entry) => new Option(`Anim · ${entry.relativePath}`, entry.path)));
  select.hidden = !assetAnimations.length && !importedFbxAnimations.length; select.value = "";
  select.title = importedFbxAnimations.length || assetAnimations.length ? `${importedFbxAnimations.length} FBX clips · ${assetAnimations.length} KSANIM files` : "No animation clips discovered";
  control.hidden = true; slider.value = "0"; $("#animation-position-output").value = "0.000";
  configureKsAnimationExport();
  renderer?.setAnimation(null, 0, "");
}

function configureKsAnimationExport() {
  const button = $("#export-ksanim"), clip = activeFbxAnimationIndex >= 0 ? importedFbxAnimations[activeFbxAnimationIndex]?.clip : null;
  button.hidden = !importedFbxAnimations.length;
  button.disabled = !clip || !clip.frameCount;
  button.textContent = clip ? `Export ${clip.name}` : "Export KSANIM";
  button.title = clip ? "Export this FBX clip in the game-compatible KSANIM v2 format" : "Select an imported FBX clip to export it";
}

function popupAnimationSelected() {
  return /(^|\/)lights\.ksanim$/i.test(activeAnimationName.replace(/\\/g, "/"));
}

function cspPreviewContext() {
  return popupAnimationSelected() ? { ...cspContext, popupPosition: animationPosition } : cspContext;
}

async function configureTrackDataChoices() {
  assetSurfaceEntries=assetFileIndex.entries.filter((entry)=>/(^|\/)data\/surfaces\.ini$/i.test(entry.relativePath));const packed=virtualAcdAssetEntry("surfaces.ini");if(packed&&!assetSurfaceEntries.some((entry)=>/^data\/surfaces\.ini$/i.test(entry.relativePath)))assetSurfaceEntries.push(packed);trackSurfaceConfig=null;trackSurfaceBaseline=null;trackSurfaceFileName="";trackAudit=null;trackSurfaceBindings=new Map();renderer?.setSurfaceBindings(trackSurfaceBindings);configureSurfaceOverlay();
  const entries=assetFileIndex.entries.filter((entry)=>/(^|\/)data\/cameras(?:_[^\/]*)?\.ini$/i.test(entry.relativePath)),splineCache=new Map();trackCameraSets=[];trackCameraChoices=[];trackCameraErrors=[];trackCameraPosition=0;window.__apexTrackCameras=[];
  for(const entry of entries){try{const set=parseTrackCamerasIni(await entry.file.text(),entry.relativePath),directory=entry.relativePath.split("/").slice(0,-1).join("/");for(const camera of set.cameras)if(camera.spline){const requested=normalizeAssetPath(`${directory}/${camera.spline}`),resolved=resolveAssetFile(assetFileIndex,requested);if(resolved.status!=="resolved"){set.warnings.push(`${set.source}: CAMERA_${camera.index} spline ${camera.spline} is ${resolved.status}`);continue;}let parsed=splineCache.get(resolved.path.toLowerCase());if(!parsed){parsed=parseCameraSplineCsv(await resolved.file.text(),resolved.path);splineCache.set(resolved.path.toLowerCase(),parsed);}const points=rotateCameraSpline(parsed.points,camera.splineRotation);camera.splineData={source:parsed.source,points,length:parsed.length,warnings:[...parsed.warnings]};set.warnings.push(...parsed.warnings);}trackCameraSets.push(set);for(const camera of set.cameras)trackCameraChoices.push({key:`${entry.path}:${camera.index}`,set,camera});}catch(error){console.error(error);trackCameraErrors.push(`${entry.relativePath}: ${error.message}`);}}
  const select=$("#track-camera-select");select.replaceChildren(new Option("Orbit camera",""),...trackCameraChoices.map((choice)=>new Option(`${choice.set.name||choice.set.source} · ${choice.camera.index}: ${choice.camera.name}`,choice.key)));select.hidden=!trackCameraChoices.length;select.value="";select.title=trackCameraChoices.length?`${trackCameraSets.length} camera sets · ${trackCameraChoices.length} cameras`:"No track camera files discovered";stopTrackCameraPlayback();$("#track-camera-position-control").hidden=true;$("#track-camera-play").hidden=true;$("#track-camera-play").disabled=true;setTrackCameraPosition(0);renderer?.setTrackCamera(null);window.__apexTrackCameras=trackCameraSets;
}

function setTrackCameraPosition(value){trackCameraPosition=Math.max(0,Math.min(1,Number(value)||0));$("#track-camera-position").value=String(trackCameraPosition);$("#track-camera-position-output").value=trackCameraPosition.toFixed(3);}
function stopTrackCameraPlayback(){if(trackCameraPlayFrame)cancelAnimationFrame(trackCameraPlayFrame);trackCameraPlayFrame=0;trackCameraPlayLast=0;const button=$("#track-camera-play");button.textContent="Play spline";button.classList.remove("active");}
function playTrackCameraSpline(now){const choice=trackCameraChoices.find((item)=>item.key===$("#track-camera-select").value);if(!choice?.camera.splineData){stopTrackCameraPlayback();return;}const duration=Math.max(.001,Number(choice.camera.splineAnimationLength)||15),delta=Math.max(0,(now-trackCameraPlayLast)/1000);trackCameraPlayLast=now;setTrackCameraPosition(trackCameraPosition+delta/duration);applyTrackCameraPreview();if(trackCameraPosition>=1){stopTrackCameraPlayback();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);return;}trackCameraPlayFrame=requestAnimationFrame(playTrackCameraSpline);}
function applyTrackCameraPreview(){const choice=trackCameraChoices.find((item)=>item.key===$("#track-camera-select").value)||null,control=$("#track-camera-position-control"),play=$("#track-camera-play"),hasSpline=Boolean(choice?.camera.splineData);control.hidden=!hasSpline;play.hidden=!hasSpline;play.disabled=!hasSpline;if(!choice){renderer?.setTrackCamera(null);return;}const camera=choice.camera,offset=hasSpline?sampleCameraSpline(camera.splineData.points,trackCameraPosition):[0,0,0];renderer?.setTrackCamera({...camera,position:camera.position.map((value,index)=>value+offset[index]),basePosition:[...camera.position],splinePreviewPosition:trackCameraPosition,splineOffset:offset});}

async function configurePackedData() {
  packedDataArchive=null;packedDataFileName="";packedDataError="";window.__apexPackedData=null;
  const candidates=assetFileIndex.entries.filter((entry)=>/(^|\/)data\.acd$/i.test(entry.relativePath)),entry=candidates.find((candidate)=>/^data\.acd$/i.test(candidate.relativePath))||(candidates.length===1?candidates[0]:null);
  if(!entry)return;
  try{packedDataArchive=parseAcd(await entry.file.arrayBuffer(),assetFolderName,entry.relativePath);packedDataFileName=entry.relativePath;window.__apexPackedData=packedDataArchive;}
  catch(error){console.error(error);packedDataError=error.message;packedDataFileName=entry.relativePath;}
}

async function configureCarCollider() {
  carColliderModel=null;carColliderAudit=null;carColliderFileName="";carColliderError="";bottomColliderConfig=null;bottomColliderFileName="";window.__apexColliderAudit=null;window.__apexBottomColliders=null;
  const candidates=assetFileIndex.entries.filter((entry)=>/(^|\/)collider\.kn5$/i.test(entry.relativePath)),entry=candidates.find((candidate)=>/^collider\.kn5$/i.test(candidate.relativePath))||(candidates.length===1?candidates[0]:null);
  if(entry){carColliderFileName=entry.relativePath;try{carColliderModel=parseKn5(await entry.file.arrayBuffer());}catch(error){console.error(error);carColliderError=error.message;}}
  const configs=assetFileIndex.entries.filter((candidate)=>/(^|\/)data\/colliders\.ini$/i.test(candidate.relativePath)),configEntry=configs.find((candidate)=>/^data\/colliders\.ini$/i.test(candidate.relativePath))||(configs.length===1?configs[0]:null)||virtualAcdAssetEntry("colliders.ini");
  if(configEntry){bottomColliderFileName=configEntry.relativePath;try{bottomColliderConfig=parseBottomCollidersIni(await configEntry.file.text(),configEntry.relativePath);window.__apexBottomColliders=bottomColliderConfig;}catch(error){console.error(error);carColliderError=carColliderError||error.message;}}
  renderer?.setCollider(carColliderModel);configureColliderOverlay();
}

function refreshCarColliderAudit() {
  carColliderAudit=carColliderModel&&model?auditCarCollider(carColliderModel,model):null;window.__apexColliderAudit=carColliderAudit;configureColliderOverlay();
}

function configureColliderOverlay(){const button=$("#collider-overlay"),available=Boolean(model&&carColliderModel);button.hidden=!available;button.disabled=!available;if(!available){button.classList.remove("active");if(renderer)renderer.colliderVisible=false;}}
function configureSurfaceOverlay(){const button=$("#surface-overlay"),available=Boolean(model?.workspace?.kind==="track"&&trackSurfaceBindings.size);button.hidden=!available;button.disabled=!available;button.classList.toggle("active",available&&Boolean(renderer?.surfaceOverlay));if(!available&&renderer)renderer.surfaceOverlay=false;}

function virtualAcdAssetEntry(path) {
  const entry=findAcdEntry(packedDataArchive,path);if(!entry)return null;const relativePath=normalizeAssetPath(`data/${entry.path}`),data=entry.data;
  const file={name:entry.path.split("/").at(-1),size:data.byteLength,relativePath,arrayBuffer:async()=>data.slice().buffer,text:async()=>new TextDecoder().decode(data)};
  return {file,path:`acd:${entry.path.toLowerCase()}`,relativePath,packed:true};
}

async function configureDriverData(){driverConfig=null;driverPose=null;driverAudit=null;driverConfigFileName="";driverPoseFileName="";driverDataError="";window.__apexDriverAudit=null;
  const configs=assetFileIndex.entries.filter((entry)=>/(^|\/)data\/driver3d\.ini$/i.test(entry.relativePath)),configEntry=configs.find((entry)=>/^data\/driver3d\.ini$/i.test(entry.relativePath))||(configs.length===1?configs[0]:null)||virtualAcdAssetEntry("driver3d.ini"),poses=assetFileIndex.entries.filter((entry)=>/(^|\/)driver_base_pos\.knh$/i.test(entry.relativePath)),poseEntry=poses.find((entry)=>/^driver_base_pos\.knh$/i.test(entry.relativePath))||(poses.length===1?poses[0]:null);
  try{if(configEntry){driverConfigFileName=configEntry.relativePath;driverConfig=parseDriver3dIni(await configEntry.file.text(),configEntry.relativePath);}if(poseEntry){driverPoseFileName=poseEntry.relativePath;driverPose=parseKnh(await poseEntry.file.arrayBuffer(),poseEntry.relativePath);}const animation=(name)=>assetAnimations.find((entry)=>entry.relativePath.split("/").at(-1).toLowerCase()===String(name||"").toLowerCase());const steerEntry=animation(driverConfig?.steer.name),shiftEntry=driverConfig?.shift?animation(driverConfig.shift.name):null,steerAnimation=steerEntry?parseKsAnimation(await steerEntry.file.arrayBuffer(),steerEntry.relativePath):null,shiftAnimation=shiftEntry?parseKsAnimation(await shiftEntry.file.arrayBuffer(),shiftEntry.relativePath):null;if(driverConfig||driverPose)driverAudit=auditDriverRig(driverConfig,{pose:driverPose,steerAnimation,shiftAnimation});driverAssetResolution=findDriverModelAsset(assetFileIndex.entries,driverConfig?.model.name);if(!sharedDriverFile&&driverAssetResolution.status==="resolved"){sharedDriverFile=driverAssetResolution.entry.file;driverAutoSelected=true;}window.__apexDriverAudit=driverAudit;}catch(error){console.error(error);driverDataError=error.message;}configureDriverModelButton();
}

function configureDriverModelButton(){const button=$("#driver-model-button"),name=driverConfig?.model.name||"";button.hidden=!name;button.firstChild.nodeValue=sharedDriverFile?`Driver: ${sharedDriverFile.name}${driverAutoSelected?" · auto":""}`:name?`Open ${name}.kn5`:"Open driver KN5";button.classList.toggle("active",Boolean(sharedDriverFile));button.title=driverAssetResolution?.status==="ambiguous"?`${driverAssetResolution.matches.length} files named ${driverAssetResolution.expected} are in the selected folder; choose one explicitly`:sharedDriverFile?"Configured shared driver geometry":"Choose the shared driver KN5 named by driver3d.ini";}

function configureDriverViewButton(){const button=$("#driver-cockpit-view"),available=Boolean(model?.workspace?.files.some((file)=>file.auxiliary==="driver")&&sharedDriverHideAudit?.matched);button.hidden=!sharedDriverFile;button.disabled=!available;button.classList.toggle("active",available&&driverCockpitView);button.textContent=driverCockpitView?"Cockpit driver · hidden":"Cockpit driver";button.title=sharedDriverHideAudit?`${sharedDriverHideAudit.matched}/${sharedDriverHideAudit.requested} configured objects match; ${sharedDriverHideAudit.meshCount} mesh${sharedDriverHideAudit.meshCount===1?"":"es"} hidden in cockpit mode`:"No shared driver geometry is loaded";}

async function selectTrackSurfaceConfig(manifestEntry) {
  trackSurfaceConfig=null;trackSurfaceBaseline=null;trackSurfaceFileName="";trackAudit=null;if(!assetSurfaceEntries.length)return;
  const parts=manifestEntry.relativePath.split("/"),directory=parts.slice(0,-1).join("/"),basename=parts.at(-1),layout=basename.match(/^models_(.+)\.ini$/i)?.[1];
  const preferred=[layout?normalizeAssetPath(`${directory}/${layout}/data/surfaces.ini`):"",normalizeAssetPath(`${directory}/data/surfaces.ini`)].filter(Boolean);
  let entry=preferred.map((path)=>assetSurfaceEntries.find((candidate)=>candidate.relativePath.toLowerCase()===path.toLowerCase())).find(Boolean);if(!entry&&assetSurfaceEntries.length===1)entry=assetSurfaceEntries[0];if(!entry)return;
  try{trackSurfaceConfig=parseSurfacesIni(await entry.file.text(),entry.relativePath);trackSurfaceBaseline=captureSurfaceBaseline(trackSurfaceConfig);trackSurfaceFileName=entry.relativePath;}catch(error){console.error(error);status.textContent=`Could not read ${entry.relativePath}: ${error.message}`;}
}

async function loadSelectedAnimation(event) {
  const entry = assetAnimations.find((candidate) => candidate.path === event.target.value);
  const importedMatch = event.target.value.match(/^fbx:(\d+)$/), importedIndex = importedMatch ? Number(importedMatch[1]) : -1, imported = importedFbxAnimations[importedIndex];
  activeAnimation = null; activeAnimationName = ""; activeFbxAnimationIndex = -1; animationPosition = 0;
  $("#animation-position").value = "0"; $("#animation-position-output").value = "0.000";
  if (imported) {
    activeAnimation = imported.clip; activeAnimationName = `${imported.sourceName} · ${imported.clip.name}`; activeFbxAnimationIndex = importedIndex;
    $("#animation-position-control").hidden = !imported.clip.frameCount;
    configureKsAnimationExport();
    renderer?.setAnimation(activeAnimation, 0, activeAnimationName);
    if (activeCspConfig) applyCspConfig();
    if (modelFile) status.textContent = modelStatusText();
    if (model && !selectedNode) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
    return;
  }
  if (!entry) {
    $("#animation-position-control").hidden = true;
    configureKsAnimationExport();
    renderer?.setAnimation(null, 0, "");
    if (activeCspConfig) applyCspConfig();
    if (modelFile) status.textContent = modelStatusText();
    if (model && !selectedNode) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
    return;
  }
  status.textContent = `Loading animation ${entry.relativePath}…`;
  try {
    activeAnimation = parseKsAnimation(await entry.file.arrayBuffer(), entry.relativePath);
    activeAnimationName = entry.relativePath;
    configureKsAnimationExport();
    $("#animation-position-control").hidden = false;
    renderer?.setAnimation(activeAnimation, 0, activeAnimationName);
    if (activeCspConfig) applyCspConfig();
    if (modelFile) status.textContent = modelStatusText();
    if (model && !selectedNode) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
  } catch (error) {
    console.error(error); status.textContent = `Could not open animation: ${error.message}`;
    event.target.value = ""; $("#animation-position-control").hidden = true;
    configureKsAnimationExport();
    renderer?.setAnimation(null, 0, "");
    if (activeCspConfig) applyCspConfig();
  }
}

async function loadSelectedLayout() {
  const manifestEntry = assetLayoutEntries.find(({entry}) => entry.path === $("#layout-select").value)?.entry;
  if (!manifestEntry) return;
  if ($("#layout-select").selectedOptions[0]?.dataset.kind === "carLods") return loadSelectedCarLods(manifestEntry);
  try {
    await selectTrackSurfaceConfig(manifestEntry);
    const manifest = parseModelsIni(await manifestEntry.file.text(), manifestEntry.relativePath), base = manifestEntry.relativePath.split("/").slice(0, -1).join("/"), descriptors = [], unresolved = [];
    for (const item of [...manifest.models, ...manifest.dynamicObjects]) {
      const requested = normalizeAssetPath(`${base}/${item.file}`), match = resolveAssetFile(assetFileIndex, requested);
      if (match.status !== "resolved") unresolved.push(`${item.file} (${match.status})`);
      else descriptors.push({ file: match.file, name: item.file, position: item.position || item.positionCenter, rotation: item.rotation || [0, 0, 0], manifestIndex: item.positionCenter ? null : item.index, dynamic: item.positionCenter ? item : null });
    }
    if (!descriptors.length) throw new Error("The selected manifest has no resolvable MODEL_n files");
    if (unresolved.length) throw new Error(`Could not resolve ${unresolved.length} layout file${unresolved.length === 1 ? "" : "s"}: ${unresolved.slice(0, 5).join(", ")}`);
    await load(descriptors, { name: manifestEntry.path.split("/").at(-1).replace(/\.ini$/i, ""), manifest: manifestEntry.relativePath, warnings: manifest.warnings, forceWorkspace: true });
  } catch (error) {
    console.error(error); status.textContent = `Could not open layout: ${error.message}`;
  }
}

async function loadSelectedCarLods(manifestEntry) {
  try {
    trackSurfaceConfig=null;trackSurfaceBaseline=null;trackSurfaceFileName="";trackAudit=null;
    const manifest = parseCarLodsIni(await manifestEntry.file.text(), manifestEntry.relativePath);
    const parts = manifestEntry.relativePath.split("/"), manifestDirectory = parts.slice(0, -1), dataDirectory = manifestDirectory.at(-1) || "";
    const base = /^data(?:[-_].*)?$/i.test(dataDirectory) ? manifestDirectory.slice(0, -1).join("/") : manifestDirectory.join("/");
    const descriptors = [], unresolved = [];
    for (const item of manifest.lods) {
      if (!item.file) { unresolved.push(`${item.section} (missing FILE)`); continue; }
      const requested = normalizeAssetPath(`${base}/${item.file}`), match = resolveAssetFile(assetFileIndex, requested);
      if (match.status !== "resolved") unresolved.push(`${item.file} (${match.status})`);
      else descriptors.push({ file: match.file, name: item.file, lod: item });
    }
    if(sharedDriverFile)descriptors.push({file:sharedDriverFile,name:`Driver · ${sharedDriverFile.name}`,position:driverConfig?.model.position||[0,0,0],auxiliary:"driver",driverPose});
    if (!descriptors.length) throw new Error("The selected lods.ini has no resolvable LOD_n files");
    if (unresolved.length) throw new Error(`Could not resolve ${unresolved.length} car LOD file${unresolved.length === 1 ? "" : "s"}: ${unresolved.slice(0, 5).join(", ")}`);
    const displayName = base.split("/").at(-1) || assetFolderName || "Car LOD workspace";
    await load(descriptors, { name: displayName, kind: "carLods", manifest: manifestEntry.relativePath, warnings: manifest.warnings, cockpitHrDistance: manifest.cockpitHrDistance, driverHrDistance: manifest.driverHrDistance, forceWorkspace: true });
  } catch (error) {
    console.error(error); status.textContent = `Could not open car LODs: ${error.message}`;
  }
}

function configureLodPreview(preserveSelection = false) {
  const select = $("#lod-preview"), workspace = model?.workspace;
  const carWorkspace = workspace?.kind === "carLods";
  const previous = preserveSelection ? select.value : "auto";
  select.hidden = !carWorkspace;
  select.replaceChildren(new Option("LOD auto (45° camera)", "auto"), ...(carWorkspace ? workspace.files.filter((file)=>file.lod).map((file) => new Option(`LOD ${file.lod.index} · ${format(file.lod.in)}–${format(file.lod.out)} m`, String(file.lod.index))) : []));
  select.value = [...select.options].some((option) => option.value === previous) ? previous : "auto";
  renderer?.setWorkspaceLod(select.value === "auto" ? null : Number(select.value));
}

function updateWorkspaceLodUi(value) {
  const select = $("#lod-preview"), auto = select?.querySelector('option[value="auto"]');
  if (!auto || select.hidden) return;
  const active = value.activeIndices.length ? value.activeIndices.map((index) => `LOD ${index}`).join(" + ") : "none";
  auto.textContent = `LOD auto · ${format(value.effectiveDistance)} m → ${active}`;
  select.title = `Assetto Corsa normalized distance: ${format(value.effectiveDistance)} m (camera ${format(value.cameraDistance)} m at 45° FOV)`;
}

async function loadCspConfig(file) {
  if (!file) return;
  try {
    cspConfig = expandCspMaterialTemplates(parseCspIni(await file.text(), file.name));
    cspFileName = file.name;
    applyCspConfig();
    if (model && !selectedNode) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
  } catch (error) {
    console.error(error);
    status.textContent = `Could not read CSP config ${file.name}`;
  }
}

function projectStorageKey(file = modelFile) {
  if (!file) return "";
  const identity = file.workspaceKey || `${file.name}:${file.size}`;
  let hash = 2166136261;
  for (const character of identity) hash = Math.imul(hash ^ character.charCodeAt(0), 16777619);
  return `apex-editor:project:${file.name}:${(hash >>> 0).toString(16)}`;
}

function initializeNodeAuthoring(root) {
  nodePathByNode = new WeakMap(); nodeByPath = new Map(); nodeBaselines = new WeakMap();
  for (const { node, path } of nodePathEntries(root)) {
    nodePathByNode.set(node, path); nodeByPath.set(path, node);
    nodeBaselines.set(node, { name: node.name, active: node.active, transform: node.transform ? [...node.transform] : null });
  }
}

function initializeWorkspaceAuthoring() {
  workspaceBaseline = captureWorkspaceBaseline(model);
}

function nodeBaseline(node) {
  return nodeBaselines.get(node) || { name: node.name, active: node.active, transform: node.transform ? [...node.transform] : null };
}

function restoreNodeBaselines() {
  if (!model?.root) return;
  for (const { node } of nodePathEntries(model.root)) {
    const baseline = nodeBaselines.get(node);
    if (!baseline) continue;
    node.name = baseline.name; node.active = baseline.active;
    if (baseline.transform) node.transform = [...baseline.transform];
  }
}

function applyProjectNodeEdits() {
  if (!model?.root) return;
  restoreNodeBaselines();
  const warnings = [];
  applyNodeEdits(model.root, editorProject?.nodeEdits, warnings);
  window.__apexNodeAuthoring = { paths: nodeByPath.size, edits: Object.keys(editorProject?.nodeEdits || {}).length, warnings };
}

function applyProjectWorkspaceEdits() {
  const workspace = model?.workspace;
  if (!workspace || !workspaceBaseline) return;
  const edits = editorProject?.workspaceEdits || {}, fileEdits = edits.files || {};
  const applied = applyWorkspaceEdits(model, edits, workspaceBaseline);
  try {
    const text = workspace.kind === "carLods" ? serializeCarLodsIni(workspace) : serializeModelsIni(workspace);
    workspace.authoredWarnings = workspace.kind === "carLods" ? parseCarLodsIni(text, workspace.manifest || "data/lods.ini").warnings : parseModelsIni(text, workspace.manifest || "models.ini").warnings;
  } catch (error) { workspace.authoredWarnings = [error.message]; }
  window.__apexWorkspaceAuthoring = { edits: applied, files: Object.keys(fileEdits).length, warnings: [...workspace.authoredWarnings] };
}

function applyProjectSurfaceEdits() {
  if (!trackSurfaceConfig || !trackSurfaceBaseline) return;
  const edits = editorProject?.surfaceEdits || {}, applied = applySurfaceEdits(trackSurfaceConfig, edits, trackSurfaceBaseline);
  try {
    const text = serializeSurfacesIni(trackSurfaceConfig);
    trackSurfaceConfig.authoredWarnings = parseSurfacesIni(text, trackSurfaceFileName || "data/surfaces.ini").warnings;
  } catch (error) { trackSurfaceConfig.authoredWarnings = [error.message]; }
  window.__apexSurfaceAuthoring = { edits: applied, surfaces: Object.keys(edits).length, warnings: [...trackSurfaceConfig.authoredWarnings] };
}

function applyProjectSceneEdits() {
  applyProjectSurfaceEdits();
  applyProjectNodeEdits();
  applyProjectWorkspaceEdits();
  const warnings = [];
  const applied = model?.root ? applyGeometryEdits(model.root, editorProject?.geometryEdits, geometryBaselines, warnings) : 0;
  window.__apexGeometryAuthoring = { edits: Object.keys(editorProject?.geometryEdits || {}).length, applied, warnings };
}

function refreshHierarchyAuthoring(geometryChanged = false) {
  if (!model) return;
  const nodes = walkNodes(model.root), meshes = nodes.filter(({ node }) => node.kind === "mesh" || node.kind === "skinnedMesh");
  sceneVisibility = computeKn5Visibility(model.root);
  const previewMeshes = meshes.filter(({ node }) => sceneVisibility.get(node)), triangles = meshes.reduce((sum, { node }) => sum + node.indices.length / 3, 0);
  modelSummary = { nodes, meshes, previewMeshes, triangles };
  trackAudit = model.workspace?.kind === "track" ? auditTrackModel(model, trackSurfaceConfig) : null;
  trackSurfaceBindings = trackAudit ? new Map(meshes.map(({ node }) => [node, resolveTrackSurface(node.name, trackSurfaceConfig)])) : new Map();
  window.__apexTrackAudit = trackAudit;
  carHierarchyAudit = model.workspace?.kind === "carLods" ? auditCarHierarchy(model) : null;
  window.__apexCarHierarchyAudit = carHierarchyAudit;
  $("#stats").innerHTML = `${previewMeshes.length.toLocaleString()} renderable / ${meshes.length.toLocaleString()} meshes<br>${Math.round(triangles).toLocaleString()} triangle${Math.round(triangles) === 1 ? "" : "s"}<br>${model.materials.length} materials${model.workspace ? `<br>${model.workspace.files.length} KN5 files` : ""}`;
  $("#node-count").textContent = nodes.length;
  renderTree($("#search").value);
  if (geometryChanged) renderer?.refreshGeometry(); else renderer?.refreshHierarchy();
  renderer?.setSurfaceBindings(trackSurfaceBindings);
  if (geometryChanged) { refreshCarColliderAudit(); applyVaoPatch(); }
  configureSurfaceOverlay();
  configureLodPreview(true);
  configureReflectionCaptureChoices();
}

function initializeEditorProject(file) {
  const fresh = createEditorProject({ name: file.name, size: file.size, kn5Version: model.version });
  editorProject = fresh;
  undoStack = []; redoStack = [];
  try {
    const saved = localStorage.getItem(projectStorageKey(file));
    if (saved) {
      const recovered = normalizeEditorProject(JSON.parse(saved));
      if (recovered.asset.size === file.size && recovered.asset.kn5Version === model.version) editorProject = recovered;
    }
  } catch (error) { console.warn("Could not recover the local Apex project", error); }
  refreshAuthoredConfig();
  updateEditorButtons();
}

function refreshAuthoredConfig() {
  let liveProject = editorProject;
  if (editorProject && modelSummary?.meshes) {
    liveProject = cloneEditorProject(editorProject);
    for (const { node } of modelSummary.meshes) {
      const baselineName = nodeBaseline(node).name, edit = matchingProjectEdit(editorProject.meshEdits, baselineName)?.value;
      if (edit && node.name.toLowerCase() !== baselineName.toLowerCase()) liveProject.meshEdits[node.name] = edit;
    }
  }
  authoredConfig = liveProject && editorProjectCspEditCount(liveProject)
    ? parseCspIni(serializeEditorCsp(liveProject), "<Apex authored edits>")
    : null;
}

function persistEditorProject() {
  if (!editorProject || !modelFile) return;
  try { localStorage.setItem(projectStorageKey(), serializeEditorProject(editorProject)); }
  catch (error) { console.warn("Could not autosave the Apex project", error); }
}

function commitEditorChange(label, mutate) {
  if (!editorProject) return false;
  const before = serializeEditorProject(editorProject), next = cloneEditorProject(editorProject);
  mutate(next);
  const normalized = normalizeEditorProject(next), after = serializeEditorProject(normalized);
  if (after === before) return false;
  const geometryChanged = JSON.stringify(normalized.geometryEdits) !== JSON.stringify(editorProject.geometryEdits);
  const sceneChanged = geometryChanged || JSON.stringify(normalized.nodeEdits) !== JSON.stringify(editorProject.nodeEdits) || JSON.stringify(normalized.workspaceEdits) !== JSON.stringify(editorProject.workspaceEdits) || JSON.stringify(normalized.surfaceEdits) !== JSON.stringify(editorProject.surfaceEdits);
  undoStack.push({ label, snapshot: before });
  if (undoStack.length > 100) undoStack.shift();
  redoStack = [];
  editorProject = normalized;
  if (sceneChanged) { applyProjectSceneEdits(); refreshHierarchyAuthoring(geometryChanged); }
  refreshAuthoredConfig(); persistEditorProject(); applyCspConfig();
  if (sceneChanged && !selectedNode && !inspectedMaterial) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
  return true;
}

function restoreEditorSnapshot(snapshot) {
  const restored = normalizeEditorProject(JSON.parse(snapshot)), geometryChanged = JSON.stringify(restored.geometryEdits) !== JSON.stringify(editorProject?.geometryEdits), sceneChanged = geometryChanged || JSON.stringify(restored.nodeEdits) !== JSON.stringify(editorProject?.nodeEdits) || JSON.stringify(restored.workspaceEdits) !== JSON.stringify(editorProject?.workspaceEdits) || JSON.stringify(restored.surfaceEdits) !== JSON.stringify(editorProject?.surfaceEdits);
  editorProject = restored;
  if (sceneChanged) { applyProjectSceneEdits(); refreshHierarchyAuthoring(geometryChanged); }
  refreshAuthoredConfig(); persistEditorProject(); applyCspConfig();
  if (sceneChanged && !selectedNode && !inspectedMaterial) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
}

function undoEditorChange() {
  const entry = undoStack.pop();
  if (!entry || !editorProject) return;
  redoStack.push({ label: entry.label, snapshot: serializeEditorProject(editorProject) });
  restoreEditorSnapshot(entry.snapshot);
}

function redoEditorChange() {
  const entry = redoStack.pop();
  if (!entry || !editorProject) return;
  undoStack.push({ label: entry.label, snapshot: serializeEditorProject(editorProject) });
  restoreEditorSnapshot(entry.snapshot);
}

function updateEditorButtons() {
  const edits = editorProjectEditCount(editorProject), cspEdits = editorProjectCspEditCount(editorProject), available = Boolean(editorProject && model);
  $("#undo").disabled = !undoStack.length; $("#undo").title = undoStack.length ? `Undo ${undoStack.at(-1).label}` : "Nothing to undo";
  $("#redo").disabled = !redoStack.length; $("#redo").title = redoStack.length ? `Redo ${redoStack.at(-1).label}` : "Nothing to redo";
  $("#save-project").disabled = !available;
  $("#open-project").disabled = !available;
  $("#export-csp").disabled = !available || !cspEdits;
  $("#export-csp").textContent = cspEdits ? `Export CSP (${cspEdits})` : "Export CSP";
  const kn5Blocked=model?.encryption?"CSP-protected KN5 files cannot be safely rewritten":model?.workspace?"Export each source model separately; assembled workspaces are not a single source asset":model?.fbx?"Export the imported FBX scene as a game-compatible KN5":"Export a game-compatible KN5 copy with compatible authored edits baked in";
  $("#export-kn5").disabled=!available||Boolean(model?.encryption)||Boolean(model?.workspace);$("#export-kn5").title=kn5Blocked;$("#export-kn5").textContent=edits?`Export KN5 (${edits})`:model?.fbx?"Export KN5":"Export KN5 copy";
}

function projectBaseName() {
  return (modelFile?.name || "asset").replace(/\.(?:kn5|fbx)$/i, "").replace(/[^a-z0-9._-]+/gi, "_");
}

function downloadText(name, text, type) {
  const url = URL.createObjectURL(new Blob([text], { type })), anchor = document.createElement("a");
  anchor.href = url; anchor.download = name; anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function exportFbxAnimation() {
  const clip = activeFbxAnimationIndex >= 0 ? importedFbxAnimations[activeFbxAnimationIndex]?.clip : null;
  if (!clip?.frameCount) return;
  try {
    const bytes = serializeKsAnimation(clip), clipName = clip.name.replace(/[^a-z0-9._-]+/gi, "_").replace(/^_+|_+$/g, "") || "animation";
    const name = `${projectBaseName()}_${clipName}.ksanim`, url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" })), anchor = document.createElement("a");
    anchor.href = url; anchor.download = name; anchor.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
    window.__apexLastKsAnimationExport = { name, bytes: bytes.byteLength, version: 2, tracks: clip.tracks.length, frames: clip.frameCount };
    status.textContent = `Exported game-compatible KSANIM v2 · ${clip.tracks.length} tracks · ${clip.frameCount} frames`;
  } catch (error) { console.error(error); status.textContent = `Could not export KSANIM: ${error.message}`; }
}

function saveEditorProject() {
  if (editorProject) downloadText(`${projectBaseName()}.apex.json`, serializeEditorProject(editorProject), "application/json");
}

function exportEditorCsp() {
  if (editorProject && editorProjectCspEditCount(editorProject)) downloadText(`${projectBaseName()}_apex.ini`, serializeEditorCsp(editorProject), "text/plain");
}

function exportBakedKn5() {
  if(!model||!editorProject||model.encryption||model.workspace)return;
  try{restoreNodeBaselines();applyGeometryEdits(model.root,{},geometryBaselines);const baked=bakeEditorProjectIntoKn5(model,editorProject),bytes=serializeKn5(baked.model),url=URL.createObjectURL(new Blob([bytes],{type:"application/octet-stream"})),anchor=document.createElement("a");anchor.href=url;anchor.download=`${projectBaseName()}_apex.kn5`;anchor.click();setTimeout(()=>URL.revokeObjectURL(url),1000);window.__apexLastKn5Export={bytes:bytes.byteLength,warnings:[...baked.warnings],applied:{...baked.applied}};status.textContent=baked.warnings.length?`Exported KN5 with ${baked.warnings.length} CSP-only edit warning${baked.warnings.length===1?"":"s"}; export CSP to preserve them`:`Exported game-compatible KN5 · ${(bytes.byteLength/1048576).toFixed(1)} MB`;}
  catch(error){console.error(error);status.textContent=`Could not export KN5: ${error.message}`;}
  finally{applyProjectSceneEdits();}
}

async function loadEditorProject(file) {
  if (!file || !model) return;
  try {
    const loaded = normalizeEditorProject(JSON.parse(await file.text()));
    if (loaded.asset.size && loaded.asset.size !== modelFile.size) throw new Error(`Project expects a ${loaded.asset.size}-byte KN5, but ${modelFile.name} is ${modelFile.size} bytes`);
    if (loaded.asset.kn5Version && loaded.asset.kn5Version !== model.version) throw new Error(`Project expects KN5 v${loaded.asset.kn5Version}, but this model is v${model.version}`);
    const geometryChanged = JSON.stringify(loaded.geometryEdits) !== JSON.stringify(editorProject?.geometryEdits);
    editorProject = loaded; undoStack = []; redoStack = [];
    applyProjectSceneEdits(); refreshHierarchyAuthoring(geometryChanged); refreshAuthoredConfig(); persistEditorProject(); applyCspConfig();
    if (!selectedNode && !inspectedMaterial) renderModelInspector(modelFile, modelSummary.nodes, modelSummary.triangles);
  } catch (error) {
    console.error(error); status.textContent = `Could not open project: ${error.message}`;
  } finally { $("#project-file").value = ""; }
}

function applyCspConfig() {
  activeCspConfig = combinedCspConfig();
  cspEvaluation = model && activeCspConfig ? evaluateCspConfig(model, activeCspConfig, cspPreviewContext()) : null;
  window.__apexCsp = cspEvaluation;
  grassFxEvaluation=model&&activeCspConfig?evaluateGrassFx(model,activeCspConfig):null;window.__apexGrassFx=grassFxEvaluation;
  rainFxEvaluation=model&&activeCspConfig?evaluateRainFx(model,activeCspConfig):null;window.__apexRainFx=rainFxEvaluation;
  renderer?.setCsp(cspEvaluation);
  configureSeasonalControls();
  renderer?.setGrassFx(grassFxEvaluation);configureGrassFxButton();
  renderer?.setRainFx(rainFxEvaluation);configureRainFxControls();
  $("#pipeline").textContent = pipelineLabel();
  if (modelFile) status.textContent = modelStatusText();
  updateEditorButtons();
  if (selectedNode) renderNodeInspector(selectedNode);
  else if (inspectedMaterial) renderMaterialInspector(inspectedMaterial);
}

function configureGrassFxButton(){const button=$("#grass-fx-toggle"),available=Boolean(grassFxEvaluation?.sourceMeshes);button.hidden=!available;button.disabled=!available;button.classList.toggle("active",available&&Boolean(renderer?.grassVisible));if(!available&&renderer)renderer.grassVisible=false;}
function configureRainFxControls(){const button=$("#rain-fx-toggle"),control=$("#rain-wetness-control"),available=Boolean(rainFxEvaluation?.matchedMeshes||rainFxEvaluation?.streams.length);button.hidden=!available;button.disabled=!available;control.hidden=!available;button.classList.toggle("active",available&&Boolean(renderer?.rainVisible));if(!available&&renderer)renderer.rainVisible=false;}
function configureSeasonalControls(){const control=$("#year-progress-control"),input=$("#year-progress"),entry=cspEvaluation?.usedInputs.get("YEAR_PROGRESS");control.hidden=!entry;if(!entry)return;input.value=String(entry.value);$("#year-progress-output").value=Number(entry.value).toFixed(3);}

async function loadVaoPatch(file){
  if(!file)return;vaoFileName=file.name;vaoError="";loading.hidden=false;status.textContent=`Loading ${file.name}…`;
  try{vaoPatch=await parseVaoPatch(await file.arrayBuffer(),file.name);applyVaoPatch();}
  catch(error){console.error(error);vaoPatch=null;vaoBinding=null;vaoError=error.message;renderer?.setVaoBindings(new Map(),{source:file.name,error:error.message});configureVaoButton();}
  finally{loading.hidden=true;if(model)status.textContent=modelStatusText();if(model&&!selectedNode)renderModelInspector(modelFile,modelSummary.nodes,modelSummary.triangles);}
}
function applyVaoPatch(){
  vaoBinding=model&&vaoPatch?bindVaoPatch(model,vaoPatch):null;
  const summary=vaoBinding?{source:vaoFileName,version:vaoPatch.version,entry:vaoPatch.entry,records:vaoPatch.recordCount,matchedRecords:vaoBinding.matchedRecords,unmatchedRecords:vaoBinding.unmatchedRecords,alternateRecords:vaoBinding.alternateRecords,normalRecords:vaoBinding.normalRecords,matchedMeshes:vaoBinding.matchedMeshes,primaryMeshes:vaoBinding.primaryMeshes,secondaryMeshes:vaoBinding.secondaryMeshes,vertices:vaoBinding.vertices,minimum:vaoBinding.minimum,maximum:vaoBinding.maximum,mean:vaoBinding.mean,extraSamples:vaoPatch.extraSamples,treeSamples:vaoPatch.treeSamples}:{};
  renderer?.setVaoBindings(vaoBinding?.bindings||new Map(),summary);window.__apexVaoAudit=vaoBinding?{...summary,enabled:renderer?.vaoEnabled}:null;configureVaoButton();if(model)status.textContent=modelStatusText();
}
function configureVaoButton(){const button=$("#vao-toggle"),fileButton=$("#vao-file-button"),available=Boolean(vaoBinding?.matchedMeshes);button.hidden=!vaoPatch&&!vaoError;button.disabled=!available;button.classList.toggle("active",available&&Boolean(renderer?.vaoEnabled));fileButton.childNodes[0].nodeValue=vaoFileName?`VAO: ${vaoFileName}`:"Open VAO patch";fileButton.title=vaoError?vaoError:vaoBinding?`${vaoBinding.matchedRecords}/${vaoPatch.recordCount} default records bind to ${vaoBinding.matchedMeshes} loaded meshes`:vaoPatch?"VAO patch loaded; open its matching car or track geometry":"Open a CSP vertex ambient-occlusion patch for this car or track";$("#pipeline").textContent=pipelineLabel();}

function modelStatusText() {
  if (!model || !modelFile) return "No model loaded";
  const workspace = model.workspace, format = workspace ? `${workspace.files.length} model files · KN5 v${workspace.versions.join("/")}` : model.fbx ? `FBX ${model.fbx.version} ${model.fbx.format} → KN5 v${model.version}` : `KN5 v${model.version}`;
  const protectedTextures = model.encryption?.protectedTextures.length || workspace?.protectedFiles.reduce((sum, entry) => sum + (entry.encryption.protectedTextures?.length || 0), 0) || 0;
  const edits = editorProjectEditCount(editorProject);
  return `${modelFile.name} · ${format} · ${(modelFile.size / 1048576).toFixed(1)} MB${assetSkinName ? ` · skin ${assetSkinName}` : ""}${activeAnimationName ? ` · animation ${activeAnimationName} @ ${animationPosition.toFixed(3)}` : ""}${trackAudit ? ` · track audit ${trackAudit.errors} errors / ${trackAudit.warnings} warnings` : ""}${carHierarchyAudit ? ` · hierarchy ${carHierarchyAudit.errors} errors / ${carHierarchyAudit.warnings} warnings` : ""}${carColliderAudit ? ` · collider ${carColliderAudit.errors} errors / ${carColliderAudit.warnings} warnings` : ""}${driverAudit ? ` · driver ${driverAudit.errors} errors / ${driverAudit.warnings} warnings` : ""}${protectedTextures ? ` · CSP protected (${protectedTextures} textures)` : ""}${cspConfig ? ` · ${cspFileName}` : ""}${vaoBinding?` · VAO ${vaoBinding.matchedMeshes} meshes`:vaoError?" · VAO error":""}${edits ? ` · ${edits} authored edit${edits === 1 ? "" : "s"}` : ""}`;
}

function combinedCspConfig() {
  if (!cspConfig) return authoredConfig;
  if (!authoredConfig) return cspConfig;
  return {
    ...cspConfig,
    sections: [...cspConfig.sections, ...authoredConfig.sections],
    warnings: [...(cspConfig.warnings || []), ...(authoredConfig.warnings || [])]
  };
}

function renderTree(filter = "") {
  if (!model) return;
  const needle = filter.trim().toLowerCase();
  const rows = [];
  const visit = (node, depth) => {
    const matches = !needle || node.name.toLowerCase().includes(needle);
    if (matches) rows.push({ node, depth: needle ? 0 : depth });
    for (const child of node.children) visit(child, depth + 1);
  };
  visit(model.root, 0);
  tree.className = "tree";
  tree.innerHTML = rows.map(({ node, depth }, index) => `<div class="node-row node-depth-${Math.min(depth, 24)} ${node.kind} ${sceneVisibility.get(node) ? "" : "node-off"} ${node === selectedNode ? "selected" : ""}" data-row="${index}"><span class="node-icon">${node.kind === "mesh" || node.kind === "skinnedMesh" ? "◆" : "›"}</span><span class="node-name">${escapeHtml(node.name)}</span></div>`).join("");
  tree.querySelectorAll(".node-row").forEach((row) => row.addEventListener("click", () => {
    tree.querySelector(".selected")?.classList.remove("selected");
    row.classList.add("selected");
    selectedNode = rows[Number(row.dataset.row)].node;
    $("#isolate").disabled = selectedNode.kind !== "mesh" && selectedNode.kind !== "skinnedMesh";
    $("#frame").textContent = $("#isolate").disabled ? "Frame scene" : "Frame selection";
    renderNodeInspector(selectedNode);
    renderer?.select(selectedNode);
  }));
}

function renderNodeInspector(node) {
  const material = node.kind === "mesh" || node.kind === "skinnedMesh" ? model.materials[node.materialId] : null;
  inspectedMaterial = material;
  const override = cspEvaluation?.nodeOverrides.get(node);
  inspector.className = "inspector";
  const animated = activeAnimation?.tracks.some((track) => track.animated && track.name === node.name);
  inspector.innerHTML = `<div class="section"><h3>${node.kind}</h3>${kv("Name", node.name)}${kv("Active", node.active ? "Yes" : "No")}${animated ? kv("Animation", `${activeAnimationName} @ ${animationPosition.toFixed(3)}`) : ""}${node.kind === "mesh" || node.kind === "skinnedMesh" ? `${kv("Visible", node.visible ? "Yes" : "No")}${kv("Renderable", node.renderable ? "Yes" : "No")}${kv("Game preview", sceneVisibility.get(node) ? "Visible" : "Hidden")}${kv("Vertices", (node.vertices.length / node.vertexStride).toLocaleString())}${kv("Triangles", (node.indices.length / 3).toLocaleString())}${node.bones ? kv("Bones", node.bones.length) : ""}${kv("Layer", node.layer)}${kv("LOD range", `${format(node.lodIn)} – ${format(node.lodOut)}`)}` : kv("Children", node.children.length)}</div>${nodeAuthoringHtml(node)}${geometryAuthoringHtml(node)}${override ? overrideHtml(override) : ""}${material ? meshAuthoringHtml(node, override) + materialHtml(material, override, node) : ""}`;
  bindNodeEditors(node);
  bindGeometryEditors(node);
  if (material) { bindMeshEditors(node); bindMaterialEditors(material); }
}

function renderModelInspector(file, nodes, triangles) {
  inspectedMaterial = null;
  inspector.className = "inspector";
  const protection = model.encryption ? `<div class="section"><h3>CSP-protected payload</h3>${kv("Format", model.encryption.format)}${kv("Payload", `${(model.encryption.payloadBytes / 1048576).toFixed(2)} MB`)}${kv("Records", model.encryption.recordCount.toLocaleString())}${kv("Protected textures", model.encryption.protectedTextures.length)}${kv("Protected meshes", model.encryption.protectedMeshes.length)}${kv("Structure", model.encryption.valid ? "Recognized" : `Invalid: ${model.encryption.error}`)}<span class="empty">The public KN5 scene is available. Protected geometry and textures remain placeholders until a compatible payload decoder is available.</span></div>` : "";
  inspector.innerHTML = `<div class="section"><h3>Model</h3>${kv("File", file.name)}${kv("Format", `KN5 v${model.version}`)}${kv("Size", `${(file.size / 1048576).toFixed(2)} MB`)}${kv("Nodes", nodes.length.toLocaleString())}${kv("Triangles", Math.round(triangles).toLocaleString())}${kv("Textures", model.textures.length)}${kv("Parsed", model.bytesRead === model.byteLength ? "Complete" : model.encryption?.valid ? "Public scene + protected payload" : `${model.bytesRead} / ${model.byteLength}`)}${kv("Authored edits", editorProjectEditCount(editorProject))}</div>${protection}${cspEvaluation ? `<div class="section"><h3>CSP configuration</h3>${kv("File", cspConfig ? cspFileName : "Apex authored edits")}${kv("Sections", activeCspConfig?.sections.length || 0)}${kv("Expanded templates", cspConfig?.expandedTemplates?.length || 0)}${kv("Matched override sections", cspEvaluation.matchedSections)}${kv("Overridden meshes", cspEvaluation.nodeOverrides.size)}${kv("Custom-emissive meshes", cspEvaluation.customEmissiveMeshes)}${kv("Matched light sections", cspEvaluation.matchedLightSections)}${kv("Light instances", cspEvaluation.lights.length)}${kv("Active light instances", cspEvaluation.lights.filter((light) => Math.max(...light.color.map(Math.abs)) > 1e-5).length)}${kv("Track light occluders", cspEvaluation.trackOccluders?.length || 0)}${kv("Unresolved includes", cspEvaluation.unresolvedIncludes.length)}${cspEvaluation.unresolvedIncludes.map((name) => `<div class="resource"><span>${escapeHtml(name)}</span></div>`).join("")}</div>${cspEvaluation.usedConditions.size ? `<div class="section"><h3>Condition preview</h3>${[...cspEvaluation.usedConditions].sort().map((name) => conditionHtml(name, cspEvaluation.conditions.get(name) ?? 0)).join("")}</div>` : ""}${cspEvaluation.usedInputs.size ? `<div class="section"><h3>Input preview</h3>${[...cspEvaluation.usedInputs].sort(([a], [b]) => a.localeCompare(b)).map(([name, input]) => inputHtml(name, input)).join("")}</div>` : ""}${cspEvaluation.lights.length ? `<div class="section"><h3>CSP lights</h3>${cspEvaluation.lights.slice(0, 16).map(lightHtml).join("")}${cspEvaluation.lights.length > 16 ? `<span class="empty">${(cspEvaluation.lights.length - 16).toLocaleString()} more light instances</span>` : ""}</div>` : ""}` : ""}<div class="section"><h3>Materials</h3>${model.materials.map((m, i) => `<div class="resource material-link" data-material="${i}"><strong>${escapeHtml(m.name)}</strong><span>${escapeHtml(m.shader)}</span></div>`).join("")}</div>`;
  const supplementalHtml = fbxImportInspectorHtml() + workspaceInspectorHtml() + packedDataInspectorHtml() + carHierarchyInspectorHtml() + carColliderInspectorHtml() + driverInspectorHtml() + trackCameraInspectorHtml() + trackValidationInspectorHtml() + shaderProfilesInspectorHtml() + vaoInspectorHtml() + seasonalInspectorHtml() + weatherLightingInspectorHtml() + reflectionCaptureInspectorHtml() + lightingInspectorHtml() + grassFxInspectorHtml() + rainFxInspectorHtml() + animationInspectorHtml() + skinTextureInspectorHtml() + externalTextureInspectorHtml(), materialSection = inspector.querySelector(".section:last-child");
  if (supplementalHtml && materialSection) materialSection.insertAdjacentHTML("beforebegin", supplementalHtml);
  inspector.querySelectorAll("[data-material]").forEach((el) => el.addEventListener("click", () => renderMaterialInspector(model.materials[Number(el.dataset.material)])));
  inspector.querySelectorAll("[data-condition]").forEach((input) => input.addEventListener("input", () => {
    cspContext.conditions[input.dataset.condition] = Number(input.value);
    input.previousElementSibling.querySelector("output").value = Number(input.value).toFixed(2);
    cspEvaluation = evaluateCspConfig(model, activeCspConfig, cspPreviewContext());
    window.__apexCsp = cspEvaluation;
    renderer?.setCsp(cspEvaluation);
    $("#pipeline").textContent = pipelineLabel();
  }));
  inspector.querySelectorAll("[data-input]").forEach((input) => input.addEventListener("input", () => {
    cspContext.inputs[input.dataset.input] = Number(input.value);
    input.previousElementSibling.querySelector("output").value = format(Number(input.value));
    cspEvaluation = evaluateCspConfig(model, activeCspConfig, cspPreviewContext());
    window.__apexCsp = cspEvaluation;
    renderer?.setCsp(cspEvaluation);
    $("#pipeline").textContent = pipelineLabel();
  }));
  bindWorkspaceEditors();
  bindSurfaceEditors();
}

function renderMaterialInspector(material) {
  inspectedMaterial = material;
  const node = modelSummary.meshes.find(({ node: candidate }) => model.materials[candidate.materialId] === material)?.node;
  const override = node ? cspEvaluation?.nodeOverrides.get(node) : null;
  inspector.innerHTML = materialHtml(material, override, node);
  bindMaterialEditors(material);
}

function fbxImportInspectorHtml() {
  if (!model?.fbx) return "";
  const source = model.fbx, textures = source.textureSummary || {}, maps = source.mapSummary || {};
  return `<div class="section"><h3>FBX import</h3>${kv("Source", `${source.format} ${source.version}`)}${kv("KN5 target", `v${model.version}`)}${kv("Animations", source.animations.length)}${kv("Materials", source.materials)}${kv("Preserved maps", maps.preserved || 0)}${maps.folded ? kv("Diffuse-alpha maps", maps.folded) : ""}${maps.ignored ? kv("Ignored maps", maps.ignored) : ""}${kv("Texture references", textures.referenced || 0)}${kv("Source textures", (textures.resolved || 0) + (textures.embedded || 0))}${textures.embedded ? kv("Embedded textures", textures.embedded) : ""}${textures.missing ? kv("Missing textures", textures.missing) : ""}${textures.ambiguous ? kv("Ambiguous textures", textures.ambiguous) : ""}${textures.unsupported ? kv("Unsupported textures", textures.unsupported) : ""}${kv("Warnings", source.warnings.length)}${source.textureReferences.map((reference) => `<div class="resource"><strong>${escapeHtml(reference.material)} · ${escapeHtml(reference.slot || "texture")}</strong><span>${escapeHtml(reference.source)} · ${escapeHtml(reference.status)}${reference.output ? ` → ${escapeHtml(reference.output)}` : ""}</span></div>`).join("")}${source.animations.map((clip) => `<div class="resource"><strong>${escapeHtml(clip.name)}</strong><span>${format(clip.duration)} s · ${clip.tracks.length} object tracks · ${clip.sourceTrackCount} source curves · ${clip.frameCount} frames</span></div>`).join("")}${source.warnings.map((warning) => `<div class="resource validation-warning"><strong>Import action</strong><span>${escapeHtml(warning)}</span></div>`).join("")}</div>`;
}

function pipelineLabel() {
  const external = renderer?.externalTextureStatus, texturePart = external?.requested ? ` · ${external.ready}/${external.requested} external textures` : "";
  const fbxTextures=model?.fbx?.textureSummary,fbxTexturePart=fbxTextures?.referenced?` · ${(fbxTextures.resolved+fbxTextures.embedded)}/${fbxTextures.referenced} source textures`:"";
  const source = model?.workspace?.kind === "carLods" ? "Car LOD workspace" : model?.workspace ? "Model workspace" : model?.fbx ? "FBX authoring" : "KN5";
  const stock = model?.workspace?.kind === "carLods" ? "Stock car LOD pipeline" : model?.workspace ? "Multi-model pipeline" : model?.fbx ? "FBX → KN5 authoring pipeline" : "Stock KN5 pipeline", skinPart = assetSkinName ? ` · skin ${assetSkinName}` : "", animationPart = activeAnimationName ? ` · ${renderer?.animationStatus.matchedNodes || 0} animated nodes` : "",vaoPart=vaoBinding?.matchedMeshes?` · VAO ${vaoBinding.matchedMeshes.toLocaleString()} meshes`:"";
  return cspEvaluation ? `${source} + CSP${skinPart}${animationPart}${vaoPart}${fbxTexturePart} · ${cspEvaluation.nodeOverrides.size.toLocaleString()} meshes · ${cspEvaluation.customEmissiveMeshes.toLocaleString()} emissive · ${cspEvaluation.lights.length.toLocaleString()} lights${texturePart}` : `${stock}${skinPart}${animationPart}${vaoPart}${fbxTexturePart}`;
}

function updateExternalTextureUi() {
  const external = renderer?.externalTextureStatus, button = $("#asset-folder-button"),cspButton=$("#csp-texture-folder-button");
  if (!external || !button) return;
  button.childNodes[0].nodeValue = assetFolderName ? `Assets: ${assetFolderName}` : "Open asset folder";
  button.title = external.requested
    ? `${external.ready} of ${external.requested} external CSP textures loaded; ${external.missing} missing, ${external.ambiguous} ambiguous, ${external.unsupported} unsupported`
    : `${external.selected.toLocaleString()} files available for project-relative CSP textures`;
  if(cspButton){cspButton.childNodes[0].nodeValue=cspTextureFolderName?`CSP: ${cspTextureFolderName}`:"Open CSP textures";cspButton.title=cspTextureFolderName?`${cspTextureFolderFiles.length.toLocaleString()} shared CSP texture files selected`:`Choose assettocorsa/extension/textures to resolve shared CSP assets such as ${GRASS_FX_DEFAULT_TEXTURE}`;}
  $("#pipeline").textContent = pipelineLabel();
}

function externalTextureInspectorHtml() {
  const external = renderer?.externalTextureStatus;
  if (!external || !external.selected && !external.requested) return "";
  const unresolved = [...external.missingPaths, ...external.ambiguousPaths];
  return `<div class="section"><h3>External CSP textures</h3>${kv("Asset folder", assetFolderName || "Not selected")}${kv("Shared CSP textures",cspTextureFolderName||"Not selected")}${kv("Available files", external.selected.toLocaleString())}${kv("Requested", external.requested)}${kv("Loaded", external.ready)}${kv("Pending", external.pending)}${kv("Missing", external.missing)}${kv("Ambiguous", external.ambiguous)}${kv("Unsupported", external.unsupported)}${unresolved.slice(0, 12).map((path) => `<div class="resource"><span>${escapeHtml(path)}</span></div>`).join("")}${unresolved.length > 12 ? `<span class="empty">${unresolved.length - 12} more unresolved paths</span>` : ""}</div>`;
}

function updateSkinTextureUi(value) {
  const select = $("#skin-select");
  if (!select) return;
  select.title = value.name ? `${value.ready}/${value.matched} matched skin textures loaded; ${value.ambiguous} ambiguous, ${value.unsupported} unsupported` : `${assetSkins.length} skin folders discovered`;
  $("#pipeline").textContent = pipelineLabel();
}

function skinTextureInspectorHtml() {
  const skin = renderer?.skinTextureStatus;
  if (!assetSkins.length && !skin?.name) return "";
  return `<div class="section"><h3>Skin textures</h3>${kv("Discovered skins", assetSkins.length)}${kv("Selected", skin?.name || "Embedded textures")}${skin?.name ? `${kv("Files in skin", skin.available)}${kv("Matched KN5 textures", skin.matched)}${kv("Loaded", skin.ready)}${kv("Pending", skin.pending)}${kv("Inherited from KN5", skin.inherited)}${kv("Ambiguous", skin.ambiguous)}${kv("Unsupported", skin.unsupported)}${skin.replacedNames.map((name) => `<div class="resource"><span>${escapeHtml(name)}</span></div>`).join("")}` : ""}</div>`;
}

function updateAnimationUi(value) {
  const select = $("#animation-select");
  if (!select || !value) return;
  select.title = value.name ? `${value.matchedTracks}/${value.animatedTracks} animated tracks matched ${value.matchedNodes} KN5 nodes` : `${importedFbxAnimations.length} FBX clips · ${assetAnimations.length} KSANIM files`;
  $("#pipeline").textContent = pipelineLabel();
}

function animationInspectorHtml() {
  const animation = renderer?.animationStatus;
  if (!assetAnimations.length && !importedFbxAnimations.length && !animation?.name) return "";
  return `<div class="section"><h3>KSANIM preview</h3>${kv("Imported FBX clips", importedFbxAnimations.length)}${kv("Discovered files", assetAnimations.length)}${kv("Selected", animation?.name || "Bind pose")}${animation?.name ? `${kv("Format", `KSANIM v${animation.version}`)}${kv("Position", animation.position.toFixed(3))}${kv("Frames", animation.frameCount)}${kv("Tracks", animation.tracks)}${kv("Animated tracks", animation.animatedTracks)}${kv("Matched tracks", animation.matchedTracks)}${kv("Matched KN5 nodes", animation.matchedNodes)}${kv("Skinned meshes", animation.skinnedMeshes)}${kv("Maximum skin displacement", `${format(animation.maxSkinnedDisplacement)} m`)}${kv("Unmatched tracks", animation.unmatchedTracks.length)}${animation.unmatchedTracks.slice(0,12).map((name)=>`<div class="resource"><span>${escapeHtml(name)}</span></div>`).join("")}${animation.unmatchedTracks.length>12?`<span class="empty">${animation.unmatchedTracks.length-12} more unmatched tracks</span>`:""}${(activeAnimation?.warnings||[]).map((warning)=>`<div class="resource"><span>${escapeHtml(warning)}</span></div>`).join("")}` : ""}</div>`;
}

function workspaceEditCount() {
  return countWorkspaceEdits(editorProject?.workspaceEdits);
}

function workspaceInspectorHtml() {
  const workspace = model?.workspace;
  if (!workspace) return "";
  const isCar = workspace.kind === "carLods", count = workspaceEditCount(), edits = editorProject?.workspaceEdits || {};
  const warnings = [...new Set([...(workspace.warnings || []), ...(workspace.authoredWarnings || [])])];
  const switchField = (key, label, current) => `<label class="author-field"><span>${label}</span><input class="${edits[key] !== undefined ? "authored" : ""}" data-edit-workspace-switch="${key}" value="${edits[key] ?? ""}" placeholder="Inherit: ${escapeHtml(String(current ?? "not set"))}"></label>`;
  const files = workspace.files.map((file, index) => {
    const edit = edits.files?.[String(index)], baseline = workspaceBaseline?.files?.[index], editable = !file.auxiliary && (isCar ? Boolean(file.lod) : true);
    const fileName = () => `<label class="author-field"><span>LOD file</span><input class="${edit?.name !== undefined ? "authored" : ""}" data-edit-workspace-file-name data-workspace-file="${index}" value="${escapeHtml(edit?.name ?? "")}" placeholder="Inherit: ${escapeHtml(baseline?.name || file.name)}" maxlength="1024" spellcheck="false"></label>`;
    const vector = (key, label, value) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-workspace-vector="${key}" data-workspace-file="${index}" value="${escapeHtml(formatEditorValue(value))}" spellcheck="false"></label>`;
    const number = (key, label, current) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-workspace-number="${key}" data-workspace-file="${index}" value="${edit?.[key] ?? ""}" placeholder="Inherit: ${escapeHtml(String(current))}"></label>`;
    const dynamicVector = (key, label, value, length = 3) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-workspace-dynamic-vector="${key}" data-vector-length="${length}" data-workspace-file="${index}" value="${escapeHtml(formatEditorValue(value))}" spellcheck="false"></label>`;
    const dynamicNumber = (key, label, value) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-workspace-dynamic-number="${key}" data-workspace-file="${index}" value="${escapeHtml(String(value))}" spellcheck="false"></label>`;
    const dynamicText = (key, label, value) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-workspace-dynamic-text="${key}" data-workspace-file="${index}" value="${escapeHtml(String(value ?? ""))}" maxlength="1024" spellcheck="false"></label>`;
    const controls = !editable ? "" : isCar
      ? `${fileName()}${number("lodIn", "LOD in", baseline?.lod?.in ?? file.lod.in)}${number("lodOut", "LOD out", baseline?.lod?.out ?? file.lod.out)}`
      : file.dynamic
        ? `${dynamicNumber("probability", "Probability %", file.dynamic.probability)}${dynamicVector("multiplicity", "Multiplicity min, max", file.dynamic.multiplicity, 2)}${dynamicText("posMode", "Position mode", file.dynamic.posMode)}${dynamicVector("positionCenter", "Position center", file.dynamic.positionCenter)}${dynamicVector("positionRange", "Position range ±", file.dynamic.positionRange)}${dynamicText("velMode", "Velocity mode", file.dynamic.velMode)}${dynamicVector("velocityBase", "Velocity base", file.dynamic.velocityBase)}${dynamicVector("velocityRange", "Velocity range ±", file.dynamic.velocityRange)}${dynamicText("playWav", "Audio file", file.dynamic.playWav)}`
        : `${vector("position", "Position", file.position)}${vector("rotation", "Rotation °", file.rotation)}`;
    return `<div class="resource"><strong>${file.auxiliary === "driver" ? "Driver · " : file.auxiliary === "reflectionEnvironment" ? "Reflection environment · " : isCar && file.lod ? `LOD ${file.lod.index} · ` : file.dynamic ? `Dynamic ${file.dynamic.index} · ` : ""}${escapeHtml(file.name)}</strong><span>${isCar && file.lod ? `${format(file.lod.in)} ≤ distance &lt; ${format(file.lod.out)} m · ` : file.auxiliary === "reflectionEnvironment" ? "Cubemap capture subtree · " : file.auxiliary ? "Shared auxiliary · " : ""}KN5 v${file.version} · ${(file.size / 1048576).toFixed(2)} MB · ${file.materials} materials · ${file.textures} textures${file.position.some((value) => value !== 0) || file.rotation.some((value) => value !== 0) ? ` · position ${file.position.map(format).join(", ")} · rotation ${file.rotation.map(format).join(", ")}` : ""}${file.dynamic ? ` · deterministic center preview · ${format(file.dynamic.probability)}% · velocity ${file.dynamic.velocityBase.map(format).join(", ")} ± ${file.dynamic.velocityRange.map(format).join(", ")}` : ""}</span>${controls}${editable ? `<div class="section-actions"><button class="mini" data-reset-workspace-file="${index}" ${edit ? "" : "disabled"}>Reset file edits</button></div>` : ""}</div>`;
  }).join("");
  return `<div class="section"><h3>${isCar ? "Car LOD workspace" : "Track workspace"}${count ? ` · <span class="edit-count">${count} edit${count === 1 ? "" : "s"}</span>` : ""}</h3>${kv("Manifest", workspace.manifest || "Manual selection")}${kv("Files", workspace.files.length)}${!isCar ? kv("Dynamic objects", workspace.files.filter((file) => file.dynamic).length) : ""}${kv("Versions", workspace.versions.join(", "))}${isCar ? `${switchField("cockpitHrDistance", "Cockpit HR", workspaceBaseline?.cockpitHrDistance)}${switchField("driverHrDistance", "Driver HR", workspaceBaseline?.driverHrDistance)}` : ""}${kv("Texture name collisions", workspace.textureCollisions.length)}${kv("Protected files", workspace.protectedFiles.length)}${kv("Warnings", warnings.length)}${files}${warnings.slice(0, 8).map((warning) => `<div class="resource validation-warning"><strong>Manifest warning</strong><span>${escapeHtml(warning)}</span></div>`).join("")}<div class="section-actions"><button class="mini" data-export-workspace>Export ${isCar ? "lods.ini" : "models.ini"}</button><button class="mini" data-reset-workspace ${count ? "" : "disabled"}>Reset manifest edits</button></div></div>`;
}

function packedDataInspectorHtml() {
  if(!packedDataFileName)return "";if(packedDataError)return `<div class="section"><h3>Packed data</h3>${kv("Archive",packedDataFileName)}${kv("Status","Could not decrypt")}${kv("Error",packedDataError)}</div>`;
  const archive=packedDataArchive;if(!archive)return "";return `<div class="section"><h3>Packed data</h3>${kv("Archive",packedDataFileName)}${kv("Asset key",archive.assetName)}${kv("Entries",archive.entries.length)}${kv("Decoded bytes",archive.entries.reduce((sum,entry)=>sum+entry.size,0).toLocaleString())}${kv("Container bytes",archive.byteLength.toLocaleString())}${kv("Header",archive.header??"Legacy / absent")}${kv("Warnings",archive.warnings.length)}${archive.entries.slice(0,16).map((entry)=>`<div class="resource"><strong>${escapeHtml(entry.path)}</strong><span>${entry.size.toLocaleString()} bytes</span></div>`).join("")}${archive.entries.length>16?`<span class="empty">${archive.entries.length-16} more packed files</span>`:""}${archive.warnings.slice(0,8).map((warning)=>`<div class="resource"><span>${escapeHtml(warning)}</span></div>`).join("")}</div>`;
}

function carColliderInspectorHtml() {
  if(!carColliderFileName&&!bottomColliderFileName)return "";if(carColliderError&&!carColliderAudit&&!bottomColliderConfig)return `<div class="section"><h3>Car collision validation</h3>${kv("Status","Could not parse")}${kv("Error",carColliderError)}</div>`;const audit=carColliderAudit,dimensions=audit?.bounds?.size.map((value)=>`${format(value)} m`).join(" × ")||"Unavailable",boxes=bottomColliderConfig?.colliders||[];
  return `<div class="section"><h3>Car collision validation</h3>${carColliderFileName?kv("3D collider",carColliderFileName):kv("3D collider","Not available")}${audit?`${kv("Meshes",audit.meshes)}${kv("Vertices",audit.vertices)}${kv("Triangles",audit.triangles)}${kv("Dimensions",dimensions)}${kv("Textures",audit.textures)}${kv("Closed manifold",audit.topology.closed?"Yes":"No")}${kv("Boundary edges",audit.topology.boundaryEdges)}${kv("Non-manifold edges",audit.topology.nonManifoldEdges)}${kv("Errors",audit.errors)}${kv("Warnings",audit.warnings)}${audit.findings.map((finding)=>`<div class="resource ${finding.severity==="error"?"validation-error":""}"><strong>${finding.severity==="error"?"Error":"Warning"}</strong><span>${escapeHtml(finding.message)}</span></div>`).join("")}`:""}${bottomColliderFileName?`${kv("Bottom boxes",`${boxes.length} · ${bottomColliderFileName}`)}${boxes.map((box)=>`<div class="resource"><strong>COLLIDER_${box.index}${box.groundEnabled?" · ground":" · disabled"}</strong><span>centre ${box.centre.map(format).join(", ")} m · size ${box.size.map(format).join(" × ")} m</span></div>`).join("")}${(bottomColliderConfig?.warnings||[]).map((warning)=>`<div class="resource"><strong>Warning</strong><span>${escapeHtml(warning)}</span></div>`).join("")}`:""}<span class="empty">Floor-plane placement needs visual verification; the lowest render-LOD vertex is only a conservative comparison.</span></div>`;
}

function carHierarchyInspectorHtml(){const audit=carHierarchyAudit;if(!audit)return "";return `<div class="section"><h3>Car hierarchy validation</h3>${kv("Required nodes per LOD",audit.requiredNodes)}${kv("Errors",audit.errors)}${kv("Warnings",audit.warnings)}${audit.lods.map((lod)=>`<div class="resource"><strong>LOD ${lod.index} · ${escapeHtml(lod.file)}</strong><span>${lod.requiredPresent}/${audit.requiredNodes} required · ${lod.nodes.toLocaleString()} nodes · ${lod.uniqueNames.toLocaleString()} unique names</span></div>`).join("")}${audit.findings.map((finding)=>`<div class="resource ${finding.severity==="error"?"validation-error":""}"><strong>${finding.severity==="error"?"Error":"Warning"} · ${escapeHtml(finding.file)}</strong><span>${escapeHtml(finding.message)}</span></div>`).join("")}</div>`;}

function driverInspectorHtml(){if(!driverConfigFileName&&!driverPoseFileName)return "";if(driverDataError)return `<div class="section"><h3>Driver rig</h3>${kv("Status","Could not parse")}${kv("Error",driverDataError)}</div>`;const audit=driverAudit;if(!audit)return "";const animation=(name,value)=>value?`<div class="resource"><strong>${name} · ${escapeHtml(value.source)}</strong><span>KSANIM v${value.version} · ${value.frames} frames · ${value.matched}/${value.animated} animated tracks match base pose</span></div>`:`<div class="resource"><strong>${name}</strong><span>Not available</span></div>`;return `<div class="section"><h3>Driver rig</h3>${kv("Config",driverConfigFileName||"Not available")}${kv("Shared driver model",audit.driverModel||"Not configured")}${kv("Selected geometry",sharedDriverFile?.name||"Not selected")}${sharedDriverPoseResult?kv("Applied pose transforms",`${sharedDriverPoseResult.applied}/${sharedDriverPoseResult.poseNodes}`):""}${kv("Model position",audit.position.map(format).join(", "))}${kv("Base pose",driverPoseFileName||"Not available")}${kv("Pose nodes",audit.poseNodes)}${kv("Steering lock",`${format(audit.steerLock)}°`)}${kv("Hidden objects",audit.hiddenObjects)}${kv("Errors",audit.errors)}${kv("Warnings",audit.warnings)}${animation("Steering",audit.steer)}${driverConfig?.shift?animation("Shift",audit.shift):""}${audit.findings.map((finding)=>`<div class="resource ${finding.severity==="error"?"validation-error":""}"><strong>${finding.severity==="error"?"Error":"Warning"}</strong><span>${escapeHtml(finding.message)}</span></div>`).join("")}${sharedDriverFile?"":`<span class="empty">Choose ${escapeHtml(audit.driverModel)}.kn5 from Assetto Corsa’s shared content/driver folder to assemble the posed geometry.</span>`}</div>`;}

function trackCameraInspectorHtml(){if(!trackCameraSets.length&&!trackCameraErrors.length)return "";const selected=trackCameraChoices.find((choice)=>choice.key===$("#track-camera-select").value),spline=selected?.camera.splineData,offset=spline?sampleCameraSpline(spline.points,trackCameraPosition):null;return `<div class="section"><h3>Track cameras</h3>${kv("Camera sets",trackCameraSets.length)}${kv("Cameras",trackCameraChoices.length)}${kv("Active preview",selected?`${selected.set.name||selected.set.source} · ${selected.camera.name}`:"Orbit camera")}${selected?`${kv("Base position",selected.camera.position.map(format).join(", "))}${offset?kv("Preview position",selected.camera.position.map((value,index)=>format(value+offset[index])).join(", ")):""}${kv("FOV",spline?`${format(selected.camera.minFov)}° · spline runtime minimum`:`${format(selected.camera.minFov)}–${format(selected.camera.maxFov)}°`)}${kv("Lap interval",selected.camera.inPoint<0?"Fixed / unbound":`${format(selected.camera.inPoint)}–${format(selected.camera.outPoint)}`)}${kv("Clip planes",`${format(selected.camera.nearPlane)}–${format(selected.camera.farPlane)} m`)}${kv("Spline",selected.camera.spline||"None")}${spline?`${kv("Spline sample",trackCameraPosition.toFixed(3))}${kv("Spline points",spline.points.length)}${kv("Spline length",`${format(spline.length)} m control polyline`)}${kv("Spline rotation",`${format(selected.camera.splineRotation)}° around world Y`)}${kv("Runtime target","Focused car; saved basis used without a live car")}`:""}`:""}${trackCameraSets.map((set)=>`<div class="resource"><strong>${escapeHtml(set.name||set.source)}</strong><span>v${set.version} · ${set.cameras.length} cameras · ${set.warnings.length} warnings · ${escapeHtml(set.source)}</span></div>`).join("")}${trackCameraSets.flatMap((set)=>set.warnings.slice(0,4)).map((warning)=>`<div class="resource"><strong>Warning</strong><span>${escapeHtml(warning)}</span></div>`).join("")}${trackCameraErrors.map((error)=>`<div class="resource validation-error"><strong>Error</strong><span>${escapeHtml(error)}</span></div>`).join("")}</div>`;}

function grassFxInspectorHtml(){const value=renderer?.grassStatus;if(!grassFxEvaluation)return "";const configurations=value?.configurationCounts||{},groups=value?.groupInstances||[],mapped=value?.mappedMeshes||[],trims=Object.entries(value?.trimStates||{}).filter(([,state])=>state.period),blur=["none","low","medium","high"][grassFxEvaluation.maskBlur]||String(grassFxEvaluation.maskBlur),passes=value?.generationPasses||[];return `<div class="section"><h3>GrassFX</h3>${kv("Config",`${grassFxEvaluation.source}:${grassFxEvaluation.line}`)}${kv("Source selector",grassFxEvaluation.grassMaterials||"None")}${kv("Source meshes",grassFxEvaluation.sourceMeshes)}${kv("Source triangles",grassFxEvaluation.sourceTriangles.toLocaleString())}${kv("Occluder meshes",grassFxEvaluation.occluderMeshes)}${kv("Generation",value?.generationMode||"Not generated")}${value?.nativeCandidateCount?kv("Native dispatch",`${value.nativeCandidateCount.toLocaleString()} threads · five 32×32 compute passes`):""}${value?.generationCandidateBudget?kv("CPU ring samples",`${(value.candidateCount||0).toLocaleString()} / ${value.nativeCandidateCount.toLocaleString()} · ${format(value.generationSamplingRate*100)}%`):kv("Sample candidates",(value?.candidateCount||0).toLocaleString())}${passes.length?kv("Camera rings",passes.map((pass)=>`P${pass.passId} ${pass.width} m · ${pass.candidates.toLocaleString()}→${pass.instances.toLocaleString()}`).join(" · ")):""}${value?.generationFallbackReason?kv("Camera fallback",value.generationFallbackReason):""}${kv("Preview blades",(value?.instanceCount||0).toLocaleString())}${kv("Eligible area",`${format(value?.totalArea||0)} m²`)}${kv("Mask rejects",(value?.rejectedByMask||0).toLocaleString())}${kv("Mask equation",value?.maskMode||"No texture sample")}${kv("Surface target",`${value?.surfaceTargetMode||"None"} · ${(value?.surfaceTargetTiles||0).toLocaleString()} tiles · ${(value?.surfaceTargetPixels||0).toLocaleString()} px`)}${kv("Surface ownership rejects",(value?.rejectedBySurfaceOwnership||0).toLocaleString())}${kv("Depth discontinuity rejects",(value?.rejectedByDepthDiscontinuity||0).toLocaleString())}${value?.rejectedByNoSurface?kv("No-surface rejects",value.rejectedByNoSurface.toLocaleString()):""}${value?.rejectedByFrustum?kv("Frustum rejects",value.rejectedByFrustum.toLocaleString()):""}${value?.distanceFadeMode!=="none"?kv("Distance fade",`${(value.rejectedByDistanceFade||0).toLocaleString()} rejects · ${format(value.fadeMin)}–${format(value.fadeMax)} A2C`):""}${value?.rejectedByConfiguredOcclusion?kv("Legacy occluder rejects",value.rejectedByConfiguredOcclusion.toLocaleString()):""}${kv("Surface samplers",`${value?.surfaceSamplers||0} decoded · ${value?.multilayerSurfaceSamplers||0} multilayer`)}${kv("Surface normals",`${value?.normalMapSamplers||0} tangent-map samplers · ${(value?.normalMapSamples||0).toLocaleString()} samples`)}${kv("Color sample mip",grassFxEvaluation.colorSampleMipLevel)}${kv("Adjustment map",`${value?.adjustmentMapMode||`direct-${blur}`} · ${value?.adjustmentSamplers||0} decoded samplers`)}${value?.adjustmentTargetTiles?kv("Adjustment target",`${value.adjustmentTargetTiles.toLocaleString()} raw tiles · ${(value?.adjustmentFinalTexels||0).toLocaleString()} filtered texels · ${(value?.adjustmentBlurSamples||0).toLocaleString()} taps`):""}${value?.adjustmentRules?kv("Adjustment draws",`${value.adjustmentRules} ordered rules · ${value.adjustmentPieces||0} world pieces (${value.adjustmentPieceSamplers||0} textured) · ${value.adjustmentExtraAreas||0} ellipse extras`):""}${kv("Shape rejects",(value?.rejectedByShape||0).toLocaleString())}${kv("Shape noise",value?.noiseMode||"Preview fallback")}${kv("Height multiplier",grassFxEvaluation.heightMult)}${kv("Area modifiers",grassFxEvaluation.areas.length)}${kv("Configurations",Object.entries(configurations).filter(([,count])=>count).map(([name,count])=>`${name} ${count.toLocaleString()}`).join(" · ")||"BASE")}${kv("Mapped meshes",mapped.map((count,index)=>count?`${"ABCD"[index]} ${count}`:"").filter(Boolean).join(" · ")||"None")}${trims.length?kv("Trim cycle",trims.map(([name,state])=>`${name} ${state.period} · ${format(state.phase)} · size ${format(state.shapeSize)}`).join(" · ")):""}${kv("Atlas",value?.atlasReady?`Loaded · ${value.texture}`:`Procedural fallback · ${value?.texture||GRASS_FX_DEFAULT_TEXTURE}`)}${kv("Atlas grid",value?.textureGrid?.length?value.textureGrid.join(" × "):"16 × 1")}${kv("Atlas routing",`${(value?.baseInstances||0).toLocaleString()} base${groups.some(Boolean)?` · ${groups.map((count,index)=>count?`G${index} ${count.toLocaleString()}`:"").filter(Boolean).join(" · ")}`:""}`)}${kv("Piece wind",`${format(value?.windMin||0)}–${format(value?.windMax||0)} response`)}${kv("Wind field",`${value?.windMapSize||64}² ${value?.windMapMode||"unavailable"} · ${value?.windParticles||32} pulses · ${(value?.windUpdates||0).toLocaleString()} updates`)}${kv("Atmospheric wind",`${format(value?.windSpeed||0)} m/s · ${format(value?.windHeading||0)}°`)}${kv("Vehicle air map",value?.airMapMode||"Unavailable")}${kv("Texture brightness",value?.textureBrightness??1)}${value?.unsupportedAdjustments?kv("Complex adjustments",`${value.unsupportedAdjustments} preserved as base`):""}${kv("Lighting",value?.weatherLit?"Weather HDR · directional backlight · fog · cascade receiver":"Preview color")}${kv("Bound local lights",(value?.localLights||0).toLocaleString())}${kv("Local-light path","Packed linear spot cone · substrate diffuse · 52.8 gloss · strongest backlight · radial ESM atlas")}${kv("Local shadow atlas",`${value?.localShadowLights||0} lights · ${value?.localShadowCasters||0} scene casters · ${value?.localShadowAtlasMode||"1024² / 4×512 ESM"}`)}${kv("Shadow caster",value?.castsShadows?`${value.instanceCount.toLocaleString()} blades`:"Disabled")}${kv("Preview",renderer?.grassVisible?"Visible":"Hidden")}<span class="empty">Local generation reproduces CSP’s five medium-profile camera rings: 32/64/128/256/512 m coverage, 0.25/0.125/0.5/0.5/1 m steps, two-metre window snapping, native PCG position jitter, depth-derived placement, frustum rejection, the pass-width distance fade, alpha-to-coverage, and far-fin ground sinking. The portable CPU preview samples 25,000 positions evenly across the exact 868,352-thread schedule; when an assembled scene frames the camera completely outside GrassFX source bounds, it explicitly falls back to the full-source editor sampler. Decoded diffuse and multilayer material passes feed CSP’s continuous color-threshold, stochastic acceptance, and fin-size equations. Supported ksPerPixel normal-mapped terrain shaders decode packed KN5 tangents and perturb the interpolated substrate normal before seasonal color and fin lean; ksMultilayer and object-space normal modes retain their geometric normal. Direct and multilayer-mask adjustments render in native rule order into a sparse 0.5 m RGBA8 world target. Rotated CENTER/SIZE texture pieces and up to eight sequential elliptical extras use CSP’s native UV, range-fade, aspect, angle, opacity, and overwrite equations; four raw mip levels and the selected MASK_BLUR kernel feed quantized bilinear configuration weights. COLOR_SAMPLE_MIP_LEVEL selects decoded material mips. A sparse 0.25 m world-space surface target applies CSP’s top-down depth ownership, bilinearly gathers four depth texels with the native 1.92 m discontinuity cutoff, applies later occluder writes, and builds the 2×2 alpha mip chain without allocating a full-track bitmap. Area modifiers, weighted atlas groups and pieces, native piece width/height scaling and wind defaults, date-driven trim behavior, and the shared 32×32 RGBA8 noise texture with native mip-0 linear wrapping are applied. Visible and shadow passes share CSP’s 64×64 ping-pong atmospheric wind field, two-frequency spatial sampling, per-atlas phase, saturated horizontal bend, and vertical length compensation. Native host tracing confirms that local-specular, cubemap-reflection, saturation, and configurable-backlight padding values are zero in this CSP build; the still-active built-in directional backlight is reproduced. Active CSP lights add the public GrassFX substrate-normal concentrated diffuse, fixed-exponent gloss, strongest-light backlighting, fake-shadow detail response, and packed linear spotlight cones in the viewport and reflection probe. A bounded static atlas shadows up to four authored spotlights with CSP’s radial exponential encode, automatic or authored shadow boost, normal-dependent receiver bias, optional extra filtering, and 10 cm GrassFX receiver offset. Dynamic/car refresh scheduling remains game-only. The editor has no moving cars, so CSP’s separate 50 m vehicle-air target is exactly zero.</span></div>`;}
function rainFxInspectorHtml(){const value=renderer?.rainStatus,binding=selectedNode&&rainFxEvaluation?.nodeBindings.get(selectedNode);if(!rainFxEvaluation)return "";return `<div class="section"><h3>RainFX</h3>${kv("Config",`${rainFxEvaluation.source}:${rainFxEvaluation.line}`)}${kv("Matched meshes",rainFxEvaluation.matchedMeshes.toLocaleString())}${kv("Puddles",rainFxEvaluation.counts.puddles.toLocaleString())}${kv("Soaking",rainFxEvaluation.counts.soaking.toLocaleString())}${kv("Smooth",rainFxEvaluation.counts.smooth.toLocaleString())}${kv("Rough",rainFxEvaluation.counts.rough.toLocaleString())}${kv("Lines",rainFxEvaluation.counts.lines.toLocaleString())}${kv("Relief",rainFxEvaluation.counts.relief.toLocaleString())}${kv("Streams",`${rainFxEvaluation.streamEdges} edges · ${rainFxEvaluation.streamPoints} points`)}${binding?kv("Selected categories",binding.categories.join(", ")||"Filter only"):""}${kv("Wetness",format(value?.wetness||0))}${kv("Preview",value?.visible?"Visible":"Hidden")}<span class="empty">Wet darkening, category-specific gloss, deterministic puddle breakup, and configured stream positions are an authoring preview. Native drainage, dynamic accumulation, occlusion, spray, rain hits, and windscreen effects are not simulated.</span></div>`;}
function vaoInspectorHtml(){if(!vaoFileName)return "";if(vaoError)return `<div class="section"><h3>CSP vertex AO</h3>${kv("Patch",vaoFileName)}${kv("Status","Could not parse")}${kv("Error",vaoError)}</div>`;if(!vaoPatch)return "";const audit=vaoBinding,value=renderer?.vaoStatus,selected=audit&&selectedNode?audit.bindings.get(selectedNode):null,range=(values)=>{if(!values)return "Not present";let minimum=255,maximum=0;for(const sample of values){minimum=Math.min(minimum,sample);maximum=Math.max(maximum,sample);}return `${minimum}–${maximum} / 255`;};return `<div class="section"><h3>CSP vertex AO</h3>${kv("Patch",vaoFileName)}${kv("Format",`Patch v${vaoPatch.version} · ${vaoPatch.entry}`)}${kv("Preview",value?.enabled?"Enabled":"Disabled")}${kv("Records",vaoPatch.recordCount.toLocaleString())}${audit?`${kv("Matched records",audit.matchedRecords.toLocaleString())}${kv("Unmatched records",audit.unmatchedRecords.toLocaleString())}${kv("Alternate-state records",audit.alternateRecords.toLocaleString())}${audit.normalRecords?kv("Normal-override records",`${audit.normalRecords.toLocaleString()} · diagnosed only`):""}${kv("Matched meshes",audit.matchedMeshes.toLocaleString())}${kv("Primary / split meshes",`${audit.primaryMeshes.toLocaleString()} / ${audit.secondaryMeshes.toLocaleString()}`)}${kv("AO vertices",audit.vertices.toLocaleString())}${kv("AO byte range",`${audit.minimum}–${audit.maximum} · mean ${format(audit.mean)}`)}`:""}${selected?`${kv("Selected primary",range(selected.primary))}${kv("Selected split channel",range(selected.secondary))}`:""}${vaoPatch.extraSamples?kv("Dynamic extra samples",`${vaoPatch.extraSamples.version===2?"v2":"v1"} · ${vaoPatch.extraSamples.bytes.toLocaleString()} bytes · diagnosed only`):""}${vaoPatch.treeSamples?kv("Tree samples",`${vaoPatch.treeSamples.bytes.toLocaleString()} bytes · diagnosed only`):""}<span class="empty">CSP’s exact name, vertex-count, and first-position binding key is used. Patch v4 bytes receive the native square-root conversion; v5 bytes remain linear. Primary AO attenuates indirect ambient and environment light. Split animation states, normal overrides, extra dynamic samples, and tree samples are retained as explicit diagnostics.</span></div>`;}
function seasonalInspectorHtml(){const value=renderer?.seasonalStatus;if(!value||(!value.affectedMeshes&&!value.legacySummerMeshes))return "";return `<div class="section"><h3>CSP seasonal materials</h3>${kv("Year progress",value.yearProgress===null?"Direct conditions":format(value.yearProgress))}${kv("Color-adjusted meshes",value.affectedMeshes.toLocaleString())}${kv("Autumn",`${value.autumnMeshes.toLocaleString()} meshes · peak ${format(value.peakAutumn)}`)}${kv("Winter",`${value.winterMeshes.toLocaleString()} meshes · peak ${format(value.peakWinter)}`)}${value.legacySummerMeshes?kv("Legacy summer",`${value.legacySummerMeshes.toLocaleString()} meshes · accepted no-op`):""}<span class="empty">Autumn yellowing and winter desaturation/brightness use CSP’s green-channel mask and upward-normal snow coverage. Tree variation uses deterministic world-space preview noise. Current CSP accepts seasonSummer for legacy configs but does not bind it to the shader.</span></div>`;}
function shaderProfilesInspectorHtml(){const value=renderer?.shaderProfileStatus;if(!value)return "";return `<div class="section"><h3>Stock shader profiles</h3>${kv("Known shipped shaders",`${value.knownStock.toLocaleString()} / ${value.materials.toLocaleString()} materials`)}${kv("Alpha blend",value.alphaBlend.toLocaleString())}${kv("Alpha-to-coverage",value.alphaToCoverage.toLocaleString())}${kv("Cutout shadow pass",value.shadowCutout.toLocaleString())}${kv("Windscreens",value.windscreens.toLocaleString())}${kv("Reflection glass",value.reflectionGlass.toLocaleString())}${kv("Refractive-capable",value.refractive.toLocaleString())}${kv("Shader package defaults",value.packageDefaults.toLocaleString())}${kv("Serialized overrides",value.serializedOverrides.toLocaleString())}${kv("Unknown shader names",value.unknownShaders.length)}${value.unknownShaders.slice(0,10).map((name)=>`<div class="resource"><span>${escapeHtml(name)}</span></div>`).join("")}${value.unknownShaders.length>10?`<span class="empty">${value.unknownShaders.length-10} more extension shaders</span>`:""}<span class="empty">Shipped alpha-tested shader packages default to multisample alpha-to-coverage. KN5 alpha-blend and alpha-to-coverage flags override that package default in native load order; every non-opaque native mode uses the cutout shadow shader.</span></div>`;}
function lightingInspectorHtml(){const value=renderer?.shadowStatus;if(!value)return "";return `<div class="section"><h3>Directional shadows</h3>${kv("Preview",value.enabled?"Enabled":"Disabled")}${kv("Shadow maps",`${value.cascades} × ${value.mapSize}²`)}${kv("Camera splits",value.splits.map((split)=>`${split} m`).join(" · "))}${kv("Native biases",value.biases.map((bias)=>bias.toExponential()).join(" · "))}${kv("Mesh casters",value.casters.toLocaleString())}${value.grassInstances?kv("GrassFX casters",value.grassInstances.toLocaleString()):""}${kv("Caster triangles",value.triangles.toLocaleString())}<span class="empty">Three camera-space cascades reproduce ksEditor’s shipped 2/12/50 m split endpoints, 2048 map size, per-cascade bias, mesh cast-shadow flags, diffuse-alpha cutouts, and tapered instanced GrassFX blades.</span></div>`;}
function weatherLightingInspectorHtml(){const value=renderer?.lightingStatus;if(!value)return "";return `<div class="section"><h3>Weather lighting</h3>${kv("Preset",value.name)}${kv("Source",value.source)}${kv("Sun",`${format(value.heading)}° heading · ${format(value.height)}° height`)}${kv("Curve blend",format(value.angleMix))}${kv("Sun HDR",value.sunColor.map(format).join(", "))}${kv("Ambient HDR",value.ambientColor.map(format).join(", "))}${kv("Sky HDR",value.skyColor.map(format).join(", "))}${kv("Fog",`${value.fogDistance.toLocaleString()} m · ${format(value.fogBlend)} blend`)}${kv("Exposure",value.autoExposure?`${format(value.effectiveExposure)} auto · ${value.exposureMin}–${value.exposureMax}`:`${format(value.effectiveExposure)} manual`)}${kv("Display",value.toneMap)}${kv("Glare",value.glareEnabled?`Yebis quality ${value.glareQuality} · ${value.bloomLevels} bloom levels · threshold ${format(value.bloomThreshold)}`:"Disabled on RGBA8 fallback")}${kv("Bloom source",`${format((value.bloomSourceScale||0)*100)}% · composite ${format(value.bloomCompositeScale||0)}`)}${kv("Bloom kernel",`${value.bloomKernel} · ${value.bloomKernelSamples.join("/")} samples · σ ${value.bloomKernelSigmas.map(format).join("/")}`)}${kv("Dither",value.dither?"Recovered 8-bit output scale":"Disabled")}${kv("HDR target",value.hdr?`${value.samples}× MSAA · RGBA16F`:"RGBA8 fallback")}<span class="empty">Version-3 curve colors use the engine’s RGB × intensity / 255 conversion and native sun-angle interpolation. The Yebis path measures exposure before glare, then applies threshold bloom, the display curve, screen composition, reciprocal gamma, and output dither.</span></div>`;}
function reflectionCaptureInspectorHtml(){const value=renderer?.sceneStatus?.reflections;if(!value)return "";const preview=!value.enabled?"Procedural fallback · live capture disabled":value.ready?"Live scene cubemap":"Procedural fallback",mode={environment:"Showroom environment",explicit:"Selected subtree",scene:"Whole scene",fallback:"Automatic fallback",disabled:"Disabled"}[value.selectionMode]||"Automatic";return `<div class="section"><h3>Scene reflections</h3>${kv("Preview",preview)}${kv("Capture selection",value.rootName?`${mode} · ${value.rootName}`:mode)}${kv("Capture target",`${value.size}² · ${value.faces||0}/6 valid faces`)}${value.ready?`${kv("Material path",value.materialPath||"Shared viewport shader")}${kv("Capture meshes",`${value.opaqueMeshes||0} opaque · ${value.transparentMeshes||0} transparent`)}${kv("Probe shadows",value.directionalShadows?`3 cascades · ${value.shadowCasters||0} mesh casters`:"Disabled")}${kv("GrassFX",value.grassFx?`${(value.grassInstances||0).toLocaleString()} lit instances${value.grassCastsShadows?" · cast/receive shadows":""}`:"Not in selected subtree")}${kv("Latest update",`${value.updatedFaces} face${value.updatedFaces===1?"":"s"} · next ${value.nextFace}`)}${kv("Capture geometry",`${value.draws.toLocaleString()} draws · ${Math.round(value.triangles).toLocaleString()} triangles`)}${kv("CSP lights",value.cspLights||0)}${kv("Clip range",`${KS_EDITOR_CUBEMAP.nearPlane}–${value.farPlane} m`)}${kv("CPU submission",`${format(value.captureMilliseconds)} ms`)}`:""}${value.reason?kv("Fallback reason",value.reason):""}<span class="empty">The capture follows ksEditor’s 512², 90° six-face camera at the active view position, initializes all faces, then refreshes one face per draw. It uses the viewport material path, probe-centered directional cascades, and weather-lit whole-track GrassFX with instanced shadow casting. Reflection sampling and refraction are disabled to prevent recursive feedback.</span></div>`;}

function trackValidationInspectorHtml() {
  if (!trackAudit) return "";
  const severity = (finding) => finding.severity === "error" ? "Error" : "Warning", edits = editorProject?.surfaceEdits || {}, editCount = countSurfaceEdits(edits);
  const surfaceControls = (trackSurfaceConfig?.surfaces || []).map((surface, position) => {
    const edit = edits[String(position)], match = trackAudit.surfaceMatches.find((item) => item.key.toUpperCase() === surface.key.toUpperCase());
    const number = (key, label) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-surface-number="${key}" data-surface-position="${position}" value="${escapeHtml(formatEditorValue(surface[key]))}" spellcheck="false"></label>`;
    const text = (key, label) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-surface-text="${key}" data-surface-position="${position}" value="${escapeHtml(String(surface[key] ?? ""))}" maxlength="${key === "key" ? 128 : 1024}" spellcheck="false"></label>`;
    const boolean = (key, label) => `<label class="author-field"><span>${label}</span><select class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-surface-boolean="${key}" data-surface-position="${position}"><option value="true" ${surface[key] ? "selected" : ""}>Yes</option><option value="false" ${surface[key] ? "" : "selected"}>No</option></select></label>`;
    return `<div class="resource"><strong>SURFACE_${surface.index} · ${escapeHtml(surface.key)}</strong><span>${(match?.count || 0).toLocaleString()} physics meshes · friction ${format(surface.friction)}${surface.isPitlane ? " · pit lane" : ""}</span>${text("key", "Surface key")}${number("friction", "Friction")}${number("damping", "Damping")}${number("dirtAdditive", "Dirt additive")}${number("blackFlagTime", "Black-flag time")}${boolean("isValidTrack", "Valid track")}${boolean("isPitlane", "Pit lane")}${number("sinHeight", "Sine height")}${number("sinLength", "Sine length")}${number("vibrationGain", "Vibration gain")}${number("vibrationLength", "Vibration length")}${text("wav", "WAV file")}${number("wavPitch", "WAV pitch")}${text("ffEffect", "FF effect")}<div class="section-actions"><button class="mini" data-reset-surface="${position}" ${edit ? "" : "disabled"}>Reset ${escapeHtml(surface.key)} edits</button></div></div>`;
  }).join("");
  const configWarnings = [...new Set([...(trackSurfaceConfig?.warnings || []), ...(trackSurfaceConfig?.authoredWarnings || [])])];
  return `<div class="section"><h3>Track validation${editCount ? ` · <span class="edit-count">${editCount} surface edit${editCount === 1 ? "" : "s"}</span>` : ""}</h3>${kv("surfaces.ini",trackSurfaceFileName||"Not available")}${kv("Track surface overrides",trackSurfaceConfig?.surfaces.length||0)}${kv("Runtime surfaces",trackAudit.runtimeSurfaces)}${kv("Starting positions",trackAudit.starts)}${kv("Pit positions",trackAudit.pits)}${kv("Timing gates",trackAudit.timeGates)}${kv("Hotlap marker",trackAudit.hotlap?"Present":"Missing")}${kv("Errors",trackAudit.errors)}${kv("Warnings",trackAudit.warnings)}${trackAudit.findings.map((finding)=>`<div class="resource ${finding.severity==="error"?"validation-error":""}"><strong>${severity(finding)}</strong><span>${escapeHtml(finding.message)}</span></div>`).join("")}${surfaceControls}${trackAudit.surfaceMatches.filter((surface)=>!(trackSurfaceConfig?.surfaces||[]).some((item)=>item.key.toUpperCase()===surface.key.toUpperCase())).map((surface)=>`<div class="resource"><strong>${escapeHtml(surface.key)}</strong><span>${surface.count.toLocaleString()} physics meshes · ${escapeHtml(surface.origin)}</span></div>`).join("")}${configWarnings.slice(0,8).map((warning)=>`<div class="resource"><strong>surfaces.ini</strong><span>${escapeHtml(warning)}</span></div>`).join("")}${trackAudit.unmatchedPhysical.slice(0,12).map((name)=>`<div class="resource"><strong>Default physics fallback</strong><span>${escapeHtml(name)}</span></div>`).join("")}${trackAudit.ambiguousPhysical.slice(0,12).map((item)=>`<div class="resource validation-error"><strong>Ambiguous physics mesh</strong><span>${escapeHtml(item.name)} → ${escapeHtml(item.keys.join(", "))}</span></div>`).join("")}${trackAudit.unmatchedPhysical.length>12?`<span class="empty">${trackAudit.unmatchedPhysical.length-12} more fallback physics meshes</span>`:""}${trackAudit.ambiguousPhysical.length>12?`<span class="empty">${trackAudit.ambiguousPhysical.length-12} more ambiguous physics meshes</span>`:""}${trackSurfaceConfig?`<div class="section-actions"><button class="mini" data-export-surfaces>Export surfaces.ini</button><button class="mini" data-reset-surfaces ${editCount ? "" : "disabled"}>Reset surface edits</button></div>`:""}</div>`;
}

function lightHtml(light) {
  const position = light.position.map(format).join(", ");
  const popup = light.popupEnabled ? ` · popup ${format(light.popupProgress)} × ${format(light.popupSecondSpotFactor)}` : "";
  const reception = light.viewMode !== "both" || light.affectsTrackMode !== "all" ? ` · ${light.viewMode} view · track ${light.affectsTrackMode}` : "";
  const detail = `${position} · ${format(light.range)} m · fade ${format(light.fadeAt)} ± ${format(light.fadeSmooth * .5)} m${reception}${popup}${light.derivedFrom ? ` · ${light.derivedFrom}` : ""}`;
  return `<div class="resource csp-source"><strong>${escapeHtml(light.section)}</strong><span>${escapeHtml(detail)}</span></div>`;
}

function conditionHtml(name, value) {
  const numeric = Math.max(0, Math.min(1, Number(value) || 0));
  return `<div class="condition-control"><label><span>${escapeHtml(name)}</span><output>${numeric.toFixed(2)}</output></label><input type="range" min="0" max="1" step="0.01" value="${numeric}" data-condition="${escapeHtml(name)}"></div>`;
}

function inputHtml(name, input) {
  const span = Math.max(1e-6, input.max - input.min), step = span <= 1 ? 0.01 : span <= 100 ? 1 : 10;
  return `<div class="condition-control"><label><span>${escapeHtml(name)}</span><output>${format(input.value)}</output></label><input type="range" min="${input.min}" max="${input.max}" step="${step}" value="${input.value}" data-input="${escapeHtml(name)}"></div>`;
}

function overrideHtml(override) {
  const custom = override.customEmissive;
  return `<div class="section"><h3>CSP effective override</h3>${override.shader ? kv("Shader", override.shader) : ""}${override.blendMode ? kv("Blend mode", override.blendMode) : ""}${override.depthMode ? kv("Depth mode", override.depthMode) : ""}${override.cullMode ? kv("Cull mode", override.cullMode) : ""}${override.isTransparent !== null ? kv("Transparent", override.isTransparent ? "Yes" : "No") : ""}${override.layer !== null ? kv("Layer", override.layer) : ""}${override.lodIn !== null || override.lodOut !== null ? kv("LOD range", `${override.lodIn ?? "inherit"} – ${override.lodOut ?? "inherit"}`) : ""}${override.castShadows !== null ? kv("Cast shadows", override.castShadows ? "Yes" : "No") : ""}${custom ? `${kv("Emissive atlas", `${custom.resolution[0]} × ${custom.resolution[1]}`)}${kv("Emissive shapes", custom.shapes.length)}${kv("Color masks", custom.colorMasks.length)}${kv("Vertex anchors", custom.vertexMask?.points.filter(Boolean).length || 0)}${kv("Bounce-back rules", custom.bounceBack.length)}${kv("Input bindings", custom.bindings.length)}${kv("Approximated operations", custom.approximatedOperations.length)}${kv("Unsupported operations", custom.unsupportedOperations.length)}` : ""}${[...override.properties].map(([key, value]) => `<div class="kv"><span>${escapeHtml(key)}</span><span class="csp-value">${escapeHtml(Array.isArray(value) ? value.map(format).join(", ") : format(value))}</span></div>`).join("")}${[...override.resources].map(([slot, resource]) => `<div class="kv"><span>${escapeHtml(slot)}</span><span class="csp-value">${escapeHtml(resource.texture || resource.file || (Array.isArray(resource.color) ? resource.color.map(format).join(", ") : "Unresolved"))}</span></div>`).join("")}${override.sources.map((source) => `<div class="resource csp-source"><strong>${escapeHtml(source.section)}</strong><span>${escapeHtml(source.source)}:${source.line}</span></div>`).join("")}</div>`;
}

function nodeEditCount(edit) {
  return ["name", "active", "transform"].filter((key) => edit?.[key] !== undefined).length;
}

function nodeAuthoringHtml(node) {
  const path = nodePathByNode.get(node), edit = editorProject?.nodeEdits?.[path], baseline = nodeBaseline(node), count = nodeEditCount(edit);
  let transform = "";
  const workspaceRoot = Boolean(model?.workspace && model.root.children.includes(node));
  if (node.kind === "node" && node.transform && !workspaceRoot) {
    const components = decomposeNodeTransform(node.transform), authored = Boolean(edit?.transform);
    const vector = (key, label, value, suffix = "") => `<label class="author-field"><span>${label}</span><input class="${authored ? "authored" : ""}" data-edit-node-transform="${key}" value="${escapeHtml(formatEditorValue(value))}" spellcheck="false"${suffix ? ` title="${escapeHtml(suffix)}"` : ""}></label>`;
    transform = `${vector("position", "Position", components.position)}${vector("rotation", "Rotation °", components.rotation, "Euler XYZ degrees")}${vector("scale", "Scale", components.scale)}${components.decomposable ? "" : `<span class="empty validation-warning">This matrix contains shear or signed scale. Changing rotation or scale replaces it with a standard TRS matrix.</span>`}<div class="section-actions"><button class="mini" data-reset-node-transform ${authored ? "" : "disabled"}>Reset transform</button></div>`;
  }
  return `<div class="section"><h3>Node authoring${count ? ` · <span class="edit-count">${count} edits</span>` : ""}</h3>${kv("Stable path", path || "Unavailable")}<label class="author-field"><span>Name</span><input class="${edit?.name !== undefined ? "authored" : ""}" data-edit-node-name value="${escapeHtml(edit?.name || "")}" placeholder="Inherit: ${escapeHtml(baseline.name)}" maxlength="1024" spellcheck="false"></label><label class="author-field"><span>Active</span><select class="${edit?.active !== undefined ? "authored" : ""}" data-edit-node-active><option value="">Inherit: ${baseline.active ? "Yes" : "No"}</option><option value="true" ${edit?.active === true ? "selected" : ""}>Yes</option><option value="false" ${edit?.active === false ? "selected" : ""}>No</option></select></label>${workspaceRoot ? `<span class="empty">Edit this workspace root transform in the model inspector so the manifest export uses the same values.</span>` : transform}<div class="section-actions"><button class="mini" data-reset-node ${edit ? "" : "disabled"}>Reset node edits</button></div></div>`;
}

function geometryAuthoringHtml(node) {
  if (node.kind !== "mesh" && node.kind !== "skinnedMesh") return "";
  if (model?.workspace) return `<div class="section"><h3>Geometry authoring</h3><span class="empty">Open this KN5 by itself to edit and export its geometry.</span></div>`;
  if (node.kind === "skinnedMesh") return `<div class="section"><h3>Geometry authoring</h3><span class="empty">Skinned bind-pose geometry stays read-only until bone-space editing can preserve every inverse bind.</span></div>`;
  const path = nodePathByNode.get(node), baseline = geometryBaselines.get(path), edit = editorProject?.geometryEdits?.[path];
  if (!path || !baseline) return "";
  const metrics = staticGeometryMetrics({ ...node, vertices: baseline.vertices, indices: baseline.indices }), currentMetrics = staticGeometryMetrics(node), components = edit?.transform ? decomposeNodeTransform(edit.transform) : { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1], decomposable: true }, count = geometryEditCount(edit);
  const vector = (key, label, value, title = "") => `<label class="author-field"><span>${label}</span><input class="${edit?.transform ? "authored" : ""}" data-edit-geometry-transform="${key}" value="${escapeHtml(formatEditorValue(value))}" spellcheck="false"${title ? ` title="${escapeHtml(title)}"` : ""}></label>`;
  const operation = (key, label, title) => `<label class="author-field"><span>${label}</span><select class="${edit?.[key] ? "authored" : ""}" data-edit-geometry-operation="${key}" title="${escapeHtml(title)}"><option value="">Keep source</option><option value="true" ${edit?.[key] ? "selected" : ""}>Apply</option></select></label>`;
  const topology = (value) => `${value.vertices.toLocaleString()} ${value.vertices === 1 ? "vertex" : "vertices"} · ${value.triangles.toLocaleString()} triangle${value.triangles === 1 ? "" : "s"}`;
  return `<div class="section"><h3>Geometry authoring${count ? ` · <span class="edit-count">${count} edit${count === 1 ? "" : "s"}</span>` : ""}</h3>${kv("Stable path", path)}${kv("Pivot", metrics.center.map(format).join(", "))}${kv("Source size", metrics.size.map(format).join(" × "))}${edit?.transform ? kv("Current size", currentMetrics.size.map(format).join(" × ")) : ""}${kv("Source topology", topology(metrics))}${count ? kv("Current topology", topology(currentMetrics)) : ""}${vector("position", "Vertex offset", components.position)}${vector("rotation", "Vertex rotation °", components.rotation, "Euler XYZ degrees")}${vector("scale", "Vertex scale", components.scale)}${components.decomposable ? "" : `<span class="empty validation-warning">This project transform cannot be represented as standard position, rotation, and scale.</span>`}${operation("removeDegenerate", "Remove degenerate triangles", "Remove repeated-index and zero-area triangles")}${operation("reverseWinding", "Reverse faces", "Reverse triangle winding and vertex normals")}${operation("recalculateNormals", "Rebuild normals", "Build area-weighted normals from the edited triangle winding")}<span class="empty">Transforms use the source bounds center. Geometry edits update vertex data, triangle data, GPU buffers, preview bounds, and KN5 output.</span><div class="section-actions"><button class="mini" data-reset-geometry ${edit ? "" : "disabled"}>Reset geometry</button></div></div>`;
}

function meshAuthoringHtml(node, override) {
  const sourceName = nodeBaseline(node).name, edit = matchingProjectEdit(editorProject?.meshEdits, sourceName)?.value, count = meshEditCount(edit), duplicateCount = modelSummary.meshes.filter(({ node: candidate }) => nodeBaseline(candidate).name.toLowerCase() === sourceName.toLowerCase()).length;
  const booleanField = (key, label, inherited) => `<label class="author-field"><span>${label}</span><select class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-mesh-boolean="${key}"><option value="">Inherit: ${inherited ? "Yes" : "No"}</option><option value="true" ${edit?.[key] === true ? "selected" : ""}>Yes</option><option value="false" ${edit?.[key] === false ? "selected" : ""}>No</option></select></label>`;
  const numericField = (key, label, inherited) => `<label class="author-field"><span>${label}</span><input class="${edit?.[key] !== undefined ? "authored" : ""}" data-edit-mesh-number="${key}" value="${edit?.[key] ?? ""}" placeholder="Inherit: ${escapeHtml(String(inherited))}"></label>`;
  return `<div class="section"><h3>Mesh authoring${count ? ` · <span class="edit-count">${count} edits</span>` : ""}</h3>${booleanField("isTransparent", "Transparent", override?.isTransparent ?? node.transparent)}${numericField("layer", "Layer", override?.layer ?? node.layer)}${numericField("lodIn", "LOD in", override?.lodIn ?? node.lodIn)}${numericField("lodOut", "LOD out", override?.lodOut ?? node.lodOut)}${booleanField("castShadows", "Shadows", override?.castShadows ?? node.castShadows)}${duplicateCount > 1 ? `<span class="empty">This name matches ${duplicateCount} meshes; the CSP export applies to all of them.</span>` : ""}<div class="section-actions"><button class="mini" data-reset-mesh ${edit ? "" : "disabled"}>Reset mesh edits</button></div></div>`;
}

function materialHtml(material, override = null, node = {}) {
  const renderProfile = resolveMaterialRenderProfile(material, node, override);
  const edit = editorProject?.materialEdits?.[material.name], stateFields = [
    ["shader", "Shader", override?.shader || material.shader], ["blendMode", "Blend", override?.blendMode ?? material.blendMode],
    ["depthMode", "Depth", override?.depthMode ?? material.depthMode], ["cullMode", "Cull", override?.cullMode || "BACK"]
  ];
  const authoring = editorProject ? `<div class="section"><h3>Authoring override${edit ? ` · <span class="edit-count">${materialEditCount(edit)} edits</span>` : ""}</h3>${stateFields.map(([key, label, inherited]) => `<label class="author-field"><span>${label}</span><input data-edit-state="${key}" value="${escapeHtml(edit?.[key] || "")}" placeholder="Inherit: ${escapeHtml(String(inherited))}"></label>`).join("")}<div class="section-actions"><button class="mini" data-reset-material ${edit ? "" : "disabled"}>Reset material edits</button></div></div>` : "";
  const properties = material.properties.map((property) => {
    const authored = authoredProperty(edit, property.name), effective = override?.properties.get(property.name.toLowerCase()), base = materialPropertyValue(property), value = authored?.value ?? effective ?? base;
    const source = authored ? "Authored" : effective !== undefined ? "CSP" : "KN5";
    return `<div class="property"><label><span>${escapeHtml(property.name)}</span><span class="${authored || effective !== undefined ? "csp-value" : ""}">${source}</span></label><div class="property-editor"><input class="${authored ? "authored" : ""}" data-edit-property="${escapeHtml(property.name)}" value="${escapeHtml(formatEditorValue(value))}" spellcheck="false"><button class="mini" data-reset-property="${escapeHtml(property.name)}" ${authored ? "" : "disabled"}>Reset</button></div></div>`;
  }).join("") || '<span class="empty">No properties</span>';
  const resourceSlots = new Map(material.resources.map((resource) => [resource.slot.toLowerCase(), resource]));
  for (const slot of Object.keys(edit?.resources || {})) if (!resourceSlots.has(slot.toLowerCase())) resourceSlots.set(slot.toLowerCase(), { slot, texture: "" });
  const resources = [...resourceSlots.values()].map((resource) => {
    const authored = authoredResource(edit, resource.slot), effective = override?.resources?.get(resource.slot.toLowerCase()), inherited = effective ? formatResourceEditor(effective) : resource.texture || "Not assigned", value = authored ? formatResourceEditor(authored.value) : inherited;
    return `<div class="resource"><strong>${escapeHtml(resource.slot)}</strong><span>${authored ? "Authored" : effective ? "CSP" : "KN5"} · ${escapeHtml(inherited)}</span><div class="property-editor"><input class="${authored ? "authored" : ""}" data-edit-resource="${escapeHtml(resource.slot)}" value="${escapeHtml(value)}" placeholder="texture.dds or color: 1, 1, 1, 1" spellcheck="false"><button class="mini" data-reset-resource="${escapeHtml(resource.slot)}" ${authored ? "" : "disabled"}>Reset</button></div></div>`;
  }).join("") || '<span class="empty">No resources</span>';
  const blendLabel=renderProfile.effectiveBlendMode===2?"alpha-to-coverage":renderProfile.effectiveBlendMode===1?renderProfile.blend:"opaque";
  const specularExponent=override?.properties.get("ksspecularexp")??propertyValue(material,"ksSpecularEXP",30),reflectionBlur=reflectionBlurFromExponent(Array.isArray(specularExponent)?specularExponent[0]:specularExponent);
  return `<div class="section"><h3>Material</h3>${kv("Name", material.name)}${kv("Shader", renderProfile.shader)}${kv("Shader profile", renderProfile.stock ? `${renderProfile.stock.vertexLayout} · shipped package` : "Unknown / extension")}${kv("Serialized blend", material.blendMode)}${kv("Effective blend", `${blendLabel} · ${renderProfile.blendSource}`)}${kv("Depth mode", override?.depthMode ?? material.depthMode)}${renderProfile.glassMode!=="none"?kv("Glass path",renderProfile.glassMode):""}${kv("Cutout shadow", renderProfile.shadowAlphaTested ? "Yes" : "No")}${kv("Reflection blur", `${format(reflectionBlur.mip)} mip${/MULTIMAP/i.test(renderProfile.shader)?" · txMaps.g modulates":""}`)}${kv("Effective cull", `${renderProfile.cull} · ${renderProfile.cullSource}`)}${kv("Effective depth", `${renderProfile.depthTest ? "test" : "off"} · ${renderProfile.depthWrite ? "write" : "read only"}`)}</div>${authoring}<div class="section"><h3>Shader properties</h3>${properties}</div><div class="section"><h3>Texture resources</h3>${resources}<span class="empty">Use <code>color:</code> for a solid RGBA value or <code>file:</code> for a project-relative CSP file.</span></div>`;
}

function materialPropertyValue(property) {
  for (const value of [property.value4, property.value3, property.value2]) if (value?.some((component) => Number(component) !== 0)) return value;
  return property.value;
}

function authoredProperty(edit, name) {
  if (!edit?.properties) return null;
  const key = Object.keys(edit.properties).find((candidate) => candidate.toLowerCase() === name.toLowerCase());
  return key ? { key, value: edit.properties[key] } : null;
}

function authoredResource(edit, name) {
  if (!edit?.resources) return null;
  const key = Object.keys(edit.resources).find((candidate) => candidate.toLowerCase() === name.toLowerCase());
  return key ? { key, value: edit.resources[key] } : null;
}

function formatResourceEditor(resource) {
  if (resource?.texture) return resource.texture;
  if (resource?.file) return `file: ${resource.file}`;
  if (Array.isArray(resource?.color)) return `color: ${formatEditorValue(resource.color)}`;
  return "";
}

function parseResourceEditor(value) {
  const text = String(value).trim();
  if (!text) return null;
  if (/^color\s*:/i.test(text)) {
    const color = parseEditorValue(text.replace(/^color\s*:/i, ""));
    if (!Array.isArray(color) || color.length < 3 || color.length > 4) throw new Error("A resource color needs three or four components");
    return { color };
  }
  if (/^file\s*:/i.test(text)) {
    const file = text.replace(/^file\s*:/i, "").trim();
    if (!file) throw new Error("Enter a project-relative resource file");
    return { file };
  }
  return { texture: text };
}

function materialEditCount(edit) {
  return Object.keys(edit?.properties || {}).length + Object.keys(edit?.resources || {}).length + ["shader", "blendMode", "depthMode", "cullMode"].filter((key) => edit?.[key]).length;
}

function meshEditCount(edit) {
  return ["isTransparent", "castShadows", "layer", "lodIn", "lodOut"].filter((key) => edit?.[key] !== undefined).length;
}

function matchingProjectEdit(record, name) {
  const key = Object.keys(record || {}).find((candidate) => candidate.toLowerCase() === String(name).toLowerCase());
  return key ? { key, value: record[key] } : null;
}

function editMesh(project, meshName, mutate) {
  const edit = project.meshEdits[meshName] || {};
  mutate(edit); project.meshEdits[meshName] = edit;
}

function editNode(project, path, mutate) {
  const edit = project.nodeEdits[path] || {};
  mutate(edit); project.nodeEdits[path] = edit;
}

function parseNodeVector(input, label) {
  const value = parseEditorValue(input.value);
  if (!Array.isArray(value) || value.length !== 3) throw new Error(`${label} needs three finite numbers`);
  return value;
}

function bindNodeEditors(node) {
  const path = nodePathByNode.get(node);
  if (!path) return;
  inspector.querySelector("[data-edit-node-name]")?.addEventListener("change", (event) => {
    const value = event.target.value.trim();
    commitEditorChange(`${value ? "Rename" : "Reset name for"} ${node.name}`, (project) => editNode(project, path, (edit) => { if (value) edit.name = value; else delete edit.name; }));
  });
  inspector.querySelector("[data-edit-node-active]")?.addEventListener("change", (event) => {
    const value = event.target.value;
    commitEditorChange(`${value ? "Set" : "Reset"} ${node.name} active`, (project) => editNode(project, path, (edit) => { if (value) edit.active = value === "true"; else delete edit.active; }));
  });
  inspector.querySelectorAll("[data-edit-node-transform]").forEach((input) => {
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") input.blur(); });
    input.addEventListener("change", () => {
      try {
        const key = input.dataset.editNodeTransform, value = parseNodeVector(input, key[0].toUpperCase() + key.slice(1));
        const current = decomposeNodeTransform(node.transform);
        let transform;
        if (key === "position") { transform = [...node.transform]; transform.splice(12, 3, ...value); }
        else transform = composeNodeTransform({ position: current.position, rotation: key === "rotation" ? value : current.rotation, scale: key === "scale" ? value : current.scale });
        commitEditorChange(`Set ${node.name} ${key}`, (project) => editNode(project, path, (edit) => { edit.transform = transform; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    });
  });
  inspector.querySelector("[data-reset-node-transform]")?.addEventListener("click", () => commitEditorChange(`Reset ${node.name} transform`, (project) => editNode(project, path, (edit) => { delete edit.transform; })));
  inspector.querySelector("[data-reset-node]")?.addEventListener("click", () => commitEditorChange(`Reset ${node.name} node edits`, (project) => { delete project.nodeEdits[path]; }));
}

function geometryTransformIsIdentity(transform) {
  const expected = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  return transform.every((value, index) => Math.abs(value - expected[index]) < 1e-7);
}

function geometryEditCount(edit) {
  return ["transform", "removeDegenerate", "reverseWinding", "recalculateNormals"].filter((key) => edit?.[key] !== undefined).length;
}

function updateGeometryEdit(project, path, mutate) {
  const edit = { ...(project.geometryEdits[path] || {}) };
  mutate(edit);
  if (Object.keys(edit).length) project.geometryEdits[path] = edit;
  else delete project.geometryEdits[path];
}

function bindGeometryEditors(node) {
  const path = nodePathByNode.get(node);
  if (!path || node.kind !== "mesh" || model?.workspace || !geometryBaselines.has(path)) return;
  inspector.querySelectorAll("[data-edit-geometry-transform]").forEach((input) => {
    const commit = () => {
      try {
        const key = input.dataset.editGeometryTransform, value = parseNodeVector(input, key[0].toUpperCase() + key.slice(1));
        if (key === "scale" && value.some((component) => Math.abs(component) < 1e-6)) throw new Error("Vertex scale cannot collapse an axis");
        const existing = editorProject?.geometryEdits?.[path], current = existing?.transform ? decomposeNodeTransform(existing.transform) : { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] };
        const transform = composeNodeTransform({ position: key === "position" ? value : current.position, rotation: key === "rotation" ? value : current.rotation, scale: key === "scale" ? value : current.scale });
        commitEditorChange(`Set ${node.name} geometry ${key}`, (project) => updateGeometryEdit(project, path, (edit) => { if (geometryTransformIsIdentity(transform)) delete edit.transform; else edit.transform = transform; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-geometry-operation]").forEach((select) => select.addEventListener("change", () => {
    const key = select.dataset.editGeometryOperation, enabled = select.value === "true";
    commitEditorChange(`${enabled ? "Apply" : "Reset"} ${node.name} ${key}`, (project) => updateGeometryEdit(project, path, (edit) => { if (enabled) edit[key] = true; else delete edit[key]; }));
  }));
  inspector.querySelector("[data-reset-geometry]")?.addEventListener("click", () => commitEditorChange(`Reset ${node.name} geometry`, (project) => { delete project.geometryEdits[path]; }));
}

function editWorkspaceFile(project, index, mutate) {
  project.workspaceEdits ||= { files: Object.create(null) };
  project.workspaceEdits.files ||= Object.create(null);
  const key = String(index), edit = project.workspaceEdits.files[key] || {};
  mutate(edit); project.workspaceEdits.files[key] = edit;
}

function editSurface(project, position, mutate) {
  project.surfaceEdits ||= Object.create(null);
  const key = String(position), edit = project.surfaceEdits[key] || {};
  mutate(edit); project.surfaceEdits[key] = edit;
}

function bindSurfaceEditors() {
  if (!trackSurfaceConfig) return;
  inspector.querySelectorAll("[data-edit-surface-number]").forEach((input) => {
    const commit = () => {
      try {
        const position = Number(input.dataset.surfacePosition), key = input.dataset.editSurfaceNumber, value = parseEditorValue(input.value);
        if (Array.isArray(value)) throw new Error(`${key} needs one finite number`);
        commitEditorChange(`Set ${trackSurfaceConfig.surfaces[position].key} ${key}`, (project) => editSurface(project, position, (edit) => { edit[key] = value; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-surface-boolean]").forEach((select) => select.addEventListener("change", () => {
    const position = Number(select.dataset.surfacePosition), key = select.dataset.editSurfaceBoolean;
    commitEditorChange(`Set ${trackSurfaceConfig.surfaces[position].key} ${key}`, (project) => editSurface(project, position, (edit) => { edit[key] = select.value === "true"; }));
  }));
  inspector.querySelectorAll("[data-edit-surface-text]").forEach((input) => {
    const commit = () => {
      try {
        const position = Number(input.dataset.surfacePosition), key = input.dataset.editSurfaceText, text = input.value.trim();
        if (text.includes(";")) throw new Error(`${key} cannot contain an INI comment marker`);
        if (key === "key") {
          if (!text) throw new Error("Surface key cannot be empty");
          const duplicate = trackSurfaceConfig.surfaces.some((surface, index) => index !== position && surface.key.toUpperCase() === text.toUpperCase());
          if (duplicate) throw new Error(`Surface key ${text.toUpperCase()} is already in use`);
        }
        commitEditorChange(`Set ${trackSurfaceConfig.surfaces[position].key} ${key}`, (project) => editSurface(project, position, (edit) => { edit[key] = key === "key" ? text.toUpperCase() : text || null; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-reset-surface]").forEach((button) => button.addEventListener("click", () => {
    const position = button.dataset.resetSurface, surface = trackSurfaceConfig.surfaces[Number(position)];
    commitEditorChange(`Reset ${surface.key} surface edits`, (project) => { delete project.surfaceEdits[position]; });
  }));
  inspector.querySelector("[data-reset-surfaces]")?.addEventListener("click", () => commitEditorChange("Reset surface edits", (project) => { project.surfaceEdits = Object.create(null); }));
  inspector.querySelector("[data-export-surfaces]")?.addEventListener("click", () => {
    try {
      const text = serializeSurfacesIni(trackSurfaceConfig), name = trackSurfaceFileName.split("/").at(-1) || "surfaces.ini";
      downloadText(name, text, "text/plain");
      window.__apexLastSurfaceExport = { name, text, warnings: [...(trackSurfaceConfig.authoredWarnings || [])] };
      const count = trackSurfaceConfig.surfaces.length;
      status.textContent = `Exported ${name} · ${count} surface${count === 1 ? "" : "s"}`;
    } catch (error) { console.error(error); status.textContent = `Could not export surfaces.ini: ${error.message}`; }
  });
}

function bindWorkspaceEditors() {
  if (!model?.workspace) return;
  inspector.querySelectorAll("[data-edit-workspace-file-name]").forEach((input) => {
    const commit = () => {
      try {
        const index = Number(input.dataset.workspaceFile), text = input.value.trim(), value = text ? normalizeCarLodFileName(text) : "";
        input.classList.remove("invalid");
        commitEditorChange(`${text ? "Set" : "Reset"} ${model.workspace.files[index].name} LOD file`, (project) => editWorkspaceFile(project, index, (edit) => { if (text) edit.name = value; else delete edit.name; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-workspace-vector]").forEach((input) => {
    const commit = () => {
      try {
        const index = Number(input.dataset.workspaceFile), key = input.dataset.editWorkspaceVector, text = input.value.trim();
        const value = text ? parseEditorValue(text) : null;
        if (text && (!Array.isArray(value) || value.length !== 3)) throw new Error(`${key} needs three finite numbers`);
        commitEditorChange(`${text ? "Set" : "Reset"} ${model.workspace.files[index].name} ${key}`, (project) => editWorkspaceFile(project, index, (edit) => { if (text) edit[key] = value; else delete edit[key]; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-workspace-number]").forEach((input) => {
    const commit = () => {
      try {
        const index = Number(input.dataset.workspaceFile), key = input.dataset.editWorkspaceNumber, text = input.value.trim(), value = text ? parseEditorValue(text) : null;
        if (Array.isArray(value)) throw new Error(`${key} needs one finite number`);
        if (text && value < 0) throw new Error(`${key} cannot be negative`);
        commitEditorChange(`${text ? "Set" : "Reset"} ${model.workspace.files[index].name} ${key}`, (project) => editWorkspaceFile(project, index, (edit) => { if (text) edit[key] = value; else delete edit[key]; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-workspace-dynamic-vector]").forEach((input) => {
    const commit = () => {
      try {
        const index = Number(input.dataset.workspaceFile), key = input.dataset.editWorkspaceDynamicVector, length = Number(input.dataset.vectorLength) || 3, value = parseEditorValue(input.value);
        if (!Array.isArray(value) || value.length !== length) throw new Error(`${key} needs ${length} finite numbers`);
        if (key === "multiplicity" && (value.some((component) => component < 0) || value[1] < value[0])) throw new Error("Multiplicity needs a nonnegative minimum followed by an equal or greater maximum");
        commitEditorChange(`Set ${model.workspace.files[index].name} ${key}`, (project) => editWorkspaceFile(project, index, (edit) => { edit[key] = value; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-workspace-dynamic-number]").forEach((input) => {
    const commit = () => {
      try {
        const index = Number(input.dataset.workspaceFile), key = input.dataset.editWorkspaceDynamicNumber, value = parseEditorValue(input.value);
        if (Array.isArray(value)) throw new Error(`${key} needs one finite number`);
        if (key === "probability" && (value < 0 || value > 100)) throw new Error("Probability must be from 0 to 100");
        commitEditorChange(`Set ${model.workspace.files[index].name} ${key}`, (project) => editWorkspaceFile(project, index, (edit) => { edit[key] = value; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-workspace-dynamic-text]").forEach((input) => {
    const commit = () => {
      try {
        const index = Number(input.dataset.workspaceFile), key = input.dataset.editWorkspaceDynamicText, text = input.value.trim();
        if (key !== "playWav" && !text) throw new Error(`${key} cannot be empty`);
        commitEditorChange(`Set ${model.workspace.files[index].name} ${key}`, (project) => editWorkspaceFile(project, index, (edit) => { edit[key] = key === "playWav" ? text || null : text.toUpperCase(); }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-edit-workspace-switch]").forEach((input) => {
    const commit = () => {
      try {
        const key = input.dataset.editWorkspaceSwitch, text = input.value.trim(), value = text ? parseEditorValue(text) : null;
        if (Array.isArray(value)) throw new Error(`${key} needs one finite number`);
        if (text && value < 0) throw new Error(`${key} cannot be negative`);
        commitEditorChange(`${text ? "Set" : "Reset"} ${key}`, (project) => { if (text) project.workspaceEdits[key] = value; else delete project.workspaceEdits[key]; });
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    };
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); commit(); } });
    input.addEventListener("change", commit);
  });
  inspector.querySelectorAll("[data-reset-workspace-file]").forEach((button) => button.addEventListener("click", () => {
    const index = button.dataset.resetWorkspaceFile;
    commitEditorChange(`Reset ${model.workspace.files[Number(index)].name} manifest edits`, (project) => { delete project.workspaceEdits.files[index]; });
  }));
  inspector.querySelector("[data-reset-workspace]")?.addEventListener("click", () => commitEditorChange("Reset manifest edits", (project) => { project.workspaceEdits = { files: Object.create(null) }; }));
  inspector.querySelector("[data-export-workspace]")?.addEventListener("click", () => {
    try {
      const car = model.workspace.kind === "carLods", text = car ? serializeCarLodsIni(model.workspace) : serializeModelsIni(model.workspace), fallback = car ? "lods.ini" : "models.ini", name = model.workspace.manifest?.split("/").at(-1) || fallback;
      downloadText(name, text, "text/plain");
      window.__apexLastManifestExport = { name, text, warnings: [...(model.workspace.authoredWarnings || [])] };
      status.textContent = `Exported ${name} · ${model.workspace.files.filter((file) => !file.auxiliary).length} entries`;
    } catch (error) { console.error(error); status.textContent = `Could not export manifest: ${error.message}`; }
  });
}

function bindMeshEditors(node) {
  const sourceName = nodeBaseline(node).name, sourceKey = matchingProjectEdit(editorProject?.meshEdits, sourceName)?.key || sourceName;
  inspector.querySelectorAll("[data-edit-mesh-boolean]").forEach((input) => input.addEventListener("change", () => {
    const key = input.dataset.editMeshBoolean, value = input.value;
    commitEditorChange(`${value ? "Set" : "Reset"} ${node.name} ${key}`, (project) => editMesh(project, sourceKey, (edit) => { if (value) edit[key] = value === "true"; else delete edit[key]; }));
  }));
  inspector.querySelectorAll("[data-edit-mesh-number]").forEach((input) => {
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") input.blur(); });
    input.addEventListener("change", () => {
      try {
        const key = input.dataset.editMeshNumber, text = input.value.trim();
        const value = text ? parseEditorValue(text) : null;
        if (Array.isArray(value)) throw new Error(`${key} needs one finite number`);
        commitEditorChange(`${text ? "Set" : "Reset"} ${node.name} ${key}`, (project) => editMesh(project, sourceKey, (edit) => { if (text) edit[key] = value; else delete edit[key]; }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    });
  });
  inspector.querySelector("[data-reset-mesh]")?.addEventListener("click", () => commitEditorChange(`Reset ${node.name}`, (project) => { delete project.meshEdits[sourceKey]; }));
}

function editMaterial(project, materialName, mutate) {
  const edit = project.materialEdits[materialName] || { properties: Object.create(null), resources: Object.create(null) };
  edit.properties ||= Object.create(null); edit.resources ||= Object.create(null);
  mutate(edit); project.materialEdits[materialName] = edit;
}

function bindMaterialEditors(material) {
  inspector.querySelectorAll("[data-edit-state]").forEach((input) => input.addEventListener("change", () => {
    const key = input.dataset.editState, value = input.value.trim();
    commitEditorChange(`${value ? "Set" : "Reset"} ${material.name} ${key}`, (project) => editMaterial(project, material.name, (edit) => { if (value) edit[key] = value; else delete edit[key]; }));
  }));
  inspector.querySelectorAll("[data-edit-property]").forEach((input) => {
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") input.blur(); });
    input.addEventListener("change", () => {
      try {
        const name = input.dataset.editProperty, value = parseEditorValue(input.value);
        commitEditorChange(`Set ${material.name}.${name}`, (project) => editMaterial(project, material.name, (edit) => {
          const existing = authoredProperty(edit, name); if (existing && existing.key !== name) delete edit.properties[existing.key]; edit.properties[name] = value;
        }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    });
  });
  inspector.querySelectorAll("[data-reset-property]").forEach((button) => button.addEventListener("click", () => {
    const name = button.dataset.resetProperty;
    commitEditorChange(`Reset ${material.name}.${name}`, (project) => editMaterial(project, material.name, (edit) => { const existing = authoredProperty(edit, name); if (existing) delete edit.properties[existing.key]; }));
  }));
  inspector.querySelectorAll("[data-edit-resource]").forEach((input) => {
    input.addEventListener("keydown", (event) => { if (event.key === "Enter") input.blur(); });
    input.addEventListener("change", () => {
      try {
        const slot = input.dataset.editResource, value = parseResourceEditor(input.value);
        commitEditorChange(`${value ? "Set" : "Reset"} ${material.name}.${slot}`, (project) => editMaterial(project, material.name, (edit) => {
          const existing = authoredResource(edit, slot); if (existing) delete edit.resources[existing.key]; if (value) edit.resources[slot] = value;
        }));
      } catch (error) { input.classList.add("invalid"); status.textContent = error.message; }
    });
  });
  inspector.querySelectorAll("[data-reset-resource]").forEach((button) => button.addEventListener("click", () => {
    const slot = button.dataset.resetResource;
    commitEditorChange(`Reset ${material.name}.${slot}`, (project) => editMaterial(project, material.name, (edit) => { const existing = authoredResource(edit, slot); if (existing) delete edit.resources[existing.key]; }));
  }));
  inspector.querySelector("[data-reset-material]")?.addEventListener("click", () => commitEditorChange(`Reset ${material.name}`, (project) => { delete project.materialEdits[material.name]; }));
}

function kv(key, value) { return `<div class="kv"><span>${escapeHtml(String(key))}</span><span>${escapeHtml(String(value))}</span></div>`; }
function format(value) { return Number.isFinite(value) ? Number(value.toFixed(4)).toString() : "—"; }
function escapeHtml(value) { const el = document.createElement("span"); el.textContent = value; return el.innerHTML; }

function createRenderer(canvas) {
  const gl = canvas.getContext("webgl2", { antialias: true, alpha: false });
  if (!gl) return null;
  const maxCspLights = 32;
  const maxCustomColorMasks = 8;
  const cameraFovDegrees = 45;
  const description = gl.getParameter(gl.RENDERER);
  const hdrEnabled=Boolean(gl.getExtension("EXT_color_buffer_float")),floatLinear=Boolean(gl.getExtension("OES_texture_float_linear")),hdrFormat=hdrEnabled?gl.RGBA16F:gl.RGBA8,hdrSampleOptions=Array.from(gl.getInternalformatParameter(gl.RENDERBUFFER,hdrFormat,gl.SAMPLES)||[]),hdrSamples=Math.max(1,...hdrSampleOptions.filter((value)=>value<=4));
  const program = link(gl, `#version 300 es
    precision highp float; layout(location=0) in vec3 position; layout(location=1) in vec3 normal; layout(location=2) in vec2 uv; layout(location=3) in vec3 tangent; layout(location=4) in float vertexAo;
    uniform mat4 world; uniform mat4 viewProjection; out vec3 vNormal; out vec3 vTangent; out vec3 vBitangent; out vec3 vWorld; out vec3 vLocal; out vec2 vUv; out float vAo;
    void main(){ vec4 p=world*vec4(position,1.0); mat3 world3=mat3(world); vWorld=p.xyz; vLocal=position; vNormal=world3*normal; vTangent=world3*tangent; vBitangent=world3*cross(tangent,normal); vUv=uv; vAo=vertexAo; gl_Position=viewProjection*p; }`, `#version 300 es
    precision highp float; in vec3 vNormal; in vec3 vTangent; in vec3 vBitangent; in vec3 vWorld; in vec3 vLocal; in vec2 vUv; in float vAo; uniform mat4 world; uniform vec3 cameraPosition; uniform vec3 baseColor; uniform sampler2D diffuseTexture; uniform bool hasDiffuseTexture; uniform bool vaoEnabled;
    uniform sampler2D normalTexture; uniform sampler2D mapsTexture; uniform sampler2D detailTexture; uniform sampler2D normalDetailTexture; uniform bool hasNormalTexture; uniform bool hasMapsTexture; uniform bool hasDetailTexture; uniform bool hasNormalDetailTexture; uniform bool normalObjectSpace; uniform float useDetail; uniform float detailUvMultiplier; uniform float detailNormalBlend;
    uniform sampler2D multiMaskTexture; uniform sampler2D multiDetailRTexture; uniform sampler2D multiDetailGTexture; uniform sampler2D multiDetailBTexture; uniform sampler2D multiDetailATexture; uniform sampler2D multiDetailNormalTexture; uniform bool hasMultiLayer; uniform vec4 multiDetailMultipliers; uniform vec2 multiDetailAMultiplier; uniform vec2 multiDetailNormalMultiplier;
    uniform sampler2D customEmissiveTexture; uniform sampler2D customColorShapeTexture; uniform sampler2D customVertexShapeTexture; uniform bool hasCustomEmissive; uniform bool hasCustomColorShape; uniform bool hasCustomVertexShape; uniform bool customColorMasksAsMultiplier; uniform float customEmissiveStrength; uniform bool customEmissiveAlpha; uniform vec4 customEmissiveLuma; uniform vec4 customEmissiveAlphaParams; uniform bool customSkipDiffuseMap; uniform vec4 customMirrorUv; uniform vec3 customBounceColor;
    const int MAX_CUSTOM_COLOR_MASKS=${maxCustomColorMasks}; uniform int customColorMaskCount; uniform vec4 customColorMaskTarget[MAX_CUSTOM_COLOR_MASKS]; uniform vec4 customColorMaskParams[MAX_CUSTOM_COLOR_MASKS]; uniform vec4 customColorMaskEmission[MAX_CUSTOM_COLOR_MASKS]; uniform vec4 customColorMaskSide[MAX_CUSTOM_COLOR_MASKS]; uniform float customColorMaskBaseChannel[MAX_CUSTOM_COLOR_MASKS]; uniform vec3 customMirrorDirection; uniform float customMirrorOffset;
    uniform vec4 customVertexAnchor[4]; uniform vec4 customVertexEmission[4]; uniform vec4 customVertexMirroredEmission[4];
    uniform float ambientLevel; uniform float diffuseLevel; uniform float specularLevel; uniform float specularPower; uniform float fresnelCValue; uniform float fresnelPower; uniform float fresnelLevel; uniform float alphaRef; uniform vec3 emissiveColor; uniform bool windscreenMaterial; uniform bool brokenGlassMaterial; uniform float glassDamage; uniform bool reflectionAlphaMaterial; uniform bool refractiveMaterial; uniform bool reflectionCapturePass; uniform bool reflectionCubeReady; uniform samplerCube reflectionCubeTexture; uniform bool sceneColorReady; uniform sampler2D sceneColorTexture; uniform vec2 viewportSize; uniform vec3 cameraRight; uniform vec3 cameraUp; uniform float cameraTangent; uniform float refractionStrength; uniform float refractionBlur; uniform bool selected; uniform vec3 surfaceOverlayColor; uniform float surfaceOverlayMix; uniform float rainWetness; uniform int rainBits; uniform float seasonAutumn; uniform float seasonWinter; uniform bool seasonTreeVariation;
    uniform bool shadowsEnabled;uniform sampler2D shadowMap0;uniform sampler2D shadowMap1;uniform sampler2D shadowMap2;uniform mat4 shadowMatrix0;uniform mat4 shadowMatrix1;uniform mat4 shadowMatrix2;uniform vec3 shadowSplits;uniform vec3 shadowBiases;uniform vec3 cameraForward;
    uniform vec3 sunDirection;uniform vec3 sunColor;uniform vec3 ambientColor;uniform vec3 horizonColor;uniform vec3 skyColor;uniform vec3 fogColor;uniform float fogDistance;uniform float fogBlend;
    const int MAX_CSP_LIGHTS=${maxCspLights}; const int MAX_CSP_LOCAL_SHADOWS=${CSP_LOCAL_SHADOW_LIMIT}; uniform int cspLightCount; uniform vec4 cspLightPositionRange[MAX_CSP_LIGHTS]; uniform vec4 cspLightColorSpot[MAX_CSP_LIGHTS]; uniform vec4 cspLightDirectionCone[MAX_CSP_LIGHTS]; uniform vec4 cspLightFalloff[MAX_CSP_LIGHTS]; uniform vec4 cspLightLineVector[MAX_CSP_LIGHTS]; uniform vec4 cspLightLineColor[MAX_CSP_LIGHTS]; uniform vec4 cspLightSecondCone[MAX_CSP_LIGHTS]; uniform vec4 cspLightSecondRange[MAX_CSP_LIGHTS]; uniform vec4 cspLightSpotEdge[MAX_CSP_LIGHTS]; uniform vec4 cspLightSpotUp[MAX_CSP_LIGHTS]; uniform int cspLightShadowSlot[MAX_CSP_LIGHTS]; uniform sampler2D cspLocalShadowAtlas; uniform mat4 cspLocalShadowMatrix[MAX_CSP_LOCAL_SHADOWS]; uniform vec4 cspLocalShadowParams[MAX_CSP_LOCAL_SHADOWS]; uniform vec4 cspLocalShadowPosition[MAX_CSP_LOCAL_SHADOWS]; uniform bool cspLocalShadowFloat; uniform bool cspTrackReceiver; uniform bool cspInteriorView; out vec4 color;
    float rainHash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453123);}float rainNoise(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);return mix(mix(rainHash(i),rainHash(i+vec2(1,0)),f.x),mix(rainHash(i+vec2(0,1)),rainHash(i+vec2(1,1)),f.x),f.y);}
    vec3 adjustSeasonColor(vec3 source,vec3 surfaceNormal){if(seasonAutumn+seasonWinter<=0.0)return source;float variation=seasonTreeVariation?rainNoise(vWorld.xz/1000.0):0.5;float leaf=clamp(seasonWinter*clamp(surfaceNormal.y,0.0,1.0)+(source.g*2.0-source.r-source.b)*20.0,0.0,1.0);if(seasonAutumn>0.0){vec3 autumnTarget=clamp(vec3(source.r*.2+source.g*1.6,source.g*(1.3-variation*variation)+source.r*.2,source.b-.4*source.g),0.0,1.0);source=mix(source,autumnTarget,seasonAutumn*leaf);}float luminance=dot(source,vec3(.2126,.7152,.0722));vec3 winterTarget=clamp(luminance*((vec3(1.0,1.1,1.2)*1.6+clamp(seasonWinter*2.0-1.0,0.0,1.0)*.4)*.85),0.0,1.0);return mix(source,winterTarget,clamp(seasonWinter*2.0,0.0,1.0)*leaf);}
    float reflectionSMin(float a,float b){float h=clamp(b-abs(a-b),0.0,1.0);return min(a,b)-h*h/max(.00001,b*3.0)*(1.0-b);}
    vec3 samplePortableReflection(vec3 direction,float blur){if(reflectionCapturePass)return vec3(0.0);direction=normalize(direction);if(reflectionCubeReady)return textureLod(reflectionCubeTexture,direction,clamp(blur,0.0,9.0)).rgb*.12;float upperY=clamp(direction.y,0.0,1.0);vec3 upper=mix(skyColor,horizonColor,pow(1.0-upperY,2.0))*clamp(.75+direction.y*2.0,0.0,1.0);upper=mix(upper,fogColor,clamp(fogBlend,0.0,1.0)*.35);float groundEdge=mix(.025,.24,clamp(blur/6.0,0.0,1.0));vec3 ground=vec3(.05,.11,.08);vec3 environment=mix(ground,upper,smoothstep(-groundEdge,groundEdge,direction.y))*.12;float roughness=clamp(blur/6.0,0.0,1.0),sunExponent=mix(4096.0,6.0,roughness);float sunLobe=pow(max(dot(direction,normalize(sunDirection)),0.0),sunExponent)*mix(1.0,.08,roughness);return environment+sunColor*.06*sunLobe;}
    float sampleShadow(sampler2D map,mat4 matrix,float bias){vec4 projected=matrix*vec4(vWorld,1.0);vec3 coordinate=projected.xyz/projected.w*.5+.5;if(coordinate.x<=0.0||coordinate.x>=1.0||coordinate.y<=0.0||coordinate.y>=1.0||coordinate.z<=0.0||coordinate.z>=1.0)return 1.0;vec2 texel=1.0/vec2(textureSize(map,0));float lit=0.0;for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++)lit+=coordinate.z-bias<=texture(map,coordinate.xy+vec2(x,y)*texel).r?1.0:0.0;return lit/9.0;}
    float sunShadow(){if(!shadowsEnabled)return 1.0;float depth=dot(vWorld-cameraPosition,cameraForward);if(depth<=shadowSplits.x)return sampleShadow(shadowMap0,shadowMatrix0,shadowBiases.x);if(depth<=shadowSplits.y)return sampleShadow(shadowMap1,shadowMatrix1,shadowBiases.y);if(depth<=shadowSplits.z)return sampleShadow(shadowMap2,shadowMatrix2,shadowBiases.z);return 1.0;}
    float localLightShadow(int lightIndex,vec3 toLight,vec3 normalW){int slot=cspLightShadowSlot[lightIndex];if(slot<0||slot>=MAX_CSP_LOCAL_SHADOWS)return 1.0;vec4 projected=cspLocalShadowMatrix[slot]*vec4(vWorld,1.0);vec3 coordinate=projected.xyz/projected.w*.5+.5;if(projected.w<=0.0||coordinate.x<=0.0||coordinate.x>=1.0||coordinate.y<=0.0||coordinate.y>=1.0)return 1.0;vec2 cell=vec2(float(slot%2),float(slot/2)),atlasUv=(cell+coordinate.xy)*.5;vec4 p=cspLocalShadowParams[slot],shadowPosition=cspLocalShadowPosition[slot];float occluder=texture(cspLocalShadowAtlas,atlasUv).r;if(!cspLocalShadowFloat)occluder=exp(occluder*p.y*p.z);float distanceToLight=min(length(vWorld-shadowPosition.xyz),p.z),biasFactor=1.0-abs(dot(toLight,normalW)),receiver=exp(p.x*biasFactor-distanceToLight*p.y);return clamp(occluder*receiver*p.w+(1.0-p.w),0.0,1.0);}
    vec3 localLightDelta(int index,vec3 position,out float lineValue){if(cspLightFalloff[index].w>.5){vec3 ab=cspLightLineVector[index].xyz;lineValue=dot(position-cspLightPositionRange[index].xyz,ab)*cspLightLineVector[index].w;return cspLightPositionRange[index].xyz+clamp(lineValue,0.0,1.0)*ab-position;}lineValue=0.0;return cspLightPositionRange[index].xyz-position;}
    vec3 localLineSpecularDirection(int index,vec3 position,vec3 normalW,vec3 toCamera,vec3 fallback){if(cspLightFalloff[index].w<.5)return fallback;vec3 r=reflect(toCamera,normalW),l0=cspLightPositionRange[index].xyz-position,ld=cspLightLineVector[index].xyz;float rld=dot(r,ld),denominator=dot(ld,ld)-rld*rld,t=abs(denominator)>.000001?(dot(r,l0)*rld-dot(l0,ld))/denominator:0.0;vec3 l=l0+clamp(t,0.0,1.0)*ld,centerToRay=dot(l,r)*r-l;float centerDistance=length(centerToRay),spread=clamp((1.2-cspLightFalloff[index].z)*.25/max(centerDistance,.0001),0.0,1.0);return normalize(l+centerToRay*spread);}
    vec3 localLightShape(int index,vec3 toLight,float distanceToLight,float attenuation){float cosAngle=dot(cspLightDirectionCone[index].xyz,-toLight),primary=cspLightDirectionCone[index].w>.5?clamp(cspLightColorSpot[index].w-cosAngle,0.0,1.0):1.0;if(cspLightFalloff[index].w>.5)return vec3(attenuation*primary);vec3 edge=cspLightSpotEdge[index].w>.5?clamp(cspLightSpotEdge[index].rgb-vec3(dot(cspLightSpotUp[index].xyz,-toLight)),0.0,1.0):vec3(1.0);vec3 shape=vec3(attenuation*primary)*edge;if(cspLightSecondRange[index].w>0.0){float secondary=clamp(cspLightSecondCone[index].x-cosAngle*cspLightSecondCone[index].y,0.0,1.0),trimmed=max(clamp(distanceToLight*cspLightSecondRange[index].z-cspLightSecondRange[index].y,0.0,1.0)-clamp(distanceToLight*cspLightSecondRange[index].x,0.0,1.0),0.0);shape+=vec3(secondary*trimmed*trimmed*cspLightSecondRange[index].w);}return shape;}
    void main(){
      vec4 diffuseTexel=hasDiffuseTexture?texture(diffuseTexture,vUv):vec4(baseColor,1.0);if(hasMultiLayer){vec4 layerMask=texture(multiMaskTexture,vUv);vec4 layerColor=texture(multiDetailRTexture,vWorld.xz*multiDetailMultipliers.xx)*layerMask.r+texture(multiDetailGTexture,vWorld.xz*multiDetailMultipliers.yy)*layerMask.g+texture(multiDetailBTexture,vWorld.xz*multiDetailMultipliers.zz)*layerMask.b+texture(multiDetailATexture,vWorld.xz*multiDetailAMultiplier)*layerMask.a;diffuseTexel*=layerColor*multiDetailMultipliers.w;} float detailMask=hasDetailTexture&&useDetail>.5?1.0-diffuseTexel.a:0.0; vec4 detailTexel=hasDetailTexture?texture(detailTexture,vUv*max(detailUvMultiplier,.0001)):vec4(1.0); vec4 surfaceTexel=mix(diffuseTexel,diffuseTexel*detailTexel,detailMask); vec4 texel=hasDiffuseTexture&&!customSkipDiffuseMap?surfaceTexel:vec4(baseColor,surfaceTexel.a); if(diffuseTexel.a<alphaRef)discard;
      vec3 geometricNormal=normalize(vNormal); vec3 tangentNormal=vec3(0.0,0.0,1.0); vec3 n=geometricNormal;float normalTextureAlpha=1.0;
      if(hasNormalTexture){vec4 sampledNormal=texture(normalTexture,vUv);normalTextureAlpha=sampledNormal.a;if(normalObjectSpace){vec3 objectNormal=brokenGlassMaterial?sampledNormal.xzy*2.0-1.0:vec3(sampledNormal.r*2.0-1.0,sampledNormal.g*2.0-1.0,1.0-sampledNormal.a*2.0);if(brokenGlassMaterial)objectNormal.z*=-1.0;n=normalize(mat3(world)*objectNormal);}else{tangentNormal=(brokenGlassMaterial?sampledNormal.xzy:sampledNormal.rgb)*2.0-1.0;vec3 mappedNormal=normalize(normalize(vTangent)*tangentNormal.x+normalize(vBitangent)*tangentNormal.y+geometricNormal*tangentNormal.z);n=brokenGlassMaterial?normalize(mix(geometricNormal,mappedNormal,clamp(glassDamage,0.0,1.0))):mappedNormal;}}
      if(hasNormalDetailTexture&&useDetail>.5){vec3 detailNormal=texture(normalDetailTexture,vUv*max(detailUvMultiplier,.0001)).rgb*2.0-1.0;vec3 detailWorld=normalize(normalize(vTangent)*detailNormal.x+normalize(vBitangent)*detailNormal.y+n*detailNormal.z);n=normalize(mix(n,detailWorld,clamp(detailMask*detailNormalBlend,0.0,1.0)));}
      if(hasMultiLayer&&dot(abs(multiDetailNormalMultiplier),vec2(1.0))>.00001){vec3 detailNormal=texture(multiDetailNormalTexture,vWorld.xz*multiDetailNormalMultiplier).rgb*2.0-1.0;n=normalize(normalize(vTangent)*detailNormal.x+normalize(vBitangent)*detailNormal.y+n*detailNormal.z);}
      texel.rgb=adjustSeasonColor(texel.rgb,n);
      vec3 maps=hasMapsTexture?texture(mapsTexture,vUv).rgb:vec3(1.0);float mappedSpecular=specularLevel*maps.r;if(hasDetailTexture&&useDetail>.5)mappedSpecular=mix(mappedSpecular,mappedSpecular*detailTexel.a,detailMask);float mappedPower=max(1.0,specularPower*maps.g+1.0);
      float upward=smoothstep(.2,.82,geometricNormal.y),puddlePattern=smoothstep(.54,.76,rainNoise(vWorld.xz*.095)+rainNoise(vWorld.xz*.31)*.25);float puddle=((rainBits&1)!=0?1.0:0.0)*rainWetness*upward*puddlePattern;float soaking=((rainBits&2)!=0?1.0:0.0)*rainWetness;float smoothWet=((rainBits&4)!=0?1.0:0.0)*rainWetness;float roughWet=((rainBits&8)!=0?1.0:0.0)*rainWetness;float lineWet=((rainBits&16)!=0?1.0:0.0)*rainWetness*upward;float reliefWet=((rainBits&32)!=0?1.0:0.0)*rainWetness*upward*.35;float wet=max(max(puddle,soaking*.72),max(max(smoothWet*.62,lineWet*.82),max(roughWet*.28,reliefWet)));float gloss=max(max(puddle,smoothWet*.75),lineWet*.88);texel.rgb*=mix(1.0,.48,wet);n=normalize(mix(n,geometricNormal,clamp(gloss*.68,0.0,.68)));mappedSpecular=mix(mappedSpecular,max(mappedSpecular,.9),gloss);mappedPower=mix(mappedPower,max(mappedPower,150.0),gloss);
      vec3 l=normalize(sunDirection); vec3 v=normalize(cameraPosition-vWorld); float ndl=max(dot(n,l),0.0);float shadow=sunShadow();
      float ao=vaoEnabled?vAo:1.0;vec3 diffuse=texel.rgb*(ambientColor*ambientLevel*ao+sunColor*diffuseLevel*ndl*shadow); vec3 spec=windscreenMaterial?vec3(0.0):sunColor*(pow(max(dot(n,normalize(l+v)),0.0),mappedPower)*mappedSpecular*shadow);float facing=1.0-max(dot(n,v),0.0);float frFactor=reflectionSMin(pow(facing,max(.01,fresnelPower))+max(0.0,fresnelCValue),max(0.0,fresnelLevel))*maps.b;frFactor=max(frFactor,gloss*(.035+.55*pow(facing,3.0)));float reflectionBlurBase=clamp(1.0-mappedPower/255.0,0.0,1.0),reflectionBlur=reflectionBlurBase*reflectionBlurBase*6.0;vec3 reflected=reflect(-v,n);vec3 environment=samplePortableReflection(reflected,reflectionBlur)*ao;vec3 fr=windscreenMaterial?vec3(0.0):environment*max(0.0,frFactor);if(windscreenMaterial)frFactor=0.0;
      vec3 localLight=vec3(0.0);
      for(int i=0;i<MAX_CSP_LIGHTS;i++){
        if(i>=cspLightCount)break;
        float trackMode=cspLightLineColor[i].w;if(cspTrackReceiver&&trackMode>.5&&(trackMode<1.5||!cspInteriorView))continue;
        float lineValue;vec3 delta=localLightDelta(i,vWorld,lineValue); float range=cspLightPositionRange[i].w; float distanceToLight=length(delta);
        if(distanceToLight<range){
          vec3 toLight=delta/max(distanceToLight,0.0001); float gradient=cspLightFalloff[i].x;
          float attenuation=clamp((1.0-distanceToLight/range)/max(0.001,1.0-gradient),0.0,1.0); attenuation*=attenuation;
          float localNdl=max(dot(n,toLight),0.0); float shapedDiffuse=cspLightFalloff[i].w>.5?clamp(mix(1.0,dot(n,toLight),cspLightFalloff[i].z),0.0,1.0):pow(localNdl,1.0+3.0*cspLightFalloff[i].z);
          vec3 lightColor=cspLightColorSpot[i].rgb+cspLightLineColor[i].rgb*clamp(lineValue,0.0,1.0);vec3 radiance=lightColor*localLightShape(i,toLight,distanceToLight,attenuation)*localLightShadow(i,toLight,n);
          vec3 specularDirection=localLineSpecularDirection(i,vWorld,n,-v,toLight);float localSpec=pow(max(dot(n,normalize(specularDirection+v)),0.0),mappedPower)*mappedSpecular*cspLightFalloff[i].y*(cspLightFalloff[i].w>.5?shapedDiffuse:1.0);
          localLight+=radiance*(texel.rgb*diffuseLevel*shapedDiffuse+vec3(localSpec));
        }
      }
      vec2 customUv=vUv;if(customMirrorUv.x>0.5){vec2 mirrorDirection=normalize(customMirrorUv.zw);float projected=dot(customUv,mirrorDirection);if(projected>customMirrorUv.y)customUv-=2.0*(projected-customMirrorUv.y)*mirrorDirection;}
      vec3 customEmissive=hasCustomEmissive?texture(customEmissiveTexture,customUv).rgb*customEmissiveStrength:vec3(0.0);
      vec4 customColorShape=hasCustomColorShape?texture(customColorShapeTexture,customUv):vec4(1.0);
      vec4 vertexShapes=hasCustomVertexShape?texture(customVertexShapeTexture,customUv):vec4(0.0);int nearestAnchor=-1;float nearestDistance=1e20;
      if(hasCustomVertexShape)for(int i=0;i<4;i++){if(customVertexAnchor[i].w>.5){float anchorDistance=distance(vLocal,customVertexAnchor[i].xyz);if(anchorDistance<nearestDistance){nearestDistance=anchorDistance;nearestAnchor=i;}}}
      for(int i=0;i<MAX_CUSTOM_COLOR_MASKS;i++){
        if(i>=customColorMaskCount)break;
        vec3 sourceColor=diffuseTexel.rgb,targetColor=customColorMaskTarget[i].rgb;float similarity;
        if(customColorMaskTarget[i].w>0.001)similarity=dot(normalize(max(sourceColor,vec3(0.00001))),targetColor);else similarity=1.0-length(sourceColor-targetColor)/1.7320508;
        float edge=1.0/max(1.0,customColorMaskParams[i].y);float mask=smoothstep(customColorMaskParams[i].x-edge,customColorMaskParams[i].x+edge,similarity);
        if(customColorMaskParams[i].w>customColorMaskParams[i].z)mask*=smoothstep(customColorMaskParams[i].z,customColorMaskParams[i].w,diffuseTexel.a);if(customColorMasksAsMultiplier&&i<4)mask*=customColorShape[i];if(nearestAnchor>=0&&int(customColorMaskBaseChannel[i]+.5)!=nearestAnchor)mask=0.0;
        float side=dot(vLocal,customColorMaskSide[i].xyz)-customMirrorOffset;if(customColorMaskSide[i].w<-.5&&side>0.0)mask=0.0;if(customColorMaskSide[i].w>.5&&side<=0.0)mask=0.0;
        customEmissive+=customColorMaskEmission[i].rgb*customEmissiveStrength*customColorMaskEmission[i].w*mask;
      }
      if(nearestAnchor>=0){float side=dot(vLocal,customMirrorDirection)-customMirrorOffset;vec3 vertexColor=side>0.0?customVertexMirroredEmission[nearestAnchor].rgb:customVertexEmission[nearestAnchor].rgb;customEmissive+=vertexColor*customEmissiveStrength*vertexShapes[nearestAnchor];}
      customEmissive+=customBounceColor*(.25+.75*dot(diffuseTexel.rgb,vec3(.333333)));
      if(customEmissiveAlpha)customEmissive*=diffuseTexel.a;
      if(customEmissiveAlphaParams.x>0.5){float span=max(.0001,customEmissiveAlphaParams.z-customEmissiveAlphaParams.y);customEmissive*=pow(clamp((diffuseTexel.a-customEmissiveAlphaParams.y)/span,0.0,1.0),max(.001,customEmissiveAlphaParams.w));}
      if(customEmissiveLuma.x>0.5){float sourceLuma=dot(diffuseTexel.rgb,vec3(.2126,.7152,.0722));float span=max(.0001,customEmissiveLuma.z-customEmissiveLuma.y);customEmissive*=pow(clamp((sourceLuma-customEmissiveLuma.y)/span,0.0,1.0),max(.001,customEmissiveLuma.w));}
      vec3 c=diffuse+spec+fr+texel.rgb*emissiveColor+customEmissive+localLight;if(surfaceOverlayMix<.9){float fogAmount=pow(clamp(distance(cameraPosition,vWorld)/max(1.0,fogDistance),0.0,1.0),max(.05,fogBlend));c=mix(c,fogColor,fogAmount);}c=mix(c,surfaceOverlayColor,surfaceOverlayMix);float outputAlpha=reflectionAlphaMaterial&&!reflectionCapturePass?mix(texel.a,1.0,clamp(frFactor,0.0,1.0)):texel.a;if(windscreenMaterial&&texel.a<.5)outputAlpha=texel.a*max(dot(v,l),0.0)*shadow;if(brokenGlassMaterial){float damageAlpha=clamp(glassDamage*normalTextureAlpha,0.0,1.0);if(damageAlpha<.05)discard;outputAlpha=clamp(damageAlpha*4.0,0.0,1.0);}bool useRefraction=!reflectionCapturePass&&refractiveMaterial&&sceneColorReady&&(!brokenGlassMaterial||abs(dot(n,v))<.5);if(useRefraction){vec2 refractionDirection=vec2(dot(n,cameraRight),dot(n,cameraUp));float distanceScale=cameraTangent/max(.1,distance(cameraPosition,vWorld));vec2 offset=refractionDirection*refractionStrength*distanceScale*mix(1.0,.5,abs(dot(v,n)));offset.x*=viewportSize.y/max(1.0,viewportSize.x);vec2 screenUv=gl_FragCoord.xy/max(viewportSize,vec2(1.0));vec3 background=textureLod(sceneColorTexture,clamp(screenUv+offset,vec2(0.001),vec2(.999)),max(0.0,refractionBlur)).rgb;c=mix(background,c,clamp(outputAlpha,0.0,1.0));outputAlpha=1.0;}else if(brokenGlassMaterial){c*=outputAlpha/.1;outputAlpha=.1;}if(selected&&!reflectionCapturePass)c=mix(c,vec3(1.0,.57,.08),.28);color=vec4(max(c,vec3(0.0)),outputAlpha);
    }`);
  const locations = Object.fromEntries(["world","viewProjection","cameraPosition","baseColor","diffuseTexture","hasDiffuseTexture","normalTexture","mapsTexture","detailTexture","normalDetailTexture","hasNormalTexture","hasMapsTexture","hasDetailTexture","hasNormalDetailTexture","normalObjectSpace","useDetail","detailUvMultiplier","detailNormalBlend","multiMaskTexture","multiDetailRTexture","multiDetailGTexture","multiDetailBTexture","multiDetailATexture","multiDetailNormalTexture","hasMultiLayer","multiDetailMultipliers","multiDetailAMultiplier","multiDetailNormalMultiplier","customEmissiveTexture","customColorShapeTexture","customVertexShapeTexture","hasCustomEmissive","hasCustomColorShape","hasCustomVertexShape","customColorMasksAsMultiplier","customEmissiveStrength","customEmissiveAlpha","customEmissiveLuma","customEmissiveAlphaParams","customSkipDiffuseMap","customMirrorUv","customBounceColor","customColorMaskCount","customMirrorDirection","customMirrorOffset","ambientLevel","diffuseLevel","specularLevel","specularPower","fresnelCValue","fresnelPower","fresnelLevel","alphaRef","emissiveColor","windscreenMaterial","brokenGlassMaterial","glassDamage","reflectionAlphaMaterial","refractiveMaterial","reflectionCapturePass","reflectionCubeReady","reflectionCubeTexture","sceneColorReady","sceneColorTexture","viewportSize","cameraRight","cameraUp","cameraTangent","refractionStrength","refractionBlur","selected","surfaceOverlayColor","surfaceOverlayMix","rainWetness","rainBits","seasonAutumn","seasonWinter","seasonTreeVariation","vaoEnabled","cspLightCount","shadowsEnabled","shadowMap0","shadowMap1","shadowMap2","shadowMatrix0","shadowMatrix1","shadowMatrix2","shadowSplits","shadowBiases","cameraForward","sunDirection","sunColor","ambientColor","horizonColor","skyColor","fogColor","fogDistance","fogBlend"].map((name) => [name, gl.getUniformLocation(program, name)]));
  locations.cspTrackReceiver=gl.getUniformLocation(program,"cspTrackReceiver");locations.cspInteriorView=gl.getUniformLocation(program,"cspInteriorView");
  locations.customColorMaskTarget = gl.getUniformLocation(program, "customColorMaskTarget[0]");
  locations.customColorMaskParams = gl.getUniformLocation(program, "customColorMaskParams[0]");
  locations.customColorMaskEmission = gl.getUniformLocation(program, "customColorMaskEmission[0]");
  locations.customColorMaskSide = gl.getUniformLocation(program, "customColorMaskSide[0]");
  locations.customColorMaskBaseChannel = gl.getUniformLocation(program, "customColorMaskBaseChannel[0]");
  locations.customVertexAnchor = gl.getUniformLocation(program, "customVertexAnchor[0]");
  locations.customVertexEmission = gl.getUniformLocation(program, "customVertexEmission[0]");
  locations.customVertexMirroredEmission = gl.getUniformLocation(program, "customVertexMirroredEmission[0]");
  locations.cspLightPositionRange = gl.getUniformLocation(program, "cspLightPositionRange[0]");
  locations.cspLightColorSpot = gl.getUniformLocation(program, "cspLightColorSpot[0]");
  locations.cspLightDirectionCone = gl.getUniformLocation(program, "cspLightDirectionCone[0]");
  locations.cspLightFalloff = gl.getUniformLocation(program, "cspLightFalloff[0]");
  locations.cspLightLineVector = gl.getUniformLocation(program, "cspLightLineVector[0]");
  locations.cspLightLineColor = gl.getUniformLocation(program, "cspLightLineColor[0]");
  locations.cspLightSecondCone = gl.getUniformLocation(program, "cspLightSecondCone[0]");
  locations.cspLightSecondRange = gl.getUniformLocation(program, "cspLightSecondRange[0]");
  locations.cspLightSpotEdge = gl.getUniformLocation(program, "cspLightSpotEdge[0]");
  locations.cspLightSpotUp = gl.getUniformLocation(program, "cspLightSpotUp[0]");
  locations.cspLightShadowSlot = gl.getUniformLocation(program, "cspLightShadowSlot[0]");
  locations.cspLocalShadowAtlas = gl.getUniformLocation(program, "cspLocalShadowAtlas");
  locations.cspLocalShadowMatrix = gl.getUniformLocation(program, "cspLocalShadowMatrix[0]");
  locations.cspLocalShadowParams = gl.getUniformLocation(program, "cspLocalShadowParams[0]");
  locations.cspLocalShadowPosition = gl.getUniformLocation(program, "cspLocalShadowPosition[0]");
  locations.cspLocalShadowFloat = gl.getUniformLocation(program, "cspLocalShadowFloat");
  const shadowProgram=link(gl,`#version 300 es
    precision highp float;layout(location=0)in vec3 position;layout(location=2)in vec2 uv;uniform mat4 world;uniform mat4 lightViewProjection;out vec2 vUv;void main(){vUv=uv;gl_Position=lightViewProjection*world*vec4(position,1.0);}`,
    `#version 300 es
    precision highp float;in vec2 vUv;uniform sampler2D diffuseTexture;uniform bool hasDiffuseTexture;uniform float alphaRef;out vec4 color;void main(){if(hasDiffuseTexture&&texture(diffuseTexture,vUv).a<alphaRef)discard;color=vec4(1.0);}`);
  const shadowLocations={world:gl.getUniformLocation(shadowProgram,"world"),viewProjection:gl.getUniformLocation(shadowProgram,"lightViewProjection"),diffuseTexture:gl.getUniformLocation(shadowProgram,"diffuseTexture"),hasDiffuseTexture:gl.getUniformLocation(shadowProgram,"hasDiffuseTexture"),alphaRef:gl.getUniformLocation(shadowProgram,"alphaRef")};
  const localShadowProgram=link(gl,`#version 300 es
    precision highp float;layout(location=0)in vec3 position;layout(location=2)in vec2 uv;uniform mat4 world;uniform mat4 lightViewProjection;out vec2 vUv;out vec3 vWorld;void main(){vec4 p=world*vec4(position,1.0);vUv=uv;vWorld=p.xyz;gl_Position=lightViewProjection*p;}`,
    `#version 300 es
    precision highp float;in vec2 vUv;in vec3 vWorld;uniform sampler2D diffuseTexture;uniform bool hasDiffuseTexture;uniform float alphaRef;uniform vec3 lightPosition;uniform float lightRangeInv;uniform float lightClipSphere;uniform float expFactor;uniform bool exponentialOutput;out vec4 color;void main(){if(hasDiffuseTexture&&texture(diffuseTexture,vUv).a<alphaRef)discard;float radial=length(vWorld-lightPosition);if(radial<lightClipSphere)discard;float normalizedDepth=clamp(radial*lightRangeInv,0.0,1.0);float value=exponentialOutput?exp(expFactor*normalizedDepth):normalizedDepth;color=vec4(value,0.0,0.0,1.0);}`);
  const localShadowLocations={world:gl.getUniformLocation(localShadowProgram,"world"),viewProjection:gl.getUniformLocation(localShadowProgram,"lightViewProjection"),diffuseTexture:gl.getUniformLocation(localShadowProgram,"diffuseTexture"),hasDiffuseTexture:gl.getUniformLocation(localShadowProgram,"hasDiffuseTexture"),alphaRef:gl.getUniformLocation(localShadowProgram,"alphaRef"),lightPosition:gl.getUniformLocation(localShadowProgram,"lightPosition"),lightRangeInv:gl.getUniformLocation(localShadowProgram,"lightRangeInv"),lightClipSphere:gl.getUniformLocation(localShadowProgram,"lightClipSphere"),expFactor:gl.getUniformLocation(localShadowProgram,"expFactor"),exponentialOutput:gl.getUniformLocation(localShadowProgram,"exponentialOutput")};
  const localShadowBlurProgram=link(gl,`#version 300 es
    precision highp float;out vec2 vUv;void main(){vec2 p=gl_VertexID==0?vec2(-1,-1):gl_VertexID==1?vec2(3,-1):vec2(-1,3);vUv=p*.5+.5;gl_Position=vec4(p,0,1);}`,
    `#version 300 es
    precision highp float;in vec2 vUv;uniform sampler2D sourceTexture;uniform vec4 region;uniform vec2 pixelStep;uniform int filterMode;out vec4 color;
    float readValue(vec2 uv){vec2 texel=.5/vec2(textureSize(sourceTexture,0));return texture(sourceTexture,clamp(uv,region.xy+texel,region.xy+region.zw-texel)).r;}
    void main(){vec2 uv=region.xy+vUv*region.zw;float result=0.0,total=0.0;if(filterMode==1){const float weights[4]=float[4](.2496147,.1924633,.0514763,.0064457);const float offsets[4]=float[4](.6443417,2.3788476,4.2911105,6.2166071);for(int i=0;i<4;i++){vec2 o=pixelStep*offsets[i];result+=(readValue(uv+o)+readValue(uv-o))*weights[i];}}else{const float weights[2]=float[2](.4490798,.0509202);const float offsets[2]=float[2](.5380487,2.0627797);for(int i=0;i<2;i++){vec2 o=pixelStep*offsets[i];float a=readValue(uv+o),b=readValue(uv-o),wa=weights[i],wb=weights[i];if(filterMode==2){wa*=1.0+a*.1;wb*=1.0+b*.1;}result+=a*wa+b*wb;total+=wa+wb;}if(filterMode==2)result/=max(.000001,total);}color=vec4(result,0,0,1);}`);
  const localShadowBlurLocations={sourceTexture:gl.getUniformLocation(localShadowBlurProgram,"sourceTexture"),region:gl.getUniformLocation(localShadowBlurProgram,"region"),pixelStep:gl.getUniformLocation(localShadowBlurProgram,"pixelStep"),filterMode:gl.getUniformLocation(localShadowBlurProgram,"filterMode")};
  const shadowTargets=Array.from({length:3},()=>{const texture=gl.createTexture(),framebuffer=gl.createFramebuffer();gl.bindTexture(gl.TEXTURE_2D,texture);gl.texImage2D(gl.TEXTURE_2D,0,gl.DEPTH_COMPONENT24,KS_SHADOW_MAP_SIZE,KS_SHADOW_MAP_SIZE,0,gl.DEPTH_COMPONENT,gl.UNSIGNED_INT,null);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);gl.bindFramebuffer(gl.FRAMEBUFFER,framebuffer);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.TEXTURE_2D,texture,0);gl.drawBuffers([gl.NONE]);gl.readBuffer(gl.NONE);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("Directional shadow framebuffer is incomplete");return {texture,framebuffer};});
  const localShadowTexture=gl.createTexture(),localShadowFramebuffer=gl.createFramebuffer(),localShadowMsFramebuffer=gl.createFramebuffer(),localShadowMsColor=gl.createRenderbuffer(),localShadowDepth=gl.createRenderbuffer(),localShadowFloat=hdrEnabled,localShadowFormat=localShadowFloat?gl.R32F:gl.RGBA8,localShadowColorSamples=Array.from(gl.getInternalformatParameter(gl.RENDERBUFFER,localShadowFormat,gl.SAMPLES)||[]),localShadowDepthSamples=Array.from(gl.getInternalformatParameter(gl.RENDERBUFFER,gl.DEPTH_COMPONENT24,gl.SAMPLES)||[]),localShadowSamples=localShadowColorSamples.includes(CSP_LOCAL_SHADOW_SAMPLES)&&localShadowDepthSamples.includes(CSP_LOCAL_SHADOW_SAMPLES)?CSP_LOCAL_SHADOW_SAMPLES:1;gl.bindTexture(gl.TEXTURE_2D,localShadowTexture);gl.texImage2D(gl.TEXTURE_2D,0,localShadowFormat,CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_ATLAS_SIZE,0,localShadowFloat?gl.RED:gl.RGBA,localShadowFloat?gl.FLOAT:gl.UNSIGNED_BYTE,null);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,localShadowFloat&&!floatLinear?gl.NEAREST:gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,localShadowFloat&&!floatLinear?gl.NEAREST:gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);gl.bindFramebuffer(gl.FRAMEBUFFER,localShadowFramebuffer);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,localShadowTexture,0);gl.drawBuffers([gl.COLOR_ATTACHMENT0]);if(localShadowSamples===CSP_LOCAL_SHADOW_SAMPLES){gl.bindRenderbuffer(gl.RENDERBUFFER,localShadowMsColor);gl.renderbufferStorageMultisample(gl.RENDERBUFFER,localShadowSamples,localShadowFormat,CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_ATLAS_SIZE);gl.bindRenderbuffer(gl.RENDERBUFFER,localShadowDepth);gl.renderbufferStorageMultisample(gl.RENDERBUFFER,localShadowSamples,gl.DEPTH_COMPONENT24,CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_ATLAS_SIZE);gl.bindFramebuffer(gl.FRAMEBUFFER,localShadowMsFramebuffer);gl.framebufferRenderbuffer(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.RENDERBUFFER,localShadowMsColor);gl.framebufferRenderbuffer(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.RENDERBUFFER,localShadowDepth);gl.drawBuffers([gl.COLOR_ATTACHMENT0]);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("CSP multisample local-shadow framebuffer is incomplete");}else{gl.bindRenderbuffer(gl.RENDERBUFFER,localShadowDepth);gl.renderbufferStorage(gl.RENDERBUFFER,gl.DEPTH_COMPONENT24,CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_ATLAS_SIZE);gl.bindFramebuffer(gl.FRAMEBUFFER,localShadowFramebuffer);gl.framebufferRenderbuffer(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.RENDERBUFFER,localShadowDepth);}gl.bindFramebuffer(gl.FRAMEBUFFER,localShadowFramebuffer);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("CSP exponential local-shadow atlas framebuffer is incomplete");let localShadowState={lights:[],matrices:[],slotByLight:new Map(),casters:0,triangles:0,casterNodes:[],ready:false};
  const localShadowPingTexture=gl.createTexture(),localShadowPingFramebuffer=gl.createFramebuffer();gl.bindTexture(gl.TEXTURE_2D,localShadowPingTexture);gl.texImage2D(gl.TEXTURE_2D,0,localShadowFloat?gl.R32F:gl.RGBA8,CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_ATLAS_SIZE,0,localShadowFloat?gl.RED:gl.RGBA,localShadowFloat?gl.FLOAT:gl.UNSIGNED_BYTE,null);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,localShadowFloat&&!floatLinear?gl.NEAREST:gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,localShadowFloat&&!floatLinear?gl.NEAREST:gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);gl.bindFramebuffer(gl.FRAMEBUFFER,localShadowPingFramebuffer);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,localShadowPingTexture,0);gl.drawBuffers([gl.COLOR_ATTACHMENT0]);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("CSP local-shadow Gaussian framebuffer is incomplete");
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  const fullscreenVao=gl.createVertexArray(),windUpdateProgram=link(gl,`#version 300 es
    precision highp float;out vec2 vUv;void main(){vec2 p=gl_VertexID==0?vec2(-1,-1):gl_VertexID==1?vec2(3,-1):vec2(-1,3);vUv=p*.5+.5;gl_Position=vec4(p,0,1);}`,
    `#version 300 es
    precision highp float;in vec2 vUv;uniform sampler2D previousMap;uniform vec4 particles[32];uniform vec2 windDelta;uniform float windSpeed;out vec4 color;
    void main(){float previous=texture(previousMap,vUv-windDelta*.05).r,added=0.0;for(int i=0;i<32;i++){vec4 p=particles[i],r=fract(vec4(p.y*.05,p.z*.05,p.w*496.411011,p.w*899.885986));vec2 d=r.xy-vUv;if(d.x>.5)d.x-=1.0;else if(d.x<-.5)d.x+=1.0;if(d.y>.5)d.y-=1.0;else if(d.y<-.5)d.y+=1.0;float d2=dot(d,d)*.7;for(int k=-3;k<=3;k++){vec2 q=d+float(k)*windDelta/12.0;d2=min(d2,dot(q,q)*(.7+float(abs(k))*.07));}float life=.5-abs(.5-p.x);if(life>0.0){float radial=max(0.0,1.0-2.0*sqrt(d2)*(3.0+2.0*r.z)/sqrt(life));added+=radial*life*life*(.2+.2*r.w)*.3;}}float decay=.99-clamp(windSpeed/35.0,0.0,1.0)*.02;color=vec4(clamp(previous*decay+added,0.0,4.0),0,0,1);}`),windUpdateLocations={previousMap:gl.getUniformLocation(windUpdateProgram,"previousMap"),particles:gl.getUniformLocation(windUpdateProgram,"particles[0]"),windDelta:gl.getUniformLocation(windUpdateProgram,"windDelta"),windSpeed:gl.getUniformLocation(windUpdateProgram,"windSpeed")};
  const windTargets=Array.from({length:2},()=>{const texture=gl.createTexture(),framebuffer=gl.createFramebuffer();gl.bindTexture(gl.TEXTURE_2D,texture);gl.texImage2D(gl.TEXTURE_2D,0,hdrEnabled?gl.R16F:gl.R8,CSP_WIND_MAP_SIZE,CSP_WIND_MAP_SIZE,0,gl.RED,hdrEnabled?gl.HALF_FLOAT:gl.UNSIGNED_BYTE,null);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.REPEAT);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.REPEAT);gl.bindFramebuffer(gl.FRAMEBUFFER,framebuffer);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,texture,0);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("CSP wind framebuffer is incomplete");gl.clearColor(0,0,0,0);gl.clear(gl.COLOR_BUFFER_BIT);return {texture,framebuffer};});gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  const skyProgram=link(gl,`#version 300 es
    precision highp float;out vec2 vNdc;void main(){vec2 p=gl_VertexID==0?vec2(-1,-1):gl_VertexID==1?vec2(3,-1):vec2(-1,3);vNdc=p;gl_Position=vec4(p,0,1);}`,
    `#version 300 es
    precision highp float;in vec2 vNdc;uniform vec3 cameraForward;uniform vec3 cameraRight;uniform vec3 cameraUp;uniform float tanHalfFov;uniform float aspect;uniform vec3 horizonColor;uniform vec3 skyColor;uniform vec3 sunColor;uniform vec3 sunDirection;out vec4 color;void main(){vec3 ray=normalize(cameraForward+cameraRight*vNdc.x*tanHalfFov*aspect+cameraUp*vNdc.y*tanHalfFov);float height=smoothstep(-.08,.42,ray.y);vec3 c=mix(horizonColor,skyColor,height);float sun=smoothstep(cos(.02618),cos(.00524),dot(ray,normalize(sunDirection)));c+=sunColor*sun;color=vec4(max(c,vec3(0)),1);}`),skyLocations=Object.fromEntries(["cameraForward","cameraRight","cameraUp","tanHalfFov","aspect","horizonColor","skyColor","sunColor","sunDirection"].map((name)=>[name,gl.getUniformLocation(skyProgram,name)]));
  const reflectionCubeTexture=gl.createTexture(),reflectionCubeFramebuffer=gl.createFramebuffer(),reflectionCubeDepth=gl.createRenderbuffer(),reflectionCubeTargets={"negative-x":gl.TEXTURE_CUBE_MAP_NEGATIVE_X,"positive-x":gl.TEXTURE_CUBE_MAP_POSITIVE_X,"positive-y":gl.TEXTURE_CUBE_MAP_POSITIVE_Y,"negative-y":gl.TEXTURE_CUBE_MAP_NEGATIVE_Y,"positive-z":gl.TEXTURE_CUBE_MAP_POSITIVE_Z,"negative-z":gl.TEXTURE_CUBE_MAP_NEGATIVE_Z};
  gl.bindTexture(gl.TEXTURE_CUBE_MAP,reflectionCubeTexture);for(const face of WEBGL_CUBEMAP_FACES)gl.texImage2D(reflectionCubeTargets[face.target],0,hdrEnabled?gl.RGBA16F:gl.RGBA8,KS_EDITOR_CUBEMAP.size,KS_EDITOR_CUBEMAP.size,0,gl.RGBA,hdrEnabled?gl.HALF_FLOAT:gl.UNSIGNED_BYTE,null);gl.texParameteri(gl.TEXTURE_CUBE_MAP,gl.TEXTURE_MIN_FILTER,gl.LINEAR_MIPMAP_LINEAR);gl.texParameteri(gl.TEXTURE_CUBE_MAP,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_CUBE_MAP,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_CUBE_MAP,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_CUBE_MAP,gl.TEXTURE_WRAP_R,gl.CLAMP_TO_EDGE);
  gl.bindRenderbuffer(gl.RENDERBUFFER,reflectionCubeDepth);gl.renderbufferStorage(gl.RENDERBUFFER,gl.DEPTH_COMPONENT24,KS_EDITOR_CUBEMAP.size,KS_EDITOR_CUBEMAP.size);gl.bindFramebuffer(gl.FRAMEBUFFER,reflectionCubeFramebuffer);gl.framebufferRenderbuffer(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.RENDERBUFFER,reflectionCubeDepth);gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  const postVertex=`#version 300 es
    precision highp float;out vec2 vUv;void main(){vec2 p=gl_VertexID==0?vec2(-1,-1):gl_VertexID==1?vec2(3,-1):vec2(-1,3);vUv=p*.5+.5;gl_Position=vec4(p,0,1);}`;
  const brightPassProgram=link(gl,postVertex,`#version 300 es
    precision highp float;in vec2 vUv;uniform sampler2D sourceTexture;uniform float exposure;uniform float threshold;uniform float remap;out vec4 color;void main(){vec3 source=texture(sourceTexture,vUv).rgb;color=vec4(min(max(source*exposure-vec3(threshold),vec3(0.0))*remap,vec3(64000.0)),1.0);}`),brightPassLocations=Object.fromEntries(["sourceTexture","exposure","threshold","remap"].map((name)=>[name,gl.getUniformLocation(brightPassProgram,name)]));
  const bloomDownsampleProgram=link(gl,postVertex,`#version 300 es
    precision highp float;in vec2 vUv;uniform sampler2D sourceTexture;out vec4 color;void main(){vec2 stepSize=.5/vec2(textureSize(sourceTexture,0));vec3 result=texture(sourceTexture,vUv+vec2(-stepSize.x,-stepSize.y)).rgb+texture(sourceTexture,vUv+vec2(stepSize.x,-stepSize.y)).rgb+texture(sourceTexture,vUv+vec2(-stepSize.x,stepSize.y)).rgb+texture(sourceTexture,vUv+stepSize).rgb;color=vec4(result*.25,1.0);}`),bloomDownsampleSource=gl.getUniformLocation(bloomDownsampleProgram,"sourceTexture");
  const bloomBlurProgram=link(gl,postVertex,`#version 300 es
    precision highp float;in vec2 vUv;uniform sampler2D sourceTexture;uniform vec2 direction;uniform int sampleCount;uniform float sampleOffsets[15];uniform float sampleWeights[15];out vec4 color;void main(){vec2 texel=direction/vec2(textureSize(sourceTexture,0));vec3 result=vec3(0.0);for(int sampleIndex=0;sampleIndex<15;sampleIndex++){if(sampleIndex>=sampleCount)break;result+=texture(sourceTexture,vUv+texel*sampleOffsets[sampleIndex]).rgb*sampleWeights[sampleIndex];}color=vec4(result,1.0);}`),bloomBlurLocations={sourceTexture:gl.getUniformLocation(bloomBlurProgram,"sourceTexture"),direction:gl.getUniformLocation(bloomBlurProgram,"direction"),sampleCount:gl.getUniformLocation(bloomBlurProgram,"sampleCount"),sampleOffsets:gl.getUniformLocation(bloomBlurProgram,"sampleOffsets[0]"),sampleWeights:gl.getUniformLocation(bloomBlurProgram,"sampleWeights[0]")};
  const postProgram=link(gl,postVertex,`#version 300 es
    precision highp float;in vec2 vUv;uniform sampler2D hdrTexture;uniform sampler2D bloom0;uniform sampler2D bloom1;uniform sampler2D bloom2;uniform sampler2D bloom3;uniform sampler2D bloom4;uniform sampler2D ditherTexture;uniform float exposure;uniform float gamma;uniform float saturation;uniform float curveScale;uniform float curveShoulder;uniform float bloomScale;uniform float ditherScale;uniform float ditherOffset;uniform bool glareEnabled;uniform bool diagnosticMode;out vec4 color;void main(){vec4 source=texture(hdrTexture,vUv);if(diagnosticMode){color=vec4(clamp(source.rgb,0.0,1.0),source.a);return;}float luminance=dot(source.rgb,vec3(.2126,.7152,.0722));vec3 exposed=max(mix(vec3(luminance),source.rgb,saturation)*exposure,vec3(6.103515625e-5));vec3 decay=exp(-exposed*curveScale);vec3 shoulder=vec3(1.0)-decay*curveShoulder;vec3 mapped=clamp((vec3(1.0)-decay)*shoulder*shoulder,0.0,1.0);if(glareEnabled){vec3 glare=(texture(bloom0,vUv).rgb+texture(bloom1,vUv).rgb+texture(bloom2,vUv).rgb+texture(bloom3,vUv).rgb+texture(bloom4,vUv).rgb)*bloomScale;glare=clamp(glare,0.0,1.0);mapped=mapped+(vec3(1.0)-mapped)*glare;}mapped=pow(min(mapped+vec3(2.384185791015625e-7),vec3(1.0)),vec3(1.0/gamma));float dither=texture(ditherTexture,gl_FragCoord.xy/8.0).r*ditherScale+ditherOffset;color=vec4(mapped+vec3(dither),source.a);}`),postLocations=Object.fromEntries(["hdrTexture","bloom0","bloom1","bloom2","bloom3","bloom4","ditherTexture","exposure","gamma","saturation","curveScale","curveShoulder","bloomScale","ditherScale","ditherOffset","glareEnabled","diagnosticMode"].map((name)=>[name,gl.getUniformLocation(postProgram,name)]));
  const hdrTexture=gl.createTexture(),hdrFramebuffer=gl.createFramebuffer(),hdrMsFramebuffer=gl.createFramebuffer(),hdrMsColor=gl.createRenderbuffer(),hdrMsDepth=gl.createRenderbuffer(),exposureFramebuffer=gl.createFramebuffer();let hdrWidth=0,hdrHeight=0,hdrMaxMip=0;
  const bloomTargets=Array.from({length:KS_EDITOR_GLARE.levels},()=>({width:1,height:1,textures:[gl.createTexture(),gl.createTexture()],framebuffers:[gl.createFramebuffer(),gl.createFramebuffer()]})),bloomKernels=Array.from({length:KS_EDITOR_GLARE.levels},(_,level)=>ksEditorBloomGaussianKernel(level));
  const ditherTexture=gl.createTexture(),bayer8=[0,48,12,60,3,51,15,63,32,16,44,28,35,19,47,31,8,56,4,52,11,59,7,55,40,24,36,20,43,27,39,23,2,50,14,62,1,49,13,61,34,18,46,30,33,17,45,29,10,58,6,54,9,57,5,53,42,26,38,22,41,25,37,21];gl.bindTexture(gl.TEXTURE_2D,ditherTexture);gl.texImage2D(gl.TEXTURE_2D,0,gl.R8,8,8,0,gl.RED,gl.UNSIGNED_BYTE,Uint8Array.from(bayer8,(value)=>Math.round((value+.5)*255/64)));gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.REPEAT);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.REPEAT);
  const grassProgram=link(gl,`#version 300 es
    precision highp float;layout(location=0)in vec3 blade;layout(location=1)in vec4 instancePositionHeight;layout(location=2)in vec4 instanceAngleColor;layout(location=3)in vec4 instanceAtlasRect;layout(location=4)in vec3 instanceShape;layout(location=5)in vec2 instanceLean;layout(location=6)in float instanceFade;layout(location=7)in float instanceType;layout(location=8)in vec2 instanceNormal;layout(location=9)in vec3 instancePixel;layout(location=10)in vec2 instanceMaterial;uniform mat4 viewProjection;uniform vec3 cameraPosition;uniform sampler2D windMapTexture;uniform vec2 windVelocity;out vec3 vGrassColor;out vec2 vBlade;out vec2 vAtlasUv;flat out vec4 vAtlasRect;out vec3 vWorld;out vec3 vNormal;flat out vec3 vSubstrateNormal;flat out vec3 vBent;out float vFade;out float vTexDimming;flat out vec2 vRndFactors;flat out vec2 vMaterial;
    void main(){float angle=instanceAngleColor.x;vec2 direction=vec2(cos(angle),sin(angle));float height=instancePositionHeight.w,width=max(.002,instanceShape.x),upY=sqrt(max(0.0,1.0-dot(instanceLean,instanceLean))),normalY=sqrt(max(0.0,1.0-dot(instanceNormal,instanceNormal)));vec3 up=vec3(instanceLean.x,upY,instanceLean.y),inNormal=vec3(instanceNormal.x,normalY,instanceNormal.y),side=normalize(cross(vec3(direction.x,0.0,direction.y),inNormal)),bent=normalize(cross(side,inNormal));vec3 position=instancePositionHeight.xyz+side*blade.x*width+up*blade.y*height;float phaseOffset=instanceType*.5,windMult=blade.y*instanceShape.y*.5*(textureLod(windMapTexture,position.xz/80.0+phaseOffset*.05,0.0).r+textureLod(windMapTexture,position.xz/21.71+phaseOffset*.05,0.0).r*.471)*up.y;vec2 windOffset=windMult*windVelocity;windOffset/=1.0+abs(windOffset);position.xz+=windOffset*height;position.y-=min(.5,1.0-sqrt(max(0.0,1.0-dot(windOffset,windOffset))))*(.5+fract(558.476*phaseOffset))*height;float atlasX=instanceAtlasRect.x+(blade.x*.5+.5)*instanceAtlasRect.z,atlasY=blade.y*(1.0-instanceShape.z*fract(instanceAtlasRect.x+atlasX*.3)),distanceToCamera=length(position-cameraPosition);float density=1.0-instancePixel.x,advFade=clamp(1.2-distanceToCamera/60.0,0.0,1.0);vGrassColor=instanceAngleColor.yzw;vBlade=blade.xy;vAtlasUv=vec2(atlasX,instanceAtlasRect.y+atlasY*instanceAtlasRect.w);vAtlasRect=instanceAtlasRect;vWorld=position;vNormal=normalize(mix(inNormal,vec3(0,1,0),blade.y));vSubstrateNormal=inNormal;vBent=bent;vFade=instanceFade;vTexDimming=advFade*advFade*density*.88*(1.0-blade.y);vRndFactors=instancePixel.yz;vMaterial=instanceMaterial;gl_Position=viewProjection*vec4(position,1.0);}`,
    `#version 300 es
    precision highp float;in vec3 vGrassColor;in vec2 vBlade;in vec2 vAtlasUv;flat in vec4 vAtlasRect;in vec3 vWorld;in vec3 vNormal;flat in vec3 vSubstrateNormal;flat in vec3 vBent;in float vFade;in float vTexDimming;flat in vec2 vRndFactors;flat in vec2 vMaterial;uniform vec3 cameraPosition;uniform vec3 cameraForward;uniform vec3 sunDirection;uniform vec3 sunColor;uniform vec3 ambientColor;uniform vec3 fogColor;uniform float fogDistance;uniform float fogBlend;uniform float wetness;uniform bool hasGrassAtlas;uniform sampler2D grassAtlasTexture;uniform float textureBrightness;uniform bool shadowsEnabled;uniform sampler2D shadowMap0;uniform sampler2D shadowMap1;uniform sampler2D shadowMap2;uniform mat4 shadowMatrix0;uniform mat4 shadowMatrix1;uniform mat4 shadowMatrix2;uniform vec3 shadowSplits;uniform vec3 shadowBiases;const int MAX_CSP_LIGHTS=${maxCspLights};const int MAX_CSP_LOCAL_SHADOWS=${CSP_LOCAL_SHADOW_LIMIT};uniform int cspLightCount;uniform vec4 cspLightPositionRange[MAX_CSP_LIGHTS];uniform vec4 cspLightColorSpot[MAX_CSP_LIGHTS];uniform vec4 cspLightDirectionCone[MAX_CSP_LIGHTS];uniform vec4 cspLightFalloff[MAX_CSP_LIGHTS];uniform vec4 cspLightLineVector[MAX_CSP_LIGHTS];uniform vec4 cspLightLineColor[MAX_CSP_LIGHTS];uniform vec4 cspLightSecondCone[MAX_CSP_LIGHTS];uniform vec4 cspLightSecondRange[MAX_CSP_LIGHTS];uniform vec4 cspLightSpotEdge[MAX_CSP_LIGHTS];uniform vec4 cspLightSpotUp[MAX_CSP_LIGHTS];uniform int cspLightShadowSlot[MAX_CSP_LIGHTS];uniform sampler2D cspLocalShadowAtlas;uniform mat4 cspLocalShadowMatrix[MAX_CSP_LOCAL_SHADOWS];uniform vec4 cspLocalShadowParams[MAX_CSP_LOCAL_SHADOWS];uniform vec4 cspLocalShadowPosition[MAX_CSP_LOCAL_SHADOWS];uniform bool cspLocalShadowFloat;out vec4 color;
    float sampleShadow(sampler2D map,mat4 matrix,float bias){vec4 projected=matrix*vec4(vWorld,1.0);vec3 coordinate=projected.xyz/projected.w*.5+.5;if(coordinate.x<=0.0||coordinate.x>=1.0||coordinate.y<=0.0||coordinate.y>=1.0||coordinate.z<=0.0||coordinate.z>=1.0)return 1.0;vec2 texel=1.0/vec2(textureSize(map,0));float lit=0.0;for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++)lit+=coordinate.z-bias<=texture(map,coordinate.xy+vec2(x,y)*texel).r?1.0:0.0;return lit/9.0;}
    float sunShadow(){if(!shadowsEnabled)return 1.0;float depth=dot(vWorld-cameraPosition,cameraForward);if(depth<=shadowSplits.x)return sampleShadow(shadowMap0,shadowMatrix0,shadowBiases.x);if(depth<=shadowSplits.y)return sampleShadow(shadowMap1,shadowMatrix1,shadowBiases.y);if(depth<=shadowSplits.z)return sampleShadow(shadowMap2,shadowMatrix2,shadowBiases.z);return 1.0;}
    float localLightShadow(int lightIndex,vec3 toLight){int slot=cspLightShadowSlot[lightIndex];if(slot<0||slot>=MAX_CSP_LOCAL_SHADOWS)return 1.0;vec3 receiverPosition=vWorld+vec3(0.0,-0.1,0.0);vec4 projected=cspLocalShadowMatrix[slot]*vec4(receiverPosition,1.0);vec3 coordinate=projected.xyz/projected.w*.5+.5;if(projected.w<=0.0||coordinate.x<=0.0||coordinate.x>=1.0||coordinate.y<=0.0||coordinate.y>=1.0)return 1.0;vec2 cell=vec2(float(slot%2),float(slot/2)),atlasUv=(cell+coordinate.xy)*.5;vec4 p=cspLocalShadowParams[slot],shadowPosition=cspLocalShadowPosition[slot];float occluder=texture(cspLocalShadowAtlas,atlasUv).r;if(!cspLocalShadowFloat)occluder=exp(occluder*p.y*p.z);float distanceToLight=min(length(receiverPosition-shadowPosition.xyz),p.z),biasFactor=1.0-abs(dot(toLight,vSubstrateNormal)),receiver=exp(p.x*biasFactor-distanceToLight*p.y);return clamp(occluder*receiver*p.w+(1.0-p.w),0.0,1.0);}
    vec3 localLightDelta(int index,vec3 position,out float lineValue){if(cspLightFalloff[index].w>.5){vec3 ab=cspLightLineVector[index].xyz;lineValue=dot(position-cspLightPositionRange[index].xyz,ab)*cspLightLineVector[index].w;return cspLightPositionRange[index].xyz+clamp(lineValue,0.0,1.0)*ab-position;}lineValue=0.0;return cspLightPositionRange[index].xyz-position;}
    vec3 localLineSpecularDirection(int index,vec3 position,vec3 normalW,vec3 toCamera,vec3 fallback){if(cspLightFalloff[index].w<.5)return fallback;vec3 r=reflect(toCamera,normalW),l0=cspLightPositionRange[index].xyz-position,ld=cspLightLineVector[index].xyz;float rld=dot(r,ld),denominator=dot(ld,ld)-rld*rld,t=abs(denominator)>.000001?(dot(r,l0)*rld-dot(l0,ld))/denominator:0.0;vec3 l=l0+clamp(t,0.0,1.0)*ld,centerToRay=dot(l,r)*r-l;float centerDistance=length(centerToRay),spread=clamp((1.2-cspLightFalloff[index].z)*.25/max(centerDistance,.0001),0.0,1.0);return normalize(l+centerToRay*spread);}
    vec3 localLightShape(int index,vec3 toLight,float distanceToLight,float attenuation){float cosAngle=dot(cspLightDirectionCone[index].xyz,-toLight),primary=cspLightDirectionCone[index].w>.5?clamp(cspLightColorSpot[index].w-cosAngle,0.0,1.0):1.0;if(cspLightFalloff[index].w>.5)return vec3(attenuation*primary);vec3 edge=cspLightSpotEdge[index].w>.5?clamp(cspLightSpotEdge[index].rgb-vec3(dot(cspLightSpotUp[index].xyz,-toLight)),0.0,1.0):vec3(1.0);vec3 shape=vec3(attenuation*primary)*edge;if(cspLightSecondRange[index].w>0.0){float secondary=clamp(cspLightSecondCone[index].x-cosAngle*cspLightSecondCone[index].y,0.0,1.0),trimmed=max(clamp(distanceToLight*cspLightSecondRange[index].z-cspLightSecondRange[index].y,0.0,1.0)-clamp(distanceToLight*cspLightSecondRange[index].x,0.0,1.0),0.0);shape+=vec3(secondary*trimmed*trimmed*cspLightSecondRange[index].w);}return shape;}
    void main(){
      vec4 atlas=vec4(1.0);if(hasGrassAtlas){vec2 inset=.5/vec2(textureSize(grassAtlasTexture,0)),uv=clamp(vAtlasUv,vAtlasRect.xy+inset,vAtlasRect.xy+vAtlasRect.zw-inset);atlas=texture(grassAtlasTexture,uv);float coverage=clamp((atlas.a-.4)/max(fwidth(atlas.a),.001)+.5,0.0,1.0);if(coverage<.25)discard;atlas.a=coverage;}else{float halfWidth=mix(.5,.06,smoothstep(.05,1.0,vBlade.y));if(abs(vBlade.x)>halfWidth)discard;}
      float texSat=clamp((atlas.g-max(atlas.r,atlas.b))*2.0,0.0,1.0),texSpec=clamp(atlas.g*10.0-9.0,0.0,1.0);vec3 colored=hasGrassAtlas?pow(max(atlas.rgb*(4.444444*textureBrightness),vec3(0.0)),vec3(2.2)):vGrassColor,base=mix(colored,vGrassColor,texSat);float baseLuma=dot(base,vec3(.333));base=mix(vec3(baseLuma),base,.7+vRndFactors.x*.6);base*=.7+vRndFactors.y*.6;float wetK=clamp(wetness,0.0,1.0),wetSpec=clamp(wetness*100.0,0.0,1.0)*.5;base*=mix(1.0,pow(.65,2.2),wetK);
      vec3 cameraVector=vWorld-cameraPosition;float distanceToCamera=max(.0001,length(cameraVector)),distSq=dot(cameraVector,cameraVector),texMult=mix(1.0,atlas.g*atlas.g,texSat);texMult=mix(1.0,texMult,1.0/(1.0+distSq*.05));vec3 sun=normalize(sunDirection),cameraDirection=cameraVector/distanceToCamera,n=gl_FrontFacing?vNormal:-vNormal,substrateNormal=normalize(vSubstrateNormal);float ndl=max(dot(n,sun),0.0),substrateNdl=max(dot(vNormal,sun),0.0),shadow=sunShadow(),rootFade=clamp(.5+vBlade.y*3.0,0.0,1.0),directFade=clamp(vFade,0.0,1.0)*rootFade,viewY=abs(cameraDirection.y),texAo=mix(1.0,viewY*.5,vTexDimming),shadowDetails=1.0-substrateNdl*substrateNdl*.5,advFade=clamp(1.2-distanceToCamera/60.0,0.0,1.0),bentDirection=abs(dot(sun,normalize(vBent))),directionMult=bentDirection*(1.0+wetSpec),specIntensity=vMaterial.y*directFade*shadow*mix(1.0,directionMult,advFade),viewSun=dot(-cameraDirection,sun),backlitShape=.25*pow(pow(bentDirection,4.0)+pow(clamp(viewSun,0.0,1.0),32.0),2.0),backlitAmount=advFade*pow(clamp(vMaterial.x,0.0,1.0),2.0)*clamp(.5+.5*viewSun,0.0,1.0)*clamp(vFade,0.0,1.0)*shadow*clamp(vBlade.y*3.0,0.0,1.0)*backlitShape,backlitMult=clamp(dot(base,vec3(5.0)),0.0,1.0);float grassLuma=dot(vGrassColor,vec3(.333));vec3 backlitColor=max(mix(vec3(grassLuma),vGrassColor,2.0),vec3(0.0));
      vec3 localLighting=vec3(0.0),mainLightDirection=vec3(0.0,1.0,0.0);float mainLightPower=0.0;
      for(int i=0;i<MAX_CSP_LIGHTS;i++){if(i>=cspLightCount)break;float lineValue;vec3 delta=localLightDelta(i,vWorld,lineValue);float range=cspLightPositionRange[i].w,distanceToLight=length(delta);if(distanceToLight<range){vec3 toLight=delta/max(distanceToLight,.0001);float gradient=cspLightFalloff[i].x,attenuation=clamp((1.0-distanceToLight/range)/max(.001,1.0-gradient),0.0,1.0);attenuation*=attenuation;vec3 lightColor=cspLightColorSpot[i].rgb+cspLightLineColor[i].rgb*clamp(lineValue,0.0,1.0);vec3 radiance=lightColor*localLightShape(i,toLight,distanceToLight,attenuation)*localLightShadow(i,toLight);float concentration=max(0.0,cspLightFalloff[i].z),localNdl=clamp(mix(1.0,dot(substrateNormal,toLight),concentration),0.0,1.0);vec3 specularDirection=localLineSpecularDirection(i,vWorld,substrateNormal,cameraDirection,toLight);float localSpec=pow(max(dot(-normalize(cameraDirection-specularDirection),substrateNormal),0.0),52.8)*localNdl*max(0.0,cspLightFalloff[i].y)*clamp(vFade,0.0,1.0)*2.0;vec3 contribution=radiance*(localNdl+localSpec);localLighting+=contribution;float power=dot(contribution,vec3(1.0))*concentration;if(power>mainLightPower){mainLightPower=power;mainLightDirection=toLight;}}}
      float localRatio=clamp(dot(localLighting,vec3(1.0))/max(1e-10,dot(sunColor*.8*ndl*shadow*directFade+localLighting,vec3(1.0))),0.0,1.0);shadowDetails=mix(shadowDetails,1.0,localRatio);float fakeShadow=mix(1.0,clamp((texAo-.2)/.5,0.0,1.0),shadowDetails*texSat),lightingFocus=clamp(mainLightPower/max(dot(sunColor*.8*ndl*shadow*directFade,vec3(1.0)),1.0),0.0,1.0),localBacklitAmount=lightingFocus*.25*clamp(dot(cameraDirection,mainLightDirection),0.0,1.0)*advFade*clamp(vFade,0.0,1.0)*shadow*clamp(vBlade.y*3.0,0.0,1.0)*pow(abs(dot(mainLightDirection,normalize(vBent))),4.0);vec3 localBacklit=backlitColor*localLighting*localBacklitAmount*backlitMult;
      vec3 lit=base*(ambientColor*.35*texAo+sunColor*.8*ndl*shadow*directFade*vMaterial.x+localLighting*vMaterial.x)+sunColor*backlitColor*backlitAmount*backlitMult+localBacklit;lit*=mix(1.0,.5,vTexDimming*vTexDimming)*texMult;lit+=sunColor*specIntensity*(.3+texSpec*2.0)*fakeShadow;float fogAmount=pow(clamp(distanceToCamera/max(1.0,fogDistance),0.0,1.0),max(.05,fogBlend));lit=mix(lit,fogColor,fogAmount);color=vec4(max(lit,vec3(.01)),atlas.a);
    }`);
  const grassLocationNames=["viewProjection","cameraPosition","cameraForward","sunDirection","sunColor","ambientColor","fogColor","fogDistance","fogBlend","wetness","hasGrassAtlas","grassAtlasTexture","textureBrightness","windMapTexture","windVelocity","shadowsEnabled","shadowMap0","shadowMap1","shadowMap2","shadowMatrix0","shadowMatrix1","shadowMatrix2","shadowSplits","shadowBiases","cspLightCount","cspLightColorSpot","cspLightDirectionCone","cspLightFalloff","cspLightSecondCone","cspLightSecondRange","cspLightSpotEdge","cspLightSpotUp","cspLocalShadowAtlas","cspLocalShadowFloat"],grassLocations=Object.fromEntries(grassLocationNames.map((name)=>[name,gl.getUniformLocation(grassProgram,name)]));grassLocations.cspLightPositionRange=gl.getUniformLocation(grassProgram,"cspLightPositionRange[0]");grassLocations.cspLightColorSpot=gl.getUniformLocation(grassProgram,"cspLightColorSpot[0]");grassLocations.cspLightDirectionCone=gl.getUniformLocation(grassProgram,"cspLightDirectionCone[0]");grassLocations.cspLightFalloff=gl.getUniformLocation(grassProgram,"cspLightFalloff[0]");grassLocations.cspLightLineVector=gl.getUniformLocation(grassProgram,"cspLightLineVector[0]");grassLocations.cspLightLineColor=gl.getUniformLocation(grassProgram,"cspLightLineColor[0]");grassLocations.cspLightSecondCone=gl.getUniformLocation(grassProgram,"cspLightSecondCone[0]");grassLocations.cspLightSecondRange=gl.getUniformLocation(grassProgram,"cspLightSecondRange[0]");grassLocations.cspLightSpotEdge=gl.getUniformLocation(grassProgram,"cspLightSpotEdge[0]");grassLocations.cspLightSpotUp=gl.getUniformLocation(grassProgram,"cspLightSpotUp[0]");grassLocations.cspLightShadowSlot=gl.getUniformLocation(grassProgram,"cspLightShadowSlot[0]");grassLocations.cspLocalShadowMatrix=gl.getUniformLocation(grassProgram,"cspLocalShadowMatrix[0]");grassLocations.cspLocalShadowParams=gl.getUniformLocation(grassProgram,"cspLocalShadowParams[0]");grassLocations.cspLocalShadowPosition=gl.getUniformLocation(grassProgram,"cspLocalShadowPosition[0]");
  const grassShadowProgram=link(gl,`#version 300 es
    precision highp float;layout(location=0)in vec3 blade;layout(location=1)in vec4 instancePositionHeight;layout(location=2)in vec4 instanceAngleColor;layout(location=3)in vec4 instanceAtlasRect;layout(location=4)in vec3 instanceShape;layout(location=5)in vec2 instanceLean;layout(location=7)in float instanceType;layout(location=8)in vec2 instanceNormal;uniform mat4 lightViewProjection;uniform sampler2D windMapTexture;uniform vec2 windVelocity;out vec2 vBlade;out vec2 vAtlasUv;flat out vec4 vAtlasRect;void main(){float angle=instanceAngleColor.x;vec2 direction=vec2(cos(angle),sin(angle));float height=instancePositionHeight.w,width=max(.002,instanceShape.x),upY=sqrt(max(0.0,1.0-dot(instanceLean,instanceLean))),normalY=sqrt(max(0.0,1.0-dot(instanceNormal,instanceNormal)));vec3 up=vec3(instanceLean.x,upY,instanceLean.y),inNormal=vec3(instanceNormal.x,normalY,instanceNormal.y),side=normalize(cross(vec3(direction.x,0.0,direction.y),inNormal));vec3 position=instancePositionHeight.xyz+side*blade.x*width+up*blade.y*height;float phaseOffset=instanceType*.5,windMult=blade.y*instanceShape.y*.5*(textureLod(windMapTexture,position.xz/80.0+phaseOffset*.05,0.0).r+textureLod(windMapTexture,position.xz/21.71+phaseOffset*.05,0.0).r*.471)*up.y;vec2 windOffset=windMult*windVelocity;windOffset/=1.0+abs(windOffset);position.xz+=windOffset*height;position.y-=min(.5,1.0-sqrt(max(0.0,1.0-dot(windOffset,windOffset))))*(.5+fract(558.476*phaseOffset))*height;float atlasX=instanceAtlasRect.x+(blade.x*.5+.5)*instanceAtlasRect.z,atlasY=blade.y*(1.0-instanceShape.z*fract(instanceAtlasRect.x+atlasX*.3));vBlade=blade.xy;vAtlasUv=vec2(atlasX,instanceAtlasRect.y+atlasY*instanceAtlasRect.w);vAtlasRect=instanceAtlasRect;gl_Position=lightViewProjection*vec4(position,1.0);}`,
    `#version 300 es
    precision highp float;in vec2 vBlade;in vec2 vAtlasUv;flat in vec4 vAtlasRect;uniform bool hasGrassAtlas;uniform sampler2D grassAtlasTexture;out vec4 color;void main(){if(hasGrassAtlas){vec2 inset=.5/vec2(textureSize(grassAtlasTexture,0)),uv=clamp(vAtlasUv,vAtlasRect.xy+inset,vAtlasRect.xy+vAtlasRect.zw-inset);if(texture(grassAtlasTexture,uv).a<.5)discard;}else{float halfWidth=mix(.5,.06,smoothstep(.05,1.0,vBlade.y));if(abs(vBlade.x)>halfWidth)discard;}color=vec4(1.0);}`),grassShadowLocations={viewProjection:gl.getUniformLocation(grassShadowProgram,"lightViewProjection"),hasGrassAtlas:gl.getUniformLocation(grassShadowProgram,"hasGrassAtlas"),grassAtlasTexture:gl.getUniformLocation(grassShadowProgram,"grassAtlasTexture"),windMapTexture:gl.getUniformLocation(grassShadowProgram,"windMapTexture"),windVelocity:gl.getUniformLocation(grassShadowProgram,"windVelocity")};
  const grassVao=gl.createVertexArray(),grassBladeBuffer=gl.createBuffer(),grassInstanceBuffer=gl.createBuffer(),grassBladeVertices=new Float32Array([-1,0,0,1,0,0,1,1,0,-1,0,0,1,1,0,-1,1,0]),grassInstanceStride=GRASS_FX_INSTANCE_STRIDE*4;
  gl.bindVertexArray(grassVao);gl.bindBuffer(gl.ARRAY_BUFFER,grassBladeBuffer);gl.bufferData(gl.ARRAY_BUFFER,grassBladeVertices,gl.STATIC_DRAW);gl.vertexAttribPointer(0,3,gl.FLOAT,false,12,0);gl.enableVertexAttribArray(0);gl.bindBuffer(gl.ARRAY_BUFFER,grassInstanceBuffer);gl.vertexAttribPointer(1,4,gl.FLOAT,false,grassInstanceStride,0);gl.enableVertexAttribArray(1);gl.vertexAttribDivisor(1,1);gl.vertexAttribPointer(2,4,gl.FLOAT,false,grassInstanceStride,16);gl.enableVertexAttribArray(2);gl.vertexAttribDivisor(2,1);gl.vertexAttribPointer(3,4,gl.FLOAT,false,grassInstanceStride,32);gl.enableVertexAttribArray(3);gl.vertexAttribDivisor(3,1);gl.vertexAttribPointer(4,3,gl.FLOAT,false,grassInstanceStride,48);gl.enableVertexAttribArray(4);gl.vertexAttribDivisor(4,1);gl.vertexAttribPointer(5,2,gl.FLOAT,false,grassInstanceStride,60);gl.enableVertexAttribArray(5);gl.vertexAttribDivisor(5,1);gl.vertexAttribPointer(6,1,gl.FLOAT,false,grassInstanceStride,68);gl.enableVertexAttribArray(6);gl.vertexAttribDivisor(6,1);gl.vertexAttribPointer(7,1,gl.FLOAT,false,grassInstanceStride,72);gl.enableVertexAttribArray(7);gl.vertexAttribDivisor(7,1);gl.vertexAttribPointer(8,2,gl.FLOAT,false,grassInstanceStride,76);gl.enableVertexAttribArray(8);gl.vertexAttribDivisor(8,1);gl.bindVertexArray(null);
  const rainStreamProgram=link(gl,`#version 300 es
    precision highp float;layout(location=0)in vec3 position;uniform mat4 viewProjection;uniform float pointSize;void main(){gl_Position=viewProjection*vec4(position,1.0);gl_PointSize=pointSize;}`,
    `#version 300 es
    precision highp float;uniform vec4 streamColor;out vec4 color;void main(){color=streamColor;}`),rainStreamLocations={viewProjection:gl.getUniformLocation(rainStreamProgram,"viewProjection"),pointSize:gl.getUniformLocation(rainStreamProgram,"pointSize"),color:gl.getUniformLocation(rainStreamProgram,"streamColor")},rainStreamVao=gl.createVertexArray(),rainStreamBuffer=gl.createBuffer();
  gl.bindVertexArray(rainStreamVao);gl.bindBuffer(gl.ARRAY_BUFFER,rainStreamBuffer);gl.bufferData(gl.ARRAY_BUFFER,0,gl.DYNAMIC_DRAW);gl.vertexAttribPointer(0,3,gl.FLOAT,false,12,0);gl.enableVertexAttribArray(0);gl.bindVertexArray(null);
  const compression = {
    s3tc: gl.getExtension("WEBGL_compressed_texture_s3tc"),
    s3tcSrgb: gl.getExtension("WEBGL_compressed_texture_s3tc_srgb"),
    bptc: gl.getExtension("EXT_texture_compression_bptc")
  };
  gl.bindVertexArray(grassVao);gl.bindBuffer(gl.ARRAY_BUFFER,grassInstanceBuffer);gl.vertexAttribPointer(9,3,gl.FLOAT,false,grassInstanceStride,84);gl.enableVertexAttribArray(9);gl.vertexAttribDivisor(9,1);gl.vertexAttribPointer(10,2,gl.FLOAT,false,grassInstanceStride,96);gl.enableVertexAttribArray(10);gl.vertexAttribDivisor(10,1);gl.bindVertexArray(null);
  let items = [], itemByNode = new WeakMap(), sceneRoot = null,sceneModel=null, textures = [], textureLookup = new Map(), embeddedTextureNames = new Set(), solidTextures = new Map(), cspTextures = new Map(), cspTextureCache = new Map(), colliderItems = [], surfaceBindings=new Map(), selected = null, cspState = null,grassFx=null,grassStatus={instanceCount:0,sourceMeshes:0,sourceTriangles:0,rejectedByMask:0,rejectedByShape:0,rejectedByOcclusion:0,totalArea:0,density:0,shapeWidth:1},grassSurfaceMapCenter=null,grassRebuildTimer=0,rainFx=null,rainStatus={matchedMeshes:0,lineVertices:0,pointVertices:0},shadowStatus={enabled:true,mapSize:KS_SHADOW_MAP_SIZE,cascades:3,splits:[...KS_SHADOW_SPLITS],biases:[...KS_SHADOW_BIASES],casters:0,triangles:0},lightingStatus={name:KS_EDITOR_DEFAULT_WEATHER.name,source:KS_EDITOR_DEFAULT_WEATHER.source,heading:40,height:55,angleMix:0,sunColor:[0,0,0],ambientColor:[0,0,0],skyColor:[0,0,0],fogDistance:KS_EDITOR_DEFAULT_WEATHER.fogDistance,fogBlend:KS_EDITOR_DEFAULT_WEATHER.fogBlend,autoExposure:true,effectiveExposure:.35,exposureMin:KS_EDITOR_EXPOSURE.min,exposureMax:KS_EDITOR_EXPOSURE.max,toneMap:"Yebis default · function −1",hdr:hdrEnabled,samples:hdrSamples,glareEnabled:hdrEnabled,glareQuality:KS_EDITOR_GLARE.quality,bloomLevels:hdrEnabled?KS_EDITOR_GLARE.levels:0,bloomSourceScale:KS_EDITOR_GLARE.sourceScale,bloomThreshold:KS_EDITOR_GLARE.threshold,bloomCompositeScale:ksEditorBloomCompositeScale(),bloomKernel:"Yebis max-29 fallback",bloomKernelSamples:bloomKernels.map((kernel)=>kernel.sampleCount),bloomKernelSigmas:bloomKernels.map((kernel)=>kernel.sigma),dither:true}, workspaceLodIndex = null, driverCockpitMode=false, driverHiddenNames=new Set(), trackCamera=null, bounds = { center: [0,0,0], radius: 1 }, yaw = .7, pitch = .35, distance = 5, target = [0,0,0], dragging = null, modelGeneration = 0, textureStatus = { total: 0, ready: 0, pending: 0, unsupported: 0, protected: 0, formats: {} }, sceneStatus = { total: 0, visible: 0, hidden: 0 };
  let vaoBindings=new Map(),vaoStatus={source:"",version:0,records:0,matchedRecords:0,unmatchedRecords:0,alternateRecords:0,normalRecords:0,matchedMeshes:0,primaryMeshes:0,secondaryMeshes:0,vertices:0,minimum:255,maximum:255,mean:255},seasonalStatus={affectedMeshes:0,autumnMeshes:0,winterMeshes:0,legacySummerMeshes:0,peakAutumn:0,peakWinter:0,peakSummer:0},shaderProfileStatus=null;
  let reflectionCaptureStatus={enabled:true,ready:false,size:KS_EDITOR_CUBEMAP.size,faces:0,draws:0,triangles:0,farPlane:KS_EDITOR_CUBEMAP.farPlane,captureMilliseconds:0};
  let reflectionCubeInitialized=false,reflectionNextFace=0,reflectionCaptureRoot=null;
  let currentAnimation = null, currentAnimationName = "", currentAnimationPosition = 0, animationTransforms = new Map(), animationStatus = { name:"",position:0,version:0,frameCount:0,tracks:0,animatedTracks:0,matchedTracks:0,matchedNodes:0,unmatchedTracks:[] };
  let externalFileIndex=createAssetFileIndex([]),externalTextures=new Map(),externalCpuTextures=new Map(),externalGpuTextures=new Set(),externalGeneration=0,externalRequirementSignature="",externalTextureStatus={selected:0,requested:0,ready:0,pending:0,missing:0,ambiguous:0,unsupported:0,formats:{},missingPaths:[],ambiguousPaths:[]};
  let selectedSkinFiles=[],selectedSkinName="",skinTextures=new Map(),skinGpuTextures=new Set(),skinGeneration=0,skinTextureStatus={name:"",available:0,matched:0,ready:0,pending:0,inherited:0,ambiguous:0,unsupported:0,formats:{},replacedNames:[]};
  const windParticles=createCspWindParticles();let windTargetIndex=0,windLastTime=0,windAccumulator=0,windUpdateCount=0,windRandomState=0x6d2b79f5,windAnimationTimer=0;
  const api = { description, wireframe: false, isolate: false,showHidden:false, colliderVisible:false,surfaceOverlay:false,grassVisible:true,rainVisible:true,rainWetness:1,shadowsEnabled:true,vaoEnabled:true,weatherPreset:KS_EDITOR_DEFAULT_WEATHER,sunHeading:40,sunHeight:55,windHeading:50,windSpeed:5,autoExposure:true,exposure:.35, setModel,refreshHierarchy, setCollider, setCsp,setGrassFx,setRainFx,setVaoBindings, setAnimation, setExternalFiles, setSkinFiles,setDriverCockpitMode,setShowHidden(value){api.showHidden=Boolean(value);refreshAnimationWorlds();draw();},setTrackCamera,setReflectionCaptureRoot(value){reflectionCaptureRoot=value||null;reflectionCubeInitialized=false;reflectionNextFace=0;draw();},setSurfaceBindings(value){surfaceBindings=value instanceof Map?value:new Map();draw();},setWorkspaceLod(value){workspaceLodIndex=value===null||value===undefined?null:Number(value);draw();},onExternalTextureStatus:null,onSkinTextureStatus:null,onWorkspaceLodStatus:null,onAnimationStatus:null,onTrackCameraChange:null,frame,draw,get textureStatus(){return {...textureStatus,formats:{...textureStatus.formats}};},get externalTextureStatus(){return {...externalTextureStatus,formats:{...externalTextureStatus.formats},missingPaths:[...externalTextureStatus.missingPaths],ambiguousPaths:[...externalTextureStatus.ambiguousPaths]};},get skinTextureStatus(){return {...skinTextureStatus,formats:{...skinTextureStatus.formats},replacedNames:[...skinTextureStatus.replacedNames]};},get animationStatus(){return {...animationStatus,unmatchedTracks:[...animationStatus.unmatchedTracks]};},get grassStatus(){const active=api.grassVisible&&grassStatus.instanceCount>0,shadowed=active&&api.shadowsEnabled&&!api.surfaceOverlay,path=grassAtlasPath(),atlasReady=Boolean(path&&externalTextures.get(path.toLowerCase())),hasGroups=(grassStatus.groupInstances||[]).some((count)=>count>0);return {...grassStatus,texture:path,textureGrid:[...(grassFx?.textureGrid||[])],textureBrightness:grassFx?.textureBrightness??1,maskBlur:grassFx?.maskBlur??1,colorSampleMipLevel:grassFx?.colorSampleMipLevel??0,atlasReady,atlasMode:atlasReady?(hasGroups?"texture-groups":"base-row"):"procedural-fallback",windMapMode:`csp-${hdrEnabled?CSP_WIND_MAP_FORMAT:"r8-fallback"}`,windMapSize:CSP_WIND_MAP_SIZE,windParticles:CSP_WIND_PARTICLE_COUNT,windUpdates:windUpdateCount,windSpeed:api.windSpeed,windHeading:api.windHeading,airMapMode:"static-editor-zero",visible:api.grassVisible,weatherLit:active,castsShadows:shadowed,receivesShadows:shadowed};},get rainStatus(){return {...rainStatus,visible:api.rainVisible&&Boolean(rainFx),wetness:api.rainVisible?api.rainWetness:0};},get shadowStatus(){return {...shadowStatus,enabled:api.shadowsEnabled&&!api.surfaceOverlay,splits:[...shadowStatus.splits],biases:[...shadowStatus.biases]};},get lightingStatus(){return {...lightingStatus,sunColor:[...lightingStatus.sunColor],ambientColor:[...lightingStatus.ambientColor],skyColor:[...lightingStatus.skyColor]};},get vaoStatus(){return {...vaoStatus,enabled:api.vaoEnabled&&vaoStatus.matchedMeshes>0};},get seasonalStatus(){return {...seasonalStatus,yearProgress:cspState?.usedInputs.get("YEAR_PROGRESS")?.value??null,gpuAutumn:gl.getUniform(program,locations.seasonAutumn),gpuWinter:gl.getUniform(program,locations.seasonWinter)};},get shaderProfileStatus(){return shaderProfileStatus?{...shaderProfileStatus,unknownShaders:[...shaderProfileStatus.unknownShaders]}:null;},get sceneStatus(){const previewVisible=items.filter(itemPreviewVisible).length;return {...sceneStatus,gameVisible:sceneStatus.visible,gameHidden:sceneStatus.hidden,previewVisible,previewHidden:items.length-previewVisible,visible:previewVisible,hidden:items.length-previewVisible,showHidden:api.showHidden,driverCockpitMode,driverHidden:items.filter((item)=>item.sceneVisible&&!itemPreviewVisible(item)).length,trackCamera:trackCamera?{name:trackCamera.name,position:[...trackCamera.position],fov:trackCamera.splineData?trackCamera.minFov:(trackCamera.minFov+trackCamera.maxFov)/2,splinePosition:trackCamera.splinePreviewPosition??null,splineOffset:trackCamera.splineOffset?[...trackCamera.splineOffset]:null}:null,colliderMeshes:colliderItems.length,colliderVisible:api.colliderVisible,surfaceOverlay:api.surfaceOverlay,surfacePhysicsMeshes:[...surfaceBindings.values()].filter((binding)=>binding.status!=="not-physics").length,grassBlades:grassStatus.instanceCount,grassVisible:api.grassVisible&&grassStatus.instanceCount>0,rainMeshes:rainStatus.matchedMeshes,rainVisible:api.rainVisible&&Boolean(rainFx),rainWetness:api.rainVisible?api.rainWetness:0,shadowsEnabled:api.shadowsEnabled&&!api.surfaceOverlay,shadowCasters:shadowStatus.casters,vaoEnabled:api.vaoEnabled&&vaoStatus.matchedMeshes>0,vaoMeshes:vaoStatus.matchedMeshes,seasonalMeshes:seasonalStatus.affectedMeshes,shaderAlphaMaterials:shaderProfileStatus?.shadowCutout||0,weather:lightingStatus.name,exposure:lightingStatus.effectiveExposure};},get workspaceLodStatus(){const cameraDistance=Math.sqrt(distanceSquared(bounds.center,cameraEye())),effectiveDistance=carLodDistance(cameraDistance,cameraFovDegrees),activeIndices=[...new Set(items.filter((item)=>item.workspaceLod&&carLodVisible(item.workspaceLod,effectiveDistance,workspaceLodIndex)).map((item)=>item.workspaceLod.index))].sort((a,b)=>a-b);return {selectedIndex:workspaceLodIndex,cameraDistance,effectiveDistance,activeIndices};},select(node){selected=node;draw();}};
  api.reflectionsEnabled=true;
  api.refreshGeometry=refreshGeometry;

  function itemPreviewVisible(item){return (api.showHidden||item.sceneVisible)&&!(driverCockpitMode&&item.workspaceAuxiliary==="driver"&&item.driverPath.some((name)=>driverHiddenNames.has(name)));}
  function setDriverCockpitMode(value,names=[]){driverCockpitMode=Boolean(value);driverHiddenNames=new Set((names||[]).map((name)=>String(name||"").trim().toUpperCase()).filter(Boolean));refreshAnimationWorlds();draw();}
  function setTrackCamera(value){const next=value||null,changed=trackCamera!==next;if(!changed)return;trackCamera=next;scheduleGrassRebuild();draw();}
  function grassAtlasPath(){return grassFx?normalizeAssetPath(grassFx.texture||GRASS_FX_DEFAULT_TEXTURE):"";}
  function grassAtlasTexture(){const path=grassAtlasPath();return path?externalTextures.get(path.toLowerCase())||null:null;}
  function bindGrassAtlas(uniforms){const texture=grassAtlasTexture();gl.activeTexture(gl.TEXTURE13);gl.bindTexture(gl.TEXTURE_2D,texture);gl.uniform1i(uniforms.grassAtlasTexture,13);gl.uniform1i(uniforms.hasGrassAtlas,Boolean(texture));if(uniforms.textureBrightness!==undefined&&uniforms.textureBrightness!==null)gl.uniform1f(uniforms.textureBrightness,grassFx?.textureBrightness??1);return Boolean(texture);}
  function requiredExternalTexturePaths(){const paths=[...externalResourcePaths(cspState)];if(grassFx){paths.push(grassAtlasPath());for(const rule of grassFx.adjustments||[])if(rule.piece?.texture)paths.push(rule.piece.texture);}return [...new Map(paths.filter(Boolean).map((path)=>[normalizeAssetPath(path).toLowerCase(),normalizeAssetPath(path)])).values()];}
  function refreshExternalRequirements(force=false){const paths=requiredExternalTexturePaths(),signature=paths.map((path)=>path.toLowerCase()).join("\n");if(!force&&signature===externalRequirementSignature)return Promise.resolve();externalRequirementSignature=signature;return refreshExternalTextures(paths);}
  function decodedLevelSampler(levels){const sampleLevel=(level,u,v)=>{const pixel=(x,y,channel)=>level.pixels[(((y%level.height+level.height)%level.height)*level.width+(x%level.width+level.width)%level.width)*4+channel],px=(u-Math.floor(u))*level.width-.5,py=(v-Math.floor(v))*level.height-.5,x=Math.floor(px),y=Math.floor(py),fx=px-x,fy=py-y,result=[];for(let channel=0;channel<4;channel++){const a=pixel(x,y,channel)+(pixel(x+1,y,channel)-pixel(x,y,channel))*fx,b=pixel(x,y+1,channel)+(pixel(x+1,y+1,channel)-pixel(x,y+1,channel))*fx;result[channel]=a+(b-a)*fy;}return result;};return(u,v,mip=0)=>{const level=Math.max(0,Math.min(levels.length-1,Number(mip)||0)),lower=Math.floor(level),upper=Math.ceil(level),amount=level-lower,a=sampleLevel(levels[lower],u,v);if(lower===upper)return a;const b=sampleLevel(levels[upper],u,v);return a.map((value,index)=>value+(b[index]-value)*amount);};}
  function grassSampler(item,cache,slot="txdiffuse"){
    const requested=item.resourceNames?.[slot];if(!requested||!sceneModel)return null;let candidates=sceneModel.textures.filter((texture)=>normalizeAssetPath(texture.name).split("/").at(-1).toLowerCase()===requested),texture=candidates.find((candidate)=>String(candidate.workspaceFile||"").toLowerCase()===String(item.material?.workspaceFile||"").toLowerCase())||candidates[0];if(!texture?.data)return null;
    if(!cache.has(texture)){try{const descriptor=inspectDds(texture.data),levels=descriptor?decodeDdsRgba(texture.data,descriptor):null;cache.set(texture,levels?.length?levels:null);}catch{cache.set(texture,null);}}
    const levels=cache.get(texture);if(!levels)return null;
    return decodedLevelSampler(levels);
  }
  function grassConfigTextureSampler(path,cache){const normalized=normalizeAssetPath(path),external=externalCpuTextures.get(normalized.toLowerCase());if(external)return decodedLevelSampler(external);const basename=normalized.split("/").at(-1).toLowerCase(),texture=sceneModel?.textures.find((candidate)=>normalizeAssetPath(candidate.name).toLowerCase()===normalized.toLowerCase())||sceneModel?.textures.find((candidate)=>normalizeAssetPath(candidate.name).split("/").at(-1).toLowerCase()===basename);if(!texture?.data)return null;if(!cache.has(texture)){try{const descriptor=inspectDds(texture.data),levels=descriptor?decodeDdsRgba(texture.data,descriptor):null;cache.set(texture,levels?.length?levels:null);}catch{cache.set(texture,null);}}const levels=cache.get(texture);return levels?decodedLevelSampler(levels):null;}
  function grassPropertyVector(item,name,fallback=[1,1]){const override=cspState?.nodeOverrides.get(item.node),changed=override?.properties.get(name.toLowerCase());if(Array.isArray(changed))return [Number(changed[0])||0,Number(changed[1]??changed[0])||0];if(typeof changed==="number")return [changed,changed];const property=item.material?.properties.find((entry)=>entry.name.toLowerCase()===name.toLowerCase());if(property?.value2?.some((value)=>Number(value)!==0))return property.value2.map((value)=>Number(value)||0);if(property)return [Number(property.value)||0,Number(property.value)||0];return [...fallback];}
  function grassSurfaceSampler(item,cache){const diffuse=grassSampler(item,cache),override=cspState?.nodeOverrides.get(item.node),shader=override?.shader||item.material?.shader||"",multilayer=/multilayer/i.test(shader),multimap=/multimap/i.test(shader),mask=multilayer?grassSampler(item,cache,"txmask"):null,maps=multimap?grassSampler(item,cache,"txmaps"):null,details=multilayer?["txdetailr","txdetailg","txdetailb","txdetaila"].map((slot)=>grassSampler(item,cache,slot)):[],scales=multilayer?["multR","multG","multB","multA"].map((name)=>grassPropertyVector(item,name)):[],ambient=effectiveScalar(item.material,override,"ksAmbient",.4),diffuseLevel=effectiveScalar(item.material,override,"ksDiffuse",.4),specular=effectiveScalar(item.material,override,"ksSpecular",0),specularExp=effectiveScalar(item.material,override,"ksSpecularEXP",1),magic=effectiveScalar(item.material,override,"magicMult",1),alphaRef=effectiveScalar(item.material,override,"ksAlphaRef",0),inverseMask=effectiveScalar(item.material,override,"ksExposure",0)===-17,autumn=effectiveScalar(item.material,override,"seasonAutumn",0),winter=effectiveScalar(item.material,override,"seasonWinter",0);if(!diffuse)return null;const white=[255,255,255,255],materialAlpha=(u,v,mip)=>multilayer||alphaRef<=0?1:diffuse(u,v,mip)[3]/255,surfaceAlpha=(u,v,mip)=>{const alpha=materialAlpha(u,v,mip);return inverseMask?1-alpha:alpha;},combinedDetail=(u,v,position,mip)=>{const weights=mask?.(u,v,mip)||white,combined=[0,0,0,0];for(let layer=0;layer<4;layer++){const scale=scales[layer],detail=details[layer]?.(position[0]*scale[0],position[2]*scale[1],mip)||white,weight=weights[layer]/255;for(let channel=0;channel<4;channel++)combined[channel]+=detail[channel]/255*weight;}return combined;};return {multilayer,sampleAlpha:materialAlpha,sampleEdgeAlpha:surfaceAlpha,sampleMaterial(u,v,position){let specularValue=specular,exponent=specularExp;if(multimap&&maps){const value=maps(u,v,0);specularValue*=value[0]/255;exponent=value[1]/255*specularExp+1;}else if(multilayer)specularValue*=combinedDetail(u,v,position,0)[3];return [diffuseLevel,ambient,specularValue,exponent/255];},sample(u,v,position,mip=0,normal=[0,1,0]){const base=diffuse(u,v,mip);let rgb=base.slice(0,3).map((value)=>value/255);if(multilayer){const combined=combinedDetail(u,v,position,mip);rgb=rgb.map((value,index)=>value*combined[index]*magic);}rgb=adjustCspSeasonColor(rgb,normal,.5,autumn,winter).map((value)=>Math.max(0,Math.min(1,value*ambient)));return [...rgb.map((value)=>value*255),surfaceAlpha(u,v,mip)*255];}};}
  function grassNormalSampler(item,cache){const override=cspState?.nodeOverrides.get(item.node),shader=override?.shader||item.material?.shader||"",supported=/^ksPerPixel(?:MultiMap(?:_|$)|NM(?:_|$)|AT_NM(?:_|$))/i.test(shader),objectSpace=effectiveScalar(item.material,override,"nmObjectSpace",0)!==0,normalMap=supported&&!objectSpace?grassSampler(item,cache,"txnormal"):null;if(!normalMap)return null;return {shader,sample(u,v,position,mip=0,frame={}){return grassTangentSpaceNormal(normalMap(u,v,mip),frame);}};}
  function grassAdjustmentSampler(item,cache){const rules=grassFx?.sourceAdjustments?.get(item.node)?.filter((rule)=>rule.multilayer)||[];if(!rules.length)return null;const direct=grassFx.sourceProfiles?.get(item.node)||[0,0,0,0],samplers=rules.map((rule)=>({rule,sample:grassSampler(item,cache,rule.useMultilayerMask?"txmask":"txdiffuse")})).filter((entry)=>entry.sample),byRule=new Map(samplers.map((entry)=>[entry.rule,entry.sample]));if(!samplers.length)return null;const sampleRule=(rule,u,v)=>{const texture=byRule.get(rule)?.(u,v,0);return texture?rule.maskBase.map((value,index)=>Math.max(0,Math.min(1,value+texture.reduce((sum,channel,source)=>sum+channel/255*rule.maskChannels[source][index],0)))):null;},combined=(u,v)=>{const result=[...direct];for(const {rule}of samplers){const mapped=sampleRule(rule,u,v);for(let index=0;index<4;index++)result[index]=Math.max(result[index],mapped[index]);}return result;};combined.sampleRule=sampleRule;return combined;}
  function rebuildGrassFx(){
    if(grassRebuildTimer){clearTimeout(grassRebuildTimer);grassRebuildTimer=0;}
    for(const item of items){delete item.sampleDiffuse;delete item.sampleMask;delete item.sampleGrassSurface;delete item.sampleGrassMaterial;delete item.sampleGrassEdgeAlpha;delete item.sampleGrassOccluderAlpha;delete item.sampleGrassNormal;delete item.sampleGrassAdjustment;delete item.sampleGrassAdjustmentRule;}
    if(!grassFx){gl.bindBuffer(gl.ARRAY_BUFFER,grassInstanceBuffer);gl.bufferData(gl.ARRAY_BUFFER,0,gl.DYNAMIC_DRAW);grassStatus={instanceCount:0,sourceMeshes:0,sourceTriangles:0,rejectedByMask:0,rejectedByShape:0,rejectedByOcclusion:0,totalArea:0,density:0,shapeWidth:1};return;}
    const cache=new Map();let surfaceSamplers=0,multilayerSurfaceSamplers=0,materialSamplers=0,normalMapSamplers=0,adjustmentSamplers=0,adjustmentPieceSamplers=0;
    for(const rule of grassFx.adjustments||[])if(rule.piece?.texture){rule.sampleTexture=grassConfigTextureSampler(rule.piece.texture,cache);if(rule.sampleTexture)adjustmentPieceSamplers++;}
    for(const item of items){const source=grassFx.sourceNodes?.has(item.node),occluder=grassFx.occluderNodes?.has(item.node);if(!source&&!occluder)continue;const surface=grassSurfaceSampler(item,cache),normalMap=grassNormalSampler(item,cache),adjustment=source?grassAdjustmentSampler(item,cache):null;if(source&&surface){item.sampleGrassSurface=surface.sample;item.sampleGrassMaterial=surface.sampleMaterial;item.sampleGrassEdgeAlpha=surface.sampleEdgeAlpha;surfaceSamplers++;materialSamplers++;if(surface.multilayer)multilayerSurfaceSamplers++;}if(occluder&&surface)item.sampleGrassOccluderAlpha=(u,v,position,mip)=>1-surface.sampleAlpha(u,v,mip);if(normalMap){item.sampleGrassNormal=normalMap.sample;if(source)normalMapSamplers++;}if(adjustment){item.sampleGrassAdjustment=adjustment;item.sampleGrassAdjustmentRule=adjustment.sampleRule;adjustmentSamplers++;}}
    const generationCamera=grassGenerationCamera();generationCamera.lightDirection=sunDirectionFromAngles(api.sunHeading,api.sunHeight);const generated=buildGrassInstances(items,grassFx,{maxInstances:40000,maxGenerationCandidates:25000,yearProgress:cspState?.usedInputs.get("YEAR_PROGRESS")?.value??.5,surfaceTarget:true,camera:generationCamera,surfaceMapCenter:grassSurfaceMapCenter}),{instances,...status}=generated;grassSurfaceMapCenter=status.surfaceTargetCenter||null;gl.bindBuffer(gl.ARRAY_BUFFER,grassInstanceBuffer);gl.bufferData(gl.ARRAY_BUFFER,instances,gl.DYNAMIC_DRAW);grassStatus={...status,surfaceSamplers,multilayerSurfaceSamplers,materialSamplers,materialTargetMode:status.surfaceTargetResolution?"rgba8-bilinear-mip0":"direct-material",deformationMapMode:"static-editor-zero-rg8-unorm",deformationMapSize:1024,deformationMapRadius:100,deformationOcclusion:false,wetLightingMode:"custom_l-albedo+near-substrate-specular",backlitLightingMode:"custom_l-zero-pad-directional",localLightMode:"custom_l-packed-point+finite-line+view-receiver-gates+camera-occluders+substrate-diffuse+segment-gloss+main-backlight+radial-esm-atlas",localLights:0,localShadowLights:0,normalMapSamplers,adjustmentSamplers,adjustmentPieceSamplers};
  }
  function setGrassFx(value){const enabled=Boolean(value);if(enabled&&!grassFx)api.grassVisible=true;grassFx=value||null;grassSurfaceMapCenter=null;rebuildGrassFx();refreshExternalRequirements();draw();}
  function rebuildRainFx(){const lines=[],points=[];for(const stream of rainFx?.streams||[]){if(stream.type==="point")points.push(...stream.point);else lines.push(...stream.from,...stream.to);}gl.bindBuffer(gl.ARRAY_BUFFER,rainStreamBuffer);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([...lines,...points]),gl.DYNAMIC_DRAW);rainStatus={matchedMeshes:rainFx?.matchedMeshes||0,lineVertices:lines.length/3,pointVertices:points.length/3,counts:{...(rainFx?.counts||{})}};}
  function setRainFx(value){const enabled=Boolean(value);if(enabled&&!rainFx)api.rainVisible=true;rainFx=value||null;rebuildRainFx();draw();}
  function surfaceOverlayColor(binding){if(binding?.status==="fallback")return [1,.02,.72];if(binding?.status==="ambiguous")return [1,.02,.02];const key=binding?.surface?.key?.toUpperCase()||"";if(key==="WALL")return [1,.06,.03];if(key==="ROAD")return [.08,.38,1];if(key==="GRASS")return [.06,.75,.18];if(key==="KERB")return [1,.5,.02];if(key==="SAND")return [.95,.68,.16];return hashColor(`surface:${key}`).map((value)=>.15+.85*value);}
  function clearTrackCamera(){if(!trackCamera)return;trackCamera=null;api.onTrackCameraChange?.(null);}

  function cameraEye() { return trackCamera?[...trackCamera.position]:[target[0]+distance*Math.cos(pitch)*Math.sin(yaw), target[1]+distance*Math.sin(pitch), target[2]+distance*Math.cos(pitch)*Math.cos(yaw)]; }
  function grassGenerationCamera(){const position=cameraEye(),viewTarget=trackCamera?trackCamera.position.map((value,index)=>value+trackCamera.forward[index]):target,direction=norm(sub(viewTarget,position)),fovDegrees=trackCamera?Math.max(1,Math.min(160,trackCamera.splineData?trackCamera.minFov:(trackCamera.minFov+trackCamera.maxFov)/2)):cameraFovDegrees;return {position,direction,fovDegrees,aspect:Math.max(.1,(canvas.width||canvas.clientWidth||1)/(canvas.height||canvas.clientHeight||1))};}
  function scheduleGrassRebuild(delay=90){if(!grassFx)return;if(grassRebuildTimer)clearTimeout(grassRebuildTimer);grassRebuildTimer=setTimeout(()=>{grassRebuildTimer=0;rebuildGrassFx();draw();},delay);}

  function setCsp(value) {
    const nextTextures=new Map(),nextCache=new Map();cspState=value;seasonalStatus=analyzeCspSeasonalOverrides(value);
    for (const override of value?.nodeOverrides.values() || []) {
      const custom=override.customEmissive;if(!custom||nextTextures.has(custom))continue;
      const key=customEmissiveTextureKey(custom),baked=nextCache.get(key)||cspTextureCache.get(key)||uploadCustomEmissive(gl,custom);nextTextures.set(custom,baked);nextCache.set(key,baked);
    }
    for(const [key,baked] of cspTextureCache)if(!nextCache.has(key))deleteCustomEmissiveTexture(gl,baked);
    cspTextures=nextTextures;cspTextureCache=nextCache;
    refreshExternalRequirements();
    draw();
  }

  function notifyExternalTextureStatus(){api.onExternalTextureStatus?.(api.externalTextureStatus);}

  async function setExternalFiles(files) {
    externalFileIndex=createAssetFileIndex(files);
    return refreshExternalRequirements(true);
  }

  function notifySkinTextureStatus(){api.onSkinTextureStatus?.(api.skinTextureStatus);}

  async function setSkinFiles(files, name = "") {
    selectedSkinFiles=[...(files || [])];selectedSkinName=String(name || "");
    return refreshSkinTextures();
  }

  async function refreshSkinTextures() {
    const generation=++skinGeneration;
    for(const texture of skinGpuTextures)gl.deleteTexture(texture);
    skinGpuTextures=new Set();skinTextures=new Map();
    const matched=matchSkinTextures(selectedSkinFiles,embeddedTextureNames);
    skinTextureStatus={name:selectedSkinName,available:selectedSkinFiles.length,matched:matched.files.length,ready:0,pending:matched.files.length,inherited:matched.missing.length,ambiguous:matched.ambiguous.length,unsupported:0,formats:{},replacedNames:[]};
    notifySkinTextureStatus();draw();
    await Promise.all(matched.files.map(async({name,entry})=>{
      const result=await uploadSkinFile(entry.file || entry,generation);if(generation!==skinGeneration)return;
      skinTextureStatus.pending--;
      if(!result)skinTextureStatus.unsupported++;
      else{skinTextures.set(name,result.texture);skinTextureStatus.ready++;skinTextureStatus.formats[result.format]=(skinTextureStatus.formats[result.format]||0)+1;skinTextureStatus.replacedNames.push(name);}
      notifySkinTextureStatus();draw();
    }));
    if(generation===skinGeneration){skinTextureStatus.replacedNames.sort((a,b)=>a.localeCompare(b));notifySkinTextureStatus();draw();}
  }

  async function uploadSkinFile(file,generation) {
    try{
      const bytes=new Uint8Array(await file.arrayBuffer());if(generation!==skinGeneration)return null;
      const uploaded=uploadEmbeddedTexture(gl,compression,bytes,()=>generation===skinGeneration);if(!uploaded)return null;
      skinGpuTextures.add(uploaded.texture);
      if(uploaded.ready&&!await uploaded.ready){skinGpuTextures.delete(uploaded.texture);gl.deleteTexture(uploaded.texture);return null;}
      if(generation!==skinGeneration){skinGpuTextures.delete(uploaded.texture);gl.deleteTexture(uploaded.texture);return null;}
      return {texture:uploaded.texture,format:uploaded.format};
    }catch(error){console.warn(`Could not load skin texture ${file.name}`,error);return null;}
  }

  async function refreshExternalTextures(paths) {
    const generation=++externalGeneration;
    for(const texture of externalGpuTextures)gl.deleteTexture(texture);
    externalGpuTextures=new Set();externalTextures=new Map();externalCpuTextures=new Map();
    const resolved=[];
    externalTextureStatus={selected:externalFileIndex.entries.length,requested:paths.length,ready:0,pending:0,missing:0,ambiguous:0,unsupported:0,formats:{},missingPaths:[],ambiguousPaths:[]};
    for(const path of paths){const match=resolveAssetFile(externalFileIndex,path);if(match.status==="resolved")resolved.push({path,match});else if(match.status==="ambiguous"){externalTextureStatus.ambiguous++;externalTextureStatus.ambiguousPaths.push(path);}else{externalTextureStatus.missing++;externalTextureStatus.missingPaths.push(path);}}
    externalTextureStatus.pending=resolved.length;notifyExternalTextureStatus();
    const uploads=new Map();
    await Promise.all(resolved.map(async({path,match})=>{
      let upload=uploads.get(match.file);
      if(!upload){upload=uploadExternalFile(match.file,generation);uploads.set(match.file,upload);}
      const result=await upload;if(generation!==externalGeneration)return;
      externalTextureStatus.pending--;
      if(!result){externalTextureStatus.unsupported++;}
      else{const key=normalizeAssetPath(path).toLowerCase();externalTextures.set(key,result.texture);if(result.levels)externalCpuTextures.set(key,result.levels);externalTextureStatus.ready++;externalTextureStatus.formats[result.format]=(externalTextureStatus.formats[result.format]||0)+1;}
      notifyExternalTextureStatus();draw();
    }));
    if(generation===externalGeneration){notifyExternalTextureStatus();if(grassFx?.adjustments?.some((rule)=>rule.piece?.texture))rebuildGrassFx();}
  }

  async function uploadExternalFile(file,generation){
    try{
      const bytes=new Uint8Array(await file.arrayBuffer());if(generation!==externalGeneration)return null;
      const uploaded=uploadEmbeddedTexture(gl,compression,bytes,()=>generation===externalGeneration);if(!uploaded)return null;
      externalGpuTextures.add(uploaded.texture);
      if(uploaded.ready&&!await uploaded.ready){externalGpuTextures.delete(uploaded.texture);gl.deleteTexture(uploaded.texture);return null;}
      if(generation!==externalGeneration){externalGpuTextures.delete(uploaded.texture);gl.deleteTexture(uploaded.texture);return null;}
      let levels=null;try{const descriptor=inspectDds(bytes);levels=descriptor?decodeDdsRgba(bytes,descriptor):null;}catch{}
      return {texture:uploaded.texture,format:uploaded.format,levels};
    }catch(error){console.warn(`Could not load external texture ${file.name}`,error);return null;}
  }

  function setCollider(value){
    for(const item of colliderItems){gl.deleteBuffer(item.vertex);gl.deleteBuffer(item.index);gl.deleteVertexArray(item.vao);}colliderItems=[];api.colliderVisible=false;
    if(!value?.root){draw();return;}
    const visit=(node,parent)=>{const world=node.transform?multiply(parent,node.transform):parent;if(node.kind==="mesh"||node.kind==="skinnedMesh"){
      const edgeSet=new Set(),edgeIndices=[];for(let offset=0;offset+2<node.indices.length;offset+=3){const triangle=[node.indices[offset],node.indices[offset+1],node.indices[offset+2]];for(let edge=0;edge<3;edge++){const a=triangle[edge],b=triangle[(edge+1)%3],key=a<b?`${a}:${b}`:`${b}:${a}`;if(edgeSet.has(key))continue;edgeSet.add(key);edgeIndices.push(a,b);}}
      const vao=gl.createVertexArray(),vertex=gl.createBuffer(),index=gl.createBuffer();gl.bindVertexArray(vao);gl.bindBuffer(gl.ARRAY_BUFFER,vertex);gl.bufferData(gl.ARRAY_BUFFER,node.vertices,gl.STATIC_DRAW);gl.vertexAttribPointer(0,3,gl.FLOAT,false,node.vertexStride*4,0);gl.enableVertexAttribArray(0);gl.vertexAttribPointer(1,3,gl.FLOAT,false,node.vertexStride*4,12);gl.enableVertexAttribArray(1);gl.vertexAttribPointer(2,2,gl.FLOAT,false,node.vertexStride*4,24);gl.enableVertexAttribArray(2);gl.vertexAttribPointer(3,3,gl.FLOAT,false,node.vertexStride*4,32);gl.enableVertexAttribArray(3);gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,index);gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,new Uint16Array(edgeIndices),gl.STATIC_DRAW);colliderItems.push({vao,vertex,index,world,count:edgeIndices.length});}
      for(const child of node.children||[])visit(child,world);};visit(value.root,identity());draw();
  }

  function setVaoBindings(value,status={}){
    vaoBindings=value instanceof Map?value:new Map();vaoStatus={source:"",version:0,records:0,matchedRecords:0,unmatchedRecords:0,alternateRecords:0,normalRecords:0,matchedMeshes:0,primaryMeshes:0,secondaryMeshes:0,vertices:0,minimum:255,maximum:255,mean:255,...status};
    for(const item of items){const binding=vaoBindings.get(item.node),count=item.node.vertices.length/item.node.vertexStride,values=binding?.primary&&binding.primary.length===count?binding.primary:new Uint8Array(count).fill(255);gl.bindBuffer(gl.ARRAY_BUFFER,item.vaoAo);gl.bufferData(gl.ARRAY_BUFFER,values,gl.DYNAMIC_DRAW);item.vaoBound=Boolean(binding?.primary);}
    draw();
  }

  function setModel(model) {
    const generation = ++modelGeneration;
    grassSurfaceMapCenter=null;
    sceneModel=model;shaderProfileStatus=auditMaterialShaderProfiles(model.materials);reflectionCaptureRoot=null;reflectionCubeInitialized=false;reflectionNextFace=0;
    items.forEach((x) => { gl.deleteBuffer(x.vertex); gl.deleteBuffer(x.index);gl.deleteBuffer(x.vaoAo); gl.deleteVertexArray(x.vao); });
    textures.forEach((texture) => gl.deleteTexture(texture));
    items = []; itemByNode = new WeakMap(); sceneRoot = model.root; textures = []; textureLookup = new Map(); embeddedTextureNames = new Set(); solidTextures = new Map();
    const textureMap = new Map();
    textureStatus={total:model.textures.length,ready:0,pending:0,unsupported:0,protected:model.encryption?.protectedTextures.length||0,formats:{}};
    for (const texture of model.textures) {
      embeddedTextureNames.add(normalizeAssetPath(texture.name).split("/").at(-1).toLowerCase());
      const uploaded = texture.data && uploadEmbeddedTexture(gl,compression,texture.data,()=>generation===modelGeneration);
      if(!uploaded){textureStatus.unsupported++;continue;}
      textureStatus.formats[uploaded.format]=(textureStatus.formats[uploaded.format]||0)+1;const textureName=texture.name.toLowerCase();textureMap.set(textureName,uploaded.texture);if(texture.workspaceFile)textureMap.set(`${texture.workspaceFile.toLowerCase()}\0${textureName}`,uploaded.texture);textures.push(uploaded.texture);
      if(uploaded.ready){textureStatus.pending++;uploaded.ready.then((success)=>{if(generation!==modelGeneration)return;textureStatus.pending--;if(success)textureStatus.ready++;else textureStatus.unsupported++;draw();});}else textureStatus.ready++;
    }
    textureLookup=textureMap;refreshSkinTextures();
    const visit = (node, parentWorld, parentActive, parentWorkspaceLod = null, parentWorkspaceAuxiliary = null, parentDriverPath = [], parentWorkspaceFile = "", parentReflectionAncestors = []) => {
      const world = node.transform ? multiply(parentWorld, node.transform) : parentWorld;
      const branchActive = parentActive && node.active;
      const workspaceLod = node.workspaceLod || parentWorkspaceLod;
      const workspaceAuxiliary=node.workspaceAuxiliary||parentWorkspaceAuxiliary,workspaceFile=node.workspaceFile||parentWorkspaceFile,reflectionAncestors=[...parentReflectionAncestors,node],driverPath=workspaceAuxiliary==="driver"?[...parentDriverPath,node.name.toUpperCase()]:[];
      if (node.kind === "mesh" || node.kind === "skinnedMesh") {
        const vao = gl.createVertexArray(), vertex = gl.createBuffer(), index = gl.createBuffer(),vaoAo=gl.createBuffer();
        gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, vertex); gl.bufferData(gl.ARRAY_BUFFER, node.vertices, gl.STATIC_DRAW);
        gl.vertexAttribPointer(0, 3, gl.FLOAT, false, node.vertexStride * 4, 0); gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(1, 3, gl.FLOAT, false, node.vertexStride * 4, 12); gl.enableVertexAttribArray(1);
        gl.vertexAttribPointer(2, 2, gl.FLOAT, false, node.vertexStride * 4, 24); gl.enableVertexAttribArray(2);
        gl.vertexAttribPointer(3, 3, gl.FLOAT, false, node.vertexStride * 4, 32); gl.enableVertexAttribArray(3);
        gl.bindBuffer(gl.ARRAY_BUFFER,vaoAo);gl.bufferData(gl.ARRAY_BUFFER,new Uint8Array(node.vertices.length/node.vertexStride).fill(255),gl.DYNAMIC_DRAW);gl.vertexAttribPointer(4,1,gl.UNSIGNED_BYTE,true,1,0);gl.enableVertexAttribArray(4);
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, node.indices, gl.STATIC_DRAW);
        const material = model.materials[node.materialId];
        const resourceNames=Object.fromEntries((material?.resources||[]).map((resource)=>[resource.slot.toLowerCase(),normalizeAssetPath(resource.texture).split("/").at(-1).toLowerCase()]));
        const resourceTexture=(slot)=>{const name=material?.resources.find((resource)=>resource.slot.toLowerCase()===slot)?.texture.toLowerCase();return textureMap.get(`${String(material?.workspaceFile||"").toLowerCase()}\0${name}`)||textureMap.get(name);};
        const localMin=[Infinity,Infinity,Infinity],localMax=[-Infinity,-Infinity,-Infinity];
        for (let i = 0; i < node.vertices.length; i += node.vertexStride) {
          for (let axis=0;axis<3;axis++){const value=node.vertices[i+axis];localMin[axis]=Math.min(localMin[axis],value);localMax[axis]=Math.max(localMax[axis],value);}
        }
        const sceneVisible=Boolean(branchActive&&node.visible&&node.renderable),item={ node, world, branchActive,sceneVisible, workspaceLod, workspaceAuxiliary,workspaceFile,reflectionAncestors, driverPath, material, resourceNames, texture: resourceTexture("txdiffuse"), normalTexture:resourceTexture("txnormal"), mapsTexture:resourceTexture("txmaps"), detailTexture:resourceTexture("txdetail"), normalDetailTexture:resourceTexture("txnormaldetail")||resourceTexture("txdetailnm"), multiMaskTexture:resourceTexture("txmask"),multiDetailRTexture:resourceTexture("txdetailr"),multiDetailGTexture:resourceTexture("txdetailg"),multiDetailBTexture:resourceTexture("txdetailb"),multiDetailATexture:resourceTexture("txdetaila"),multiDetailNormalTexture:resourceTexture("txdetailnm"), vao, vertex, index,vaoAo,vaoBound:false, localMin, localMax, center:[0,0,0] };items.push(item);itemByNode.set(node,item);
      }
      for (const child of node.children) visit(child, world, branchActive, workspaceLod, workspaceAuxiliary, driverPath,workspaceFile,reflectionAncestors);
    };
    visit(model.root, identity(), true);sceneStatus={total:items.length,visible:items.filter((item)=>item.sceneVisible).length,hidden:items.filter((item)=>!item.sceneVisible).length};
    refreshAnimationWorlds();frame();
  }

  function setAnimation(animation, position = 0, name = "") {
    currentAnimation=animation||null;currentAnimationName=String(name||animation?.source||"");currentAnimationPosition=Math.max(0,Math.min(1,Number(position)||0));
    animationTransforms=currentAnimation?sampleKsAnimation(currentAnimation,currentAnimationPosition):new Map();
    refreshAnimationWorlds();draw();
  }

  function refreshHierarchy() {
    refreshAnimationWorlds(); reflectionCubeInitialized=false; reflectionNextFace=0; scheduleGrassRebuild(); draw();
  }

  function refreshGeometry() {
    for (const item of items) {
      gl.bindBuffer(gl.ARRAY_BUFFER, item.vertex);
      gl.bufferData(gl.ARRAY_BUFFER, item.node.vertices, gl.STATIC_DRAW);
      gl.bindVertexArray(item.vao);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, item.index);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, item.node.indices, gl.STATIC_DRAW);
      item.localMin = [Infinity, Infinity, Infinity]; item.localMax = [-Infinity, -Infinity, -Infinity];
      for (let offset = 0; offset < item.node.vertices.length; offset += item.node.vertexStride) for (let axis = 0; axis < 3; axis++) {
        const value = item.node.vertices[offset + axis];
        item.localMin[axis] = Math.min(item.localMin[axis], value); item.localMax[axis] = Math.max(item.localMax[axis], value);
      }
    }
    gl.bindVertexArray(null);
    refreshAnimationWorlds(); reflectionCubeInitialized=false; reflectionNextFace=0; scheduleGrassRebuild(); draw();
  }

  function refreshAnimationWorlds() {
    const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity],matchedTracks=new Set(),worldByName=new Map();let matchedNodes=0;
    const visit=(node,parentWorld,parentActive)=>{
      const animated=animationTransformForNode(node,animationTransforms);if(animated){matchedTracks.add(node.name);matchedNodes++;}
      const local=animated||node.transform,world=local?multiply(parentWorld,local):parentWorld,item=itemByNode.get(node),branchActive=parentActive&&node.active;if(!worldByName.has(node.name))worldByName.set(node.name,world);
      if(item){item.world=world;item.branchActive=branchActive;item.sceneVisible=Boolean(branchActive&&node.visible&&node.renderable);const itemBounds=transformBounds(item.localMin,item.localMax,world);item.center=itemBounds.min.map((value,index)=>(value+itemBounds.max[index])/2);if(itemPreviewVisible(item))for(let axis=0;axis<3;axis++){min[axis]=Math.min(min[axis],itemBounds.min[axis]);max[axis]=Math.max(max[axis],itemBounds.max[axis]);}}
      for(const child of node.children)visit(child,world,branchActive);
    };
    let skinnedMeshes=0,skinnedVertices=0,maxSkinnedDisplacement=0;if(sceneRoot)visit(sceneRoot,identity(),true);for(const item of items)if(item.node.kind==="skinnedMesh"){const skinned=currentAnimation?skinMeshVertices(item.node,worldByName,item.world):item.node.vertices;skinnedMeshes++;skinnedVertices+=item.node.vertices.length/item.node.vertexStride;if(currentAnimation)for(let offset=0;offset<skinned.length;offset+=item.node.vertexStride)maxSkinnedDisplacement=Math.max(maxSkinnedDisplacement,Math.hypot(skinned[offset]-item.node.vertices[offset],skinned[offset+1]-item.node.vertices[offset+1],skinned[offset+2]-item.node.vertices[offset+2]));gl.bindBuffer(gl.ARRAY_BUFFER,item.vertex);gl.bufferSubData(gl.ARRAY_BUFFER,0,skinned);}
    sceneStatus={...sceneStatus,total:items.length,visible:items.filter((item)=>item.sceneVisible).length,hidden:items.filter((item)=>!item.sceneVisible).length};
    const center=min[0]===Infinity?[0,0,0]:min.map((value,index)=>(value+max[index])/2);bounds={center,radius:min[0]===Infinity?1:Math.hypot(...sub(max,center))};
    const animatedTracks=currentAnimation?.tracks.filter((track)=>track.animated)||[];
    animationStatus={name:currentAnimationName,position:currentAnimationPosition,version:currentAnimation?.version||0,frameCount:currentAnimation?.frameCount||0,tracks:currentAnimation?.tracks.length||0,animatedTracks:animatedTracks.length,matchedTracks:matchedTracks.size,matchedNodes,skinnedMeshes,skinnedVertices,maxSkinnedDisplacement,unmatchedTracks:animatedTracks.map((track)=>track.name).filter((name)=>!matchedTracks.has(name))};
    api.onAnimationStatus?.(api.animationStatus);
  }
  function frame(node = null) {
    clearTrackCamera();
    const item = node && items.find((candidate) => candidate.node === node);
    if (!item) { target = [...bounds.center]; distance = Math.max(bounds.radius * 2.35, .2); rebuildGrassFx();draw(); return; }
    const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity];
    for(let i=0;i<item.node.vertices.length;i+=item.node.vertexStride){const p=transformPoint(item.world,item.node.vertices[i],item.node.vertices[i+1],item.node.vertices[i+2]);for(let axis=0;axis<3;axis++){min[axis]=Math.min(min[axis],p[axis]);max[axis]=Math.max(max[axis],p[axis]);}}
    target=min.map((value,index)=>(value+max[index])/2);distance=Math.max(Math.hypot(...sub(max,target))*2.35,.02);rebuildGrassFx();draw();
  }
  function resizeHdrTarget(){if(hdrWidth===canvas.width&&hdrHeight===canvas.height)return;hdrWidth=canvas.width;hdrHeight=canvas.height;hdrMaxMip=Math.floor(Math.log2(Math.max(1,hdrWidth,hdrHeight)));const colorFormat=hdrEnabled?gl.RGBA16F:gl.RGBA8,colorType=hdrEnabled?gl.HALF_FLOAT:gl.UNSIGNED_BYTE;gl.bindTexture(gl.TEXTURE_2D,hdrTexture);gl.texImage2D(gl.TEXTURE_2D,0,colorFormat,hdrWidth,hdrHeight,0,gl.RGBA,colorType,null);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR_MIPMAP_LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);gl.bindFramebuffer(gl.FRAMEBUFFER,hdrFramebuffer);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,hdrTexture,0);gl.bindRenderbuffer(gl.RENDERBUFFER,hdrMsColor);gl.renderbufferStorageMultisample(gl.RENDERBUFFER,hdrSamples,colorFormat,hdrWidth,hdrHeight);gl.bindRenderbuffer(gl.RENDERBUFFER,hdrMsDepth);gl.renderbufferStorageMultisample(gl.RENDERBUFFER,hdrSamples,gl.DEPTH_COMPONENT24,hdrWidth,hdrHeight);gl.bindFramebuffer(gl.FRAMEBUFFER,hdrMsFramebuffer);gl.framebufferRenderbuffer(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.RENDERBUFFER,hdrMsColor);gl.framebufferRenderbuffer(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.RENDERBUFFER,hdrMsDepth);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("HDR multisample framebuffer is incomplete");gl.bindFramebuffer(gl.FRAMEBUFFER,hdrFramebuffer);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("HDR resolve framebuffer is incomplete");let width=Math.max(1,Math.ceil(hdrWidth*KS_EDITOR_GLARE.sourceScale)),height=Math.max(1,Math.ceil(hdrHeight*KS_EDITOR_GLARE.sourceScale));for(const target of bloomTargets){target.width=width;target.height=height;for(let side=0;side<2;side++){gl.bindTexture(gl.TEXTURE_2D,target.textures[side]);gl.texImage2D(gl.TEXTURE_2D,0,colorFormat,width,height,0,gl.RGBA,colorType,null);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);gl.bindFramebuffer(gl.FRAMEBUFFER,target.framebuffers[side]);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,target.textures[side],0);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("Yebis bloom framebuffer is incomplete");}width=Math.max(1,Math.ceil(width*.5));height=Math.max(1,Math.ceil(height*.5));}gl.bindFramebuffer(gl.FRAMEBUFFER,null);}
  function bindMainTextureUnits(){
    const samplers=[["diffuseTexture",0],["customEmissiveTexture",1],["customColorShapeTexture",2],["customVertexShapeTexture",3],["normalTexture",4],["mapsTexture",5],["detailTexture",6],["normalDetailTexture",7],["multiMaskTexture",8],["multiDetailRTexture",9],["multiDetailGTexture",10],["multiDetailBTexture",11],["multiDetailATexture",12],["multiDetailNormalTexture",13],["sceneColorTexture",17]];
    for(const [name,unit] of samplers)gl.uniform1i(locations[name],unit);
  }
  function bindMainMaterial(rendered,{capturePass=false}={}){
    const item=rendered.item,m=item.material,override=rendered.override,custom=override?.customEmissive,color=hashColor(m?.name||item.node.name);
    applyItemRenderState(gl,rendered.profile,rendered.transparent);if(capturePass&&rendered.profile.alphaToCoverage)gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);
    gl.uniformMatrix4fv(locations.world,false,item.world);gl.uniform3fv(locations.baseColor,color);gl.uniform1i(locations.cspTrackReceiver,itemUsesTrackReceiver(item));gl.uniform1i(locations.cspInteriorView,driverCockpitMode);
    gl.uniform3fv(locations.surfaceOverlayColor,!capturePass&&api.surfaceOverlay?surfaceOverlayColor(surfaceBindings.get(item.node)):[0,0,0]);gl.uniform1f(locations.surfaceOverlayMix,!capturePass&&api.surfaceOverlay?0.94:0);
    const rainBinding=rainFx?.nodeBindings.get(item.node);gl.uniform1i(locations.rainBits,rainBinding?.bits||0);gl.uniform1f(locations.rainWetness,api.rainVisible&&!api.surfaceOverlay?api.rainWetness:0);
    gl.uniform1i(locations.vaoEnabled,api.vaoEnabled&&item.vaoBound&&!api.surfaceOverlay);
    gl.uniform1f(locations.seasonAutumn,effectiveScalar(m,override,"seasonAutumn",0));gl.uniform1f(locations.seasonWinter,effectiveScalar(m,override,"seasonWinter",0));gl.uniform1i(locations.seasonTreeVariation,/KSTREE/i.test(override?.shader||m?.shader||""));
    gl.uniform1f(locations.ambientLevel,effectiveScalar(m,override,"ksAmbient",.35));gl.uniform1f(locations.diffuseLevel,effectiveScalar(m,override,"ksDiffuse",.8));gl.uniform1f(locations.specularLevel,effectiveScalar(m,override,"ksSpecular",.2));gl.uniform1f(locations.specularPower,effectiveScalar(m,override,"ksSpecularEXP",30));gl.uniform1f(locations.fresnelCValue,effectiveScalar(m,override,"fresnelC",0));gl.uniform1f(locations.fresnelPower,effectiveScalar(m,override,"fresnelEXP",5));gl.uniform1f(locations.fresnelLevel,effectiveScalar(m,override,"fresnelMaxLevel",.05));
    gl.uniform1f(locations.alphaRef,capturePass&&rendered.profile.alphaToCoverage?Math.max(.5,effectiveScalar(m,override,"ksAlphaRef",.5)):0);gl.uniform3fv(locations.emissiveColor,custom?[0,0,0]:effectiveEmissive(m,override));gl.uniform1i(locations.windscreenMaterial,rendered.profile.windscreen);gl.uniform1i(locations.brokenGlassMaterial,rendered.profile.brokenGlass);gl.uniform1f(locations.glassDamage,effectiveScalar(m,override,"glassDamage",1));gl.uniform1i(locations.reflectionAlphaMaterial,rendered.profile.reflectionAlpha);gl.uniform1i(locations.refractiveMaterial,rendered.profile.refractive);gl.uniform1f(locations.refractionStrength,effectiveScalar(m,override,"extRefraction",.02));gl.uniform1f(locations.refractionBlur,effectiveScalar(m,override,"pbReflectionBlurEnv",0));gl.uniform1i(locations.selected,!capturePass&&item.node===selected);
    const diffuseTexture=effectiveResourceTexture(item,override,"txdiffuse",item.texture),normalTexture=effectiveResourceTexture(item,override,"txnormal",item.normalTexture),mapsTexture=effectiveResourceTexture(item,override,"txmaps",item.mapsTexture),detailTexture=effectiveResourceTexture(item,override,"txdetail",item.detailTexture),normalDetailTexture=effectiveResourceTexture(item,override,"txnormaldetail",item.normalDetailTexture,"txdetailnm");
    for(const [unit,texture,flag] of [[0,diffuseTexture,"hasDiffuseTexture"],[4,normalTexture,"hasNormalTexture"],[5,mapsTexture,"hasMapsTexture"],[6,detailTexture,"hasDetailTexture"],[7,normalDetailTexture,"hasNormalDetailTexture"]]){gl.activeTexture(gl.TEXTURE0+unit);gl.bindTexture(gl.TEXTURE_2D,texture||null);gl.uniform1i(locations[flag],Boolean(texture));}
    gl.uniform1i(locations.normalObjectSpace,effectiveScalar(m,override,"nmObjectSpace",0)!==0);gl.uniform1f(locations.useDetail,effectiveScalar(m,override,"useDetail",0));gl.uniform1f(locations.detailUvMultiplier,effectiveScalar(m,override,"detailUVMultiplier",1));gl.uniform1f(locations.detailNormalBlend,effectiveScalar(m,override,"detailNormalBlend",1));
    const multiBindings=[effectiveResourceTexture(item,override,"txmask",item.multiMaskTexture),effectiveResourceTexture(item,override,"txdetailr",item.multiDetailRTexture),effectiveResourceTexture(item,override,"txdetailg",item.multiDetailGTexture),effectiveResourceTexture(item,override,"txdetailb",item.multiDetailBTexture),effectiveResourceTexture(item,override,"txdetaila",item.multiDetailATexture),effectiveResourceTexture(item,override,"txdetailnm",item.multiDetailNormalTexture)],multiLayer=/^KSMULTILAYER/i.test(override?.shader||m?.shader||"")&&multiBindings.slice(0,5).every(Boolean);
    for(let index=0;index<multiBindings.length;index++){gl.activeTexture(gl.TEXTURE8+index);gl.bindTexture(gl.TEXTURE_2D,multiBindings[index]||null);}gl.uniform1i(locations.hasMultiLayer,multiLayer);gl.uniform4f(locations.multiDetailMultipliers,effectiveScalar(m,override,"multR",1),effectiveScalar(m,override,"multG",1),effectiveScalar(m,override,"multB",1),effectiveScalar(m,override,"magicMult",1));gl.uniform2fv(locations.multiDetailAMultiplier,effectiveVector2(m,override,"multA",[0,0]));gl.uniform2fv(locations.multiDetailNormalMultiplier,multiBindings[5]?effectiveVector2(m,override,"detailNMMult",[0,0]):[0,0]);
    const baked=custom&&cspTextures.get(custom);gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,baked?.texture||null);gl.uniform1i(locations.hasCustomEmissive,Boolean(baked));gl.uniform1f(locations.customEmissiveStrength,baked?.strength||0);gl.uniform1i(locations.customEmissiveAlpha,Boolean(custom?.alphaFromDiffuse));
    gl.activeTexture(gl.TEXTURE2);gl.bindTexture(gl.TEXTURE_2D,baked?.colorMaskTexture||null);gl.uniform1i(locations.hasCustomColorShape,Boolean(baked?.colorMaskTexture));gl.uniform1i(locations.customColorMasksAsMultiplier,Boolean(custom?.colorMasksAsMultiplier));gl.activeTexture(gl.TEXTURE3);gl.bindTexture(gl.TEXTURE_2D,baked?.vertexMaskTexture||null);gl.uniform1i(locations.hasCustomVertexShape,Boolean(baked?.vertexMaskTexture));
    const luma=custom?.diffuseLuminance,alpha=custom?.diffuseAlpha,mirrorUv=custom?.mirrorUv;gl.uniform4f(locations.customEmissiveLuma,luma?1:0,luma?.from||0,luma?.to??1,luma?.exponent||1);gl.uniform4f(locations.customEmissiveAlphaParams,alpha?1:0,alpha?.from||0,alpha?.to??1,alpha?.exponent||1);gl.uniform1i(locations.customSkipDiffuseMap,Boolean(custom?.skipDiffuseMap));gl.uniform4f(locations.customMirrorUv,mirrorUv?1:0,mirrorUv?.offset||0,mirrorUv?.direction?.[0]||0,mirrorUv?.direction?.[1]||0);uploadCustomControls(custom,baked?.strength||1);
  }
  function cspWindVelocity(){const heading=api.windHeading*Math.PI/180,speed=Math.max(0,Number(api.windSpeed)||0);return [Math.sin(heading)*speed,Math.cos(heading)*speed];}
  function nextWindRandom(){windRandomState^=windRandomState<<13;windRandomState^=windRandomState>>>17;windRandomState^=windRandomState<<5;return (windRandomState>>>0)/0xffffffff;}
  function updateWindMap(now=performance.now()){
    if(!windLastTime){windLastTime=now;windAccumulator=1/60;}else{windAccumulator+=Math.min(.25,Math.max(0,(now-windLastTime)/1000));windLastTime=now;}
    const velocity=cspWindVelocity();let steps=0;gl.useProgram(windUpdateProgram);gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.bindVertexArray(fullscreenVao);gl.uniform1i(windUpdateLocations.previousMap,19);gl.uniform1f(windUpdateLocations.windSpeed,Math.hypot(...velocity));
    while(windAccumulator>=1/60&&steps<15){const windDelta=updateCspWindParticles(windParticles,1/60,velocity,nextWindRandom),next=1-windTargetIndex;gl.bindFramebuffer(gl.FRAMEBUFFER,windTargets[next].framebuffer);gl.viewport(0,0,CSP_WIND_MAP_SIZE,CSP_WIND_MAP_SIZE);gl.activeTexture(gl.TEXTURE19);gl.bindTexture(gl.TEXTURE_2D,windTargets[windTargetIndex].texture);gl.uniform4fv(windUpdateLocations.particles,windParticles);gl.uniform2fv(windUpdateLocations.windDelta,windDelta);gl.drawArrays(gl.TRIANGLES,0,3);windTargetIndex=next;windAccumulator-=1/60;windUpdateCount++;steps++;}
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.depthMask(true);
  }
  function bindGrassWind(targetLocations){const velocity=cspWindVelocity();gl.activeTexture(gl.TEXTURE19);gl.bindTexture(gl.TEXTURE_2D,windTargets[windTargetIndex].texture);gl.uniform1i(targetLocations.windMapTexture,19);gl.uniform2fv(targetLocations.windVelocity,velocity);}
  function scheduleWindAnimation(){if(windAnimationTimer||document.hidden||!api.grassVisible||api.surfaceOverlay||!grassStatus.instanceCount)return;windAnimationTimer=setTimeout(()=>{windAnimationTimer=0;requestAnimationFrame(()=>draw());},100);}
  function renderShadowCasterMaps(casters,shadowData,{grass=false}={}){
    gl.useProgram(shadowProgram);gl.uniform1i(shadowLocations.diffuseTexture,0);gl.colorMask(false,false,false,false);gl.depthMask(true);gl.enable(gl.DEPTH_TEST);gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);
    for(let cascadeIndex=0;cascadeIndex<shadowTargets.length;cascadeIndex++){
      const target=shadowTargets[cascadeIndex],cascade=shadowData.cascades[cascadeIndex];gl.bindFramebuffer(gl.FRAMEBUFFER,target.framebuffer);gl.viewport(0,0,KS_SHADOW_MAP_SIZE,KS_SHADOW_MAP_SIZE);gl.clear(gl.DEPTH_BUFFER_BIT);gl.uniformMatrix4fv(shadowLocations.viewProjection,false,cascade.matrix);
      for(const {item,override,profile} of casters){
        const material=item.material;if(profile.cull==="none")gl.disable(gl.CULL_FACE);else{gl.enable(gl.CULL_FACE);gl.cullFace(profile.cull==="front"?gl.FRONT:gl.BACK);}
        const alphaRef=profile.shadowAlphaTested?Math.max(0,effectiveScalar(material,override,"ksAlphaRef",.5)):0,diffuseTexture=effectiveResourceTexture(item,override,"txdiffuse",item.texture);
        gl.uniformMatrix4fv(shadowLocations.world,false,item.world);gl.uniform1f(shadowLocations.alphaRef,alphaRef);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,diffuseTexture||null);gl.uniform1i(shadowLocations.hasDiffuseTexture,Boolean(diffuseTexture));gl.bindVertexArray(item.vao);gl.drawElements(gl.TRIANGLES,item.node.indices.length,gl.UNSIGNED_SHORT,0);
      }
      if(grass){gl.useProgram(grassShadowProgram);gl.uniformMatrix4fv(grassShadowLocations.viewProjection,false,cascade.matrix);bindGrassAtlas(grassShadowLocations);bindGrassWind(grassShadowLocations);gl.disable(gl.CULL_FACE);gl.bindVertexArray(grassVao);gl.drawArraysInstanced(gl.TRIANGLES,0,6,grassStatus.instanceCount);gl.useProgram(shadowProgram);}
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.colorMask(true,true,true,true);
  }
  function cspLightFocusDistance(light,focus){const point=light.lineLight?cspLineClosestPoint(light.lineFrom,light.lineTo,focus).point:light.position;return Math.hypot(...sub(point,focus));}
  function sceneUsesTrackReceivers(){const kind=sceneModel?.workspace?.kind;return kind==="track"||(kind!=="carLods"&&bounds.radius>20);}
  function itemUsesTrackReceiver(item){return sceneUsesTrackReceivers()||item?.workspaceAuxiliary==="reflectionEnvironment";}
  function selectCspLights(source,focus,{trackReceiver=null}={}){const target=(light)=>light.lineLight?cspLineClosestPoint(light.lineFrom,light.lineTo,focus).point:light.position,occluders=cspState?.trackOccluders||[];const active=source.filter((light)=>cspLightReceiverVisible(light,{interiorView:driverCockpitMode,trackReceiver})&&Math.max(...light.color.map(Math.abs))>1e-6&&cspLightDistanceFade(cspLightFocusDistance(light,focus),light.fadeAt,light.fadeSmooth)>0&&!cspTrackOccluded(occluders,focus,target(light)));active.sort((a,b)=>cspLightFocusDistance(a,focus)-cspLightFocusDistance(b,focus));return active.slice(0,maxCspLights);}
  function filterLocalShadowAtlas(lights){
    gl.useProgram(localShadowBlurProgram);gl.uniform1i(localShadowBlurLocations.sourceTexture,0);gl.bindVertexArray(fullscreenVao);gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.disable(gl.CULL_FACE);gl.enable(gl.SCISSOR_TEST);
    for(const [framebuffer,texture,pixelStep] of [[localShadowPingFramebuffer,localShadowTexture,[1/CSP_LOCAL_SHADOW_ATLAS_SIZE,0]],[localShadowFramebuffer,localShadowPingTexture,[0,1/CSP_LOCAL_SHADOW_ATLAS_SIZE]]]){gl.bindFramebuffer(gl.FRAMEBUFFER,framebuffer);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,texture);gl.uniform2fv(localShadowBlurLocations.pixelStep,pixelStep);for(let slot=0;slot<lights.length;slot++){const x=(slot%2)*CSP_LOCAL_SHADOW_CELL_SIZE,y=Math.floor(slot/2)*CSP_LOCAL_SHADOW_CELL_SIZE,filter=cspLocalShadowFilter(lights[slot]);gl.viewport(x,y,CSP_LOCAL_SHADOW_CELL_SIZE,CSP_LOCAL_SHADOW_CELL_SIZE);gl.scissor(x,y,CSP_LOCAL_SHADOW_CELL_SIZE,CSP_LOCAL_SHADOW_CELL_SIZE);gl.uniform4f(localShadowBlurLocations.region,x/CSP_LOCAL_SHADOW_ATLAS_SIZE,y/CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_CELL_SIZE/CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_CELL_SIZE/CSP_LOCAL_SHADOW_ATLAS_SIZE);gl.uniform1i(localShadowBlurLocations.filterMode,filter.mode);gl.drawArrays(gl.TRIANGLES,0,3);}}
    gl.disable(gl.SCISSOR_TEST);gl.depthMask(true);
  }
  function renderLocalShadowAtlas(renderItems,focus){
    const casters=renderItems.filter(({item,override})=>shadowCasterEnabled(item.node,override)),lights=api.shadowsEnabled&&!api.surfaceOverlay?selectCspLights(cspState?.lights||[],focus,{trackReceiver:sceneUsesTrackReceivers()}).filter((light)=>light.castsShadows&&light.shadowSpot>0).slice(0,CSP_LOCAL_SHADOW_LIMIT):[];
    const casterNodes=casters.map(({item})=>item.node),cached=localShadowState.ready&&lights.length===localShadowState.lights.length&&lights.every((light,index)=>light===localShadowState.lights[index])&&casterNodes.length===localShadowState.casterNodes.length&&casterNodes.every((node,index)=>node===localShadowState.casterNodes[index]);grassStatus.localShadowLights=lights.length;grassStatus.localShadowCasters=lights.length?casters.length:0;grassStatus.localShadowFilterModes=lights.map((light)=>cspLocalShadowFilter(light).kernel+(cspLocalShadowFilter(light).valueAware?"-headlight":""));grassStatus.localShadowSamples=localShadowSamples;grassStatus.localShadowAtlasMode=`portable-static-${localShadowFloat?"r32f-esm":"rgba8-log-esm"}-1024-4x512-${localShadowSamples}x-resolve-separable-gaussian`;if(cached)return localShadowState;localShadowState={lights,matrices:lights.map(computeLocalLightShadow),slotByLight:new Map(lights.map((light,index)=>[light,index])),casters:casters.length,triangles:casters.reduce((sum,{item})=>sum+item.node.indices.length/3,0),casterNodes,ready:false};
    if(!lights.length)return localShadowState;
    gl.bindFramebuffer(gl.FRAMEBUFFER,localShadowSamples===CSP_LOCAL_SHADOW_SAMPLES?localShadowMsFramebuffer:localShadowFramebuffer);gl.useProgram(localShadowProgram);gl.uniform1i(localShadowLocations.diffuseTexture,0);gl.uniform1i(localShadowLocations.exponentialOutput,localShadowFloat);gl.colorMask(true,true,true,true);gl.depthMask(true);gl.enable(gl.DEPTH_TEST);gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.enable(gl.SCISSOR_TEST);
    for(let slot=0;slot<lights.length;slot++){const shadow=localShadowState.matrices[slot],x=(slot%2)*CSP_LOCAL_SHADOW_CELL_SIZE,y=Math.floor(slot/2)*CSP_LOCAL_SHADOW_CELL_SIZE;gl.viewport(x,y,CSP_LOCAL_SHADOW_CELL_SIZE,CSP_LOCAL_SHADOW_CELL_SIZE);gl.scissor(x,y,CSP_LOCAL_SHADOW_CELL_SIZE,CSP_LOCAL_SHADOW_CELL_SIZE);const emptyValue=localShadowFloat?Math.exp(shadow.expFactor):1;gl.clearColor(emptyValue,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);gl.uniformMatrix4fv(localShadowLocations.viewProjection,false,shadow.matrix);gl.uniform3fv(localShadowLocations.lightPosition,shadow.position);gl.uniform1f(localShadowLocations.lightRangeInv,shadow.rangeInv);gl.uniform1f(localShadowLocations.lightClipSphere,shadow.clipSphere);gl.uniform1f(localShadowLocations.expFactor,shadow.expFactor);for(const {item,override,profile}of casters){const material=item.material;if(profile.cull==="none")gl.disable(gl.CULL_FACE);else{gl.enable(gl.CULL_FACE);gl.cullFace(profile.cull==="front"?gl.FRONT:gl.BACK);}const alphaRef=profile.shadowAlphaTested?Math.max(0,effectiveScalar(material,override,"ksAlphaRef",.5)):0,diffuseTexture=effectiveResourceTexture(item,override,"txdiffuse",item.texture);gl.uniformMatrix4fv(localShadowLocations.world,false,item.world);gl.uniform1f(localShadowLocations.alphaRef,alphaRef);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,diffuseTexture||null);gl.uniform1i(localShadowLocations.hasDiffuseTexture,Boolean(diffuseTexture));gl.bindVertexArray(item.vao);gl.drawElements(gl.TRIANGLES,item.node.indices.length,gl.UNSIGNED_SHORT,0);}}
    if(localShadowSamples===CSP_LOCAL_SHADOW_SAMPLES){gl.disable(gl.SCISSOR_TEST);gl.bindFramebuffer(gl.READ_FRAMEBUFFER,localShadowMsFramebuffer);gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER,localShadowFramebuffer);gl.blitFramebuffer(0,0,CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_ATLAS_SIZE,0,0,CSP_LOCAL_SHADOW_ATLAS_SIZE,CSP_LOCAL_SHADOW_ATLAS_SIZE,gl.COLOR_BUFFER_BIT,gl.NEAREST);}filterLocalShadowAtlas(lights);gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.clearColor(0,0,0,1);localShadowState.ready=true;return localShadowState;
  }
  function drawGeneratedGrass(viewProjection,{eye,forward,lighting,shadowData,shadowsEnabled=false}={}){
    if(!api.grassVisible||api.surfaceOverlay||!grassStatus.instanceCount)return false;
    gl.useProgram(grassProgram);gl.uniformMatrix4fv(grassLocations.viewProjection,false,viewProjection);gl.uniform3fv(grassLocations.cameraPosition,eye||[0,0,0]);gl.uniform3fv(grassLocations.cameraForward,forward||[0,0,-1]);gl.uniform3fv(grassLocations.sunDirection,lighting?.sunDirection||[0,1,0]);gl.uniform3fv(grassLocations.sunColor,lighting?.sunColor||[1,1,1]);gl.uniform3fv(grassLocations.ambientColor,lighting?.ambientColor||[.2,.2,.2]);gl.uniform3fv(grassLocations.fogColor,lighting?.fogColor||[0,0,0]);gl.uniform1f(grassLocations.fogDistance,lighting?.fogDistance||12000);gl.uniform1f(grassLocations.fogBlend,lighting?.fogBlend||.8);gl.uniform1f(grassLocations.wetness,api.rainVisible&&!api.surfaceOverlay?api.rainWetness:0);grassStatus.localLights=uploadCspLights(cspState?.lights||[],eye||[0,0,0],grassLocations,{trackReceiver:true});bindGrassAtlas(grassLocations);bindGrassWind(grassLocations);gl.uniform1i(grassLocations.shadowsEnabled,Boolean(shadowsEnabled&&shadowData));gl.uniform3fv(grassLocations.shadowSplits,shadowData?.splits||KS_SHADOW_SPLITS);gl.uniform3fv(grassLocations.shadowBiases,KS_SHADOW_BIASES);for(let index=0;index<3;index++){if(shadowData)gl.uniformMatrix4fv(grassLocations[`shadowMatrix${index}`],false,shadowData.cascades[index].matrix);gl.activeTexture(gl.TEXTURE14+index);gl.bindTexture(gl.TEXTURE_2D,shadowTargets[index].texture);gl.uniform1i(grassLocations[`shadowMap${index}`],14+index);}gl.enable(gl.DEPTH_TEST);gl.depthMask(true);gl.disable(gl.BLEND);gl.enable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.disable(gl.CULL_FACE);gl.bindVertexArray(grassVao);gl.drawArraysInstanced(gl.TRIANGLES,0,6,grassStatus.instanceCount);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.useProgram(program);return true;
  }
  function captureReflectionCube(renderItems,eye,lighting){
    const selection=selectReflectionCaptureItems(renderItems,{explicitRoot:reflectionCaptureRoot,workspaceKind:sceneModel?.workspace?.kind||"",boundsRadius:bounds.radius,isolated:api.isolate}),captureItems=[...selection.items].sort((a,b)=>a.transparent!==b.transparent?(a.transparent?1:-1):a.layer!==b.layer?a.layer-b.layer:a.transparent?distanceSquared(b.item.center,eye)-distanceSquared(a.item.center,eye):0);if(!captureItems.length){reflectionCaptureStatus={enabled:true,ready:false,size:KS_EDITOR_CUBEMAP.size,faces:0,updatedFaces:0,draws:0,triangles:0,farPlane:KS_EDITOR_CUBEMAP.farPlane,captureMilliseconds:0,geometryIncluded:false,selectionMode:selection.mode,rootName:selection.rootName,reason:selection.reason};return;}
    const started=performance.now(),faces=reflectionCubeInitialized?[WEBGL_CUBEMAP_FACES[reflectionNextFace]]:WEBGL_CUBEMAP_FACES,opaqueItems=captureItems.filter((item)=>!item.transparent),transparentItems=captureItems.filter((item)=>item.transparent),probeShadowCasters=captureItems.filter(({item,override})=>shadowCasterEnabled(item.node,override)),probeShadowData=computeDirectionalProbeShadowCascades({eye,sunDirection:lighting.sunDirection,mapSize:KS_SHADOW_MAP_SIZE,sceneRadius:bounds.radius}),grassIncluded=selection.mode==="scene"&&api.grassVisible&&grassStatus.instanceCount>0,probeShadowsEnabled=api.shadowsEnabled&&(probeShadowCasters.length>0||grassIncluded);let draws=0,triangles=0,cspLights=0;if(probeShadowsEnabled)renderShadowCasterMaps(probeShadowCasters,probeShadowData,{grass:grassIncluded});gl.activeTexture(gl.TEXTURE18);gl.bindTexture(gl.TEXTURE_CUBE_MAP,null);
    gl.bindFramebuffer(gl.FRAMEBUFFER,reflectionCubeFramebuffer);gl.viewport(0,0,KS_EDITOR_CUBEMAP.size,KS_EDITOR_CUBEMAP.size);gl.enable(gl.DEPTH_TEST);gl.depthMask(true);gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);
    for(const face of faces){
      gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,reflectionCubeTargets[face.target],reflectionCubeTexture,0);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error(`Reflection cubemap face ${face.target} is incomplete`);gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
      const forward=face.direction,up=face.up,right=norm(cross(forward,up)),faceUp=norm(cross(right,forward)),faceTarget=eye.map((value,index)=>value+forward[index]),vp=multiply(perspective(Math.PI/2,1,KS_EDITOR_CUBEMAP.nearPlane,KS_EDITOR_CUBEMAP.farPlane),lookAt(eye,faceTarget,up));
      gl.useProgram(skyProgram);gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.uniform3fv(skyLocations.cameraForward,forward);gl.uniform3fv(skyLocations.cameraRight,right);gl.uniform3fv(skyLocations.cameraUp,faceUp);gl.uniform1f(skyLocations.tanHalfFov,1);gl.uniform1f(skyLocations.aspect,1);gl.uniform3fv(skyLocations.horizonColor,lighting.horizonColor);gl.uniform3fv(skyLocations.skyColor,lighting.skyColor);gl.uniform3fv(skyLocations.sunColor,lighting.sunColor);gl.uniform3fv(skyLocations.sunDirection,lighting.sunDirection);gl.bindVertexArray(fullscreenVao);gl.drawArrays(gl.TRIANGLES,0,3);
      gl.useProgram(program);bindMainTextureUnits();gl.enable(gl.DEPTH_TEST);gl.depthMask(true);gl.uniformMatrix4fv(locations.viewProjection,false,vp);gl.uniform3fv(locations.cameraPosition,eye);gl.uniform3fv(locations.sunDirection,lighting.sunDirection);gl.uniform3fv(locations.sunColor,lighting.sunColor);gl.uniform3fv(locations.ambientColor,lighting.ambientColor);gl.uniform3fv(locations.horizonColor,lighting.horizonColor);gl.uniform3fv(locations.skyColor,lighting.skyColor);gl.uniform3fv(locations.fogColor,lighting.fogColor);gl.uniform1f(locations.fogDistance,lighting.fogDistance);gl.uniform1f(locations.fogBlend,lighting.fogBlend);gl.uniform2f(locations.viewportSize,KS_EDITOR_CUBEMAP.size,KS_EDITOR_CUBEMAP.size);gl.uniform3fv(locations.cameraRight,right);gl.uniform3fv(locations.cameraUp,faceUp);gl.uniform1f(locations.cameraTangent,1);gl.uniform1i(locations.shadowsEnabled,probeShadowsEnabled);gl.uniform3fv(locations.cameraForward,forward);gl.uniform3fv(locations.shadowSplits,probeShadowData.splits);gl.uniform3fv(locations.shadowBiases,KS_SHADOW_BIASES);for(let index=0;index<3;index++){gl.uniformMatrix4fv(locations[`shadowMatrix${index}`],false,probeShadowData.cascades[index].matrix);gl.activeTexture(gl.TEXTURE14+index);gl.bindTexture(gl.TEXTURE_2D,shadowTargets[index].texture);gl.uniform1i(locations[`shadowMap${index}`],14+index);}gl.uniform1i(locations.sceneColorReady,0);gl.uniform1i(locations.reflectionCubeReady,0);gl.uniform1i(locations.reflectionCapturePass,1);cspLights=uploadCspLights(cspState?.lights||[],eye);
      for(const rendered of opaqueItems){bindMainMaterial(rendered,{capturePass:true});gl.bindVertexArray(rendered.item.vao);gl.drawElements(gl.TRIANGLES,rendered.item.node.indices.length,gl.UNSIGNED_SHORT,0);draws++;triangles+=rendered.item.node.indices.length/3;}
      if(grassIncluded&&drawGeneratedGrass(vp,{eye,forward,lighting,shadowData:probeShadowData,shadowsEnabled:probeShadowsEnabled})){draws++;triangles+=grassStatus.instanceCount*2;}
      for(const rendered of transparentItems){bindMainMaterial(rendered,{capturePass:true});gl.bindVertexArray(rendered.item.vao);gl.drawElements(gl.TRIANGLES,rendered.item.node.indices.length,gl.UNSIGNED_SHORT,0);draws++;triangles+=rendered.item.node.indices.length/3;}
    }
    reflectionCubeInitialized=true;reflectionNextFace=(reflectionNextFace+faces.length)%WEBGL_CUBEMAP_FACES.length;gl.activeTexture(gl.TEXTURE18);gl.bindTexture(gl.TEXTURE_CUBE_MAP,reflectionCubeTexture);gl.generateMipmap(gl.TEXTURE_CUBE_MAP);gl.uniform1i(locations.reflectionCapturePass,0);gl.bindFramebuffer(gl.FRAMEBUFFER,null);reflectionCaptureStatus={enabled:true,ready:true,size:KS_EDITOR_CUBEMAP.size,faces:WEBGL_CUBEMAP_FACES.length,updatedFaces:faces.length,nextFace:reflectionNextFace,draws,triangles,opaqueMeshes:opaqueItems.length,transparentMeshes:transparentItems.length,cspLights,materialPath:"Shared viewport shader",directionalShadows:probeShadowsEnabled,shadowCasters:probeShadowCasters.length,grassFx:grassIncluded,grassInstances:grassIncluded?grassStatus.instanceCount:0,grassCastsShadows:grassIncluded&&probeShadowsEnabled,grassReceivesShadows:grassIncluded&&probeShadowsEnabled,farPlane:KS_EDITOR_CUBEMAP.farPlane,captureMilliseconds:performance.now()-started,geometryIncluded:true,selectionMode:selection.mode,rootName:selection.rootName,reason:""};
  }
  function resolveOpaqueSceneColor(){gl.bindFramebuffer(gl.READ_FRAMEBUFFER,hdrMsFramebuffer);gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER,hdrFramebuffer);gl.blitFramebuffer(0,0,hdrWidth,hdrHeight,0,0,hdrWidth,hdrHeight,gl.COLOR_BUFFER_BIT,gl.NEAREST);gl.activeTexture(gl.TEXTURE17);gl.bindTexture(gl.TEXTURE_2D,hdrTexture);gl.generateMipmap(gl.TEXTURE_2D);gl.bindFramebuffer(gl.FRAMEBUFFER,hdrMsFramebuffer);gl.viewport(0,0,canvas.width,canvas.height);gl.useProgram(program);}
  function generateBloom(effectiveExposure){if(!hdrEnabled||api.surfaceOverlay)return false;gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.bindVertexArray(fullscreenVao);const first=bloomTargets[0];gl.bindFramebuffer(gl.FRAMEBUFFER,first.framebuffers[0]);gl.viewport(0,0,first.width,first.height);gl.useProgram(brightPassProgram);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,hdrTexture);gl.uniform1i(brightPassLocations.sourceTexture,0);gl.uniform1f(brightPassLocations.exposure,effectiveExposure);gl.uniform1f(brightPassLocations.threshold,KS_EDITOR_GLARE.threshold);gl.uniform1f(brightPassLocations.remap,KS_EDITOR_GLARE.brightPassRemap);gl.drawArrays(gl.TRIANGLES,0,3);for(let level=0;level<bloomTargets.length;level++){const target=bloomTargets[level];if(level){const previous=bloomTargets[level-1];gl.bindFramebuffer(gl.FRAMEBUFFER,target.framebuffers[0]);gl.viewport(0,0,target.width,target.height);gl.useProgram(bloomDownsampleProgram);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,previous.textures[0]);gl.uniform1i(bloomDownsampleSource,0);gl.drawArrays(gl.TRIANGLES,0,3);}const kernel=bloomKernels[level];gl.useProgram(bloomBlurProgram);gl.uniform1i(bloomBlurLocations.sourceTexture,0);gl.uniform1i(bloomBlurLocations.sampleCount,kernel.sampleCount);gl.uniform1fv(bloomBlurLocations.sampleOffsets,kernel.offsets);gl.uniform1fv(bloomBlurLocations.sampleWeights,kernel.weights);gl.bindFramebuffer(gl.FRAMEBUFFER,target.framebuffers[1]);gl.viewport(0,0,target.width,target.height);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,target.textures[0]);gl.uniform2f(bloomBlurLocations.direction,1,0);gl.drawArrays(gl.TRIANGLES,0,3);gl.bindFramebuffer(gl.FRAMEBUFFER,target.framebuffers[0]);gl.bindTexture(gl.TEXTURE_2D,target.textures[1]);gl.uniform2f(bloomBlurLocations.direction,0,1);gl.drawArrays(gl.TRIANGLES,0,3);}return true;}
  function resolveExposureAndPresent(){gl.bindFramebuffer(gl.READ_FRAMEBUFFER,hdrMsFramebuffer);gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER,hdrFramebuffer);gl.blitFramebuffer(0,0,hdrWidth,hdrHeight,0,0,hdrWidth,hdrHeight,gl.COLOR_BUFFER_BIT,gl.NEAREST);gl.bindTexture(gl.TEXTURE_2D,hdrTexture);gl.generateMipmap(gl.TEXTURE_2D);let measured=[.32,.32,.32];gl.bindFramebuffer(gl.FRAMEBUFFER,exposureFramebuffer);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,hdrTexture,hdrMaxMip);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)===gl.FRAMEBUFFER_COMPLETE){if(hdrEnabled){const pixel=new Float32Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.FLOAT,pixel);measured=pixel;}else{const pixel=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);measured=[pixel[0]/255,pixel[1]/255,pixel[2]/255];}}gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,null,0);const luminance=Math.max(.0001,measured[0]*.2126+measured[1]*.7152+measured[2]*.0722),effectiveExposure=api.autoExposure?ksEditorAutoExposure(luminance):api.exposure,glareEnabled=generateBloom(effectiveExposure);lightingStatus={...lightingStatus,autoExposure:api.autoExposure,effectiveExposure,measuredLuminance:luminance,glareEnabled,glareQuality:KS_EDITOR_GLARE.quality,bloomLevels:glareEnabled?KS_EDITOR_GLARE.levels:0,bloomSourceScale:KS_EDITOR_GLARE.sourceScale,bloomThreshold:KS_EDITOR_GLARE.threshold,bloomCompositeScale:ksEditorBloomCompositeScale(),dither:true};gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.viewport(0,0,canvas.width,canvas.height);gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.useProgram(postProgram);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,hdrTexture);gl.uniform1i(postLocations.hdrTexture,0);for(let level=0;level<bloomTargets.length;level++){gl.activeTexture(gl.TEXTURE1+level);gl.bindTexture(gl.TEXTURE_2D,bloomTargets[level].textures[0]);gl.uniform1i(postLocations[`bloom${level}`],1+level);}gl.activeTexture(gl.TEXTURE6);gl.bindTexture(gl.TEXTURE_2D,ditherTexture);gl.uniform1i(postLocations.ditherTexture,6);gl.uniform1f(postLocations.exposure,effectiveExposure);gl.uniform1f(postLocations.gamma,KS_EDITOR_EXPOSURE.gamma);gl.uniform1f(postLocations.saturation,KS_EDITOR_EXPOSURE.saturation);gl.uniform1f(postLocations.curveScale,KS_EDITOR_TONEMAP.curveScale);gl.uniform1f(postLocations.curveShoulder,KS_EDITOR_TONEMAP.curveShoulder);gl.uniform1f(postLocations.bloomScale,ksEditorBloomCompositeScale());gl.uniform1f(postLocations.ditherScale,KS_EDITOR_GLARE.ditherScale);gl.uniform1f(postLocations.ditherOffset,KS_EDITOR_GLARE.ditherOffset);gl.uniform1i(postLocations.glareEnabled,glareEnabled);gl.uniform1i(postLocations.diagnosticMode,api.surfaceOverlay);gl.bindVertexArray(fullscreenVao);gl.drawArrays(gl.TRIANGLES,0,3);}
  function renderShadowMaps(renderItems,eye,viewTarget,viewUp,viewFov,viewNear,viewFar,lighting){
    const casters=renderItems.filter(({item,override})=>shadowCasterEnabled(item.node,override)),grassCaster=api.grassVisible&&!api.surfaceOverlay&&grassStatus.instanceCount>0,triangles=casters.reduce((sum,{item})=>sum+item.node.indices.length/3,0)+(grassCaster?grassStatus.instanceCount*2:0),enabled=api.shadowsEnabled&&!api.surfaceOverlay;
    shadowStatus={enabled,mapSize:KS_SHADOW_MAP_SIZE,cascades:3,splits:[...KS_SHADOW_SPLITS],biases:[...KS_SHADOW_BIASES],casters:casters.length,grassInstances:grassCaster?grassStatus.instanceCount:0,triangles};
    const shadowData=computeDirectionalShadowCascades({eye,target:viewTarget,up:viewUp,fovRadians:viewFov*Math.PI/180,aspect:canvas.width/canvas.height,near:viewNear,far:viewFar,sunDirection:lighting.sunDirection,mapSize:KS_SHADOW_MAP_SIZE,sceneRadius:bounds.radius});
    if(!enabled||(!casters.length&&!grassCaster))return shadowData;
    renderShadowCasterMaps(casters,shadowData,{grass:grassCaster});return shadowData;
  }
  function draw() {
    resize();resizeHdrTarget();updateWindMap();gl.viewport(0,0,canvas.width,canvas.height); gl.depthMask(true);gl.enable(gl.DEPTH_TEST);gl.disable(gl.BLEND);gl.frontFace(gl.CCW); gl.clearColor(.055,.065,.073,1); gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT); gl.useProgram(program);
    const eye = cameraEye(),viewFov=trackCamera?Math.max(1,Math.min(160,trackCamera.splineData?trackCamera.minFov:(trackCamera.minFov+trackCamera.maxFov)/2)):cameraFovDegrees,viewTarget=trackCamera?trackCamera.position.map((value,index)=>value+trackCamera.forward[index]):target,viewUp=trackCamera?trackCamera.up:[0,1,0],viewNear=trackCamera?trackCamera.nearPlane:Math.max(.01,distance/10000),viewFar=trackCamera?trackCamera.farPlane:Math.max(100,distance*10);
    const vp = multiply(perspective(viewFov*Math.PI/180, canvas.width/canvas.height, viewNear, viewFar), lookAt(eye,viewTarget,viewUp)),sunDirection=sunDirectionFromAngles(api.sunHeading,api.sunHeight),lighting=evaluateKsLighting(api.weatherPreset,sunDirection);
    lightingStatus={...lightingStatus,name:lighting.preset.name,source:lighting.preset.source,heading:api.sunHeading,height:api.sunHeight,angleMix:lighting.angleMix,sunColor:[...lighting.sunColor],ambientColor:[...lighting.ambientColor],skyColor:[...lighting.skyColor],fogDistance:lighting.fogDistance,fogBlend:lighting.fogBlend,autoExposure:api.autoExposure,exposureMin:KS_EDITOR_EXPOSURE.min,exposureMax:KS_EDITOR_EXPOSURE.max,hdr:hdrEnabled,samples:hdrSamples};
    gl.uniformMatrix4fv(locations.viewProjection,false,vp);gl.uniform3fv(locations.cameraPosition,eye);bindMainTextureUnits();
    const effectiveCarLodDistance=carLodDistance(Math.sqrt(distanceSquared(bounds.center,eye)),cameraFovDegrees),renderItems=items.filter((item)=>api.isolate?item.node===selected:api.surfaceOverlay?item.branchActive&&surfaceBindings.get(item.node)?.status!=="not-physics":itemPreviewVisible(item)).map((item)=>{const override=cspState?.nodeOverrides.get(item.node),profile=resolveMaterialRenderProfile(item.material,item.node,override),meshDistance=Math.sqrt(distanceSquared(item.center,eye)),lodIn=override?.lodIn??item.node.lodIn??0,lodOut=override?.lodOut??item.node.lodOut??0;return {item,override,profile,meshDistance,lodIn,lodOut,layer:override?.layer??item.node.layer??0,transparent:api.surfaceOverlay?false:profile.transparent};}).filter((rendered)=>api.surfaceOverlay||((api.isolate||carLodVisible(rendered.item.workspaceLod,effectiveCarLodDistance,workspaceLodIndex))&&rendered.meshDistance>=rendered.lodIn&&(rendered.lodOut<=0||rendered.meshDistance<=rendered.lodOut)));
    api.onWorkspaceLodStatus?.(api.workspaceLodStatus);
    renderLocalShadowAtlas(renderItems,target);
    if(!api.surfaceOverlay&&api.reflectionsEnabled)captureReflectionCube(renderItems,eye,lighting);else reflectionCaptureStatus={...reflectionCaptureStatus,enabled:false,ready:false};sceneStatus.reflections={...reflectionCaptureStatus};
    const shadowData=renderShadowMaps(renderItems,eye,viewTarget,viewUp,viewFov,viewNear,viewFar,lighting);gl.bindFramebuffer(gl.FRAMEBUFFER,hdrMsFramebuffer);gl.viewport(0,0,canvas.width,canvas.height);gl.colorMask(true,true,true,true);gl.depthMask(true);gl.enable(gl.DEPTH_TEST);gl.disable(gl.BLEND);gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
    const skyForward=norm(sub(viewTarget,eye)),skyRight=norm(cross(skyForward,viewUp)),skyUp=norm(cross(skyRight,skyForward));gl.useProgram(skyProgram);gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.uniform3fv(skyLocations.cameraForward,skyForward);gl.uniform3fv(skyLocations.cameraRight,skyRight);gl.uniform3fv(skyLocations.cameraUp,skyUp);gl.uniform1f(skyLocations.tanHalfFov,Math.tan(viewFov*Math.PI/360));gl.uniform1f(skyLocations.aspect,canvas.width/canvas.height);gl.uniform3fv(skyLocations.horizonColor,lighting.horizonColor);gl.uniform3fv(skyLocations.skyColor,lighting.skyColor);gl.uniform3fv(skyLocations.sunColor,lighting.sunColor);gl.uniform3fv(skyLocations.sunDirection,lighting.sunDirection);gl.bindVertexArray(fullscreenVao);gl.drawArrays(gl.TRIANGLES,0,3);gl.depthMask(true);gl.enable(gl.DEPTH_TEST);gl.useProgram(program);
    // Reflection capture uses this program and leaves its cube-face camera active.
    gl.uniformMatrix4fv(locations.viewProjection,false,vp);
    gl.uniform1i(locations.shadowsEnabled,api.shadowsEnabled&&!api.surfaceOverlay&&shadowStatus.casters>0);gl.uniform3fv(locations.cameraForward,shadowData.forward);gl.uniform3fv(locations.shadowSplits,shadowData.splits);gl.uniform3fv(locations.shadowBiases,KS_SHADOW_BIASES);for(let index=0;index<3;index++){gl.uniformMatrix4fv(locations[`shadowMatrix${index}`],false,shadowData.cascades[index].matrix);gl.activeTexture(gl.TEXTURE14+index);gl.bindTexture(gl.TEXTURE_2D,shadowTargets[index].texture);gl.uniform1i(locations[`shadowMap${index}`],14+index);}
    gl.uniform3fv(locations.sunDirection,lighting.sunDirection);gl.uniform3fv(locations.sunColor,lighting.sunColor);gl.uniform3fv(locations.ambientColor,lighting.ambientColor);gl.uniform3fv(locations.horizonColor,lighting.horizonColor);gl.uniform3fv(locations.skyColor,lighting.skyColor);gl.uniform3fv(locations.fogColor,lighting.fogColor);gl.uniform1f(locations.fogDistance,lighting.fogDistance);gl.uniform1f(locations.fogBlend,lighting.fogBlend);gl.uniform2f(locations.viewportSize,canvas.width,canvas.height);gl.uniform3fv(locations.cameraRight,skyRight);gl.uniform3fv(locations.cameraUp,skyUp);gl.uniform1f(locations.cameraTangent,Math.tan(viewFov*Math.PI/360));gl.activeTexture(gl.TEXTURE18);gl.bindTexture(gl.TEXTURE_CUBE_MAP,reflectionCubeTexture);gl.uniform1i(locations.reflectionCubeTexture,18);gl.uniform1i(locations.reflectionCubeReady,reflectionCaptureStatus.enabled&&reflectionCaptureStatus.ready);gl.uniform1i(locations.sceneColorReady,0);gl.uniform1i(locations.reflectionCapturePass,0);uploadCspLights(cspState?.lights||[],target);
    sceneStatus.glass={windscreenDraws:renderItems.filter(({profile})=>profile.windscreen).length,reflectionDraws:renderItems.filter(({profile})=>profile.reflectionAlpha&&!profile.brokenGlass).length,brokenGlassDraws:renderItems.filter(({profile})=>profile.brokenGlass).length,refractiveDraws:renderItems.filter(({profile})=>profile.refractive).length,opaqueSceneResolved:false};
    renderItems.sort((a,b)=>a.transparent!==b.transparent?(a.transparent?1:-1):a.layer!==b.layer?a.layer-b.layer:a.transparent?distanceSquared(b.item.center,eye)-distanceSquared(a.item.center,eye):0);
    const grassDrawOptions={eye,forward:shadowData.forward,lighting,shadowData,shadowsEnabled:api.shadowsEnabled&&!api.surfaceOverlay&&(shadowStatus.casters>0||shadowStatus.grassInstances>0)};let sceneColorResolved=false,grassRendered=false;
    for (const rendered of renderItems) {
      const item=rendered.item;
      if(rendered.transparent&&!sceneColorResolved){grassRendered=drawGeneratedGrass(vp,grassDrawOptions);resolveOpaqueSceneColor();sceneColorResolved=true;sceneStatus.glass.opaqueSceneResolved=true;gl.activeTexture(gl.TEXTURE17);gl.bindTexture(gl.TEXTURE_2D,hdrTexture);gl.uniform1i(locations.sceneColorReady,1);}
      bindMainMaterial(rendered);
      gl.bindVertexArray(item.vao); gl.drawElements(api.wireframe?gl.LINES:gl.TRIANGLES,item.node.indices.length,gl.UNSIGNED_SHORT,0);
    }
    if(!grassRendered)drawGeneratedGrass(vp,grassDrawOptions);
    if(api.rainVisible&&!api.surfaceOverlay&&api.rainWetness>0&&(rainStatus.lineVertices||rainStatus.pointVertices)){gl.useProgram(rainStreamProgram);gl.uniformMatrix4fv(rainStreamLocations.viewProjection,false,vp);gl.uniform1f(rainStreamLocations.pointSize,7);gl.uniform4f(rainStreamLocations.color,.1,.72,1,.9);gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);gl.bindVertexArray(rainStreamVao);if(rainStatus.lineVertices)gl.drawArrays(gl.LINES,0,rainStatus.lineVertices);if(rainStatus.pointVertices)gl.drawArrays(gl.POINTS,rainStatus.lineVertices,rainStatus.pointVertices);gl.useProgram(program);}
    if(api.colliderVisible&&colliderItems.length){
      gl.disable(gl.BLEND);gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);gl.disable(gl.CULL_FACE);gl.disable(gl.DEPTH_TEST);gl.depthMask(false);gl.uniform1i(locations.shadowsEnabled,0);gl.uniform1i(locations.vaoEnabled,0);gl.uniform1i(locations.cspLightCount,0);gl.uniform1i(locations.rainBits,0);gl.uniform1f(locations.rainWetness,0);gl.uniform1f(locations.seasonAutumn,0);gl.uniform1f(locations.seasonWinter,0);gl.uniform1i(locations.seasonTreeVariation,0);gl.uniform3f(locations.baseColor,1,.18,.015);gl.uniform3f(locations.surfaceOverlayColor,0,0,0);gl.uniform1f(locations.surfaceOverlayMix,0);gl.uniform1f(locations.ambientLevel,1);gl.uniform1f(locations.diffuseLevel,.15);gl.uniform1f(locations.specularLevel,0);gl.uniform1f(locations.specularPower,1);gl.uniform1f(locations.fresnelCValue,0);gl.uniform1f(locations.fresnelPower,1);gl.uniform1f(locations.fresnelLevel,0);gl.uniform1f(locations.alphaRef,0);gl.uniform3f(locations.emissiveColor,.85,.08,0);gl.uniform1i(locations.windscreenMaterial,0);gl.uniform1i(locations.brokenGlassMaterial,0);gl.uniform1f(locations.glassDamage,0);gl.uniform1i(locations.reflectionAlphaMaterial,0);gl.uniform1i(locations.refractiveMaterial,0);gl.uniform1i(locations.selected,0);gl.uniform1i(locations.hasDiffuseTexture,0);gl.uniform1i(locations.hasNormalTexture,0);gl.uniform1i(locations.hasMapsTexture,0);gl.uniform1i(locations.hasDetailTexture,0);gl.uniform1i(locations.hasNormalDetailTexture,0);gl.uniform1i(locations.hasMultiLayer,0);gl.uniform1i(locations.hasCustomEmissive,0);gl.uniform1i(locations.hasCustomColorShape,0);gl.uniform1i(locations.hasCustomVertexShape,0);gl.uniform1i(locations.customColorMaskCount,0);gl.uniform1i(locations.customEmissiveAlpha,0);gl.uniform4f(locations.customEmissiveLuma,0,0,1,1);gl.uniform4f(locations.customEmissiveAlphaParams,0,0,1,1);gl.uniform1i(locations.customSkipDiffuseMap,0);gl.uniform4f(locations.customMirrorUv,0,0,0,0);gl.uniform3f(locations.customBounceColor,0,0,0);
      for(const item of colliderItems){gl.uniformMatrix4fv(locations.world,false,item.world);gl.bindVertexArray(item.vao);gl.drawElements(gl.LINES,item.count,gl.UNSIGNED_SHORT,0);}
    }
    gl.depthMask(true);gl.disable(gl.BLEND);resolveExposureAndPresent();scheduleWindAnimation();
  }
  function uploadCspLights(source, focus, targetLocations=locations, selectionOptions={}) {
    const lights = selectCspLights(source, focus, selectionOptions);
    if(targetLocations===locations)lightingStatus={...lightingStatus,localLights:lights.length,localLightCandidates:source.length,viewMode:driverCockpitMode?"interior":"exterior"};
    gl.uniform1i(targetLocations.cspLightCount, lights.length);
    const shadowSlots=new Int32Array(maxCspLights);shadowSlots.fill(-1);for(let index=0;index<lights.length;index++)shadowSlots[index]=localShadowState.slotByLight.get(lights[index])??-1;gl.activeTexture(gl.TEXTURE20);gl.bindTexture(gl.TEXTURE_2D,localShadowTexture);gl.uniform1i(targetLocations.cspLocalShadowAtlas,20);gl.uniform1i(targetLocations.cspLocalShadowFloat,localShadowFloat);gl.uniform1iv(targetLocations.cspLightShadowSlot,shadowSlots);const shadowMatrices=[],shadowParams=[],shadowPositions=[];for(let slot=0;slot<CSP_LOCAL_SHADOW_LIMIT;slot++){const shadow=localShadowState.matrices[slot];shadowMatrices.push(...(shadow?.matrix||identity()));shadowParams.push(shadow?.biasMult||0,shadow?.rangeInvExpFactor||0,shadow?.maxRange||1,shadow?.boost||1);shadowPositions.push(...(shadow?.position||[0,0,0]),shadow?.extraBlur?1:0);}gl.uniformMatrix4fv(targetLocations.cspLocalShadowMatrix,false,new Float32Array(shadowMatrices));gl.uniform4fv(targetLocations.cspLocalShadowParams,new Float32Array(shadowParams));gl.uniform4fv(targetLocations.cspLocalShadowPosition,new Float32Array(shadowPositions));
    if (!lights.length) return 0;
    const positionRange = [], colorSpot = [], directionCone = [], falloff = [], lineVector = [], lineColor = [], secondCone = [], secondRange = [], spotEdge = [], spotUp = [];
    for (const light of lights) {
      const spotCone = cspSpotConePacking(light.direction, light.spot, light.spotSharpness);
      const secondary = cspSpotConePacking(light.direction, light.secondSpot, light.secondSpotSharpness);
      const secondaryRange = cspSecondarySpotPacking(light.secondSpotRange, light.secondSpotSkip);
      const secondaryEnabled = Boolean(light.hasSecondSpot && spotCone.enabled && secondary.enabled && secondaryRange.enabled);
      const edge = cspSpotEdgePacking(light.spotUp, light.spotEdge, light.spotEdgeSharpness);
      const edgeEnabled = Boolean(light.hasSpotEdge && spotCone.enabled && edge.enabled);
      const line=Boolean(light.lineLight),baseColor=line?(light.lineColorFrom||light.color):light.color,endColor=line?(light.lineColorTo||baseColor):baseColor;
      positionRange.push(...(line?light.lineFrom:light.position), light.range);
      const cameraFade=cspLightDistanceFade(cspLightFocusDistance(light,focus),light.fadeAt,light.fadeSmooth);
      colorSpot.push(...baseColor.map((value)=>value*cameraFade), spotCone.start);
      directionCone.push(...spotCone.direction, spotCone.enabled?1:0);
      falloff.push(light.rangeGradientOffset, light.specularMult, light.diffuseConcentration, line?1:0);
      lineVector.push(...(line?light.lineVector:[0,0,0]),line?light.lineDistanceInverse:0);
      const trackMode=light.affectsTrackMode==="none"?1:light.affectsTrackMode==="interior-only"?2:0;
      lineColor.push(...endColor.map((value,index)=>(value-baseColor[index])*cameraFade),trackMode);
      secondCone.push(secondary.start, secondaryEnabled?secondary.inverseWidth/spotCone.inverseWidth:0, 0, secondaryEnabled?1:0);
      secondRange.push(secondaryEnabled?secondaryRange.rangeInverse:0, secondaryEnabled?secondaryRange.trimStart:0, secondaryEnabled?secondaryRange.trimLengthInverse:0, secondaryEnabled?light.secondSpotIntensity:0);
      spotEdge.push(...(edgeEnabled?edge.offsets:[1,1,1]), edgeEnabled?1:0);
      spotUp.push(...(edgeEnabled?edge.up:[0,0,0]), 0);
    }
    gl.uniform4fv(targetLocations.cspLightPositionRange, positionRange);
    gl.uniform4fv(targetLocations.cspLightColorSpot, colorSpot);
    gl.uniform4fv(targetLocations.cspLightDirectionCone, directionCone);
    gl.uniform4fv(targetLocations.cspLightFalloff, falloff);
    gl.uniform4fv(targetLocations.cspLightLineVector, lineVector);
    gl.uniform4fv(targetLocations.cspLightLineColor, lineColor);
    gl.uniform4fv(targetLocations.cspLightSecondCone, secondCone);
    gl.uniform4fv(targetLocations.cspLightSecondRange, secondRange);
    gl.uniform4fv(targetLocations.cspLightSpotEdge, spotEdge);
    gl.uniform4fv(targetLocations.cspLightSpotUp, spotUp);
    return lights.length;
  }
  function uploadCustomControls(custom, strength) {
    const masks=(custom?.colorMasks || []).slice(0,maxCustomColorMasks),colors=new Map(custom?.channelColors || []),targets=[],params=[],emissions=[],sides=[],baseChannels=[];
    for(const mask of masks){const color=colors.get(mask.channel)||[0,0,0];targets.push(...mask.color,mask.normalization);params.push(mask.thresholdLevel,mask.thresholdSharpness,mask.alphaMinMax?.[0]||0,mask.alphaMinMax?.[1]||0);emissions.push((color[0]||0)/strength,(color[1]||0)/strength,(color[2]||0)/strength,mask.opacity);sides.push(...(custom.mirrorDirection||[1,0,0]),mask.mirrorSide||0);baseChannels.push(baseChannelForPreview(mask.channel));}
    gl.uniform1i(locations.customColorMaskCount,masks.length);gl.uniform3fv(locations.customMirrorDirection,custom?.mirrorDirection||[1,0,0]);gl.uniform1f(locations.customMirrorOffset,custom?.mirrorOffset||0);
    if(masks.length){gl.uniform4fv(locations.customColorMaskTarget,targets);gl.uniform4fv(locations.customColorMaskParams,params);gl.uniform4fv(locations.customColorMaskEmission,emissions);gl.uniform4fv(locations.customColorMaskSide,sides);gl.uniform1fv(locations.customColorMaskBaseChannel,baseChannels);}
    const channelColors=new Map(custom?.channelColors||[]),anchors=[],vertexColors=[],mirroredColors=[],colorMaskBases=new Set((custom?.colorMasks||[]).map((mask)=>baseChannelForPreview(mask.channel)));
    for(let channel=0;channel<4;channel++){const point=custom?.vertexMask?.points?.[channel],rawColor=channelColors.get(channel)||[0,0,0],rawMirrored=channelColors.get(mirroredChannelForPreview(channel))||rawColor,color=colorMaskBases.has(channel)?[0,0,0]:rawColor,mirrored=colorMaskBases.has(channel)?[0,0,0]:rawMirrored;anchors.push(...(point||[0,0,0]),point?1:0);vertexColors.push((color[0]||0)/strength,(color[1]||0)/strength,(color[2]||0)/strength,0);mirroredColors.push((mirrored[0]||0)/strength,(mirrored[1]||0)/strength,(mirrored[2]||0)/strength,0);}
    gl.uniform4fv(locations.customVertexAnchor,anchors);gl.uniform4fv(locations.customVertexEmission,vertexColors);gl.uniform4fv(locations.customVertexMirroredEmission,mirroredColors);
    const bounce=[0,0,0];for(const rule of custom?.bounceBack||[])for(let channel=0;channel<4;channel++){const color=channelColors.get(channel)||[0,0,0],weight=(rule.mask[channel]||0)*rule.intensity*.015;for(let component=0;component<3;component++)bounce[component]+=(color[component]||0)*weight;}gl.uniform3fv(locations.customBounceColor,bounce);
  }
  function effectiveResourceTexture(item,override,slot,original,alternate=""){
    const skinOriginal=skinTextures.get(item.resourceNames?.[slot]||alternate&&item.resourceNames?.[alternate])||original,resource=override?.resources?.get(slot)||alternate&&override?.resources?.get(alternate);if(!resource)return skinOriginal;
    if(Array.isArray(resource.color)){const normalized=[0,1,2,3].map((index)=>Math.max(0,Math.min(1,Number(resource.color[index]??(index===3?1:0))||0))),key=normalized.join(",");if(!solidTextures.has(key)){const texture=uploadRgbaTexture(gl,new Uint8Array(normalized.map((value)=>Math.round(value*255))),1,1);solidTextures.set(key,texture);textures.push(texture);}return solidTextures.get(key);}
    if(resource.file)return externalTextures.get(normalizeAssetPath(resource.file).toLowerCase())||skinOriginal;
    const name=String(resource.texture||"").toLowerCase(),basename=normalizeAssetPath(name).split("/").at(-1),scope=String(item.material?.workspaceFile||"").toLowerCase();return skinTextures.get(basename)||textureLookup.get(`${scope}\0${name}`)||textureLookup.get(name)||skinOriginal;
  }
  function resize(){const d=Math.min(devicePixelRatio,2),w=Math.floor(canvas.clientWidth*d),h=Math.floor(canvas.clientHeight*d);if(canvas.width!==w||canvas.height!==h){canvas.width=w;canvas.height=h;}}
  canvas.addEventListener("pointerdown",e=>{clearTrackCamera();dragging={x:e.clientX,y:e.clientY,shift:e.shiftKey};canvas.setPointerCapture(e.pointerId);});
  canvas.addEventListener("pointermove",e=>{if(!dragging)return;const dx=e.clientX-dragging.x,dy=e.clientY-dragging.y;dragging.x=e.clientX;dragging.y=e.clientY;if(dragging.shift){const s=distance*.0015;target[0]-=dx*s*Math.cos(yaw);target[2]+=dx*s*Math.sin(yaw);target[1]+=dy*s;}else{yaw-=dx*.006;pitch=Math.max(-1.5,Math.min(1.5,pitch-dy*.006));}draw();});
  canvas.addEventListener("pointerup",()=>{dragging=null;scheduleGrassRebuild();}); canvas.addEventListener("wheel",e=>{e.preventDefault();clearTrackCamera();distance*=Math.exp(e.deltaY*.001);distance=Math.max(.02,Math.min(1e7,distance));scheduleGrassRebuild();draw();},{passive:false}); window.addEventListener("resize",()=>{scheduleGrassRebuild();draw();});
  return api;
}

function compile(gl,type,source){const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(s));return s;}
function link(gl,vs,fs){const p=gl.createProgram();gl.attachShader(p,compile(gl,gl.VERTEX_SHADER,vs));gl.attachShader(p,compile(gl,gl.FRAGMENT_SHADER,fs));gl.linkProgram(p);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(p));return p;}
function identity(){return [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];}
function multiply(a,b){const o=new Array(16);for(let c=0;c<4;c++)for(let r=0;r<4;r++){o[c*4+r]=0;for(let k=0;k<4;k++)o[c*4+r]+=a[k*4+r]*b[c*4+k];}return o;}
function transformPoint(m,x,y,z){return [m[0]*x+m[4]*y+m[8]*z+m[12],m[1]*x+m[5]*y+m[9]*z+m[13],m[2]*x+m[6]*y+m[10]*z+m[14]];}
function transformBounds(localMin,localMax,m){const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity];for(const x of [localMin[0],localMax[0]])for(const y of [localMin[1],localMax[1]])for(const z of [localMin[2],localMax[2]]){const point=transformPoint(m,x,y,z);for(let axis=0;axis<3;axis++){min[axis]=Math.min(min[axis],point[axis]);max[axis]=Math.max(max[axis],point[axis]);}}return {min,max};}
function perspective(fov,aspect,near,far){const f=1/Math.tan(fov/2),nf=1/(near-far);return [f/aspect,0,0,0,0,f,0,0,0,0,(far+near)*nf,-1,0,0,2*far*near*nf,0];}
function lookAt(e,t,u){let z=norm(sub(e,t)),x=norm(cross(u,z)),y=cross(z,x);return [x[0],y[0],z[0],0,x[1],y[1],z[1],0,x[2],y[2],z[2],0,-dot(x,e),-dot(y,e),-dot(z,e),1];}
function sub(a,b){return a.map((v,i)=>v-b[i]);} function dot(a,b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];} function cross(a,b){return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];} function norm(a){const l=Math.hypot(...a)||1;return a.map(v=>v/l);}
function hashColor(name){let h=2166136261;for(const c of name)h=Math.imul(h^c.charCodeAt(0),16777619);return [.28+((h>>>0)&255)/900,.28+((h>>>8)&255)/900,.28+((h>>>16)&255)/900];}
function distanceSquared(a,b){const x=a[0]-b[0],y=a[1]-b[1],z=a[2]-b[2];return x*x+y*y+z*z;}
function applyItemRenderState(gl,profile,transparent=profile.transparent){
  if(profile.alphaToCoverage)gl.enable(gl.SAMPLE_ALPHA_TO_COVERAGE);else gl.disable(gl.SAMPLE_ALPHA_TO_COVERAGE);
  if(profile.blendEnabled){gl.enable(gl.BLEND);if(profile.blend==="multiply")gl.blendFunc(gl.DST_COLOR,gl.ZERO);else if(profile.blend==="transparent-as-black")gl.blendFunc(gl.ONE,gl.ONE_MINUS_SRC_ALPHA);else gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);}else gl.disable(gl.BLEND);
  if(profile.depthTest)gl.enable(gl.DEPTH_TEST);else gl.disable(gl.DEPTH_TEST);gl.depthMask(profile.depthWrite);
  if(profile.cull==="none")gl.disable(gl.CULL_FACE);else{gl.enable(gl.CULL_FACE);gl.cullFace(profile.cull==="front"?gl.FRONT:gl.BACK);}
}
function effectiveScalar(material,override,name,fallback){const changed=override?.properties.get(name.toLowerCase());if(typeof changed==="number")return changed;if(Array.isArray(changed))return Number(changed[0])||0;return propertyValue(material,name,fallback);}
function effectiveVector2(material,override,name,fallback){const changed=override?.properties.get(name.toLowerCase());if(Array.isArray(changed))return [Number(changed[0])||0,Number(changed[1]??changed[0])||0];if(typeof changed==="number")return [changed,changed];const property=material?.properties.find((item)=>item.name.toLowerCase()===name.toLowerCase());if(property?.value2)return property.value2.map((value)=>Number(value)||0);return fallback;}
function effectiveEmissive(material,override){const changed=override?.properties.get("ksemissive");const property=material?.properties.find((item)=>item.name.toLowerCase()==="ksemissive");let value=Array.isArray(changed)?changed:typeof changed==="number"?[changed,changed,changed]:property?.value3||[0,0,0];let color=[Number(value[0])||0,Number(value[1])||0,Number(value[2])||0];if(Math.max(...color)>16)color=color.map(component=>component/255);const strength=value.length>3?(Number(value[3])||0):1;return color.map(component=>component*strength);}
function mirroredChannelForPreview(channel){return channel===1?6:channel===2?5:channel===3?4:channel;}
function baseChannelForPreview(channel){return channel===4?3:channel===5?2:channel===6?1:channel;}
function customEmissiveTextureKey(descriptor){
  const excluded=new Set(descriptor.colorMasksAsMultiplier?(descriptor.colorMasks||[]).map((mask)=>mask.channel):[]);
  for(let channel=0;channel<4;channel++)if(descriptor.vertexMask?.points?.[channel]){excluded.add(channel);excluded.add(mirroredChannelForPreview(channel));}
  const used=new Set((descriptor.shapes||[]).map((shape)=>shape.channel).filter((channel)=>!excluded.has(channel))),colors=new Map(descriptor.channelColors||[]);
  const signature=[...used].sort((a,b)=>a-b).map((channel)=>[channel,colors.get(channel)||[0,0,0]]);
  return `${descriptor.source}:${descriptor.line}:${descriptor.name}:${JSON.stringify(signature)}`;
}
function deleteCustomEmissiveTexture(gl,baked){gl.deleteTexture(baked.texture);if(baked.colorMaskTexture)gl.deleteTexture(baked.colorMaskTexture);if(baked.vertexMaskTexture)gl.deleteTexture(baked.vertexMaskTexture);}

function uploadCustomEmissive(gl, descriptor) {
  const {sourceWidth,sourceHeight,width,height}=customEmissiveAtlasSize(descriptor.resolution);
  const channelColors=new Map(descriptor.channelColors || []),lumaOpacity=descriptor.diffuseLuminance?.opacity ?? 1;
  let strength=1;
  for(const color of channelColors.values())for(const component of color)strength=Math.max(strength,Number(component)||0);
  const vertexPoints=descriptor.vertexMask?.points||[],activeVertexBases=new Set(vertexPoints.map((point,index)=>point?index:-1).filter((index)=>index>=0)),vertexChannels=new Set([...activeVertexBases].flatMap((channel)=>[channel,mirroredChannelForPreview(channel)]));
  const pixels=new Uint8Array(width*height*4),multiplierMasks=descriptor.colorMasksAsMultiplier?(descriptor.colorMasks||[]).slice(0,4):[],maskPixels=multiplierMasks.length?new Uint8Array(width*height*4):null,vertexPixels=activeVertexBases.size?new Uint8Array(width*height*4):null,maskedChannels=new Set([...multiplierMasks.map((mask)=>mask.channel),...vertexChannels]);
  const multiplierDefaults=multiplierMasks.map((mask)=>descriptor.shapes.some((shape)=>shape.channel===mask.channel)?0:1),vertexDefaults=[0,1,2,3].map((channel)=>!activeVertexBases.has(channel)?0:descriptor.shapes.some((shape)=>baseChannelForPreview(shape.channel)===channel)?0:1),stepX=sourceWidth/width,stepY=sourceHeight/height;
  for(let py=0;py<height;py++)for(let px=0;px<width;px++){
    const x=(px+.5)*sourceWidth/width,y=(py+.5)*sourceHeight/height;
    const masked=!descriptor.maskShapes.length||descriptor.maskShapes.some((shape)=>sampledShapeCoverage(shape,x,y,stepX,stepY,sourceWidth,sourceHeight)>0);
    const rgb=[0,0,0];
    const multiplierCoverage=[...multiplierDefaults],vertexCoverage=[...vertexDefaults];
    if(masked)for(const shape of descriptor.shapes){
      const coverage=sampledShapeCoverage(shape,x,y,stepX,stepY,sourceWidth,sourceHeight)*(Number(shape.opacity) || 0)*lumaOpacity,color=channelColors.get(shape.channel);
      for(let index=0;index<multiplierMasks.length;index++)if(multiplierMasks[index].channel===shape.channel)multiplierCoverage[index]=Math.max(multiplierCoverage[index],coverage);
      const baseChannel=baseChannelForPreview(shape.channel);if(activeVertexBases.has(baseChannel))vertexCoverage[baseChannel]=Math.max(vertexCoverage[baseChannel],coverage);
      if(maskedChannels.has(shape.channel))continue;
      if(!color||coverage<=0)continue;
      for(let component=0;component<3;component++)rgb[component]+=(Number(color[component])||0)*coverage;
    }
    const offset=(py*width+px)*4;
    for(let component=0;component<3;component++)pixels[offset+component]=Math.round(Math.max(0,Math.min(1,rgb[component]/strength))*255);
    pixels[offset+3]=255;
    if(maskPixels){for(let component=0;component<4;component++)maskPixels[offset+component]=component<multiplierCoverage.length?(masked?Math.round(Math.max(0,Math.min(1,multiplierCoverage[component]))*255):0):255;}
    if(vertexPixels){for(let component=0;component<4;component++)vertexPixels[offset+component]=activeVertexBases.has(component)&&masked?Math.round(Math.max(0,Math.min(1,vertexCoverage[component]))*255):0;}
  }
  const texture=uploadRgbaTexture(gl,pixels,width,height),colorMaskTexture=maskPixels?uploadRgbaTexture(gl,maskPixels,width,height):null,vertexMaskTexture=vertexPixels?uploadRgbaTexture(gl,vertexPixels,width,height):null;
  return {texture,colorMaskTexture,vertexMaskTexture,strength,width,height};
}

function uploadRgbaTexture(gl,pixels,width,height){const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,width,height,0,gl.RGBA,gl.UNSIGNED_BYTE,pixels);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.REPEAT);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.REPEAT);return texture;}

function sampledShapeCoverage(shape,x,y,stepX,stepY,sourceWidth,sourceHeight){return shapeCoverage(shape,x,y,sourceWidth,sourceHeight);}

function shapeCoverage(shape,x,y,sourceWidth=1,sourceHeight=1){
  if(shape.type==="poly"){
    let inside=false;const points=shape.points || [];
    for(let i=0,j=points.length-1;i<points.length;j=i++){
      const [xi,yi]=points[i],[xj,yj]=points[j];
      if(((yi>y)!==(yj>y))&&x<(xj-xi)*(y-yi)/((yj-yi)||1e-9)+xi)inside=!inside;
    }
    let minimum=Infinity;
    for(let i=0,j=points.length-1;i<points.length;j=i++){
      const [ax,ay]=points[j],[bx,by]=points[i],dx=bx-ax,dy=by-ay,t=Math.max(0,Math.min(1,((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy||1))),distance=Math.hypot((x-(ax+t*dx))/sourceWidth,(y-(ay+t*dy))/sourceHeight);minimum=Math.min(minimum,distance);
    }
    const signed=(inside?minimum:-minimum)+(Number(shape.offset)||0),edge=1/Math.max(1,Number(shape.sharpness)||1000),base=smoothUnit((signed+edge)/(2*edge));
    return Math.pow(base,Math.max(.01,Number(shape.exponent)||1));
  }
  const [sx,sy]=shape.start,[w,h]=shape.size;
  if(x<sx||y<sy||x>sx+w||y>sy+h)return 0;
  if(shape.type==="circle"){
    const dx=(x-sx-w/2)/Math.max(w/2,1e-9),dy=(y-sy-h/2)/Math.max(h/2,1e-9);
    return dx*dx+dy*dy<=1?1:0;
  }
  const radius=shape.cornerRadius || [0,0],rx=Math.min(Math.abs(radius[0]||0)*w/2,w/2),ry=Math.min(Math.abs(radius[1]||radius[0]||0)*h/2,h/2);
  if(!rx||!ry||x>=sx+rx&&x<=sx+w-rx||y>=sy+ry&&y<=sy+h-ry)return 1;
  const cx=x<sx+rx?sx+rx:sx+w-rx,cy=y<sy+ry?sy+ry:sy+h-ry;
  return ((x-cx)/rx)**2+((y-cy)/ry)**2<=1?1:0;
}
function smoothUnit(value){const t=Math.max(0,Math.min(1,value));return t*t*(3-2*t);}

function textureParameters(gl, mipmapped) {
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,mipmapped?gl.LINEAR_MIPMAP_LINEAR:gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.REPEAT);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.REPEAT);
}

function uploadFloatDds(gl,bytes,descriptor){
  const formats={R16F:[gl.R16F,gl.RED,gl.HALF_FLOAT],RG16F:[gl.RG16F,gl.RG,gl.HALF_FLOAT],RGBA16F:[gl.RGBA16F,gl.RGBA,gl.HALF_FLOAT],R32F:[gl.R32F,gl.RED,gl.FLOAT],RG32F:[gl.RG32F,gl.RG,gl.FLOAT],RGBA32F:[gl.RGBA32F,gl.RGBA,gl.FLOAT]},gpu=formats[descriptor.format];if(!gpu)return false;
  const maximum=Math.min(2048,gl.getParameter(gl.MAX_TEXTURE_SIZE)),step=Math.max(1,Math.ceil(Math.max(descriptor.width,descriptor.height)/maximum)),width=Math.ceil(descriptor.width/step),height=Math.ceil(descriptor.height/step),Output=descriptor.componentBytes===4?Float32Array:Uint16Array,pixels=new Output(width*height*descriptor.channels),view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
  for(let y=0;y<height;y++)for(let x=0;x<width;x++){const sourcePixel=(Math.min(descriptor.height-1,y*step)*descriptor.width+Math.min(descriptor.width-1,x*step))*descriptor.channels,targetPixel=(y*width+x)*descriptor.channels;for(let channel=0;channel<descriptor.channels;channel++){const offset=descriptor.dataOffset+(sourcePixel+channel)*descriptor.componentBytes;pixels[targetPixel+channel]=descriptor.componentBytes===4?view.getFloat32(offset,true):view.getUint16(offset,true);}}
  gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);gl.texImage2D(gl.TEXTURE_2D,0,gpu[0],width,height,0,gpu[1],gpu[2],pixels);const filterable=descriptor.componentBytes===2||Boolean(gl.getExtension("OES_texture_float_linear"));if(filterable){gl.generateMipmap(gl.TEXTURE_2D);textureParameters(gl,true);}else{gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.REPEAT);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.REPEAT);}return true;
}

function uploadDds(gl, compression, bytes) {
  const descriptor=inspectDds(bytes);if(!descriptor)return null;
  const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);
  try {
    const s3tc=compression.s3tc,srgb=compression.s3tcSrgb,bptc=compression.bptc,gpuFormats={};
    if(s3tc)Object.assign(gpuFormats,{BC1:[s3tc.COMPRESSED_RGBA_S3TC_DXT1_EXT,8],BC2:[s3tc.COMPRESSED_RGBA_S3TC_DXT3_EXT,16],BC2_PREMULT:[s3tc.COMPRESSED_RGBA_S3TC_DXT3_EXT,16],BC3:[s3tc.COMPRESSED_RGBA_S3TC_DXT5_EXT,16],BC3_PREMULT:[s3tc.COMPRESSED_RGBA_S3TC_DXT5_EXT,16]});
    if(srgb)Object.assign(gpuFormats,{BC1_SRGB:[srgb.COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT,8],BC2_SRGB:[srgb.COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT,16],BC3_SRGB:[srgb.COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT,16]});else if(s3tc)Object.assign(gpuFormats,{BC1_SRGB:[s3tc.COMPRESSED_RGBA_S3TC_DXT1_EXT,8],BC2_SRGB:[s3tc.COMPRESSED_RGBA_S3TC_DXT3_EXT,16],BC3_SRGB:[s3tc.COMPRESSED_RGBA_S3TC_DXT5_EXT,16]});
    if(bptc)Object.assign(gpuFormats,{BC6H_SF16:[bptc.COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT,16],BC6H_UF16:[bptc.COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT,16],BC7:[bptc.COMPRESSED_RGBA_BPTC_UNORM_EXT,16],BC7_SRGB:[bptc.COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT,16]});
    if(descriptor.float){if(uploadFloatDds(gl,bytes,descriptor))return texture;}
    const gpu=gpuFormats[descriptor.format];
    if(gpu){let offset=descriptor.dataOffset,width=descriptor.width,height=descriptor.height;for(let level=0;level<descriptor.mipCount;level++){const size=Math.max(1,Math.ceil(width/4))*Math.max(1,Math.ceil(height/4))*gpu[1];if(offset+size>bytes.byteLength)throw new Error(`DDS mip ${level} exceeds texture data`);gl.compressedTexImage2D(gl.TEXTURE_2D,level,gpu[0],width,height,0,bytes.subarray(offset,offset+size));offset+=size;width=Math.max(1,width>>1);height=Math.max(1,height>>1);}textureParameters(gl,descriptor.mipCount>1);return texture;}
    if(!descriptor.compressed||descriptor.cpu||descriptor.cpuFallback){const levels=decodeDdsRgba(bytes,descriptor);for(let level=0;level<levels.length;level++){const item=levels[level];gl.texImage2D(gl.TEXTURE_2D,level,gl.RGBA,item.width,item.height,0,gl.RGBA,gl.UNSIGNED_BYTE,item.pixels);}textureParameters(gl,levels.length>1);return texture;}
  } catch(error){console.warn(`Could not decode ${descriptor.format} DDS`,error);}
  gl.deleteTexture(texture);return null;
}

function imageMime(bytes) {
  if(bytes.byteLength>=8&&bytes[0]===0x89&&bytes[1]===0x50&&bytes[2]===0x4e&&bytes[3]===0x47)return "image/png";
  if(bytes.byteLength>=3&&bytes[0]===0xff&&bytes[1]===0xd8&&bytes[2]===0xff)return "image/jpeg";
  if(bytes.byteLength>=12&&String.fromCharCode(...bytes.subarray(0,4))==="RIFF"&&String.fromCharCode(...bytes.subarray(8,12))==="WEBP")return "image/webp";
  return "";
}

function uploadEmbeddedTexture(gl, compression, bytes, isCurrent) {
  const descriptor=inspectDds(bytes),dds=descriptor&&uploadDds(gl,compression,bytes);if(dds)return {texture:dds,format:descriptor.format};
  const mime=imageMime(bytes);if(!mime||typeof createImageBitmap!=="function")return null;
  const texture=uploadRgbaTexture(gl,new Uint8Array([255,0,255,255]),1,1);
  const ready=createImageBitmap(new Blob([bytes],{type:mime}),{premultiplyAlpha:"none",colorSpaceConversion:"none"}).then((bitmap)=>{if(!isCurrent()){bitmap.close();return false;}gl.bindTexture(gl.TEXTURE_2D,texture);gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,bitmap);gl.generateMipmap(gl.TEXTURE_2D);textureParameters(gl,true);bitmap.close();return true;}).catch((error)=>{console.warn(`Could not decode embedded ${mime} texture`,error);return false;});
  return {texture,format:mime.slice(6).toUpperCase(),ready};
}
