import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { Kn5WriteError, serializeKn5 } from "../src/kn5-write.js";

const fixtures=[
  "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_nissan_370z/collider.kn5",
  "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/abarth500/collider.kn5",
  "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_nissan_370z/nissan_370z.kn5"
];

test("round-trips real v5/v6, textured, and skinned KN5 data byte for byte",async(t)=>{
  for(const path of fixtures){let source;try{source=await readFile(path);}catch{t.skip("Assetto Corsa writer fixtures are not installed");return;}const output=serializeKn5(parseKn5(source));assert.equal(output.byteLength,source.byteLength,path);assert.deepEqual(output,new Uint8Array(source),path);}
});

test("writes edited material and hierarchy data that parses again",async(t)=>{
  let source;try{source=await readFile(fixtures[0]);}catch{t.skip("Assetto Corsa writer fixture is not installed");return;}const model=parseKn5(source),mesh=walkNodes(model.root).find(({node})=>node.kind==="mesh").node;model.materials[0].shader="apexTest";mesh.lodOut=123;mesh.visible=false;const reparsed=parseKn5(serializeKn5(model));assert.equal(reparsed.materials[0].shader,"apexTest");const outputMesh=walkNodes(reparsed.root).find(({node})=>node.kind==="mesh").node;assert.equal(outputMesh.lodOut,123);assert.equal(outputMesh.visible,false);
});

test("rejects metadata-only textures and protected payload rewrites",()=>{
  assert.throws(()=>serializeKn5({magic:"sc6969",version:6,source:0,textures:[{name:"x.dds"}],materials:[],root:{type:1,name:"root",children:[],active:true,transform:new Array(16).fill(0)}}),/has no data/);
  assert.throws(()=>serializeKn5({magic:"sc6969",version:6,encryption:{},textures:[],materials:[],root:{}}),Kn5WriteError);
});
