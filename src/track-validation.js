import { lastValue, parseCspIni } from "./csp-config.js";
import { walkNodes } from "./kn5.js";

function number(section,key,fallback,warnings,source){const raw=lastValue(section,key);if(raw==="")return fallback;const value=Number(raw);if(Number.isFinite(value))return value;warnings.push(`${source}:${section.line}: ${section.name} ${key} must be finite`);return fallback;}

export function parseSurfacesIni(text,source="data/surfaces.ini"){
  const config=parseCspIni(text,source),warnings=config.warnings.map((warning)=>`${warning.source}:${warning.line}: ${warning.message}`),surfaces=[],keys=new Set(),indices=new Set();
  for(const section of config.sections){const match=section.name.match(/^SURFACE_(\d+)$/i);if(!match)continue;const index=Number(match[1]);if(indices.has(index))warnings.push(`${source}:${section.line}: duplicate SURFACE_${index} section`);indices.add(index);const key=lastValue(section,"KEY").trim();if(!key){warnings.push(`${source}:${section.line}: ${section.name} has no KEY`);continue;}const normalized=key.toUpperCase();if(keys.has(normalized))warnings.push(`${source}:${section.line}: duplicate surface KEY ${key}`);keys.add(normalized);surfaces.push({index,key,friction:number(section,"FRICTION",1,warnings,source),damping:number(section,"DAMPING",0,warnings,source),dirtAdditive:number(section,"DIRT_ADDITIVE",0,warnings,source),blackFlagTime:number(section,"BLACK_FLAG_TIME",0,warnings,source),isValidTrack:number(section,"IS_VALID_TRACK",0,warnings,source)!==0,isPitlane:number(section,"IS_PITLANE",0,warnings,source)!==0,sinHeight:number(section,"SIN_HEIGHT",0,warnings,source),sinLength:number(section,"SIN_LENGTH",0,warnings,source),vibrationGain:number(section,"VIBRATION_GAIN",0,warnings,source),vibrationLength:number(section,"VIBRATION_LENGTH",0,warnings,source),wav:lastValue(section,"WAV").trim(),wavPitch:number(section,"WAV_PITCH",0,warnings,source),ffEffect:lastValue(section,"FF_EFFECT").trim(),section:section.name,line:section.line});}
  surfaces.sort((a,b)=>a.index-b.index);return {source,surfaces,warnings,ignoredSections:config.sections.length-surfaces.length};
}

function surfaceNumber(value, label) {
  const number = Number(value);
  if (!Number.isFinite(number)) throw new TypeError(`${label} must be finite`);
  return Number(number.toFixed(6)).toString();
}

function surfaceText(value, label, required = false) {
  const text = String(value || "").trim();
  if (required && !text) throw new TypeError(`${label} cannot be empty`);
  if (/\r|\n/.test(text)) throw new TypeError(`${label} cannot contain a line break`);
  if (text.includes(";")) throw new TypeError(`${label} cannot contain an INI comment marker`);
  return text;
}

export function serializeSurfacesIni(config) {
  const surfaces = Array.isArray(config) ? config : config?.surfaces;
  if (!Array.isArray(surfaces) || !surfaces.length) throw new TypeError("A surface manifest needs at least one surface");
  const used = new Set(), sections = [];
  for (let position = 0; position < surfaces.length; position++) {
    const surface = surfaces[position], index = Number.isInteger(surface.index) && surface.index >= 0 ? surface.index : position;
    if (used.has(index)) throw new TypeError(`SURFACE_${index} is duplicated`);
    used.add(index);
    sections.push([
      `[SURFACE_${index}]`,
      `KEY=${surfaceText(surface.key, "KEY", true)}`,
      `FRICTION=${surfaceNumber(surface.friction, "FRICTION")}`,
      `DAMPING=${surfaceNumber(surface.damping, "DAMPING")}`,
      `DIRT_ADDITIVE=${surfaceNumber(surface.dirtAdditive, "DIRT_ADDITIVE")}`,
      `BLACK_FLAG_TIME=${surfaceNumber(surface.blackFlagTime, "BLACK_FLAG_TIME")}`,
      `IS_VALID_TRACK=${surface.isValidTrack ? 1 : 0}`,
      `IS_PITLANE=${surface.isPitlane ? 1 : 0}`,
      `SIN_HEIGHT=${surfaceNumber(surface.sinHeight, "SIN_HEIGHT")}`,
      `SIN_LENGTH=${surfaceNumber(surface.sinLength, "SIN_LENGTH")}`,
      `VIBRATION_GAIN=${surfaceNumber(surface.vibrationGain, "VIBRATION_GAIN")}`,
      `VIBRATION_LENGTH=${surfaceNumber(surface.vibrationLength, "VIBRATION_LENGTH")}`,
      `WAV=${surfaceText(surface.wav, "WAV")}`,
      `WAV_PITCH=${surfaceNumber(surface.wavPitch, "WAV_PITCH")}`,
      `FF_EFFECT=${surfaceText(surface.ffEffect, "FF_EFFECT")}`
    ].join("\n"));
  }
  return `${sections.join("\n\n")}\n`;
}

