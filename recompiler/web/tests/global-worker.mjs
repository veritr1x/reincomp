// Exercise Bunker's port patch and the production trace ABI using a locally
// selected Global APK. The resulting diagnostic executable returns 42 only
// if the translated getter returns 38003 and delivers its cooperative poll.
import { openApk, patchedMember } from "../apk.mjs";
import { loadImage, translateLibrary } from "../translate.mjs";
import { WindowsToolchain } from "../toolchain.mjs";

self.onmessage = async ({ data: { apk, inputsOnly } }) => {
  const asset = async path => {
    const response = await fetch(new URL(path, import.meta.url));
    if (!response.ok) throw new Error("Missing " + path);
    return response;
  };
  try {
    const plan = await (await asset("../game-plan.json")).json();
    const read = await openApk(apk);
    await patchedMember(read, plan.metadata);
    const library = plan.libraries.find(l => l.name === "il2cpp");
    const unity = plan.libraries.find(l => l.name === "unity");
    await patchedMember(read, unity);
    const elf = await patchedMember(read, library);
    if (inputsOnly) {
      const { start, executable } = await loadImage(elf);
      self.postMessage({ verified: true, start, executableSegments: executable.length });
      return;
    }
    const tools = new WindowsToolchain(new URL("../", import.meta.url));
    const files = { "checkpoint.o": await (await asset("global-checkpoint.o")).blob() };
    await translateLibrary({
      elf, tools, heads: [0x8361d54c], bias: library.bias, patches: plan.patches,
      semantics: await (await asset("../aarch64.bc")).blob(),
      onObject(index, blob) { files["trace-" + index + ".o"] = blob; }
    });
    const exe = await tools.link(files, ["/entry:start", "/subsystem:console", "/nodefaultlib",
      ...Object.keys(files).map(name => "/src/" + name)]);
    self.postMessage({ exe });
  } catch (error) {
    self.postMessage({ error: String(error.stack || error) });
  }
};
