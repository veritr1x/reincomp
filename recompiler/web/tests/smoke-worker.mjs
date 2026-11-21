import { WindowsToolchain } from "../toolchain.mjs";
import { checkSystemRegisters } from "./system-registers.mjs";

async function resource(path) {
  const response = await fetch(new URL(path, import.meta.url));
  if (!response.ok) throw new Error("Missing test resource " + path);
  return response.blob();
}

self.onmessage = async () => {
  try {
    const tools = new WindowsToolchain(new URL("../", import.meta.url));
    const semantics = await resource("../aarch64.bc");
    await checkSystemRegisters(tools, semantics);
    // Store 40 in the guest thread pointer, clear x0, read the value back
    // and add 2. The native checkpoint verifies both x0 and poll delivery.
    const image = new Blob([Uint32Array.of(
      0xd2800500, 0xd51bd040, 0xd2800000, 0xd53bd040, 0x91000800, 0xd65f03c0
    )]);
    const entries = new Blob(["0x1000\n"]);
    const { object, targets } = await tools.shard({
      image, address: 0x1000, entries, heads: entries, semantics,
      pollFlags: ["--speed", "--inline-poll=teb", "--direct-calls"]
    });
    if (!targets.split("\n").includes("defined 1000")) throw new Error("Missing translated trace");
    const files = { "trace.o": object, "checkpoint.o": await resource("checkpoint.o") };
    const dll = await tools.linkRuntime(files, ["-e", "start", "/src/trace.o", "/src/checkpoint.o"]);
    const header = new DataView(await dll.slice(0,512).arrayBuffer());
    if (!(header.getUint16(header.getUint32(60,true)+22,true)&0x2000)) throw Error("Expected a Windows DLL");
    const exe = await tools.link(files,
      ["/entry:start", "/subsystem:console", "/nodefaultlib", "/src/trace.o", "/src/checkpoint.o"]);
    self.postMessage({ exe });
  } catch (error) {
    self.postMessage({ error: String(error.stack || error) });
  }
};