function numbered(names,prefix){return [...names].map((name)=>name.match(new RegExp(`^${prefix}_(\\d+)$`,"i"))).filter(Boolean).map((match)=>Number(match[1])).sort((a,b)=>a-b);}
function gaps(values){if(!values.length)return [];const set=new Set(values),missing=[];for(let index=0;index<=values.at(-1);index++)if(!set.has(index))missing.push(index);return missing;}
const STOCK_RUNTIME_SURFACES=[
  {key:"WALL",origin:"Built-in"},
  {key:"ROAD",origin:"system/data/surfaces.ini"},
  {key:"GRASS",origin:"system/data/surfaces.ini"},
  {key:"KERB",origin:"system/data/surfaces.ini"},
  {key:"SAND",origin:"system/data/surfaces.ini"}
];

function physicsSectorId(name){const match=String(name||"").match(/^\s*([+-]?\d+)/);if(!match)return 0;const value=Number(match[1]);return Number.isSafeInteger(value)?value:0;}
function runtimeSurfaces(surfaceConfig){const merged=new Map(STOCK_RUNTIME_SURFACES.map((surface)=>[surface.key,{...surface,normalized:surface.key}]));for(const surface of surfaceConfig?.surfaces||[]){const normalized=surface.key.toUpperCase();merged.set(normalized,{...surface,origin:surfaceConfig.source,normalized});}return [...merged.values()];}
function resolveSurface(name,configured){const sectorId=physicsSectorId(name);if(sectorId===0)return {status:"not-physics",sectorId,surface:null,candidates:[]};const normalized=String(name||"").toUpperCase(),candidates=configured.filter((surface)=>normalized.includes(surface.normalized));return candidates.length===1?{status:"matched",sectorId,surface:candidates[0],candidates}:candidates.length===0?{status:"fallback",sectorId,surface:null,candidates}: {status:"ambiguous",sectorId,surface:null,candidates};}

export function resolveTrackSurface(name,surfaceConfig=null){return resolveSurface(name,runtimeSurfaces(surfaceConfig));}

export function auditTrackModel(model,surfaceConfig=null){
  const nodes=walkNodes(model.root).map(({node})=>node),names=new Set(nodes.map((node)=>node.name)),meshes=nodes.filter((node)=>node.kind==="mesh"||node.kind==="skinnedMesh"),findings=[];
  const starts=numbered(names,"AC_START"),pits=numbered(names,"AC_PIT"),startGaps=gaps(starts),pitGaps=gaps(pits);
  if(!starts.includes(0))findings.push({severity:"error",message:"AC_START_0 is missing"});if(startGaps.length)findings.push({severity:"warning",message:`Starting-grid indices have gaps: ${startGaps.join(", ")}`});
  if(!pits.includes(0))findings.push({severity:"warning",message:"AC_PIT_0 is missing"});if(pitGaps.length)findings.push({severity:"warning",message:`Pit indices have gaps: ${pitGaps.join(", ")}`});
  const timeSides=new Map();for(const name of names){const match=name.match(/^AC_TIME_(\d+)_([LR])$/i);if(match){const index=Number(match[1]),sides=timeSides.get(index)||new Set();sides.add(match[2].toUpperCase());timeSides.set(index,sides);}}
  if(!timeSides.has(0))findings.push({severity:"error",message:"AC_TIME_0_L/R timing gate is missing"});for(const [index,sides]of [...timeSides].sort(([a],[b])=>a-b))if(!sides.has("L")||!sides.has("R"))findings.push({severity:"error",message:`AC_TIME_${index} is missing its ${sides.has("L")?"R":"L"} endpoint`});
  const hotlap=names.has("AC_HOTLAP_START_0");if(!hotlap)findings.push({severity:"warning",message:"AC_HOTLAP_START_0 is missing"});
  const configured=runtimeSurfaces(surfaceConfig),resolvedCounts=new Map(configured.map((surface)=>[surface.normalized,0])),unmatchedPhysical=[],ambiguousPhysical=[];
  for(const mesh of meshes){const resolution=resolveSurface(mesh.name,configured);if(resolution.status==="matched")resolvedCounts.set(resolution.surface.normalized,resolvedCounts.get(resolution.surface.normalized)+1);else if(resolution.status==="fallback")unmatchedPhysical.push(mesh.name);else if(resolution.status==="ambiguous")ambiguousPhysical.push({name:mesh.name,keys:resolution.candidates.map((surface)=>surface.key)});}
  const surfaceMatches=configured.map((surface)=>({key:surface.key,count:resolvedCounts.get(surface.normalized),origin:surface.origin})),localKeys=new Set((surfaceConfig?.surfaces||[]).map((surface)=>surface.key.toUpperCase())),unmatchedSurfaces=surfaceMatches.filter((surface)=>localKeys.has(surface.key.toUpperCase())&&surface.count===0).map((surface)=>surface.key);
  if(unmatchedSurfaces.length)findings.push({severity:"warning",message:`Configured surfaces without uniquely matching physical meshes: ${unmatchedSurfaces.join(", ")}`});if(unmatchedPhysical.length)findings.push({severity:"warning",message:`${unmatchedPhysical.length} physics meshes fall back because no runtime surface key matches`});if(ambiguousPhysical.length)findings.push({severity:"error",message:`${ambiguousPhysical.length} physics meshes match multiple runtime surface keys`});
  return {starts:starts.length,pits:pits.length,timeGates:timeSides.size,hotlap,runtimeSurfaces:configured.length,surfaceMatches,unmatchedSurfaces,unmatchedPhysical:[...new Set(unmatchedPhysical)].sort(),ambiguousPhysical,findings,errors:findings.filter((item)=>item.severity==="error").length,warnings:findings.filter((item)=>item.severity==="warning").length};
}
