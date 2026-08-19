import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { cspLightDistanceFade, cspLightReceiverVisible, cspLineClosestPoint, cspLineLightSample, cspSecondarySpotAttenuation, cspSecondarySpotPacking, cspSpotConeFactor, cspSpotConePacking, cspSpotEdgeFactors, cspSpotEdgePacking, CSP_SPOT_HALF_ANGLE_MAX, CSP_SPOT_SHARPNESS_MAX, evaluateKsLighting, ksEditorAutoExposure, ksEditorYebisToneMap, KS_EDITOR_DEFAULT_WEATHER, KS_EDITOR_TONEMAP, parseKsWeatherLighting, STOCK_WEATHER_PRESETS, sunDirectionFromAngles } from "../src/lighting.js";

test("matches the initialized default Yebis display curve and reciprocal gamma", () => {
  assert.deepEqual(KS_EDITOR_TONEMAP, { function: -1, mappingFactor: 32, characteristicCurve: 0.5, curveScale: 2.6581413745880127, curveShoulder: 0.6653175950050354, inputFloor: 1 / 16384, outputEpsilon: 1 / 4194304 });
  const neutral = ksEditorYebisToneMap([1, 1, 1], 1, { gamma: 1.2, saturation: 0.95 });
  const decay = Math.exp(-KS_EDITOR_TONEMAP.curveScale);
  const expectedCurve = (1 - decay) * Math.pow(1 - decay * KS_EDITOR_TONEMAP.curveShoulder, 2);
  const expected = Math.pow(expectedCurve + 1 / 4194304, 1 / 1.2);
  assert.ok(neutral.every((value) => Math.abs(value - expected) < 1e-12));
  const colored = ksEditorYebisToneMap([1, 0.25, 0.05], 0.5);
  assert.ok(colored[0] > colored[1] && colored[1] > colored[2]);
  assert.ok(colored.every((value) => Number.isFinite(value) && value >= 0 && value <= 1));
});

test("clamps full-frame automatic exposure to the installed editor range", () => {
  assert.equal(ksEditorAutoExposure(1), 0.32);
  assert.equal(ksEditorAutoExposure(10), 0.2);
  assert.equal(ksEditorAutoExposure(0.01), 0.5);
  assert.equal(ksEditorAutoExposure(Number.NaN), 0.5);
});

test("ships all seven stock SDK weather-lighting presets", () => {
  assert.equal(STOCK_WEATHER_PRESETS.length, 7);
  assert.equal(KS_EDITOR_DEFAULT_WEATHER.id, "5_light_clouds");
  assert.deepEqual(KS_EDITOR_DEFAULT_WEATHER.sunHigh.map((value)=>Number(value.toFixed(6))), [4,4,3.764706]);
});

test("reproduces native high-to-low sun-angle interpolation", () => {
  const high=evaluateKsLighting(KS_EDITOR_DEFAULT_WEATHER,[0,1,0]),low=evaluateKsLighting(KS_EDITOR_DEFAULT_WEATHER,[1,0,0]),middle=evaluateKsLighting(KS_EDITOR_DEFAULT_WEATHER,[0,0.5,Math.sqrt(.75)]);
  assert.equal(high.angleMix,0);assert.equal(low.angleMix,1);assert.equal(Number(middle.angleMix.toFixed(6)),Number(Math.pow(.5,1.8).toFixed(6)));
  assert.deepEqual(high.sunColor,KS_EDITOR_DEFAULT_WEATHER.sunHigh);assert.deepEqual(low.ambientColor,KS_EDITOR_DEFAULT_WEATHER.ambientLow);
});

test("constructs normalized authoring sun directions",()=>{const direction=sunDirectionFromAngles(90,30);assert.ok(Math.abs(Math.hypot(...direction)-1)<1e-12);assert.ok(Math.abs(direction[0]-Math.cos(Math.PI/6))<1e-12);assert.ok(Math.abs(direction[1]-.5)<1e-12);});

test("matches CSP's symmetric camera-distance light fade interval",()=>{assert.equal(cspLightDistanceFade(159),1);assert.equal(cspLightDistanceFade(160),1);assert.equal(cspLightDistanceFade(200),.5);assert.equal(cspLightDistanceFade(240),0);assert.equal(cspLightDistanceFade(10,10,0),0);assert.equal(cspLightDistanceFade(9.9,10,0),1);});

test("matches CSP light view and track-receiver gates",()=>{const interior={viewMode:"interior",affectsTrackMode:"all"},exterior={viewMode:"exterior",affectsTrackMode:"all"},carOnly={viewMode:"both",affectsTrackMode:"none"},interiorTrack={viewMode:"both",affectsTrackMode:"interior-only"};assert.equal(cspLightReceiverVisible(interior,{interiorView:false}),false);assert.equal(cspLightReceiverVisible(interior,{interiorView:true}),true);assert.equal(cspLightReceiverVisible(exterior,{interiorView:true}),false);assert.equal(cspLightReceiverVisible(carOnly,{trackReceiver:false}),true);assert.equal(cspLightReceiverVisible(carOnly,{trackReceiver:true}),false);assert.equal(cspLightReceiverVisible(interiorTrack,{trackReceiver:true,interiorView:false}),false);assert.equal(cspLightReceiverVisible(interiorTrack,{trackReceiver:true,interiorView:true}),true);});

