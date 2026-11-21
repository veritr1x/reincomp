import {prepareLibrary} from "./translate.mjs";

// A transaction commits the object and its trace index together. Reloading
// the builder can resume a long compilation without trusting a partial file.
export async function openBuildCache(generation) {
  if (!/^[a-f0-9]{64}$/.test(generation)) throw Error("Invalid compiler generation");
  const db = await new Promise((resolve,reject)=>{
    const request=indexedDB.open("bunker-windows-"+generation,1);
    request.onupgradeneeded=()=>request.result.createObjectStore("objects");
    request.onerror=()=>reject(request.error);
    request.onsuccess=()=>resolve(request.result);
  });
  function access(mode,operation){
    return new Promise((resolve,reject)=>{
      const tx=db.transaction("objects",mode);
      const request=operation(tx.objectStore("objects"));
      tx.oncomplete=()=>resolve(request.result);
      tx.onabort=()=>reject(tx.error||request.error||Error("Build cache transaction aborted"));
    });
  }
  return {get:key=>access("readonly",store=>store.get(key)),
    put:(key,value)=>access("readwrite",store=>store.put(value,key)),close:()=>db.close()};
}

function command(worker,data){
  return new Promise((resolve,reject)=>{
    worker.onerror=e=>reject(Error(e.message || "Browser compilation worker stopped"));
    worker.onmessage=({data})=>data.error?reject(Error(data.error)):resolve(data);
    worker.postMessage(data);
  });
}

export async function compileLibrary({name,elf,bias,patches,heads,semantics,cache,
    workers=Math.max(1,Math.min(8,Math.floor((navigator.hardwareConcurrency||2)/2))), progress=()=>{}}){
  const {image,address,valid}=await prepareLibrary(elf,bias,patches);
  const known=new Set(heads), defined=new Set(), objects=[];
  if(!known.size || !Number.isInteger(workers) || workers<1)throw Error("Invalid library build plan");
  for(const head of known)if(!valid(head))throw Error("Invalid "+name+" head "+head);
  const sorted=values=>[...values].sort((a,b)=>a-b);
  let pending=sorted(known), round=0;
  while(pending.length){
    const headsBlob=new Blob([Uint32Array.from(sorted(known))]);
    const count=Math.ceil(pending.length/128), offset=objects.length, needed=new Set();
    let next=0, completed=0, failure;
    const pool=[];
    async function lane(){
      let worker;
      while(!failure && next<count){
        const index=next++;
        const key=name+"-"+String(index+offset).padStart(5,"0");
        const entries=pending.slice(index*128,index*128+128);
        let result=await cache.get(key);
        if(!result){
          if(!worker){
            worker=new Worker(new URL("./shard-worker.mjs",import.meta.url),{type:"module"});
            pool.push(worker);
            await command(worker,{library:{image,address,heads:headsBlob,semantics}});
          }
          const lines=entries.map(n=>"0x"+n.toString(16)+"\n").join("");
          // LLVM 18's register allocator fails on these three Global Unity
          // groups at O1. The native build retries at O2. In WebAssembly the
          // first group can loop indefinitely before reporting a failure.
          const optimization = name === "unity" && round === 0 && [6, 301, 523].includes(index) ? "-O2" : "-O1";
          try {result=await command(worker,{entries:new Blob([lines]),optimization});}
          catch(error){throw new Error("Compiling "+key+": "+error.message,{cause:error});}
        }
        const own=new Set();
        for(const line of result.targets.trim().split("\n")){
          const m=/^(defined|extern) ([a-f0-9]+)$/.exec(line);
          if(!m)throw Error("Invalid trace index for "+key);
          const pc=parseInt(m[2],16);
          if(m[1]==="defined"){
            if(defined.has(pc))throw Error("Duplicate trace "+m[2]+" in "+key);
            own.add(pc);defined.add(pc);
          }else needed.add(pc);
        }
        if(entries.some(pc=>!own.has(pc))) throw Error("Missing requested trace in "+key);
        if(!(result.object instanceof Blob)||!result.object.size)throw Error("Empty object "+key);
        if(worker && !result.saved)await cache.put(key,{...result,saved:true});
        objects[offset+index]=key;
        progress({name,round,index,completed:++completed,count,object:key});
      }
    }
    try {
      await Promise.all(Array.from({length:Math.min(workers,count)},()=>lane().catch(e=>{failure=e;throw e})));
    } finally {
      for(const worker of pool)worker.terminate();
    }
    pending=sorted([...needed].filter(pc=>!known.has(pc)&&!defined.has(pc)&&valid(pc)));
    for(const pc of pending)known.add(pc);
    ++round;
  }
  return objects;
}
