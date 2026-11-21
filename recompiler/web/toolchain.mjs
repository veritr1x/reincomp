// Run in a dedicated browser worker. Read-only inputs are mounted as Blobs:
// a 128-entry shard can read the game image without first copying it to MEMFS.
export class WindowsToolchain {
  constructor(base = new URL("./", import.meta.url)) {
    this.base = base;
    this.tools = new Map();
  }

  async load(name) {
    const url = new URL(name + ".wasm", this.base);
    const [module, response] = await Promise.all([import(new URL(name + ".mjs", this.base)).catch(error => {
      throw new Error("Could not load " + name + ".mjs: " + error.message, {cause:error});
    }), fetch(url)]);
    if (!response.ok) throw new Error("Could not load " + url.pathname + " (HTTP " + response.status + ")");
    const binary = response.headers.get("content-type")?.split(";")[0] === "application/wasm"
      ? await WebAssembly.compileStreaming(response)
      : await WebAssembly.compile(await response.arrayBuffer());
    return { factory: module.default, binary };
  }

  async run(name, args, files, outputs) {
    if (!this.tools.has(name)) this.tools.set(name, this.load(name));
    const { factory, binary } = await this.tools.get(name);
    let log = "";
    const record = line => { log = (log + line + "\n").slice(-12000); };
    const app = await factory({
      noInitialRun: true,
      noExitRuntime: true,
      print: record, printErr: record,
      instantiateWasm(imports, ready) {
        const instance = new WebAssembly.Instance(binary, imports);
        ready(instance, binary);
        return instance.exports;
      }
    });
    app.FS.mkdir("/src");
    app.FS.mkdir("/out");
    const blobs = Object.entries(files).map(([name, data]) => ({ name, data }));
    app.FS.mount(app.WORKERFS, { blobs }, "/src");
    try {
      const status = app.callMain(args);
      if (status !== 0) throw new Error("exit " + status);
      return Object.fromEntries(outputs.map(path => [path, new Blob([app.FS.readFile("/out/" + path)])]));
    } catch (error) {
      throw new Error(name + " failed (" + error.message + ")\n" + log, { cause: error });
    } finally {
      app.FS.unmount("/src");
    }
  }

  async shard({ image, address, entries, heads, semantics, pollFlags, optimization = "-O1", binaryHeads = false }) {
    const headFile = binaryHeads ? "heads.u32" : "heads";
    const lifted = await this.run("lift", [
      "--arch", "aarch64", "--os", "linux", "--semantics_dir", "/src",
      "--bytes_file", "/src/image", "--address", String(address), "--entry_addresses_file", "/src/entries",
      "--trace_heads_file", "/src/" + headFile, "--guest_call_transfer=true", "--bc_out", "/out/lifted.bc"
    ], { image, entries, [headFile]:heads, "aarch64.bc": semantics }, ["lifted.bc"]);
    const instrumented = await this.run("poll", [
      ...pollFlags, "/src/lifted.bc", "/out/instrumented.ll"
    ], lifted, ["instrumented.ll"]);
    const compile = level => this.run("coff", [
      level, "/src/instrumented.ll", "/out/shard.o", "/out/targets.txt"
    ], instrumented, ["shard.o", "targets.txt"]);
    let result;
    try {
      result = await compile(optimization);
    } catch (error) {
      if (optimization !== "-O1") throw error;
      result = await compile("-O2");
    }
    return { object: result["shard.o"], targets: await result["targets.txt"].text() };
  }

  async link(files, options) {
    const result = await this.run("link", ["/machine:x64", "/out:/out/player.exe", ...options], files, ["player.exe"]);
    return result["player.exe"];
  }

  async linkRuntime(files, options) {
    const result = await this.run("link", ["--mingw", "-m", "i386pep", "-shared", "-o", "/out/GuestRuntime.dll",
      ...options], files, ["GuestRuntime.dll"]);
    return result["GuestRuntime.dll"];
  }
}
