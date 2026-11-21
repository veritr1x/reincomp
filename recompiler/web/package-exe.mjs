// A PE loader ignores its overlay. The Windows launcher reads this footer on
// first start and materializes the locally built player beside the download.
const chunkSize = 4 * 1024 * 1024;
const encoder = new TextEncoder();
function allowedPath(path) {
  if (typeof path !== "string" || !/^(?:(?:game|pods|server|licenses)\/|data\/(?:native\/|apk-assets\/|storage\/files\/il2cpp\/|(?:game-sound\.apk|(?:original|import)-master\.bin\.e)$))/.test(path)) return false;
  return path.split("/").every(part => part.length && !/[. ]$/.test(part) && !/[<>:"\\|?*\x00-\x1f]/.test(part) &&
    !/^(?:con|prn|nul|aux|(?:com|lpt)[1-9¹²³])(?:\.|$)/i.test(part));
}
export async function packageExe(launcher, files) {
  const parts = [launcher], entries = [];
  const paths = new Set();
  let payloadBytes = 0;
  for await (const { path, blob, prepared } of files) {
    if (!(blob instanceof Blob) || !allowedPath(path) || paths.has(path.toLowerCase())) throw new Error("Invalid player member: " + path);
    paths.add(path.toLowerCase());
    // The private pack carries the original installer's checksums. Its stored
    // bytes have already been authenticated against the selected pack manifest.
    if (prepared !== undefined) {
      if (path !== "game/GuestRuntime.dll" || !blob.size || !Number.isSafeInteger(prepared.size) ||
          prepared.size <= 0 || !Array.isArray(prepared.chunks) ||
          prepared.chunks.length !== Math.ceil(prepared.size / chunkSize) ||
          !prepared.chunks.every(hash => typeof hash === "string" && /^[0-9a-f]{64}$/.test(hash))) {
        throw new Error("Invalid prepared guest runtime");
      }
      entries.push({path, size:prepared.size, chunks:prepared.chunks, compression:"gzip", storedSize:blob.size});
      parts.push(blob);
      payloadBytes += blob.size;
      continue;
    }
    const chunks = [];
    for (let pos = 0; pos < blob.size; pos += chunkSize) {
      const bytes = await crypto.subtle.digest("SHA-256", await blob.slice(pos, pos + chunkSize).arrayBuffer());
      chunks.push(Array.from(new Uint8Array(bytes), b => b.toString(16).padStart(2, "0")).join(""));
    }
    const entry = { path, size: blob.size, chunks };
    let packed = blob;
    if (path === "game/GuestRuntime.dll") {
      // Translated x64 code repeats instruction sequences across methods.
      // Store it compressed to keep the single-file download comfortably
      // below the Windows PE file-size limit.
      const stream = blob.stream().pipeThrough(new CompressionStream("gzip"));
      packed = await new Response(stream).blob();
      entry.compression = "gzip";
      entry.storedSize = packed.size;
    }
    entries.push(entry);
    parts.push(packed);
    payloadBytes += packed.size;
  }
  for (const path of ["game/windowsplayer.exe","server/bunker-server.exe","data/import-master.bin.e"]) {
    if (!paths.has(path)) throw new Error("Missing portable player member " + path);
  }
  const format = entries.some(entry => entry.compression) ? 2 : 1;
  const manifest = encoder.encode(JSON.stringify({ format, files: entries }));
  if (manifest.length>32*1024*1024 || !Number.isSafeInteger(payloadBytes)) throw new Error("Player package exceeds the installer's limits");
  const footer = new Uint8Array(28);
  footer.set(encoder.encode("BUNKER.WINDOWS.1"));
  const view = new DataView(footer.buffer);
  view.setUint32(16, manifest.length, true);
  view.setBigUint64(20, BigInt(payloadBytes), true);
  return new Blob([...parts, manifest, footer], { type: "application/vnd.microsoft.portable-executable" });
}
