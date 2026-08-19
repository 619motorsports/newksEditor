const decoder=new TextDecoder("utf-8");

export class KnhError extends Error{constructor(message,offset=0){super(`${message} at byte ${offset}`);this.name="KnhError";this.offset=offset;}}
function bytes(input){if(input instanceof Uint8Array)return input;if(input instanceof ArrayBuffer)return new Uint8Array(input);if(ArrayBuffer.isView(input))return new Uint8Array(input.buffer,input.byteOffset,input.byteLength);throw new TypeError("KNH input must be an ArrayBuffer or Uint8Array");}

export function parseKnh(input,source="driver_base_pos.knh"){
  const data=bytes(input),view=new DataView(data.buffer,data.byteOffset,data.byteLength);let offset=0,nodeCount=0;
  const need=(count,what)=>{if(!Number.isSafeInteger(count)||count<0||offset+count>data.length)throw new KnhError(`Truncated ${what}`,offset);};
  const u32=(what)=>{need(4,what);const value=view.getUint32(offset,true);offset+=4;return value;};
  const string=()=>{const at=offset,length=u32("node-name length");if(length>1_048_576)throw new KnhError(`Unreasonable node-name length ${length}`,at);need(length,"node name");const value=decoder.decode(data.subarray(offset,offset+length));offset+=length;return value;};
  const node=(depth=0)=>{if(depth>1024)throw new KnhError("KNH hierarchy is too deep",offset);const recordOffset=offset,name=string();need(64,`transform for ${name||"node"}`);const transform=[];for(let index=0;index<16;index++){transform.push(view.getFloat32(offset,true));offset+=4;}if(transform.some((value)=>!Number.isFinite(value)))throw new KnhError(`Non-finite transform for ${name||"node"}`,recordOffset);const childCount=u32(`child count for ${name||"node"}`);if(childCount>1_000_000)throw new KnhError(`Unreasonable child count ${childCount} for ${name||"node"}`,offset-4);nodeCount++;const children=[];for(let index=0;index<childCount;index++)children.push(node(depth+1));return {name,transform,children,recordOffset};};
  if(!data.length)throw new KnhError("Empty KNH file",0);const root=node();if(offset!==data.length)throw new KnhError("Unexpected trailing KNH data",offset);return {source,root,nodeCount,byteLength:data.length,bytesRead:offset};
}

export function walkKnh(root){const rows=[];const visit=(node,depth,parent)=>{rows.push({node,depth,parent});for(const child of node.children||[])visit(child,depth+1,node);};if(root)visit(root,0,null);return rows;}
