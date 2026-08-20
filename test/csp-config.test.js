import assert from "node:assert/strict";
import test from "node:test";
import { readFile } from "node:fs/promises";
import { evaluateCspConfig, expandCspMaterialTemplates, matchesSelector, parseCspIni, parseCspValue, splitCspList } from "../src/csp-config.js";
import { customEmissiveAtlasSize } from "../src/custom-emissive.js";
import { assettoPath } from "./fixture-paths.js";

function material(name, shader = "ksPerPixel") {
  return { name, shader, properties: [{ name: "ksAmbient", value: 0.2, value3: [0, 0, 0] }], resources: [{ slot: "txDiffuse", texture: `${name}.dds` }] };
}
function model() {
  const road = { kind: "mesh", name: "12ROAD005", materialId: 0, children: [] };
  const tree = { kind: "mesh", name: "TREE_LOD_A", materialId: 1, children: [] };
  return { materials: [material("Asph-old"), material("trees_new2", "ksTree")], root: { kind: "node", name: "root", children: [road, tree] }, road, tree };
}

test("parses duplicate CSP sections, comments, continuations and vectors", () => {
  const config = parseCspIni("[SHADER_REPLACEMENT_...]\nMATERIALS = body ; note\nPROP_... = ksAmbient, 0.4\n[SHADER_REPLACEMENT_...]\nMESHES=a,\\\n b\n");
  assert.equal(config.sections.length, 2);
  assert.deepEqual(splitCspList(config.sections[1].values.get("MESHES")[0]), ["a", "b"]);
  assert.deepEqual(parseCspValue("1, 0.5, 0"), [1, 0.5, 0]);
});

test("matches CSP wildcard, shader, texture and compound selectors", () => {
  const node = { name: "Engine_SUB2" }, mat = material("EXT_body", "ksTreeAT");
  assert.equal(matchesSelector("EXT_?", node, mat, "material"), true);
  assert.equal(matchesSelector("shader:ksTree?", node, mat, "material"), true);
  assert.equal(matchesSelector("texture:EXT_body.dds", node, mat), true);
  assert.equal(matchesSelector("'{ material:EXT_body & ! Engine_SUB2 }'", node, mat), false);
});

test("applies shader replacements and condition-weighted material adjustments per mesh", () => {
  const scene = model();
  const config = parseCspIni(`[SHADER_REPLACEMENT_0]\nMATERIALS=shader:ksTree?\nSHADER=ksPerPixelAlpha\nBLEND_MODE=ALPHA_BLEND\nDEPTH_MODE=READ_ONLY\nCULL_MODE=DOUBLESIDED\nPROP_0=ksAmbient,0.3\nRESOURCE_0=txNormal\nRESOURCE_TEXTURE_0=flat_nm.dds\nRESOURCE_1=txMaps\nRESOURCE_COLOR_1=1,0.5,0.25,1\n[MATERIAL_ADJUSTMENT_0]\nMESHES=?ROAD?\nKEY_0=ksAmbient\nVALUE_0=0.8\nOFF_VALUE_0=ORIGINAL\nCONDITION=SEASON_WINTER\n`);
  const result = evaluateCspConfig(scene, config, { conditions: { SEASON_WINTER: 0.5 } });
  assert.equal(result.nodeOverrides.get(scene.tree).shader, "ksPerPixelAlpha");
  assert.equal(result.nodeOverrides.get(scene.tree).blendMode, "ALPHA_BLEND");
  assert.equal(result.nodeOverrides.get(scene.tree).depthMode, "READ_ONLY");
  assert.equal(result.nodeOverrides.get(scene.tree).cullMode, "DOUBLESIDED");
  assert.equal(result.nodeOverrides.get(scene.tree).properties.get("ksambient"), 0.3);
  assert.equal(result.nodeOverrides.get(scene.tree).resources.get("txnormal").texture, "flat_nm.dds");
  assert.deepEqual(result.nodeOverrides.get(scene.tree).resources.get("txmaps").color, [1, 0.5, 0.25, 1]);
  assert.equal(result.nodeOverrides.get(scene.road).properties.get("ksambient"), 0.5);
  assert.equal(result.matchedSections, 2);
});

