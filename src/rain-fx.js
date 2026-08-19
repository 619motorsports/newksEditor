import { lastValue, matchesSelector, parseCspValue } from "./csp-config.js";
import { walkNodes } from "./kn5.js";

export const RAIN_FX_BITS={puddles:1,soaking:2,smooth:4,rough:8,lines:16,relief:32};

function enabled(section){const value=Number(lastValue(section,"ACTIVE","1"));return !Number.isFinite(value)||value!==0;}
function vector(value){const parsed=parseCspValue(value);return Array.isArray(parsed)&&parsed.every(Number.isFinite)?parsed.map(Number):[];}

export function parseRainFx(config){
  const sections=(config?.sections||[]).filter((section)=>/^RAIN_FX$/i.test(section.name)&&enabled(section));
  if(!sections.length)return null;
  const section=sections.at(-1),streams=[],warnings=[];
  for(const entry of section.entries){
    const key=entry.key.replace(/\s+/g,"").toUpperCase(),type=key.startsWith("STREAM_WALL_EDGE")?"wall-edge":key.startsWith("STREAM_EDGE")?"edge":key.startsWith("STREAM_POINT")?"point":null;
    if(!type)continue;
    const values=vector(entry.value),expected=type==="point"?3:6;
    if(values.length!==expected){warnings.push({source:section.source,line:entry.line,message:`${entry.key} expects ${expected} finite coordinates`});continue;}
    streams.push(type==="point"?{type,point:values,source:section.source,line:entry.line}:{type,from:values.slice(0,3),to:values.slice(3,6),source:section.source,line:entry.line});
  }
  return {source:section.source,line:section.line,puddlesMaterials:lastValue(section,"PUDDLES_MATERIALS"),puddlesMeshes:lastValue(section,"PUDDLES_MESHES"),soakingMaterials:lastValue(section,"SOAKING_MATERIALS"),soakingMeshes:lastValue(section,"SOAKING_MESHES"),smoothMaterials:lastValue(section,"SMOOTH_MATERIALS"),smoothMeshes:lastValue(section,"SMOOTH_MESHES"),roughMaterials:lastValue(section,"ROUGH_MATERIALS"),roughMeshes:lastValue(section,"ROUGH_MESHES"),linesMaterials:lastValue(section,"LINES_MATERIALS"),linesMeshes:lastValue(section,"LINES_MESHES"),linesFilterMaterials:lastValue(section,"LINES_FILTER_MATERIALS"),reliefMaterials:lastValue(section,"RELIEF_MATERIALS"),reliefMeshes:lastValue(section,"RELIEF_MESHES"),streams,warnings};
}

function selected(settings,category,node,material){const materials=settings[`${category}Materials`],meshes=settings[`${category}Meshes`];return Boolean((materials&&matchesSelector(materials,node,material,"material"))||(meshes&&matchesSelector(meshes,node,material,"mesh")));}

export function evaluateRainFx(model,config){
  const settings=parseRainFx(config);if(!settings)return null;
  const nodeBindings=new Map(),counts=Object.fromEntries(Object.keys(RAIN_FX_BITS).map((name)=>[name,0]));let matchedMeshes=0;
  for(const {node}of walkNodes(model.root)){
    if(node.kind!=="mesh"&&node.kind!=="skinnedMesh")continue;
    const material=model.materials[node.materialId],categories=[];let bits=0;
    for(const [category,bit]of Object.entries(RAIN_FX_BITS))if(selected(settings,category,node,material)){categories.push(category);bits|=bit;counts[category]++;}
    const lineFilter=Boolean(settings.linesFilterMaterials&&matchesSelector(settings.linesFilterMaterials,node,material,"material"));
    if(bits||lineFilter){nodeBindings.set(node,{bits,categories,lineFilter,material:material?.name||"",mesh:node.name});matchedMeshes++;}
  }
  return {...settings,nodeBindings,counts,matchedMeshes,streamEdges:settings.streams.filter((stream)=>stream.type!=="point").length,streamPoints:settings.streams.filter((stream)=>stream.type==="point").length};
}
