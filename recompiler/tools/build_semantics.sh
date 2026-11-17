#!/bin/bash
# Build the Remill AArch64 semantics bitcode (aarch64.bc) without CMake, with
# the flags of remill/cmake/BCCompiler.cmake and lib/Arch/AArch64/Runtime.
# At 3fd14f3 the result matches the accepted toolchain semantics (5a9c6807)
# apart from debug info and attribute numbering.
#   build_semantics.sh REMILL_SRC OUTDIR   (WSL, LLVM 18)
set -e
R=$1; O=$2; mkdir -p $O; C=/usr/lib/llvm-18/bin/clang++; LINK=/usr/lib/llvm-18/bin/llvm-link
SYS=$R/include/remill/Arch/Runtime/sysroot
F=(--sysroot=$SYS -nostdinc++ -isystem $SYS -emit-llvm -Wno-unknown-warning-option -Wall -Wshadow
  -Wconversion -Wpadded -pedantic -Wshorten-64-to-32 -Wgnu-alignof-expression
  -Wno-gnu-anonymous-struct -Wno-return-type-c-linkage
  -Wno-gnu-zero-variadic-macro-arguments -Wno-nested-anon-types
  -Wno-extended-offsetof -Wno-gnu-statement-expression -Wno-c99-extensions
  -Wno-ignored-attributes -fno-vectorize -fno-slp-vectorize
  -Wno-variadic-macros -Wno-c11-extensions -Wno-c++11-extensions
  -ffreestanding -fno-common -fno-builtin -fno-exceptions -fno-rtti
  -fno-asynchronous-unwind-tables -Wno-unneeded-internal-declaration
  -Wno-unused-function -Wgnu-inline-cpp-without-extern
  -Wno-pass-failed=transform-warning -Wno-packed-non-pod -std=c++17)
I=(-I$R/include -I$R)
D=(-target aarch64-none-elf -DADDRESS_SIZE_BITS=64 -DLITTLE_ENDIAN=1)
$C "${I[@]}" "${D[@]}" "${F[@]}" -O3 -g0 -c $R/lib/Arch/AArch64/Runtime/Instructions.cpp -o $O/i.bc 2>$O/i.log
$C "${I[@]}" "${D[@]}" "${F[@]}" -O0 -g3 -c $R/lib/Arch/AArch64/Runtime/BasicBlock.cpp -o $O/b.bc 2>$O/b.log
$C "${I[@]}" "${D[@]}" "${F[@]}" -c $R/lib/Arch/Runtime/Intrinsics.cpp -o $O/n.bc 2>$O/n.log
$C "${I[@]}" "${D[@]}" "${F[@]}" -c $R/lib/Arch/Runtime/HyperCall.cpp -o $O/h.bc 2>$O/h.log
$LINK --suppress-warnings $O/i.bc $O/b.bc $O/n.bc $O/h.bc -o $O/aarch64.bc
sha256sum $O/aarch64.bc