test("exposes YEAR_PROGRESS and drives seasonal LUT conditions from it", () => {
  const scene = model();
  const config = parseCspIni(`[CONDITION_0]\nNAME=SEASON_WINTER\nINPUT=YEAR_PROGRESS\nLUT=(|0=1|0.25=0|1=0|)\n[MATERIAL_ADJUSTMENT_0]\nMESHES=?ROAD?\nKEY_0=seasonWinter\nVALUE_0=0.8\nOFF_VALUE_0=0\nCONDITION=SEASON_WINTER\n`);
  const winter = evaluateCspConfig(scene, config, { inputs: { YEAR_PROGRESS: 0 } });
  assert.deepEqual(winter.usedInputs.get("YEAR_PROGRESS"), { min: 0, max: 1, value: 0 });
  assert.equal(winter.nodeOverrides.get(scene.road).properties.get("seasonwinter"), 0.8);
  const spring = evaluateCspConfig(scene, config, { inputs: { YEAR_PROGRESS: 0.25 } });
  assert.equal(spring.nodeOverrides.get(scene.road).properties.get("seasonwinter"), 0);
});

test("evaluates installed Imola seasonal conditions at native year progress", async (t) => {
  let text;
  try { text = await readFile(assettoPath("extension/config/tracks/loaded/imola.ini"), "utf8"); }
  catch { t.skip("Installed Imola CSP fixture is unavailable"); return; }
  const scene = model(), config = parseCspIni(text, "imola.ini");
  const midpoint = evaluateCspConfig(scene, config, { inputs: { YEAR_PROGRESS: 0.5 } });
  assert.ok(Math.abs(midpoint.conditions.get("SEASON_AUTUMN") - 1 / 15) < 1e-9);
  assert.equal(midpoint.conditions.get("SEASON_WINTER"), 0);
  assert.ok(Math.abs(midpoint.nodeOverrides.get(scene.road).properties.get("seasonwinter") - 0.4 / 15) < 1e-9);
  const winter = evaluateCspConfig(scene, config, { inputs: { YEAR_PROGRESS: 0.07 } });
  assert.equal(winter.conditions.get("SEASON_WINTER"), 1);
  assert.equal(winter.nodeOverrides.get(scene.road).properties.get("seasonwinter"), 0.8);
});

test("applies CSP mesh transparency, layer, LOD and shadow adjustments", () => {
  const scene = model();
  const config = parseCspIni(`[MESH_ADJUSTMENT_APEX]
MESHES = ?ROAD?
IS_TRANSPARENT = 1
LAYER = 7
LOD_IN = 12.5
LOD_OUT = 250
CAST_SHADOWS = 0
`);
  const result = evaluateCspConfig(scene, config);
  const override = result.nodeOverrides.get(scene.road);
  assert.equal(override.isTransparent, true);
  assert.equal(override.layer, 7);
  assert.equal(override.lodIn, 12.5);
  assert.equal(override.lodOut, 250);
  assert.equal(override.castShadows, false);
  assert.equal(result.matchedSections, 1);
});

test("expands built-in glass and metallic car-paint templates at their source position", () => {
  const config = expandCspMaterialTemplates(parseCspIni(`[INCLUDE: common/materials_glass.ini]\nExteriorGlassFilmedMaterials=glass\n[INCLUDE: common/materials_carpaint.ini]\nCarPaintMaterial=body\n[Material_CarPaint_Metallic]\nClearCoatThickness=0.1\n`));
  assert.equal(config.expandedTemplates.length, 2);
  assert.deepEqual([...config.resolvedIncludes].sort(), ["materials_carpaint.ini", "materials_glass.ini"]);
  const carPaint = config.sections.find((section) => section.name.startsWith("SHADER_REPLACEMENT_APEX_CARPAINT"));
  assert.equal(carPaint.values.get("MATERIALS")[0], "body");
  assert.equal(carPaint.values.get("SHADER")[0], "smCarPaint");
  assert.ok(carPaint.entries.some((entry) => entry.value === "ksDiffuse,0.45"));
  const glass = config.sections.find((section) => section.name.startsWith("SHADER_REPLACEMENT_APEX_GLASS"));
  assert.equal(glass.values.get("MATERIALS")[0], "glass");
  assert.equal(glass.values.get("SHADER")[0], "smGlass");
});

test("expands European license plate parameters without running template code", () => {
  const config = expandCspMaterialTemplates(parseCspIni(`[INCLUDE: common/materials_license_plate.ini]\n[Material_LicensePlate_Europe]\nMaterials=plate\n`));
  const generated = config.sections.find((section) => section.name.startsWith("SHADER_REPLACEMENT_APEX_LICENSE"));
  assert.equal(generated.values.get("SHADER")[0], "smLicensePlate");
  assert.ok(generated.entries.some((entry) => entry.value === "ksDiffuse,0.2"));
  assert.ok(generated.entries.some((entry) => entry.value === "extWidthRatio,4"));
});

