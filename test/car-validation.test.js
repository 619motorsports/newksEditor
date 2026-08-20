import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import { auditCarCollider, auditCarHierarchy, parseBottomCollidersIni, requiredCarNodes } from "../src/car-validation.js";
import { parseKn5 } from "../src/kn5.js";
import { mergeKn5Models, parseCarLodsIni } from "../src/kn5-workspace.js";
import { carColliderKn5, carFixtureRoot, carMainKn5 } from "./fixture-paths.js";

function colliderModel({open=false,shader="GL",textures=0,transform=null}={}){const vertices=new Float32Array([[0,0,0],[1,0,0],[0,1,0],[0,0,1]].flatMap((position)=>[...position,0,1,0,0,0,1,0,0])),indices=new Uint16Array(open?[0,2,1]:[0,2,1,0,1,3,0,3,2,1,2,3]);return {version:6,textures:Array.from({length:textures},(_,index)=>({name:`${index}.dds`})),materials:[{name:"GL",shader}],root:{kind:"node",name:"root",active:true,transform:transform||[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],children:[{kind:"mesh",name:"collider",active:true,vertices,vertexStride:11,indices,materialId:0,children:[]}]}};}

test("accepts a closed, origin-aligned, untextured GL collider",()=>{const audit=auditCarCollider(colliderModel());assert.equal(audit.errors,0);assert.equal(audit.topology.closed,true);assert.deepEqual(audit.bounds.size,[1,1,1]);});

test("reports unsafe collider material, textures, topology, and transform",()=>{const transform=[1,0,0,0,0,1,0,0,0,0,1,0,.5,0,0,1],audit=auditCarCollider(colliderModel({open:true,shader:"ksPerPixel",textures:1,transform}));assert.equal(audit.errors,3);assert.equal(audit.warnings,1);assert.match(audit.findings.map((item)=>item.message).join("\n"),/no textures[\s\S]*GL collision shader[\s\S]*not a closed manifold[\s\S]*pivot/);});

test("audits the repository car collider",async()=>{const [collider,car]=await Promise.all([readFile(carColliderKn5),readFile(carMainKn5)]),audit=auditCarCollider(parseKn5(collider),parseKn5(car));assert.equal(audit.triangles,60);assert.equal(audit.textures,0);assert.equal(audit.topology.closed,true);assert.equal(audit.errors,0);});

test("parses ordered bottom collision boxes and diagnoses invalid sections",()=>{const parsed=parseBottomCollidersIni(`[COLLIDER_0]\nCENTRE=0,-0.2,0\nSIZE=1.8,0.1,3.8\nGROUND_ENABLE=1\n[COLLIDER_2]\nCENTRE=bad\nSIZE=1,-1,2\n`);assert.equal(parsed.colliders.length,1);assert.deepEqual(parsed.colliders[0].bounds,{min:[-.9,-.25,-1.9],max:[.9,-.15000000000000002,1.9]});assert.match(parsed.warnings.join("\n"),/CENTRE must contain three finite numbers/);});

test("parses the repository car's bottom collider",async()=>{const path=join(carFixtureRoot,"data","colliders.ini"),parsed=parseBottomCollidersIni(await readFile(path,"utf8"),path);assert.equal(parsed.colliders.length,1);assert.deepEqual(parsed.colliders[0].centre,[0,-.29,.66]);assert.deepEqual(parsed.colliders[0].size,[1.9,.05,3.355]);assert.deepEqual(parsed.warnings,[]);});

function hierarchyModel(omit=""){const positions={LF:[.8,.35,1.3],LR:[.8,.35,-1.2],RF:[-.8,.35,1.3],RR:[-.8,.35,-1.2]},children=requiredCarNodes.filter((name)=>name!==omit).map((name)=>{const corner=name.match(/_(LF|LR|RF|RR)$/)?.[1],position=corner?positions[corner]:[0,0,0];return {kind:"node",name,active:true,transform:[1,0,0,0,0,1,0,0,0,0,1,0,...position,1],children:[]};});children.push({kind:"node",name:"COCKPIT_HR",active:true,transform:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],children:[]},{kind:"node",name:"STEER_HR",active:true,transform:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],children:[]});return {root:{kind:"node",name:"car",active:true,transform:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],children}};}

test("accepts the required car nodes with consistent +Z wheel pivots",()=>{const audit=auditCarHierarchy(hierarchyModel());assert.equal(audit.errors,0);assert.equal(audit.warnings,0);assert.equal(audit.lods[0].requiredPresent,14);});

test("diagnoses missing required nodes and reversed wheel axes",()=>{const model=hierarchyModel("DISC_RR"),lf=model.root.children.find((node)=>node.name==="WHEEL_LF"),rf=model.root.children.find((node)=>node.name==="WHEEL_RF");[lf.transform[12],rf.transform[12]]=[rf.transform[12],lf.transform[12]];const audit=auditCarHierarchy(model);assert.ok(audit.errors>=1);assert.ok(audit.warnings>=1);assert.match(audit.findings.map((item)=>item.message).join("\n"),/DISC_RR is missing[\s\S]*left\/right X orientation/);});

test("audits every repository car LOD",async()=>{const manifest=parseCarLodsIni(await readFile(join(carFixtureRoot,"data","lods.ini"),"utf8")),entries=[];for(const lod of manifest.lods){const bytes=await readFile(join(carFixtureRoot,lod.file));entries.push({name:lod.file,size:bytes.length,model:parseKn5(bytes),lod});}const audit=auditCarHierarchy(mergeKn5Models(entries,{kind:"carLods"}));assert.equal(audit.lods.length,4);assert.deepEqual(audit.lods.map((lod)=>lod.requiredPresent),[10,5,9,0]);assert.equal(audit.errors,0);assert.equal(audit.warnings,40);});
