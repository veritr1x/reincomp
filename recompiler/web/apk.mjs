import { applyPatches } from "./translate.mjs";

// The supported original APK is a single-disk ZIP32. Only inflate the members
// needed by the translator; textures and movies stay in the player's File.
export async function openApk(apk) {
  const tail = new DataView(await apk.slice(Math.max(0, apk.size - 65557)).arrayBuffer());
  let end = tail.byteLength - 22;
  while (end >= 0 && !(tail.getUint32(end, true) === 0x06054b50 &&
    end + 22 + tail.getUint16(end + 20, true) === tail.byteLength)) --end;
  if (end < 0 || tail.getUint32(end + 4, true) !== 0) throw new Error("Invalid APK ZIP directory");
  const count = tail.getUint16(end + 10, true);
  const size = tail.getUint32(end + 12, true), start = tail.getUint32(end + 16, true);
  if (count !== tail.getUint16(end + 8, true) || start + size > apk.size - 22) throw new Error("Incomplete APK directory");
  const directory = new DataView(await apk.slice(start, start + size).arrayBuffer());
  const members = new Map(), names = new TextDecoder("utf-8", { fatal: true });
  let pos = 0;
  for (let i = 0; i < count; ++i) {
    if (pos + 46 > size || directory.getUint32(pos, true) !== 0x02014b50) throw new Error("Invalid APK entry");
    const nameSize = directory.getUint16(pos + 28, true);
    const next = pos + 46 + nameSize + directory.getUint16(pos + 30, true) + directory.getUint16(pos + 32, true);
    if (next > size) throw new Error("Truncated APK entry");
    const name = names.decode(new Uint8Array(directory.buffer, pos + 46, nameSize));
    if (members.has(name)) throw new Error("Duplicate APK member " + name);
    members.set(name, pos);
    pos = next;
  }
  return async name => {
    const pos = members.get(name);
    if (pos === undefined) throw new Error("Missing APK member " + name);
    const offset = directory.getUint32(pos + 42, true), length = directory.getUint32(pos + 20, true);
    const local = new DataView(await apk.slice(offset, offset + 30).arrayBuffer());
    if (local.byteLength !== 30 || local.getUint32(0, true) !== 0x04034b50) throw new Error("Invalid local APK header");
    const data = offset + 30 + local.getUint16(26, true) + local.getUint16(28, true);
    if (data + length > start) throw new Error("Truncated APK member " + name);
    let file = apk.slice(data, data + length);
    const method = directory.getUint16(pos + 10, true);
    if (method === 8) file = await new Response(file.stream().pipeThrough(new DecompressionStream("deflate-raw"))).blob();
    else if (method !== 0) throw new Error("Unsupported APK compression " + method);
    if (file.size !== directory.getUint32(pos + 24, true)) throw new Error("Incomplete APK member " + name);
    return file;
  };
}

async function digest(blob) {
  const bytes = await crypto.subtle.digest("SHA-256", await blob.arrayBuffer());
  return Array.from(new Uint8Array(bytes), n => n.toString(16).padStart(2, "0")).join("");
}

export async function patchedMember(read, specification) {
  const source = await read(specification.path);
  if (await digest(source) !== specification.sourceSHA256) throw new Error("Unexpected Global " + specification.path);
  if (!specification.edits.length && specification.sourceSHA256 === specification.patchedSHA256) return source;
  const result = applyPatches(source, specification.edits);
  if (await digest(result) !== specification.patchedSHA256) throw new Error("Patched game input differs from the native build");
  return result;
}