test("expands mirrored self-lights and binds their intensity to headlights", () => {
  const scene = model();
  const config = expandCspMaterialTemplates(parseCspIni(`[INCLUDE: common/selflighting.ini]
[SelfLight_Headlights]
POSITION=0,0.5,1.8
MIRROR=0.7
COLOR=1,0.5,0.25,10
RANGE=0.6
BIND_TO_HEADLIGHTS=1
`));
  const result = evaluateCspConfig(scene, config, { conditions: { HEADLIGHTS: 0.5 } });
  assert.equal(config.expandedTemplates.at(-1).template, "SelfLight_Headlights");
  assert.ok(config.resolvedIncludes.has("selflighting.ini"));
  assert.equal(result.matchedLightSections, 1);
  assert.equal(result.lights.length, 2);
  assert.deepEqual(result.lights.map((light) => light.position[0]), [-0.7, 0.7]);
  assert.deepEqual(result.lights[0].color, [5, 2.5, 1.25]);
  assert.equal(result.lights[0].condition, "HEADLIGHTS");
  assert.equal(result.lights[0].viewMode, "exterior");
  assert.equal(result.lights[0].affectsTrackMode, "none");
  assert.equal(result.usedInputs.get("HEADLIGHTS").value, .5);
});

test("does not invent a mirror for a single self-light", () => {
  const scene = model();
  const config = expandCspMaterialTemplates(parseCspIni(`[INCLUDE: common/selflighting.ini]
[SelfLight]
POSITION=0.49,0.84,0.36
COLOR=2,1.2,1,10
INTERIOR_ONLY=1
`));
  const result = evaluateCspConfig(scene, config);
  assert.equal(result.lights.length, 1);
  assert.deepEqual(result.lights[0].position, [0.49, 0.84, 0.36]);
  assert.equal(result.lights[0].viewMode, "interior");
  assert.equal(result.lights[0].affectsTrackMode, "none");
});

test("applies native car-light role, explicit vehicle, inverse, and extra bindings", () => {
  const scene = model();
  const config = parseCspIni(`[LIGHT_HEADLIGHT_0]
POSITION=0,1,2
COLOR=1,1,1,10
RANGE=20
VISIBILITY_LEVEL=2
AFFECTS_TRACK=0
EXTERIOR_ONLY=1
[LIGHT_REVERSE_0]
POSITION=0,1,-2
COLOR=1,1,1,4
RANGE=5
[LIGHT_EXTRA_0]
POSITION=0,1,0
COLOR=1,0,0,5
RANGE=3
BIND_TO_EXTRA_A=1
NOT_WITH_HEADLIGHTS=1
BOUND_TO=Lamp:1
`);
  const result = evaluateCspConfig(scene, config, { conditions: { HEADLIGHTS: .5, REVERSE: .25 }, inputs: { EXTRA_A: .8 } });
  assert.equal(result.lights.length, 3);
  assert.deepEqual(result.lights.map((light) => light.condition), ["HEADLIGHTS", "REVERSE", "EXTRA_A × !HEADLIGHTS"]);
  assert.deepEqual(result.lights[0].color, [5,5,5]);
  assert.deepEqual(result.lights[1].color, [1,1,1]);
  assert.ok(Math.abs(result.lights[2].color[0]-2)<1e-12);
  assert.equal(result.lights[0].visibilityLevel, 2);
  assert.equal(result.lights[0].affectsTrack, false);
  assert.equal(result.lights[0].affectsTrackMode, "none");
  assert.equal(result.lights[0].exteriorOnly, true);
  assert.equal(result.lights[0].viewMode, "exterior");
  assert.deepEqual(result.lights[2].boundTo, ["Lamp:1"]);
  assert.deepEqual(result.lights[2].bindings.map((binding)=>[binding.input,binding.inverted]), [["EXTRA_A",false],["HEADLIGHTS",true]]);
  assert.equal(result.usedInputs.get("HEADLIGHTS").value, .5);
  assert.equal(result.usedInputs.get("REVERSE").value, .25);
  assert.equal(result.usedInputs.get("EXTRA_A").value, .8);
});

test("preserves native interior-only track reception and interior view precedence",()=>{const scene=model(),config=parseCspIni(`[LIGHT_EXTRA_0]
POSITION=0,1,0
COLOR=1,1,1,1
AFFECTS_TRACK=INTERIOR_ONLY
INTERIOR_ONLY=1
EXTERIOR_ONLY=1
`),light=evaluateCspConfig(scene,config).lights[0];assert.equal(light.affectsTrack,true);assert.equal(light.affectsTrackMode,"interior-only");assert.equal(light.interiorOnly,true);assert.equal(light.exteriorOnly,true);assert.equal(light.viewMode,"interior");});

