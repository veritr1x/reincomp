// Reuse the Python patch shipped with the Android and iOS builder. Only the
// public runtime and patch script are fetched; the master stays in this worker.
self.onmessage=async ({data})=>{
  try {
    const base=new URL("./pyodide/",self.location.href).href;
    importScripts(base+"pyodide.js");
    const py=await loadPyodide({indexURL:base});
    await py.loadPackage(["pycryptodome","msgpack","lz4"]);
    const patch=await fetch(new URL("patch_masterdata.py",self.location.href));
    if(!patch.ok)throw Error("Unable to load Bunker’s master data patch.");
    py.FS.writeFile("/patch_masterdata.py",await patch.text());
    py.FS.writeFile("/original.bin.e",new Uint8Array(await data.arrayBuffer()));
    py.runPython('import runpy, sys\nsys.argv = ["/patch_masterdata.py", "--input", "/original.bin.e", "--output", "/patched.bin.e"]\nrunpy.run_path(sys.argv[0], run_name="__main__")');
    const bytes=py.FS.readFile("/patched.bin.e");
    const digest=Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256",bytes)),b=>b.toString(16).padStart(2,"0")).join("");
    if(digest!=="6cd8c5e65e0ba6ec720596904d95b32da82960c07c097b38378ae9f0158f4fc1"){
      const check=await fetch(new URL("master_digest.py",self.location.href));
      if(!check.ok)throw Error("Unable to verify the master data tables.");
      py.FS.writeFile("/master_digest.py",await check.text());
      const tables=py.runPython('sys.path.insert(0,"/")\nfrom master_digest import table_digest\ntable_digest(open("/patched.bin.e","rb").read())');
      if(tables!=="b73f602bf08c80f0ee8daa4889ec833d427165f65fdf10bfde17da3dad5d2eb6")throw Error("The master data tables differ from the release ("+tables+").");
    }
    self.postMessage({blob:new Blob([bytes])});
  }catch(error){self.postMessage({error:String(error.message||error)})}
};
