#!/usr/bin/env bash
# Build the same PE linker family as the release toolchain, with its GNU --wrap
# and --defsym semantics. Run in an activated Emscripten environment.
set -euo pipefail
work=$(realpath -m "${1:?supply a scratch directory}")
mkdir -p "$work"
cd "$work"
archive=binutils-2.46.0.tar.xz
if ! test -f "$archive"; then
  curl -fLsS --retry 2 "https://sourceware.org/pub/binutils/releases/$archive" -o "$archive.part"
  mv "$archive.part" "$archive"
fi
echo "32f880bb4f69351f4ae54a5d00359625c6c49d8e76624fb5cffdf174c79c8d3212f66225b81c12933c6ed59604ab652560773dd92fab384b930c97a9d4e1fdf2  $archive" | sha512sum -c -
test -d binutils-2.46.0 || tar -xf "$archive"
# musl declares psignal with a const string. Emscripten omits its definition,
# so libiberty supplies the fallback implementation with the musl prototype.
sed -i 's/^psignal (int signo, char \*message)/psignal (int signo, const char *message)/' binutils-2.46.0/libiberty/strsignal.c
# GNU ld measures host resource usage even with --stats off. The SDK's
# resource-accounting stubs can overwrite argv in a Web Worker. Configure's
# link-only checks see those stubs as working POSIX implementations.
python3 - <<'PY'
from pathlib import Path
for filename, marker in [('ld/ldmain.c', '#include "plugin.h"'), ('libiberty/getruntime.c', '#include <sys/types.h>')]:
    source = Path('binutils-2.46.0') / filename
    original = source.read_text()
    guard = '#ifdef __EMSCRIPTEN__\n#undef HAVE_GETRUSAGE\n#undef HAVE_MALLINFO\n#undef HAVE_MALLINFO2\n#endif'
    if guard not in original:
        if original.count(marker) != 1:
            raise SystemExit('Unrecognized binutils source: ' + filename)
        source.write_text(original.replace(marker, marker + '\n' + guard))
PY
mkdir -p wasm
cd wasm
if ! test -f Makefile; then
  CC=emcc CXX=em++ AR=emar RANLIB=emranlib NM=emnm CC_FOR_BUILD=cc \
    CFLAGS="-O2" CXXFLAGS="-O2" \
    ../binutils-2.46.0/configure --build="$(../binutils-2.46.0/config.guess)" --host=wasm32-unknown-linux \
    --target=x86_64-w64-mingw32 --disable-nls --disable-gprof --disable-gprofng --disable-gdb --disable-gas \
    --disable-binutils --disable-sim --disable-gold --disable-plugins --disable-shared --disable-werror \
    --without-zstd --without-debuginfod --disable-libctf --disable-multilib --disable-install-libiberty
fi
make -j"${JOBS:-4}" MAKEINFO=true all-ld
cd ld
# Relink the completed ld with the Web Worker entry point and local Blob FS.
make -n -W ldmain.o ld-new MAKEINFO=true | sed -n '/mode=link.* -o ld-new /p' | tail -1 > link-command
test -s link-command
python3 - <<'PY'
from pathlib import Path
s = Path('link-command').read_text().strip()
flags = '-sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=worker -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=5MB -sFORCE_FILESYSTEM -sEXPORTED_RUNTIME_METHODS=FS,WORKERFS,callMain -lworkerfs.js'
Path('link-module.sh').write_text(s.replace(' -o ld-new ', ' -o gnu-link.mjs '+flags+' ')+'\n')
PY
sh link-module.sh