test("interpolates authored car-light off endpoints and treats zero mirror as disabled", () => {
  const scene = model();
  const config = parseCspIni(`[LIGHT_HEADLIGHT_0]
POSITION=0,1,2
OFF_POSITION=0,.5,1
MIRROR=2
OFF_MIRROR=1
COLOR=1,1,1,10
OFF_MULT=.2
RANGE=20
OFF_RANGE_MULT=.25
FADE_AT=100
FADE_SMOOTH=20
OFF_FADE_MULT=.5
[LIGHT_EXTRA_0]
POSITION=.2,1,0
MIRROR=0
COLOR=1,0,0,1
RANGE=2
`);
  const result = evaluateCspConfig(scene, config, { inputs: { HEADLIGHTS: .5 } });
  assert.equal(result.lights.length, 3);
  assert.deepEqual(result.lights.slice(0,2).map((light)=>light.position), [[-1.5,.75,1.5],[1.5,.75,1.5]]);
  assert.deepEqual(result.lights[0].color, [6,6,6]);
  assert.equal(result.lights[0].range, 12.5);
  assert.equal(result.lights[0].fadeAt, 75);
  assert.equal(result.lights[0].fadeSmooth, 15);
  assert.equal(result.lights[0].offMult, .2);
  assert.deepEqual(result.lights[2].position, [.2,1,0]);
  assert.equal(result.lights[2].mirrored, undefined);
});

test("applies native popup progress, output, and RGB edge curves", () => {
  const scene = model();
  const config = parseCspIni(`[LIGHT_HEADLIGHT_0]
POSITION=0,1,2
COLOR=1,1,1,10
SPOT=50
SPOT_EDGE=.3,.2,.1
SPOT_EDGE_SHARPNESS=10
POPUP_ENABLED=1
POPUP_START=.25
POPUP_END=.75
POPUP_SECOND_SPOT_INITIAL_VALUE=.4
POPUP_SECOND_SPOT_EXP=.5
POPUP_EDGE_OFFSET=.2
POPUP_EDGE_EXP=2
`);
  const result = evaluateCspConfig(scene, config, { inputs: { HEADLIGHTS: 1, POPUP_POSITION: .375 } });
  const light = result.lights[0];
  assert.deepEqual(result.usedInputs.get("POPUP_POSITION"), { min: 0, max: 1, value: .375 });
  assert.equal(light.popupProgress, .25);
  assert.equal(light.popupSecondSpotFactor, .7);
  assert.deepEqual(light.color, [7,7,7]);
  assert.deepEqual(light.spotEdge.map((value) => Number(value.toFixed(4))), [.1125,.0125,-.0875]);
  assert.equal(light.popupEdgeShift, .1875);
  assert.deepEqual(light.position, [0,1,2]);

  const open = evaluateCspConfig(scene, config, { inputs: { HEADLIGHTS: 1, POPUP_POSITION: 1 } }).lights[0];
  assert.equal(open.popupProgress, 1);
  assert.equal(open.popupSecondSpotFactor, 1);
  assert.deepEqual(open.color, [10,10,10]);
  assert.deepEqual(open.spotEdge, [.3,.2,.1]);
});

