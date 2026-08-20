import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseCspIni } from "../src/csp-config.js";
import { evaluateRainFx, parseRainFx, RAIN_FX_BITS } from "../src/rain-fx.js";
import { assettoPath } from "./fixture-paths.js";

function model(){const road={kind:"mesh",name:"1ROAD_MAIN",materialId:0,children:[],indices:new Uint16Array([0,1,2])},paint={kind:"mesh",name:"LINE_WHITE",materialId:1,children:[],indices:new Uint16Array([0,1,2])};return {root:{kind:"node",name:"ROOT",children:[road,paint]},materials:[{name:"Asphalt",shader:"ksPerPixel",resources:[]},{name:"marks",shader:"ksPerPixel",resources:[]}]};}

test("parses RainFX selectors and stream primitives",()=>{const rain=parseRainFx(parseCspIni(`[RAIN_FX]\nPUDDLES_MATERIALS=Asphalt\nSOAKING_MESHES=?ROAD?\nSTREAM_EDGE_...=1,2,3,4,5,6\nSTREAM_POINT_...=7,8,9\nSTREAM_WALL_EDGE_...=1,2,3,4,5,6`));assert.equal(rain.puddlesMaterials,"Asphalt");assert.equal(rain.streams.length,3);assert.deepEqual(rain.streams[0].from,[1,2,3]);assert.deepEqual(rain.streams[1].point,[7,8,9]);assert.equal(rain.warnings.length,0);});

test("matches overlapping RainFX material and mesh categories",()=>{const source=model(),rain=evaluateRainFx(source,parseCspIni(`[RAIN_FX]\nPUDDLES_MATERIALS=Asphalt\nSOAKING_MESHES=?ROAD?\nLINES_MATERIALS=marks\nROUGH_MATERIALS=Sand`)),road=source.root.children[0],line=source.root.children[1];assert.equal(rain.matchedMeshes,2);assert.equal(rain.nodeBindings.get(road).bits,RAIN_FX_BITS.puddles|RAIN_FX_BITS.soaking);assert.deepEqual(rain.nodeBindings.get(line).categories,["lines"]);assert.deepEqual(rain.counts,{puddles:1,soaking:1,smooth:0,rough:0,lines:1,relief:0});});

test("diagnoses malformed RainFX stream coordinates",()=>{const rain=parseRainFx(parseCspIni(`[RAIN_FX]\nPUDDLES_MATERIALS=Road\nSTREAM_EDGE_...=1,2,3`));assert.equal(rain.streams.length,0);assert.match(rain.warnings[0].message,/6 finite coordinates/);});

test("parses and evaluates the installed Imola RainFX fixture",async(t)=>{const configPath=assettoPath("extension/config/tracks/loaded/imola.ini"),modelPath=assettoPath("content/tracks/imola/imola.kn5");let configBytes,kn5Bytes;try{[configBytes,kn5Bytes]=await Promise.all([readFile(configPath,"utf8"),readFile(modelPath)]);}catch{t.skip("Installed Imola RainFX fixture is unavailable");return;}const {parseKn5}=await import("../src/kn5.js"),rain=evaluateRainFx(parseKn5(kn5Bytes),parseCspIni(configBytes,configPath));assert.equal(rain.streamEdges,4);assert.equal(rain.streamPoints,0);assert.ok(rain.counts.puddles>0);assert.ok(rain.counts.soaking>0);assert.ok(rain.counts.smooth>0);assert.ok(rain.counts.rough>0);assert.ok(rain.counts.lines>0);assert.equal(rain.warnings.length,0);});