test("matches CSP's finite-line projection and endpoint color interpolation",()=>{const middle=cspLineLightSample([0,2,0],[10,2,0],[4,0,0],[2,1,0],[0,3,4]);assert.deepEqual(middle.point,[4,2,0]);assert.equal(middle.value,.4);assert.equal(middle.clampedValue,.4);assert.equal(middle.distanceInverse,.01);assert.deepEqual(middle.color,[1.2,1.8,1.6]);const before=cspLineLightSample([0,0,0],[10,0,0],[-2,1,0],[1,2,3],[4,5,6]);assert.equal(before.value,-.2);assert.equal(before.clampedValue,0);assert.deepEqual(before.point,[0,0,0]);assert.deepEqual(before.color,[1,2,3]);const after=cspLineClosestPoint([0,0,0],[10,0,0],[14,0,0]);assert.ok(Math.abs(after.value-1.4)<1e-12);assert.equal(after.clampedValue,1);assert.deepEqual(after.point,[10,0,0]);});

test("matches CSP's packed linear spotlight cone",()=>{const packed=cspSpotConePacking([0,-2,0],90,.8);assert.equal(packed.enabled,true);assert.ok(Math.abs(packed.halfAngle-Math.PI/4)<1e-6);assert.ok(packed.inverseWidth<0);assert.ok(Math.abs(Math.hypot(...packed.direction)-Math.abs(packed.inverseWidth))<1e-12);const outer=[-Math.sin(packed.halfAngle),Math.cos(packed.halfAngle),0],inner=[-Math.sin(packed.halfAngle*packed.sharpness),Math.cos(packed.halfAngle*packed.sharpness),0],middleAngle=(packed.halfAngle+packed.halfAngle*packed.sharpness)/2,middle=[-Math.sin(middleAngle),Math.cos(middleAngle),0],middleExpected=(Math.cos(middleAngle)-packed.outerCos)/(packed.innerCos-packed.outerCos);assert.ok(cspSpotConeFactor([0,-1,0],outer,90,.8)<1e-12);assert.ok(Math.abs(cspSpotConeFactor([0,-1,0],inner,90,.8)-1)<1e-12);assert.ok(Math.abs(cspSpotConeFactor([0,-1,0],middle,90,.8)-middleExpected)<1e-12);assert.equal(cspSpotConeFactor(null,[0,-1,0],90,.8),1);const clamped=cspSpotConePacking([0,-1,0],999,2);assert.equal(clamped.halfAngle,CSP_SPOT_HALF_ANGLE_MAX);assert.equal(clamped.sharpness,CSP_SPOT_SHARPNESS_MAX);});

test("matches CSP's packed RGB spot edge and normalized trimmed secondary range",()=>{const packed=cspSpotEdgePacking([0,2,0],[.1,.2,.3],10);assert.deepEqual(packed.up,[0,10,0]);assert.deepEqual(packed.offsets,[1,2,3]);assert.deepEqual(cspSpotEdgeFactors([0,1,0],[.1,.2,.3],10,[0,1,0]),[1,1,1]);assert.deepEqual(cspSpotEdgeFactors([0,1,0],[.1,.2,.3],10,[0,-1,0]),[0,0,0]);const transition=cspSpotEdgeFactors([0,1,0],[.1,.2,.3],10,[Math.sqrt(.9975),-.05,0]);assert.ok(Math.abs(transition[0]-.5)<1e-12);assert.deepEqual(transition.slice(1),[1,1]);assert.deepEqual(cspSpotEdgeFactors(null,null,0,[0,1,0]),[1,1,1]);assert.deepEqual(cspSecondarySpotPacking(20,.3),{enabled:true,rangeInverse:.05,trimStart:0,trimLengthInverse:1/6,skip:.3});assert.equal(cspSecondarySpotAttenuation(0,20,.3),0);assert.ok(Math.abs(cspSecondarySpotAttenuation(3,20,.3)-.1225)<1e-12);assert.ok(Math.abs(cspSecondarySpotAttenuation(6,20,.3)-.49)<1e-12);assert.equal(cspSecondarySpotAttenuation(20,20,.3),0);assert.equal(cspSecondarySpotAttenuation(1,0,.3),0);});

test("matches all installed ksEditor stock weather byte semantics",async(t)=>{const root="/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/sdk/editor/content/weather";for(const expected of STOCK_WEATHER_PRESETS){let curves,weather;try{[curves,weather]=await Promise.all([readFile(`${root}/${expected.id}/colorCurves.ini`,"utf8"),readFile(`${root}/${expected.id}/weather.ini`,"utf8")]);}catch{t.skip("Installed ksEditor weather fixtures are unavailable");return;}const parsed=parseKsWeatherLighting(curves,weather,expected.id);assert.equal(parsed.name,expected.name);for(const key of ["horizonLow","horizonHigh","skyLow","skyHigh","sunLow","sunHigh","ambientLow","ambientHigh","fogColor"])assert.deepEqual(parsed[key],expected[key],`${expected.id} ${key}`);for(const key of ["angleGamma","fogDistance","fogBlend","cloudCover","cloudCutoff","cloudColor"])assert.equal(parsed[key],expected[key],`${expected.id} ${key}`);}});
