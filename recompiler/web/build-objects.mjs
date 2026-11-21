import {openApk,patchedMember} from "./apk.mjs";
import {openBuildCache,compileLibrary} from "./compile-library.mjs";

const base=new URL("./",import.meta.url);
async function resource(name){
  const response=await fetch(new URL(name,base));
  if(!response.ok)throw Error("Cannot load "+name+" (HTTP "+response.status+")");
  return response;
}
async function hash(blob){
  const digest=await crypto.subtle.digest("SHA-256",await blob.arrayBuffer());
  return Array.from(new Uint8Array(digest),b=>b.toString(16).padStart(2,"0")).join("");
}

export async function verifyOriginalInputs(apk,master,plan){
  if(await hash(apk)!==plan.apkSHA256 || await hash(master)!=="60555788242de9a0580b0d86f6d109d67bd9cabd16d7bcda9b9a99745f270778")
    throw Error("Choose the original Global APK and April 2024 master data.");
}

export async function buildObjects({apk,master,log,progress,workers}){
  const plan=await (await resource("game-plan.json")).json();
  await verifyOriginalInputs(apk,master,plan);
  const read=await openApk(apk), metadata=await patchedMember(read,plan.metadata);
  const semantics=await (await resource("aarch64.bc")).blob();
  const versions=[await hash(semantics),await hash(new Blob([JSON.stringify(plan)]))];
  for(const name of ["lift.wasm","poll.wasm","coff.wasm","toolchain.mjs","translate.mjs","shard-worker.mjs","compile-library.mjs"]){
    versions.push(await hash(await (await resource(name)).blob()));
  }
  const heads=new Map();
  for(const lib of plan.libraries){
    const file=await (await resource(lib.heads)).blob();
    versions.push(await hash(file));
    heads.set(lib.name,(await file.text()).trim().split(/\s+/).map(Number));
  }
  const generation=await hash(new Blob(versions)), cache=await openBuildCache(generation);
  const groups=[];
  try{
    for(const lib of plan.libraries){
      if(log)log("Compiling "+lib.name+" for Windows…");
      const elf=await patchedMember(read,lib);
      const objects=await compileLibrary({name:lib.name,elf,bias:lib.bias,patches:plan.patches,
        heads:heads.get(lib.name),semantics,cache,workers,progress});
      groups.push({name:lib.name,objects});
    }
    return {generation,groups,metadata};
  }finally{cache.close()}
}
