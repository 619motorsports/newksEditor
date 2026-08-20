import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { mergeKn5Models, parseModelsIni } from "../src/kn5-workspace.js";
import { parseKn5 } from "../src/kn5.js";
import { auditTrackModel, parseSurfacesIni, resolveTrackSurface, serializeSurfacesIni } from "../src/track-validation.js";

test("parses stock surface physics and diagnoses malformed values",()=>{const parsed=parseSurfacesIni("[SURFACE_0]\nKEY=ROAD\nFRICTION=0.98\nIS_VALID_TRACK=1\n[SURFACE_1]\nKEY=ROAD\nDAMPING=nope\n[SURFACE_1]\nKEY=GRASS");assert.equal(parsed.surfaces[0].friction,.98);assert.equal(parsed.surfaces[0].isValidTrack,true);assert.ok(parsed.warnings.some((warning)=>/duplicate surface KEY/.test(warning)));assert.ok(parsed.warnings.some((warning)=>/duplicate SURFACE_1/.test(warning)));assert.ok(parsed.warnings.some((warning)=>/DAMPING must be finite/.test(warning)));});

test("round-trips all authored surface physics fields", () => {
  const config = { surfaces: [{ index: 4, key: "TARMAC", friction: 1.05, damping: 0.02, dirtAdditive: 0.1, blackFlagTime: 3, isValidTrack: true, isPitlane: false, sinHeight: 0.001, sinLength: 2.5, vibrationGain: 0.15, vibrationLength: 0.4, wav: "road.wav", wavPitch: 1.2, ffEffect: "GRAIN" }] };
  const text = serializeSurfacesIni(config), parsed = parseSurfacesIni(text, "exported-surfaces.ini");
  assert.deepEqual(parsed.warnings, []);
  assert.deepEqual(parsed.surfaces[0], { ...config.surfaces[0], section: "SURFACE_4", line: 1 });
  assert.throws(() => serializeSurfacesIni({ surfaces: [] }), /at least one surface/);
  assert.throws(() => serializeSurfacesIni({ surfaces: [{ ...config.surfaces[0], wav: "bad\nname.wav" }] }), /line break/);
  assert.throws(() => serializeSurfacesIni({ surfaces: [{ ...config.surfaces[0], key: "ROAD;HIDDEN" }] }), /comment marker/);
});

test("audits starts, pits, timing pairs, hotlap, and runtime surface names",()=>{const leaf=(name,kind="node")=>({name,kind,children:[]}),root={name:"root",kind:"node",children:[leaf("AC_START_0"),leaf("AC_START_2"),leaf("AC_PIT_0"),leaf("AC_TIME_0_L"),leaf("AC_HOTLAP_START_0"),leaf("1ROAD_main","mesh"),leaf("01ROAD_alt","mesh"),leaf("1GRASS","mesh"),leaf("2MUD","mesh"),leaf("0ROAD_visual","mesh")]};const surfaces=parseSurfacesIni("[SURFACE_0]\nKEY=ROAD\n[SURFACE_1]\nKEY=KERB");const audit=auditTrackModel({root},surfaces);assert.equal(audit.starts,2);assert.equal(audit.runtimeSurfaces,5);assert.equal(audit.surfaceMatches.find((surface)=>surface.key==="ROAD").count,2);assert.equal(audit.surfaceMatches.find((surface)=>surface.key==="GRASS").count,1);assert.deepEqual(audit.unmatchedSurfaces,["KERB"]);assert.deepEqual(audit.unmatchedPhysical,["2MUD"]);assert.ok(audit.findings.some((item)=>/Starting-grid indices have gaps: 1/.test(item.message)));assert.ok(audit.findings.some((item)=>/missing its R endpoint/.test(item.message)));});

test("reports the game's ambiguous substring surface matches",()=>{const leaf=(name,kind="node")=>({name,kind,children:[]}),root={name:"root",kind:"node",children:[leaf("1ROAD_MAIN","mesh")]};const surfaces=parseSurfacesIni("[SURFACE_0]\nKEY=ROAD_MAIN");const audit=auditTrackModel({root},surfaces);assert.deepEqual(audit.ambiguousPhysical,[{name:"1ROAD_MAIN",keys:["ROAD","ROAD_MAIN"]}]);assert.ok(audit.findings.some((item)=>item.severity==="error"&&/match multiple runtime surface keys/.test(item.message)));});

test("resolves individual meshes for spatial surface overlays",()=>{const surfaces=parseSurfacesIni("[SURFACE_0]\nKEY=TARMAC-A","track/data/surfaces.ini");assert.equal(resolveTrackSurface("21TARMAC-A019",surfaces).surface.key,"TARMAC-A");assert.equal(resolveTrackSurface("0ROAD_visual",surfaces).status,"not-physics");assert.equal(resolveTrackSurface("2MUD",surfaces).status,"fallback");});

test("audits a complete installed unpacked track layout",async(t)=>{const base="/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/tracks/nukedrop_rainier_raceway";let manifestText,surfaceText;try{[manifestText,surfaceText]=await Promise.all([readFile(`${base}/models_layout_gp.ini`,"utf8"),readFile(`${base}/layout_gp/data/surfaces.ini`,"utf8")]);}catch{t.skip("Unpacked track validation fixture is not installed");return;}const manifest=parseModelsIni(manifestText),entries=[];for(const item of manifest.models){const bytes=await readFile(`${base}/${item.file}`);entries.push({name:item.file,size:bytes.length,model:parseKn5(bytes),position:item.position,rotation:item.rotation});}const audit=auditTrackModel(mergeKn5Models(entries),parseSurfacesIni(surfaceText));assert.equal(audit.starts,52);assert.equal(audit.pits,52);assert.equal(audit.timeGates,3);assert.equal(audit.hotlap,true);assert.equal(audit.errors,0);assert.ok(audit.surfaceMatches.find((surface)=>surface.key==="KERB").count>0);});
