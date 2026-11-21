// Translate the same ELF load segments, trace boundaries and direct-call
// closure as recompiler/lift_all.py. The caller owns storage for finished COFFs.
export async function loadImage(elf) {
  const header = new DataView(await elf.slice(0, 64).arrayBuffer());
  if (header.byteLength !== 64 || header.getUint32(0) !== 0x7f454c46 ||
      header.getUint8(4) !== 2 || header.getUint8(5) !== 1 || header.getUint16(18, true) !== 183)
    throw new Error("Expected a little-endian AArch64 ELF64 library");
  const offset = Number(header.getBigUint64(32, true));
  const stride = header.getUint16(54, true), count = header.getUint16(56, true);
  if (stride < 56 || !count || offset + count * stride > elf.size) throw new Error("Invalid ELF program headers");
  const table = new DataView(await elf.slice(offset, offset + count * stride).arrayBuffer());
  const segments = [];
  for (let i = 0; i < count; ++i) {
    const pos = i * stride;
    if (table.getUint32(pos, true) !== 1) continue;
    const file = Number(table.getBigUint64(pos + 8, true));
    const start = Number(table.getBigUint64(pos + 16, true));
    const size = Number(table.getBigUint64(pos + 32, true));
    if (file + size > elf.size || start + size >= 2 ** 32) throw new Error("Invalid ELF load segment");
    if (size) segments.push({ file, start, end: start + size, executable: (table.getUint32(pos + 4, true) & 1) !== 0 });
  }
  if (!segments.some(s => s.executable)) throw new Error("No executable ELF segment");
  const edges = [...new Set(segments.flatMap(s => [s.start, s.end]))].sort((a,b) => a-b);
  const parts = [];
  const zeros = new Uint8Array(65536);
  for (let i = 0; i + 1 < edges.length; ++i) {
    const start = edges[i], end = edges[i + 1];
    // Later program headers overwrite earlier segments exactly like the native loader.
    const segment = segments.findLast(s => s.start <= start && start < s.end);
    if (segment) parts.push(elf.slice(segment.file + start - segment.start, segment.file + end - segment.start));
    else for (let left = end - start; left > 0; left -= zeros.length) parts.push(zeros.subarray(0, Math.min(left, zeros.length)));
  }
  return { data: new Blob(parts), start: edges[0], executable: segments.filter(s => s.executable) };
}

export function applyPatches(blob, patches) {
  for (const { offset, bytes } of patches) {
    if (!Number.isSafeInteger(offset) || offset < 0 || !/^(?:[a-f0-9]{2})+$/i.test(bytes) ||
        offset + bytes.length / 2 > blob.size) throw new Error("Invalid code patch");
    const replacement = Uint8Array.from(bytes.match(/../g), n => parseInt(n, 16));
    blob = new Blob([blob.slice(0, offset), replacement, blob.slice(offset + replacement.length)]);
  }
  return blob;
}

export async function prepareLibrary(elf, bias, patches) {
  const { data, start, executable } = await loadImage(elf);
  const address = bias + start;
  const edits = patches.map(p => ({ offset: Number(p.address) - address, bytes: p.bytes }))
    .filter(p => p.offset >= 0 && p.offset + p.bytes.length / 2 <= data.size);
  const image = applyPatches(data, edits);
  const valid = pc => Number.isSafeInteger(pc) && pc % 4 === 0 &&
    executable.some(s => pc - bias >= s.start && pc - bias < s.end);
  return { image, address, valid };
}

export async function translateLibrary({ tools, elf, bias, patches, heads, semantics, onObject, shardSize = 128 }) {
  const {image,address,valid}=await prepareLibrary(elf,bias,patches);
  const ordered = values => [...values].sort((a,b) => a-b);
  const lines = values => new Blob([values.map(v => "0x" + v.toString(16) + "\n").join("")]);
  const known = new Set(heads);
  if (!known.size || !Number.isSafeInteger(shardSize) || shardSize < 1) throw new Error("Invalid trace plan");
  for (const pc of known) if (!valid(pc)) throw new Error("Trace outside the library: " + pc);
  const lifted = new Set();
  let pending = ordered(known), index = 0;
  while (pending.length) {
    const boundaries = lines(ordered(known));
    const needed = new Set();
    for (let i = 0; i < pending.length; i += shardSize) {
      const entries = pending.slice(i, i + shardSize);
      const { object, targets } = await tools.shard({
        image, address, semantics, entries: lines(entries), heads: boundaries,
        pollFlags: ["--speed", "--inline-poll=teb", "--direct-calls"]
      });
      for (const row of targets.trim().split("\n")) {
        const m = /^(defined|extern) ([0-9a-f]+)$/.exec(row);
        if (!m) throw new Error("Invalid trace index: " + row);
        const pc = parseInt(m[2],16);
        if (m[1] === "defined" && lifted.has(pc)) throw new Error("Multiple shards define trace 0x" + m[2]);
        (m[1] === "defined" ? lifted : needed).add(pc);
      }
      for (const pc of entries) if (!lifted.has(pc)) throw new Error("Missing compiled trace 0x" + pc.toString(16));
      await onObject(index++, object);
    }
    pending = ordered([...needed].filter(pc => !lifted.has(pc) && !known.has(pc) && valid(pc)));
    for (const pc of pending) known.add(pc);
  }
  return { objects: index, traces: lifted.size };
}
