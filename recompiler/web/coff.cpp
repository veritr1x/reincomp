// Compile the instrumented guest LLVM IR to the Windows player's x86-64 ABI.
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <memory>

int main(int argc, char **argv) {
    if ((argc != 4 && argc != 5) || (llvm::StringRef(argv[1]) != "-O1" && llvm::StringRef(argv[1]) != "-O2")) {
        llvm::errs() << "usage: coff -O1|-O2 input.ll output.o [trace-index.txt]\n";
        return 2;
    }
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser(); // The inline TEB poll uses x86 inline assembly.
    const std::string triple = "x86_64-w64-windows-gnu";
    std::string error;
    auto target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        llvm::errs() << error << '\n';
        return 1;
    }
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseIRFile(argv[2], diagnostic, context);
    if (!module) {
        diagnostic.print(argv[0], llvm::errs());
        return 1;
    }
    if (argc == 5) {
        std::error_code ec;
        llvm::raw_fd_ostream index(argv[4], ec);
        if (ec) {
            llvm::errs() << ec.message() << '\n';
            return 1;
        }
        for (const auto &f : *module) {
            auto name = f.getName();
            if (name.consume_front("sub_"))
                index << (f.isDeclaration() ? "extern " : "defined ") << name << '\n';
        }
    }
    auto level = llvm::StringRef(argv[1]) == "-O1" ? llvm::OptimizationLevel::O1 : llvm::OptimizationLevel::O2;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        triple, "x86-64", "", llvm::TargetOptions{}, llvm::Reloc::PIC_, std::nullopt,
        level == llvm::OptimizationLevel::O1 ? llvm::CodeGenOptLevel::Less : llvm::CodeGenOptLevel::Default));
    module->setTargetTriple(triple);
    module->setDataLayout(machine->createDataLayout());
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager components;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder passes(machine.get());
    passes.registerModuleAnalyses(modules);
    passes.registerCGSCCAnalyses(components);
    passes.registerFunctionAnalyses(functions);
    passes.registerLoopAnalyses(loops);
    passes.crossRegisterProxies(loops, functions, components, modules);
    passes.buildPerModuleDefaultPipeline(level).run(*module, modules);
    std::error_code ec;
    llvm::raw_fd_ostream output(argv[3], ec, llvm::sys::fs::OF_None);
    if (ec) {
        llvm::errs() << ec.message() << '\n';
        return 1;
    }
    llvm::legacy::PassManager codegen;
    if (machine->addPassesToEmitFile(codegen, output, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        llvm::errs() << "x86-64 object emission unavailable\n";
        return 1;
    }
    codegen.run(*module);
    output.close();
    return output.has_error() ? 1 : 0;
}
