// Compare a representative 128-entry game shard with lift_all.py's native
// reference. Serve the user's APK, compiler assets and native .done locally.
import { openApk, patchedMember } from "../apk.mjs";
import { prepareLibrary } from "../translate.mjs";
import { WindowsToolchain } from "../toolchain.mjs";

self.onmessage = async ({ data: { apk } }) => {
  try {
    const base = new URL("../", import.meta.url);
    const get = async name => {
      const response = await fetch(new URL(name,base));
      if (!response.ok) throw new Error("Missing test input " + name);
      return response;
    };
    const plan = await (await get("game-plan.json")).json();
    const lib = plan.libraries.find(l => l.name === "il2cpp");
    const elf = await patchedMember(await openApk(apk), lib);
    const { image, address } = await prepareLibrary(elf, lib.bias, plan.patches);
    const heads = await (await get(lib.heads)).blob();
    const sorted = [...new Set((await heads.text()).trim().split(/\s+/).map(Number))].sort((a,b)=>a-b);
    const index = Math.floor(sorted.indexOf(0x8361d54c)/128);
    if (index<0) throw Error("Missing game server entry");
    const reference = await (await get("tests/shard-" + String(index).padStart(5,"0") + ".done")).json();
    const entries = new Blob([sorted.slice(index*128,index*128+128).map(n=>"0x"+n.toString(16)+"\n").join("")]);
    const tools = new WindowsToolchain(base);
    const {object, targets} = await tools.shard({image,address,heads,entries,semantics:await (await get("aarch64.bc")).blob(),
      pollFlags:["--speed","--inline-poll=teb","--direct-calls"]});
    for (const [tag,expected] of [["defined",reference.defs],["extern",reference.decls]]) {
      const actual=targets.trim().split("\n").filter(row=>row.startsWith(tag+" ")).map(row=>parseInt(row.split(" ")[1],16)).sort((a,b)=>a-b);
      if (JSON.stringify(actual)!==JSON.stringify(expected)) throw Error("Shard " + index + " " + tag + " addresses differ: "+actual.length+" vs "+expected.length);
    }
    self.postMessage({verified:true,shard:index,bytes:object.size,definitions:reference.defs.length,externals:reference.decls.length});
  } catch(error){self.postMessage({error:String(error.stack||error)})}
};