test("derives spaced track light-series positions from mesh geometry", () => {
  const vertices = new Float32Array([
    0,0,0, 0,-1,0, 0,0, 1,0,0,  1,0,0, 0,-1,0, 0,0, 1,0,0,  0,0,1, 0,-1,0, 0,0, 1,0,0,
    10,0,0, 0,-1,0, 0,0, 1,0,0,  11,0,0, 0,-1,0, 0,0, 1,0,0,  10,0,1, 0,-1,0, 0,0, 1,0,0
  ]);
  const lamp = { kind: "mesh", name: "LAMPS", materialId: 0, children: [], vertices, vertexStride: 11, indices: new Uint16Array([0,1,2,3,4,5]) };
  const scene = { materials: [material("light")], root: { kind: "node", name: "root", transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 5,0,0,1], children: [lamp] } };
  const config = parseCspIni(`[LIGHT_SERIES_0]
MESHES=LAMPS
OFFSET=0,2,0
DIRECTION=NORMAL
COLOR=255,128,0,0.1
RANGE=20
SPOT=48
SPOT_SHARPNESS=0
SECOND_SPOT=120
SECOND_SPOT_SHARPNESS=0.5
SECOND_SPOT_SKIP=0.3
SECOND_SPOT_RANGE=16
SECOND_SPOT_INTENSITY=0.21
SPOT_EDGE=0.12,0.1,0.08
SPOT_EDGE_SHARPNESS=10
SPOT_UP=0,1,0
FADE_AT=100
FADE_SMOOTH=20
CLUSTER_THRESHOLD=1
CONDITION=NIGHT_SMOOTH
SHADOWS=1
SHADOWS_SPOT=150
SHADOWS_RANGE=45
SHADOWS_EXP_FACTOR=12
SHADOWS_BOOST=5
SHADOWS_CLIP_PLANE=0.07
SHADOWS_CLIP_SPHERE=0.08
SHADOWS_EXTRA_BLUR=1
`);
  const result = evaluateCspConfig(scene, config, { conditions: { NIGHT_SMOOTH: 1 } });
  assert.equal(result.lights.length, 2);
  assert.deepEqual(result.lights.map((light) => Number(light.position[0].toFixed(3))), [5.333, 15.333]);
  assert.deepEqual(result.lights.map((light) => Number(light.position[1].toFixed(3))), [2, 2]);
  assert.deepEqual(result.lights[0].direction, [0, -1, 0]);
  assert.equal(result.lights[0].range, 20);
  assert.equal(result.lights[0].fadeAt, 100);
  assert.equal(result.lights[0].fadeSmooth, 20);
  assert.equal(result.lights[0].spotSharpness, 0);
  assert.equal(result.lights[0].secondSpot, 120);
  assert.equal(result.lights[0].secondSpotSharpness, 0.5);
  assert.equal(result.lights[0].secondSpotSkip, 0.3);
  assert.equal(result.lights[0].secondSpotRange, 16);
  assert.equal(result.lights[0].secondSpotIntensity, 0.21);
  assert.equal(result.lights[0].hasSecondSpot, true);
  assert.deepEqual(result.lights[0].spotEdge, [0.12, 0.1, 0.08]);
  assert.equal(result.lights[0].spotEdgeSharpness, 10);
  assert.deepEqual(result.lights[0].spotUp, [0, 1, 0]);
  assert.equal(result.lights[0].hasSpotEdge, true);
  assert.equal(result.lights[0].rangeGradientOffset, 0.2);
  assert.equal(result.lights[0].castsShadows, true);
  assert.equal(result.lights[0].shadowSpot, 150);
  assert.equal(result.lights[0].shadowRange, 45);
  assert.equal(result.lights[0].shadowExpFactor, 12);
  assert.equal(result.lights[0].shadowBoost, 5);
  assert.equal(result.lights[0].shadowClipPlane, 0.07);
  assert.equal(result.lights[0].shadowClipSphere, 0.08);
  assert.equal(result.lights[0].shadowExtraBlur, true);
});

test("keeps a CSP line light as one finite segment with endpoint colors", () => {
  const scene = model();
  const config = parseCspIni(`[LIGHT_...]
LINE_FROM=0,2,0
LINE_TO=30,2,0
COLOR_FROM=1,0.8,0.6,4
COLOR_TO=1,0.8,0.6,8
RANGE=10
CONDITION=NIGHT_SMOOTH
`);
  const result = evaluateCspConfig(scene, config, { conditions: { NIGHT_SMOOTH: 1 } });
  assert.equal(result.lights.length, 1);
  assert.deepEqual(result.lights[0].position, [15, 2, 0]);
  assert.deepEqual(result.lights[0].lineFrom, [0, 2, 0]);
  assert.deepEqual(result.lights[0].lineTo, [30, 2, 0]);
  assert.deepEqual(result.lights[0].lineVector, [30, 0, 0]);
  assert.equal(result.lights[0].lineDistanceInverse, 1 / 900);
  assert.deepEqual(result.lights[0].lineColorFrom.map((value) => Number(value.toFixed(3))), [4, 3.2, 2.4]);
  assert.deepEqual(result.lights[0].lineColorTo.map((value) => Number(value.toFixed(3))), [8, 6.4, 4.8]);
  assert.equal(result.lights[0].lineLight, true);
  assert.equal(result.lights[0].castsShadows, false);
});

test("parses CSP track occluder walls, boxes, exclusions, and culling settings", () => {
  const scene = model();
  const config = parseCspIni(`[TRACK_OCCLUDERS]\nCELL_SIZE=750\nSIZE_WEIGHT_FACTOR=.35\n[TRACK_OCCLUDER_WALL_0]\nDESCRIPTION=Pit wall\nPOINT_0=0,5,0\nPOINT_1=10,6,0\nEXCLUSION_0=0,0,-2\nEXCLUSION_1=2,0,-2\nEXCLUSION_2=2,0,-1\nEXCLUSION_3=0,0,-1\n[TRACK_OCCLUDER_BOX_0]\nPOINT_0=0,4,0\nPOINT_1=4,4,0\nPOINT_2=4,4,4\nPOINT_3=0,4,4\n[TRACK_OCCLUDER_WALL_1]\nCULLING=0\nPOINT_0=0,0,0\nPOINT_1=1,0,0\n`);
  const result = evaluateCspConfig(scene, config);
  assert.equal(result.trackOccluders.length, 2);
  assert.equal(result.trackOccluders[0].type, "wall");
  assert.equal(result.trackOccluders[0].description, "Pit wall");
  assert.equal(result.trackOccluders[0].exclusion.length, 4);
  assert.equal(result.trackOccluders[1].type, "box");
  assert.deepEqual(result.trackOccluderSettings, { cellSize: 750, sizeWeightFactor: .35 });
});

