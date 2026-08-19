import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { sceneWorldMatrices, skinMeshVertices } from "../src/skinning.js";

test("skins a weighted vertex through an animated bone",()=>{const mesh={kind:"skinnedMesh",name:"skin",vertexStride:19,vertices:new Float32Array([0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,-1,-1,-1]),bones:[{name:"bone",transform:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]},world=new Map([["bone",[1,0,0,0,0,1,0,0,0,0,1,0,2,3,4,1]]]),output=skinMeshVertices(mesh,world);assert.deepEqual(Array.from(output.slice(0,3)),[2,3,4]);assert.deepEqual(Array.from(output.slice(3,6)),[0,1,0]);});

test("reproduces an installed driver bind pose from bone worlds and inverse binds",async(t)=>{const path="/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/driver/driver_no_HANS.kn5";let data;try{data=await readFile(path);}catch{t.skip("Assetto Corsa driver fixture is not installed");return;}const model=parseKn5(data),worlds=sceneWorldMatrices(model.root),mesh=walkNodes(model.root).map(({node})=>node).find((node)=>node.kind==="skinnedMesh"),output=skinMeshVertices(mesh,worlds.byName,worlds.byNode.get(mesh));let maximum=0,average=0,count=0;for(let offset=0;offset<Math.min(mesh.vertices.length,19*1000);offset+=19){const error=Math.hypot(output[offset]-mesh.vertices[offset],output[offset+1]-mesh.vertices[offset+1],output[offset+2]-mesh.vertices[offset+2]);maximum=Math.max(maximum,error);average+=error;count++;}assert.ok(average/count<1e-5,`average error ${average/count}`);assert.ok(maximum<.002,`maximum error ${maximum}`);});
