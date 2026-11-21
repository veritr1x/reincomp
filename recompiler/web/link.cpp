#include <lld/Common/Driver.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/raw_ostream.h>

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(mingw)

int main(int argc, char **argv) {
    // The browser mounts only the object files and libraries for this link.
    // Each invocation gets its own module, compiler globals and filesystem.
    llvm::SmallVector<const char *> args(argv, argv + argc);
    if (argc > 1 && llvm::StringRef(argv[1]) == "--mingw") {
        args.erase(args.begin());
        args[0] = "ld.lld";
        return lld::mingw::link(args, llvm::outs(), llvm::errs(), false, false) ? 0 : 1;
    }
    args[0] = "lld-link";
    return lld::coff::link(args, llvm::outs(), llvm::errs(), false, false) ? 0 : 1;
}