test("expands mirrored custom-emissive polygons and evaluates reverse and turn inputs", () => {
  const rear = { kind: "mesh", name: "REAR_GLASS", materialId: 0, children: [] };
  const scene = { materials: [material("rear")], root: { kind: "node", name: "root", children: [rear] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[INCLUDE: common/custom_emissive.ini]
[CustomEmissive]
Meshes=REAR_GLASS
Resolution=512,512
@=CustomEmissive_Poly, Channel=1, Mirror, P1="450,405", P2="310,430", P3="253,507", P4="388,508"
@=CustomEmissive_Poly, Channel=3, P1="130,510", P2="110,452", P3="308,428", P4="253,508"
@=ReverseLights, Channel=3, Intensity=0.2
@=TurningLightsRear, Channel=1, Intensity=0.8
`));
  const result = evaluateCspConfig(scene, config, { inputs: { REVERSE: 1, TURNSIGNAL_LEFT: 1, TURNSIGNAL_RIGHT: 0 } });
  const custom = result.nodeOverrides.get(rear).customEmissive;
  assert.ok(config.resolvedIncludes.has("custom_emissive.ini"));
  assert.equal(result.customEmissiveMeshes, 1);
  assert.deepEqual(custom.shapes.map((shape) => shape.channel), [1, 6, 3]);
  assert.deepEqual(custom.shapes[1].points[0], [62, 405]);
  assert.deepEqual(new Map(custom.channelColors).get(3), [8, 8, 8]);
  assert.deepEqual(new Map(custom.channelColors).get(1), [20, 9.600000000000001, 0]);
  assert.deepEqual(new Map(custom.channelColors).get(6), [0, 0, 0]);
  assert.deepEqual([...result.usedInputs.keys()].sort(), ["REVERSE", "TURNSIGNAL_LEFT", "TURNSIGNAL_RIGHT"]);
});

test("evaluates multi-item dashboard masks, inverse inputs, thresholds and diffuse alpha", () => {
  const decals = { kind: "mesh", name: "DASH_DECALS", materialId: 0, children: [] };
  const scene = { materials: [material("decals")], root: { kind: "node", name: "root", children: [decals] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[CustomEmissiveMulti]
Meshes=DASH_DECALS
Resolution=2048,1024
UseEmissive0AsFallback=COVER_ALL
DashHighlightColor=0.6,0,0
@=DashHighlight
@=AlphaFromTxDiffuse
@=MultiItem, Role=TRACTIONCONTROL, InputInverse, Start="1037.6,172.2", Size="123.8,126.1", Intensity=0.3
@=MultiItem, Role=HAZARD, Start="766.4,81.1", Size="112,109.6"
@=MultiItem, Role=RPM, InputThreshold=500, Start="1978,46.4", Size="58.1,80.9", Color="3,3,0"
`));
  const result = evaluateCspConfig(scene, config, { inputs: { LIGHT: 1, TRACTIONCONTROL: 0, HAZARD: 1, RPM: 600 } });
  const custom = result.nodeOverrides.get(decals).customEmissive, colors = new Map(custom.channelColors);
  assert.equal(custom.multi, true);
  assert.equal(custom.alphaFromDiffuse, true);
  assert.deepEqual(custom.shapes.map((shape) => shape.channel), [0, 1, 2, 3]);
  assert.deepEqual(colors.get(0), [0.6, 0, 0]);
  assert.deepEqual(colors.get(1), [6, 3.5999999999999996, 0]);
  assert.deepEqual(colors.get(2), [20, 12, 0]);
  assert.deepEqual(colors.get(3), [3, 3, 0]);
  assert.deepEqual(result.usedInputs.get("RPM"), { min: 0, max: 8000, value: 600 });
});

