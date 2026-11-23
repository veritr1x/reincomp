#!/usr/bin/env bash
# Run from the repository root. All generated game code stays in OUT.
set -eu
if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 LIBIL2CPP TOOLCHAIN POLL-INSTRUMENT OUT [UNITY-LINUX-MODULE]" >&2
    exit 2
fi
out=$4
python3 recompiler/lift_all.py --lib "$1" --bias 0x80000000 \
    --heads recompiler/tests/linux-il2cpp-heads.txt --toolchain "$2" --poll "$3" \
    --target x86_64-linux-gnu --out "$out"
cxx=${CXX:-c++}
flags=(-std=c++17 -O2 -pthread -DADDRESS_SIZE_BITS=64 -Ithird_party/remill-include -Iruntime)
"$cxx" "${flags[@]}" -shared -fPIC -Wl,-z,defs runtime/runtime.cpp runtime/cooperative_poll.cpp \
    "$out/shard-00000.o" -o "$out/libil2cpp-test.so" -latomic
"$cxx" "${flags[@]}" recompiler/tests/linux-il2cpp.cpp -L"$out" -lil2cpp-test \
    '-Wl,-rpath,$ORIGIN' -o "$out/test-il2cpp"
"$out/test-il2cpp"
"$cxx" "${flags[@]}" -Ihost recompiler/tests/linux-pthreads.cpp host/signal_host.cpp -L"$out" -lil2cpp-test \
    '-Wl,-rpath,$ORIGIN' -o "$out/test-pthreads"
"$out/test-pthreads"
"$cxx" "${flags[@]}" -Ihost recompiler/tests/linux-mappings.cpp -o "$out/test-mappings"
"$out/test-mappings"
"$cxx" "${flags[@]}" -Ihost recompiler/tests/linux-input.cpp -o "$out/test-input"
"$out/test-input"
if [ "$#" -eq 5 ]; then
    python3 build/linux_player/engine_calls.py --unity "$5" --out "$out/engine-calls.json"
    transform=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["UnityEngine.Matrix4x4::TRS_Injected"])' "$out/engine-calls.json")
    printf '0x7091bce0\n' > "$out/matrix-head.txt"
    python3 recompiler/lift_all.py --lib "$(dirname "$1")/libunity.so" --bias 0x70000000 \
        --heads "$out/matrix-head.txt" --toolchain "$2" --poll "$3" --target x86_64-linux-gnu --out "$out/matrix"
    "$cxx" "${flags[@]}" recompiler/tests/linux-engine.cpp "$out/matrix/shard-00000.o" \
        -L"$out" -lil2cpp-test '-Wl,-rpath,$ORIGIN' -ldl -o "$out/test-engine"
    "$out/test-engine" "$5/UnityPlayer.so" "$transform"
fi
