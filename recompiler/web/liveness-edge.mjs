// Replace one external x64 tail jump while its named COFF relocation still
// exists. This is the pre-link equivalent of route-liveness-stop.py.
export async function routeLiveness(object) {
  const bytes=new Uint8Array(await object.arrayBuffer()), v=new DataView(bytes.buffer);
  const u16=p=>v.getUint16(p,true), u32=p=>v.getUint32(p,true);
  const count=u32(12), symbols=u32(8), strings=symbols+18*count;
  if(u16(0)!==0x8664 || u16(16)!==0 || strings+4>bytes.length || strings+u32(strings)!==bytes.length)
    throw Error("Invalid Windows liveness object");
  const decode=new TextDecoder();
  function text(p,end){const nul=bytes.indexOf(0,p);return decode.decode(bytes.subarray(p,nul<0?end:Math.min(nul,end)))}
  const functions=[];
  let caller,callee;
  for(let i=0;i<count;){
    const p=symbols+18*i;
    const name=u32(p)?text(p,p+8):text(strings+u32(p+4),bytes.length);
    const symbol={i,section:u16(p+12),address:u32(p+8)};
    if(name==="sub_825b208c")caller=symbol;
    if(name==="sub_825aad50")callee=symbol;
    if(u16(p+14)===0x20 && symbol.section)functions.push(symbol);
    i+=1+bytes[p+17];
  }
  if(!caller?.section||callee?.section!==0)throw Error("Unexpected liveness call sites");
  const section=20+40*(caller.section-1);
  const raw=u32(section+20),rel=u32(section+24);
  const end=Math.min(u32(section+16),...functions.filter(f=>f.section===caller.section && f.address>caller.address).map(f=>f.address));
  let changed=0;
  for(let i=0;i<u16(section+32);++i){
    const p=rel+10*i, pc=u32(p);
    if(u32(p+4)!==callee.i||pc<=caller.address||pc>=end)continue;
    if(u16(p+8)!==4||bytes[raw+pc-1]!==0xe9)throw Error("Unexpected GC tail-jump instruction");
    v.setUint32(p+4,count,true);++changed;
  }
  if(changed!==1)throw Error("Expected one liveness tail jump, found "+changed);
  const newSymbol=new Uint8Array(18), sv=new DataView(newSymbol.buffer);
  sv.setUint32(4,u32(strings),true);sv.setUint16(14,0x20,true);newSymbol[16]=2;
  const name=new TextEncoder().encode("NativeStopWorldForLiveness\0");
  v.setUint32(12,count+1,true);v.setUint32(strings,u32(strings)+name.length,true);
  return new Blob([bytes.subarray(0,strings),newSymbol,bytes.subarray(strings),name]);
}