test("pairs repeated ellipsis adjustment keys with custom-emissive atlas channels", () => {
  const ambulance = { kind: "mesh", name: "AMBULANCE", materialId: 0, children: [] };
  const scene = { materials: [material("MB_Sprinter_2014")], root: { kind: "node", name: "root", children: [ambulance] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[CustomEmissive]
Materials=MB_Sprinter_2014
Resolution=1024,1024
@=CustomEmissive_Rect, Channel=0, Start="763,95", Size="102,44"
@=CustomEmissive_Rect, Channel=1, Start="914,95", Size="105,45"
[MATERIAL_ADJUSTMENT_...]
Materials=MB_Sprinter_2014
Key_...=ksEmissive
Value_...=255,255,255,0.3
Key_...=ksEmissive1
Value_...=255,0,0,0.2
Condition=AMBULANCE
`));
  const result = evaluateCspConfig(scene, config, { conditions: { AMBULANCE: 1 } });
  const override = result.nodeOverrides.get(ambulance), colors = new Map(override.customEmissive.channelColors);
  assert.deepEqual(override.properties.get("ksemissive"), [255, 255, 255, 0.3]);
  assert.deepEqual(override.properties.get("ksemissive1"), [255, 0, 0, 0.2]);
  assert.deepEqual(colors.get(0), [0.3, 0.3, 0.3]);
  assert.deepEqual(colors.get(1), [0.2, 0, 0]);
});

test("parses color, vertex, bounce-back and UV operations without executing Inipp", () => {
  const lamp = { kind: "mesh", name: "LAMP", materialId: 0, children: [] };
  const scene = { materials: [material("lamp")], root: { kind: "node", name: "root", children: [lamp] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[CustomEmissive]
Meshes=LAMP
ColorMasksAsMultiplier=1
MirrorDir=-1,0,0
MirrorOffset=0.25
@=CustomEmissive_Rect, Channel=1, Mirror, Start=0, Size=1
@=CustomEmissive_Color, Channel=1, Mirror, Color="1,0.5,0", Threshold="0.2,5", Opacity=0.75
@=CustomEmissive_VertexMask, Point2="0.5,0,0", Point3="0.8,0,0"
@=CustomEmissive_BounceBack, Mask="1,0,0,0", Intensity=2
@=CustomEmissive_BounceBack, Channel=1, Intensity=4
@=CustomEmissive_MirrorUV, Offset=0.6, Direction="-1,0"
@=CustomEmissive_SkipDiffuseMap
@=TurningLightsRear, Channel=1
`));
  const result = evaluateCspConfig(scene, config, { inputs: { TURNSIGNAL_LEFT: 1, TURNSIGNAL_RIGHT: 0 } });
  const custom = result.nodeOverrides.get(lamp).customEmissive;
  assert.equal(custom.colorMasksAsMultiplier, true);
  assert.deepEqual(custom.mirrorDirection, [-1, 0, 0]);
  assert.equal(custom.mirrorOffset, 0.25);
  assert.deepEqual(custom.colorMasks.map((mask) => [mask.channel, mask.mirrorSide]), [[1, -1], [6, 1]]);
  assert.equal(custom.colorMasks[0].thresholdLevel, 0.2);
  assert.equal(custom.colorMasks[0].thresholdSharpness, 5);
  assert.deepEqual(custom.vertexMask.points, [null, null, [0.5, 0, 0], [0.8, 0, 0]]);
  assert.deepEqual(custom.bounceBack, [{ mask: [0, 1, 0, 0], intensity: 4 }]);
  assert.deepEqual(custom.mirrorUv, { offset: 0.6, direction: [1, 0] });
  assert.equal(custom.useRawUv, false);
  assert.equal(custom.skipDiffuseMap, true);
  assert.deepEqual(custom.unsupportedOperations, []);
  assert.deepEqual(custom.approximatedOperations, ["CustomEmissive_VertexMask"]);
});

test("normalizes the installed MirrorUV rule and bounds malformed directions", () => {
  const display = { kind: "mesh", name: "DISPLAY", materialId: 0, children: [] };
  const malformed = { kind: "mesh", name: "BAD", materialId: 0, children: [] };
  const local = { kind: "mesh", name: "LOCAL", materialId: 0, children: [] };
  const scene = { materials: [material("display")], root: { kind: "node", name: "root", children: [display, malformed, local] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[CustomEmissive]
Meshes=DISPLAY
Resolution=1024,512
UseRawUV=1
@=CustomEmissive_MirrorUV, Offset=612, Direction="-1,0,99"

[CustomEmissive]
Meshes=BAD
Resolution=1024,512
@=CustomEmissive_MirrorUV, Offset=invalid, Direction="nan,inf"

[CustomEmissive]
Meshes=LOCAL
@=CustomEmissive_MirrorUV, Offset=512, Direction="-1,0"
@MIXIN=CustomEmissive_Rect, Resolution="1024,512", Channel=0, Start="0,0", Size="1,1"
`));
  const result = evaluateCspConfig(scene, config);
  const custom = result.nodeOverrides.get(display).customEmissive;
  assert.equal(custom.useRawUv, true);
  assert.deepEqual(custom.mirrorUv, { offset: 612 / 1024, direction: [1, 0] });
  assert.deepEqual(custom.approximatedOperations, ["USERAWUV"]);
  const bounded = result.nodeOverrides.get(malformed).customEmissive;
  assert.deepEqual(bounded.mirrorUv, { offset: 0.5 / 1024, direction: [-1, 0] });
  assert.ok(bounded.mirrorUv.direction.every(Number.isFinite));
  assert.deepEqual(bounded.approximatedOperations, []);
  assert.deepEqual(result.nodeOverrides.get(local).customEmissive.mirrorUv, { offset: 0.5, direction: [1, 0] });
});

test("bounds a malformed bounce-back mask to four finite channels", () => {
  const lamp = { kind: "mesh", name: "LAMP", materialId: 0, children: [] };
  const scene = { materials: [material("lamp")], root: { kind: "node", name: "root", children: [lamp] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[CustomEmissive]
Meshes=LAMP
@=CustomEmissive_BounceBack, Mask="1,broken"
`));
  const custom = evaluateCspConfig(scene, config).nodeOverrides.get(lamp).customEmissive;
  assert.deepEqual(custom.bounceBack, [{ mask: [1, 1, 1, 1], intensity: 20 }]);
});

