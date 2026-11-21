import {openApk,patchedMember} from "./apk.mjs";
import {buildObjects,verifyOriginalInputs} from "./build-objects.mjs";
import {openBuildCache} from "./compile-library.mjs";
import {WindowsToolchain} from "./toolchain.mjs";
import {playerMethods} from "./player-methods.mjs";
import {routeLiveness} from "./liveness-edge.mjs";
import {linkPlayer} from "./gnu-link.mjs";
import {packageExe} from "./package-exe.mjs";

const base=new URL("./",import.meta.url);
async function get(name) {
  const res=await fetch(new URL(name,base));
  if(!res.ok)throw Error("Unable to read "+name+" (HTTP "+res.status+")");
  return res;
}
async function checked(name,expected) {
  const blob=await (await get(name)).blob();
  const bytes=new Uint8Array(await crypto.subtle.digest("SHA-256",await blob.arrayBuffer()));
  if(Array.from(bytes,b=>b.toString(16).padStart(2,"0")).join("")!==expected)throw Error("Damaged browser kit file "+name);
  return blob;
}
export async function patchMaster(master){
  const worker=new Worker(new URL("./master-worker.js",base));
  try{return await new Promise((resolve,reject)=>{
    worker.onmessage=({data})=>data.error?reject(Error(data.error)):resolve(data.blob);
    worker.onerror=e=>reject(Error(e.message));worker.postMessage(master);
  })}finally{worker.terminate()}
}
async function brand(player,slots,read) {
  const edits=[];
  for(const slot of slots){
    const png=await read(slot.path);
    if(png.size!==slot.size)throw Error("Unexpected APK icon size");
    for(const offset of slot.offsets)edits.push({offset,png});
  }
  const parts=[];let pos=0;
  for(const {offset,png} of edits.sort((a,b)=>a.offset-b.offset)){
    if(offset<pos||offset+png.size>player.size)throw Error("Invalid Windows icon location");
    parts.push(player.slice(pos,offset),png);pos=offset+png.size;
  }
  return new Blob([...parts,player.slice(pos)]);
}

async function preparedPlayer({apk,master,manifestHash,log}) {
  if(!/^[0-9a-f]{64}$/.test(manifestHash))throw Error("Invalid prepared runtime identifier");
  const manifest=JSON.parse(await (await checked("runtime/runtime.json",manifestHash)).text());
  if(manifest.format!==1 || !Array.isArray(manifest.parts) || !manifest.parts.length ||
     !Number.isSafeInteger(manifest.size) || manifest.size<=0)throw Error("Invalid prepared runtime manifest");
  const kit=JSON.parse(await (await checked("kit.json",manifest.kitSHA256)).text());
  const plan=JSON.parse(await (await checked("game-plan.json",kit.files["game-plan.json"])).text());
  await verifyOriginalInputs(apk,master,plan);
  const read=await openApk(apk);
  const metadata=await patchedMember(read,plan.metadata);
  log("Applying Bunker’s master data patch…");
  const patchedMaster=await patchMaster(master);
  log("Reading the verified, precompiled Windows runtime…");
  const parts=[];
  for(const [index,part] of manifest.parts.entries()){
    if(!Number.isSafeInteger(part.size) || part.size<=0 || part.size>32*1024*1024)throw Error("Invalid runtime part size");
    const file=await checked("runtime/runtime.part"+String(index+1).padStart(3,"0"),part.sha256);
    if(file.size!==part.size)throw Error("Truncated runtime part");
    parts.push(file);
  }
  const runtime=new Blob(parts);
  const runtimeInfo={size:manifest.size,chunks:manifest.chunks};
  return packagePlayer({apk,master,patchedMaster,metadata,runtime,runtimeInfo,read,log});
}

export async function compileWindows({apk,master,log=()=>{},progress,workers,preparedRuntime}) {
  if(preparedRuntime!==undefined)return preparedPlayer({apk,master,manifestHash:preparedRuntime,log});
  const plan=await (await get("game-plan.json")).json();
  const recipe=await (await get("link/runtime-link.json")).json();
  const compiled=await buildObjects({apk,master,workers,log,progress:state=>{
    progress?.(state);
    if(state.completed===state.count || state.completed%20===0)log(state.name+" · "+state.completed+" / "+state.count);
  }});
  log("Applying Bunker’s master data patch…");
  const patchedMaster=await patchMaster(master);
  const read=await openApk(apk),il2cpp=plan.libraries.find(l=>l.name==="il2cpp");
  log("Compiling the Windows navigation, animation and garbage collection methods…");
  const files=await playerMethods(new WindowsToolchain(),await read(il2cpp.path),await patchedMember(read,il2cpp),
    await (await get(il2cpp.heads)).blob(),(await (await get("spring_group.entries")).text()).trim().split(/\s+/).map(Number),
    await (await get("aarch64.bc")).blob());
  for(const [name,digest] of Object.entries(recipe.files))files[name]=await checked("link/"+name,digest);
  const cache=await openBuildCache(compiled.generation),objects=[];
  try{
    for(const {objects:keys} of compiled.groups)for(const key of keys){
      const entry=await cache.get(key);
      if(!entry?.object)throw Error("Missing compiled object "+key);
      const name=key+".o";
      files[name]=/^defined 825b208c$/m.test(entry.targets)?await routeLiveness(entry.object):entry.object;
      objects.push("/src/"+name);
    }
  }finally{cache.close()}
  log("Linking the Windows game runtime…");
  const linked=await linkPlayer(files,recipe.arguments.flatMap(a=>a==="@game"?objects:[a]),log);
  try {
    return await packagePlayer({apk,master,patchedMaster,metadata:compiled.metadata,runtime:linked.file,read,log});
  }finally{await linked.remove()}
}

export async function packagePlayer({apk,master,patchedMaster,metadata,runtime,runtimeInfo,read,log}) {
    const player=await (await get("player.json")).json();
    const {bunkerVersion}=await (await get("kit.json")).json();
    if(!/^\d+\.\d+\.\d+$/.test(bunkerVersion))throw Error("Invalid Windows kit version");
    log("Packaging Nier Reincarnation.exe…");
    const pieces=[];
    for(const [path,hash] of player.parts)pieces.push(await checked(path,hash));
    const kit=await openApk(new Blob(pieces)),launcher=await brand(await kit("launcher.exe"),player.icons.launcher,read);
    async function* members(){
      for(const path of player.members){
        let blob=await kit(path);
        if(path==="game/WindowsPlayer.exe")blob=await brand(blob,player.icons.player,read);
        yield {path,blob};
      }
      yield {path:"game/GuestRuntime.dll",blob:runtime,prepared:runtimeInfo};
      yield {path:"game/windows-version.txt",blob:new Blob([bunkerVersion+"\n"])};
      yield {path:"data/original-master.bin.e",blob:master};
      yield {path:"data/import-master.bin.e",blob:patchedMaster};
      yield {path:"data/game-sound.apk",blob:apk};
      yield {path:"data/storage/files/il2cpp/Metadata/global-metadata.dat",blob:metadata};
      const notices=await (await get("../bundles/notices.json")).json(),chunks=[];
      for(const part of notices.parts){
        if(!/^notices\.zip(?:\.\d+)?$/.test(part))throw Error("Invalid Bunker notice pack");
        chunks.push(await (await get("../bundles/"+part)).blob());
      }
      const noticePack=new Blob(chunks);
      if(!noticePack.size||noticePack.size!==notices.size)throw Error("Incomplete Bunker notice pack");
      yield {path:"game/notices.zip",blob:noticePack};
    }
    return await packageExe(launcher,members());
}
