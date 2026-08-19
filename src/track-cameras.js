import { lastValue, parseCspIni } from "./csp-config.js";

function section(config,name){return config.sections.find((candidate)=>candidate.name.toUpperCase()===name);}
function finite(sectionValue,fallback,warnings,label){const value=Number(sectionValue);if(Number.isFinite(value))return value;warnings.push(`${label} must be finite`);return fallback;}
function vector(value,fallback,warnings,label){const parts=String(value||"").split(",").map((part)=>Number(part.trim()));if(parts.length===3&&parts.every(Number.isFinite))return parts;warnings.push(`${label} must contain three finite numbers`);return [...fallback];}
function length(value){return Math.hypot(...value);}
function dot(a,b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
const SPLINE_ENDPOINT_FACTOR=Math.fround(0.9990000128746033);

export function parseCameraSplineCsv(text,source="camera spline.csv"){
  const points=[],warnings=[];let comma=null,length=0;
  for(const [index,raw]of String(text||"").split(/\r?\n/).entries()){const line=raw.trim();if(!line)continue;if(comma===null)comma=line.includes(",");const parts=(comma?line.split(","):line.split(/\s+/)).map((part)=>Number(part.trim()));if(parts.length!==3||parts.some((value)=>!Number.isFinite(value))){warnings.push(`${source}:${index+1}: expected three finite coordinates`);continue;}if(points.length)length+=Math.hypot(parts[0]-points.at(-1)[0],parts[1]-points.at(-1)[1],parts[2]-points.at(-1)[2]);points.push(parts);}
  if(!points.length)warnings.push(`${source}: no valid spline points were found`);else if(points.length===1)warnings.push(`${source}: one spline point cannot describe camera motion`);
  return {source,points,length,warnings};
}

export function rotateCameraSpline(points,degrees=0){const angle=(Number(degrees)||0)*Math.PI/180,c=Math.cos(angle),s=Math.sin(angle);return (points||[]).map(([x,y,z])=>[x*c+z*s,y,-x*s+z*c]);}

export function sampleCameraSpline(points,position=0){const values=points||[];if(!values.length)return [0,0,0];if(values.length===1)return [...values[0]];const t=Math.max(0,Math.min(1,Number(position)||0)),last=values.length-1,index=Math.trunc(last*t*SPLINE_ENDPOINT_FACTOR),blend=(t*SPLINE_ENDPOINT_FACTOR-index/last)/(1/values.length),next=(index+1)%values.length;return values[index].map((value,axis)=>value+(values[next][axis]-value)*blend);}

export function parseTrackCamerasIni(text,source="data/cameras.ini"){
  const config=parseCspIni(text,source),warnings=config.warnings.map((warning)=>`${warning.source}:${warning.line}: ${warning.message}`),header=section(config,"HEADER"),declaredCount=header?finite(lastValue(header,"CAMERA_COUNT"),0,warnings,`${source}:${header.line}: CAMERA_COUNT`):0,sections=new Map();
  if(!header)warnings.push(`${source}: HEADER section is missing`);
  for(const candidate of config.sections){const match=candidate.name.match(/^CAMERA_(\d+)$/i);if(!match)continue;const index=Number(match[1]);if(sections.has(index))warnings.push(`${source}:${candidate.line}: duplicate CAMERA_${index} replaces the earlier section`);sections.set(index,candidate);}
  const cameras=[];
  for(let index=0;sections.has(index);index++){const camera=sections.get(index),label=`${source}:${camera.line}: CAMERA_${index}`,position=vector(lastValue(camera,"POSITION"),[0,0,0],warnings,`${label} POSITION`),forward=vector(lastValue(camera,"FORWARD"),[0,0,-1],warnings,`${label} FORWARD`),up=vector(lastValue(camera,"UP"),[0,1,0],warnings,`${label} UP`),minFov=finite(lastValue(camera,"MIN_FOV"),45,warnings,`${label} MIN_FOV`),maxFov=finite(lastValue(camera,"MAX_FOV"),minFov,warnings,`${label} MAX_FOV`),inPoint=finite(lastValue(camera,"IN_POINT"),-1,warnings,`${label} IN_POINT`),outPoint=finite(lastValue(camera,"OUT_POINT"),-1,warnings,`${label} OUT_POINT`),nearPlane=finite(lastValue(camera,"NEAR_PLANE"),.1,warnings,`${label} NEAR_PLANE`),farPlane=finite(lastValue(camera,"FAR_PLANE"),10000,warnings,`${label} FAR_PLANE`);
    if(Math.abs(length(forward)-1)>.02)warnings.push(`${label} FORWARD should be normalized`);if(Math.abs(length(up)-1)>.02)warnings.push(`${label} UP should be normalized`);if(Math.abs(dot(forward,up))>.02)warnings.push(`${label} FORWARD and UP should be perpendicular`);if(minFov<=0||maxFov<=0||minFov>maxFov)warnings.push(`${label} FOV range is invalid`);if(!((inPoint===-1&&outPoint===-1)||(inPoint>=0&&outPoint>=inPoint&&outPoint<=1)))warnings.push(`${label} lap interval must be -1/-1 or ordered from 0 to 1`);if(nearPlane<=0||farPlane<=nearPlane)warnings.push(`${label} clip planes are invalid`);
    cameras.push({index,name:lastValue(camera,"NAME",`Camera ${index}`).trim()||`Camera ${index}`,position,forward,up,minFov,maxFov,inPoint,outPoint,nearPlane,farPlane,minExposure:finite(lastValue(camera,"MIN_EXPOSURE"),0,warnings,`${label} MIN_EXPOSURE`),maxExposure:finite(lastValue(camera,"MAX_EXPOSURE"),0,warnings,`${label} MAX_EXPOSURE`),dofFactor:finite(lastValue(camera,"DOF_FACTOR"),0,warnings,`${label} DOF_FACTOR`),dofRange:finite(lastValue(camera,"DOF_RANGE"),0,warnings,`${label} DOF_RANGE`),dofFocus:finite(lastValue(camera,"DOF_FOCUS"),0,warnings,`${label} DOF_FOCUS`),dofManual:Number(lastValue(camera,"DOF_MANUAL"))!==0,spline:lastValue(camera,"SPLINE").trim(),splineRotation:finite(lastValue(camera,"SPLINE_ROTATION"),0,warnings,`${label} SPLINE_ROTATION`),splineAnimationLength:finite(lastValue(camera,"SPLINE_ANIMATION_LENGTH"),0,warnings,`${label} SPLINE_ANIMATION_LENGTH`),fovGamma:finite(lastValue(camera,"FOV_GAMMA"),1,warnings,`${label} FOV_GAMMA`),fixed:Number(lastValue(camera,"IS_FIXED"))!==0,line:camera.line});}
  for(const index of [...sections.keys()].sort((a,b)=>a-b))if(index>=cameras.length)warnings.push(`${source}: CAMERA_${index} is ignored because CAMERA_${cameras.length} is missing`);
  if(declaredCount!==cameras.length)warnings.push(`${source}: HEADER declares ${declaredCount} cameras but ${cameras.length} contiguous sections were found`);
  return {source,version:header?finite(lastValue(header,"VERSION"),0,warnings,`${source}:${header.line}: VERSION`):0,name:header?lastValue(header,"SET_NAME").trim():"",declaredCount,cameras,warnings,ignoredSections:config.sections.length-sections.size-(header?1:0)};
}