test("keeps normalized emissive atlases editable and bounds authored atlases", () => {
  assert.deepEqual(customEmissiveAtlasSize([1, 1]), { sourceWidth: 1, sourceHeight: 1, width: 512, height: 512, normalized: true });
  assert.deepEqual(customEmissiveAtlasSize([2, 1]), { sourceWidth: 2, sourceHeight: 1, width: 512, height: 256, normalized: true });
  assert.deepEqual(customEmissiveAtlasSize([2048, 1024]), { sourceWidth: 2048, sourceHeight: 1024, width: 1024, height: 512, normalized: false });
});

test("expands installed fog, door, rear-mask and light-material helper operations", () => {
  const lamp = { kind: "mesh", name: "LAMP", materialId: 0, children: [] };
  const scene = { materials: [material("lamp")], root: { kind: "node", name: "root", children: [lamp] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[CustomEmissive]
Meshes=LAMP
@=LightMaterial
@=RearLightsMask
@=UseAlphaFromTxDiffuses
@=FogLightsRear, Channel=2, Intensity=2
@=OpenDoorLight, Channel="0,1"
@=SetTxNormal
`));
  const result = evaluateCspConfig(scene, config, { inputs: { EXTRA_A: 1, OPEN_DOORS: 1 } });
  const override = result.nodeOverrides.get(lamp), custom = override.customEmissive, colors = new Map(custom.channelColors);
  assert.equal(custom.shapes.length, 3);
  assert.equal(custom.alphaFromDiffuse, true);
  assert.equal(custom.skipDiffuseMap, true);
  assert.deepEqual(colors.get(0), [16, 15.6, 6]);
  assert.deepEqual(colors.get(1), [16, 15.6, 6]);
  assert.deepEqual(colors.get(2), [40, 0, 0]);
  assert.equal(override.properties.get("ksspecularexp"), 200);
  assert.equal(override.properties.get("extcoloredbasereflection"), 1);
  assert.deepEqual(custom.approximatedOperations, ["FogLightsRear", "OpenDoorLight", "SetTxNormal"]);
  assert.deepEqual(custom.unsupportedOperations, []);
});

test("accepts declarative @MIXIN light operations with a local atlas resolution", () => {
  const lamp = { kind: "mesh", name: "LAMP", materialId: 0, children: [] };
  const scene = { materials: [material("lamp")], root: { kind: "node", name: "root", children: [lamp] } };
  const config = expandCspMaterialTemplates(parseCspIni(`[CustomEmissive]
Meshes=LAMP
AreasSubtractNext=1
@MIXIN=CustomEmissive_Rect, Channel=3, Mirror, Resolution=512, Start="300,0", Size="148,108"
@MIXIN=CustomEmissive_Color, Channel=3, Mirror, Color="1,0.5,0"
@MIXIN=TurningLights, Channel=3
`));
  const result = evaluateCspConfig(scene, config, { inputs: { TURNSIGNAL_LEFT: 1 } });
  const custom = result.nodeOverrides.get(lamp).customEmissive;
  assert.deepEqual(custom.resolution, [512, 512]);
  assert.deepEqual(custom.shapes.map((shape) => shape.channel), [3, 4]);
  assert.deepEqual(custom.colorMasks.map((mask) => mask.channel), [3, 4]);
  assert.deepEqual([...result.usedInputs.keys()].sort(), ["TURNSIGNAL_LEFT", "TURNSIGNAL_RIGHT"]);
  assert.deepEqual(custom.approximatedOperations, ["AREASSUBTRACTNEXT"]);
  assert.deepEqual(custom.unsupportedOperations, []);
});
