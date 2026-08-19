import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { KnhError, parseKnh, walkKnh } from "../src/knh.js";

function u32(value){const data=new Uint8Array(4);new DataView(data.buffer).setUint32(0,value,true);return data;}function f32(values){const data=new Uint8Array(values.length*4),view=new DataView(data.buffer);values.forEach((value,index)=>view.setFloat32(index*4,value,true));return data;}function join(chunks){const output=new Uint8Array(chunks.reduce((sum,chunk)=>sum+chunk.length,0));let offset=0;for(const chunk of chunks){output.set(chunk,offset);offset+=chunk.length;}return output;}function encode(node){const name=new TextEncoder().encode(node.name),matrix=node.transform||[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],children=node.children||[];return join([u32(name.length),name,f32(matrix),u32(children.length),...children.map(encode)]);}

test("parses recursive KNH names and transforms",()=>{const source=encode({name:"root",children:[{name:"DRIVER:RIG_Center",transform:[1,0,0,0,0,1,0,0,0,0,1,0,.4,.5,-.1,1]}]}),parsed=parseKnh(source);assert.equal(parsed.nodeCount,2);assert.equal(parsed.bytesRead,source.length);assert.deepEqual(walkKnh(parsed.root).map(({node,depth})=>[node.name,depth]),[["root",0],["DRIVER:RIG_Center",1]]);assert.ok(Math.abs(parsed.root.children[0].transform[12]-.4)<1e-6);});

test("rejects truncated and trailing KNH bytes",()=>{const source=encode({name:"root"});assert.throws(()=>parseKnh(source.subarray(0,source.length-1)),KnhError);assert.throws(()=>parseKnh(join([source,new Uint8Array([0])])),/trailing KNH data/);});

test("parses a complete installed driver base pose",async(t)=>{const path="/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_nissan_370z/driver_base_pos.knh";let data;try{data=await readFile(path);}catch{t.skip("Assetto Corsa KNH fixture is not installed");return;}const parsed=parseKnh(data,path),names=new Set(walkKnh(parsed.root).map(({node})=>node.name));assert.equal(parsed.bytesRead,data.length);assert.equal(parsed.nodeCount,71);assert.ok(names.has("DRIVER:RIG_Center"));assert.ok(names.has("DRIVER:RIG_HAND_L"));assert.ok(names.has("DRIVER:RIG_HAND_R"));});
