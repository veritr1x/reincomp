import {applyPatches} from "./translate.mjs";

const base = 0x80000000;
const flags = ["--speed","--inline-poll=teb","--direct-calls"];
const livenessLoop = "1700f0d208f97f92150540f9b5f67e92350100b4b612c0da1f2003d5807a76f8e10313aaa5000094e826d69ab502288a35ffffb5ff031feb";

// Translate the desktop overrides from the supplied APK using the same
// boundaries, ARM64 edits and renamed LLVM definitions as compile_player.py.
export async function playerMethods(tools, original, patched, heads, springHeads, semantics) {
  const files = {}, numbers = (await heads.text()).trim().split(/\s+/).map(Number);
  const walks = numbers.filter(n => n>=0x8257d5ac && n<0x8257dfa4);
  async function compile(name, image, address, entries, rename, editIR = ir=>ir) {
    const names = new Map(entries.map(n=>["sub_"+n.toString(16), rename(n)]));
    const lift = await tools.run("lift",["--arch","aarch64","--os","linux","--semantics_dir","/src",
      "--bytes_file","/src/image","--address",String(address),"--entry_addresses_file","/src/entries",
      "--trace_heads_file","/src/heads","--guest_call_transfer=true","--bc_out","/out/lifted.bc"],
      {image, entries:new Blob([entries.map(n=>"0x"+n.toString(16)+"\n").join("")]),heads,"aarch64.bc":semantics},["lifted.bc"]);
    const poll = await tools.run("poll",[...flags,"/src/lifted.bc","/out/group.ll"],lift,["group.ll"]);
    let ir = await poll["group.ll"].text();
    const defs = [...ir.matchAll(/^define ptr @(sub_[a-f\d]+)\(/gm)].map(m=>m[1]);
    if (defs.length!==names.size || defs.some(n=>!names.has(n))) throw Error("Unexpected "+name+" entry points");
    ir=editIR(ir).replace(/@(sub_[a-f\d]+)\b/g,(whole,symbol)=>names.has(symbol)?"@"+names.get(symbol):whole);
    const obj = await tools.run("coff",["-O2","/src/group.ll","/out/group.o"],{"group.ll":new Blob([ir])},["group.o"]);
    files[name]=obj["group.o"];
  }
  const slice=(start,end)=>original.slice(start-base,end-base);
  const follow=0x830cfc3c, scroll=0x830df4e8, walk=0x8257d5ac;
  await compile("follow-path.o",applyPatches(slice(follow,0x830d0010),[{offset:0x830cfe74-follow,bytes:"4f000014"}]),
    follow,[follow],()=>"StartFollowPathRelease");
  await compile("snap-scroll.o",applyPatches(slice(scroll,0x830df69c),[{offset:0x830df5a0-scroll,bytes:"00102e1e"}]),
    scroll,[scroll],()=>"UpdateSnapPositionBunker");
  await compile("walk.o",applyPatches(slice(walk,0x8257dfa4),[{offset:0x8257d730-walk,bytes:livenessLoop}]),
    walk,walks,n=>"sub_"+n.toString(16)+"_liveness",ir=>{
      const marker=/^define ptr @sub_8257d9e8\([^\n]+\n(?:(?!^\}).)*?^poll\.continue:[^\n]*\n/ms;
      if(!marker.test(ir))throw Error("Cannot locate the liveness entry poll");
      return ir.replace(marker,match=>match+"  %marked.in.place = call zeroext i1 @MarkLivenessInPlace(ptr %state)\n  br i1 %marked.in.place, label %mark.done, label %mark.original\nmark.done:\n  ret ptr %memory\nmark.original:\n")+"\ndeclare zeroext i1 @MarkLivenessInPlace(ptr)\n";
    });
  await compile("spring.o",patched,base,springHeads,n=>"sub_"+n.toString(16)+"_spring", ir=>
    ir.replace(/^(define ptr @sub_[a-f\d]+\([^\n]*\)) \{(.*?)\n\}/gms,(whole,header,body)=>
      body.split("\n").length<=650 && !/(?:call|invoke).*@(?:sub_|__bunker_guest_call\()/.test(body)
        ?header+" alwaysinline {"+body+"\n}":whole));
  return files;
}
