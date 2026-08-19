function cloneNode(node) { return { ...node, transform:node.transform?[...node.transform]:node.transform, bounds:node.bounds?[...node.bounds]:node.bounds, bones:node.bones?.map((bone)=>({...bone,transform:[...bone.transform]})), children:(node.children||[]).map(cloneNode) }; }

function cloneModel(model) {
  return { ...model, textures:(model.textures||[]).map((texture)=>({...texture})), materials:(model.materials||[]).map((material)=>({...material,properties:(material.properties||[]).map((property)=>({...property,value2:[...property.value2],value3:[...property.value3],value4:[...property.value4]})),resources:(material.resources||[]).map((resource)=>({...resource}))})), root:cloneNode(model.root) };
}

function matchingEntry(record, name) { const key=Object.keys(record||{}).find((candidate)=>candidate.toLowerCase()===String(name).toLowerCase());return key?[key,record[key]]:null; }
function finiteVector(value) { const values=Array.isArray(value)?value.map(Number):[Number(value)];return values.length>=1&&values.length<=4&&values.every(Number.isFinite)?values:null; }

function setProperty(property, value) {
  const values=finiteVector(value);if(!values)return false;
  property.value=values.length===1?values[0]:0;property.value2=[0,0];property.value3=[0,0,0];property.value4=[0,0,0,0];
  if(values.length>1)property[`value${values.length}`]=values;return true;
}

export function bakeEditorProjectIntoKn5(model, project) {
  const output=cloneModel(model),warnings=[],applied={materials:0,properties:0,resources:0,meshes:0};
  const textureIds=new Map(output.textures.map((texture,index)=>[texture.name.toLowerCase(),index]));
  for(const material of output.materials){const match=matchingEntry(project?.materialEdits,material.name);if(!match)continue;const [editName,edit]=match;let changed=false;
    if(edit.shader){material.shader=edit.shader;changed=true;}
    for(const key of ["blendMode","depthMode"])if(edit[key]!==undefined){const value=Number(edit[key]),maximum=key==="blendMode"?255:0xffffffff;if(Number.isInteger(value)&&value>=0&&value<=maximum){material[key]=value;changed=true;}else warnings.push(`${editName}: ${key} ${JSON.stringify(edit[key])} is CSP-only and was not baked`);}
    if(edit.cullMode)warnings.push(`${editName}: cullMode is not stored by KN5 and was not baked`);
    for(const [name,value] of Object.entries(edit.properties||{})){let property=material.properties.find((candidate)=>candidate.name.toLowerCase()===name.toLowerCase());if(!property){property={name,value:0,value2:[0,0],value3:[0,0,0],value4:[0,0,0,0]};material.properties.push(property);}if(setProperty(property,value)){applied.properties++;changed=true;}else warnings.push(`${editName}.${name}: property value cannot be stored by KN5`);}
    for(const [slot,value] of Object.entries(edit.resources||{})){if(!value?.texture){warnings.push(`${editName}.${slot}: external files and solid colors require CSP and were not baked`);continue;}const textureId=textureIds.get(value.texture.toLowerCase());if(textureId===undefined){warnings.push(`${editName}.${slot}: embedded texture ${value.texture} was not found and was not baked`);continue;}let resource=material.resources.find((candidate)=>candidate.slot.toLowerCase()===slot.toLowerCase());if(!resource){resource={slot,textureId,texture:value.texture};material.resources.push(resource);}else{resource.textureId=textureId;resource.texture=output.textures[textureId].name;}applied.resources++;changed=true;}
    if(changed)applied.materials++;
  }
  const visit=(node)=>{if(node.kind==="mesh"||node.kind==="skinnedMesh"){const match=matchingEntry(project?.meshEdits,node.name);if(match){const edit=match[1];if(edit.isTransparent!==undefined)node.transparent=edit.isTransparent;if(edit.castShadows!==undefined)node.castShadows=edit.castShadows;if(edit.layer!==undefined)node.layer=edit.layer;if(edit.lodIn!==undefined)node.lodIn=edit.lodIn;if(edit.lodOut!==undefined)node.lodOut=edit.lodOut;applied.meshes++;}}for(const child of node.children||[])visit(child);};visit(output.root);
  return { model:output,warnings,applied };
}
