// GNU ld seeks through input libraries and the PE it is writing. Keep the
// original COFF Blobs and final DLL out of the WebAssembly linear memory.
export async function linkPlayer(files,args,log) {
  const {default:create}=await import("./binutils.mjs");
  let diagnostics="";
  const record=line=>{diagnostics=(diagnostics+line+"\n").slice(-6000);log(line)};
  const app=await create({noInitialRun:true,print:record,printErr:record});
  const root=await navigator.storage.getDirectory();
  const temporary="bunker-link-"+crypto.randomUUID();
  const handle=await root.getFileHandle(temporary,{create:true});
  const disk=await handle.createSyncAccessHandle();
  let closed=false;
  try {
    const fs=app.FS;
    fs.mkdir("/src");fs.mkdir("/out");
    const mounted=fs.mount(app.WORKERFS,{blobs:Object.entries(files).map(([name,data])=>({name,data}))},"/src");
    const pages=new Map(),reader=new FileReaderSync(),pageSize=256*1024;
    const read=(stream,buffer,offset,length,position)=>{
      const file=stream.node.contents;
      const size=Math.min(length,Math.max(0,file.size-position));
      for(let copied=0;copied<size;){
        const pos=position+copied,first=pos-pos%pageSize,key=stream.node.id+":"+first;
        let page=pages.get(key);
        if(!page){
          page=new Uint8Array(reader.readAsArrayBuffer(file.slice(first,first+pageSize)));
          if(pages.size>=256)pages.delete(pages.keys().next().value);
          pages.set(key,page);
        }
        const span=Math.min(size-copied,page.length-(pos-first));
        buffer.set(page.subarray(pos-first,pos-first+span),offset+copied);copied+=span;
      }
      return size;
    };
    for(const node of Object.values(mounted.contents))node.stream_ops={...node.stream_ops,read};
    const dest="/out/GuestRuntime.dll", name=fs.create(dest);
    const stat=name.node_ops.getattr;
    name.node_ops={...name.node_ops,getattr(n){return {...stat(n),size:disk.getSize()}},
      setattr(n,attr){if(attr.size!==undefined)disk.truncate(attr.size);if(attr.mode!==undefined)n.mode=attr.mode}};
    name.stream_ops={...name.stream_ops,
      read(s,b,o,n,p){return disk.read(b.subarray(o,o+n),{at:p})},
      write(s,b,o,n,p){return disk.write(b.subarray(o,o+n),{at:p})},
      llseek(s,off,whence){return off+(whence===1?s.position:whence===2?disk.getSize():0)}
    };
    const code=app.callMain(["-o",dest,...args]);
    if(code!==0 || disk.getSize()<512)throw Error("The Windows release linker failed ("+code+").\n"+diagnostics);
    disk.flush();disk.close();closed=true;
    // Immutable Blob snapshot. The worker can remove scratch storage after
    // the final EXE is assembled and consumed.
    const file=await handle.getFile();
    return {file,remove:()=>root.removeEntry(temporary)};
  }catch(error){
    if(!closed)disk.close();
    await root.removeEntry(temporary);
    throw error;
  }
}
