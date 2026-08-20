import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { Kn5WriteError, serializeKn5 } from "../src/kn5-write.js";
import { carColliderKn5, carMainKn5, trackMainKn5 } from "./fixture-paths.js";

const fixtures=[carColliderKn5,carMainKn5,trackMainKn5];

test("round-trips repository v5/v6 and textured KN5 data byte for byte",async()=>{
  for(const path of fixtures){const source=await readFile(path),output=serializeKn5(parseKn5(source));assert.equal(output.byteLength,source.byteLength,path);assert.deepEqual(output,new Uint8Array(source),path);}
});

test("writes edited material and hierarchy data that parses again",async()=>{
  const source=await readFile(fixtures[0]),model=parseKn5(source),mesh=walkNodes(model.root).find(({node})=>node.kind==="mesh").node;model.materials[0].shader="apexTest";mesh.lodOut=123;mesh.visible=false;const reparsed=parseKn5(serializeKn5(model));assert.equal(reparsed.materials[0].shader,"apexTest");const outputMesh=walkNodes(reparsed.root).find(({node})=>node.kind==="mesh").node;assert.equal(outputMesh.lodOut,123);assert.equal(outputMesh.visible,false);
});

test("rejects metadata-only textures and protected payload rewrites",()=>{
  assert.throws(()=>serializeKn5({magic:"sc6969",version:6,source:0,textures:[{name:"x.dds"}],materials:[],root:{type:1,name:"root",children:[],active:true,transform:new Array(16).fill(0)}}),/has no data/);
  assert.throws(()=>serializeKn5({magic:"sc6969",version:6,encryption:{},textures:[],materials:[],root:{}}),Kn5WriteError);
});
