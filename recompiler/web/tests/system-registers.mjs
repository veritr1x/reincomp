// Check translation on the WebAssembly host. The ARM64 startup code reads TLS
// before opening the game window, and it also preserves FPCR and FPSR.
export async function checkSystemRegisters(tools, semantics) {
  const image = new Blob([Uint32Array.of(
    0xd53bd053, 0xd51bd053, // mrs x19, TPIDR_EL0 and msr TPIDR_EL0, x19
    0xd53b4401, 0xd51b4401, // mrs x1, FPCR and msr FPCR, x1
    0xd53b4422, 0xd51b4422, // mrs x2, FPSR and msr FPSR, x2
    0xd65f03c0              // ret
  )]);
  const heads = new Blob(["0x1000\n"]);
  const output = await tools.run("lift", [
    "--arch", "aarch64", "--os", "linux", "--semantics_dir", "/src",
    "--bytes_file", "/src/image", "--address", "4096",
    "--entry_addresses_file", "/src/heads", "--trace_heads_file", "/src/heads",
    "--guest_call_transfer=true", "--ir_out", "/out/registers.ll"
  ], { image, heads, "aarch64.bc": semantics }, ["registers.ll"]);
  const ir = await output["registers.ll"].text();
  const trace = ir.match(/^define[^\n]*@sub_1000\([^\n]*\{[^]*?^\}/m)?.[0];
  if (!trace || /call[^\n]*@__remill_error\(/.test(trace) || !trace.includes("@__remill_function_return")) {
    throw new Error("The system-register trace failed to reach its return instruction");
  }
}
