// LLVM18 opt-in State escape barriers at trace entries and cycle-cut blocks.
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

// CheckedFloatBinOp pins its operands and result between two compiler fences
// so a precise lift can collect FPSR exceptions. Once the speed profile has
// made that clear/test pair inert, reconstruct its arithmetic operation.
// Keep the operation sensitive to the active guest rounding mode.
static bool compilerFence(llvm::Instruction *inst) {
  auto *call = llvm::dyn_cast_or_null<llvm::CallInst>(inst);
  auto *assembly = call ? llvm::dyn_cast<llvm::InlineAsm>(call->getCalledOperand()) : nullptr;
  return assembly && assembly->hasSideEffects() && assembly->getAsmString().empty() &&
         assembly->getConstraintString() == "~{memory}" && call->arg_empty();
}

static void unpinUncheckedArithmetic(llvm::Module &module) {
  auto *clear = module.getFunction("__remill_fpu_exception_clear");
  auto *test = module.getFunction("__remill_fpu_exception_test");
  if (!clear || !test) return;
  for (auto *use : clear->users()) {
    auto *start = llvm::dyn_cast<llvm::CallInst>(use);
    if (!start) continue;
    auto *first = start->getNextNode();
    if (!compilerFence(first)) continue;
    auto *s1 = llvm::dyn_cast_or_null<llvm::StoreInst>(first->getNextNode());
    auto *s2 = s1 ? llvm::dyn_cast_or_null<llvm::StoreInst>(s1->getNextNode()) : nullptr;
    auto *l1 = s2 ? llvm::dyn_cast_or_null<llvm::LoadInst>(s2->getNextNode()) : nullptr;
    auto *l2 = l1 ? llvm::dyn_cast_or_null<llvm::LoadInst>(l1->getNextNode()) : nullptr;
    auto *op = l2 ? llvm::dyn_cast_or_null<llvm::BinaryOperator>(l2->getNextNode()) : nullptr;
    if (!op || op->getOperand(0) != l1 || op->getOperand(1) != l2) continue;
    // Packed add/multiply can exchange x86 operands and change which NaN
    // payload survives. Subtract and divide keep the original operand order.
    const bool packed = llvm::isa<llvm::FixedVectorType>(op->getType()) &&
                        op->getType()->isFPOrFPVectorTy() &&
                        (op->getOpcode() == llvm::Instruction::FSub ||
                         op->getOpcode() == llvm::Instruction::FDiv);
    if (!op->getType()->isFloatingPointTy() && !packed) continue;
    auto *result = llvm::dyn_cast_or_null<llvm::StoreInst>(op->getNextNode());
    if (!result || result->getValueOperand() != op) continue;
    auto *last = result->getNextNode();
    if (!compilerFence(last)) continue;
    auto *end = llvm::dyn_cast_or_null<llvm::CallInst>(last->getNextNode());
    if (!end || end->getCalledFunction() != test) continue;
    auto privateSlot = [](llvm::Value *ptr) {
      if (!llvm::isa<llvm::AllocaInst>(ptr)) return false;
      for (auto *user : ptr->users()) {
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user); load && !load->isAtomic()) continue;
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
            store && store->getPointerOperand() == ptr && store->getValueOperand() != ptr && !store->isAtomic()) continue;
        if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user);
            intrinsic && intrinsic->isLifetimeStartOrEnd()) continue;
        return false;
      }
      return true;
    };
    auto *a = s1->getPointerOperand(), *b = s2->getPointerOperand(), *r = result->getPointerOperand();
    if (a == b || a == r || b == r || !privateSlot(a) || !privateSlot(b) || !privateSlot(r) ||
        l1->getPointerOperand() != a || l2->getPointerOperand() != b ||
        s1->getValueOperand()->getType() != op->getType() || s2->getValueOperand()->getType() != op->getType()) continue;
    // The FPSR flag bookkeeping follows test(), then comes the result reload.
    for (auto *next = end->getNextNode(); next; next = next->getNextNode()) {
      if (llvm::isa<llvm::CallBase>(next)) break;
      if (auto *write = llvm::dyn_cast<llvm::StoreInst>(next); write && write->getPointerOperand() == r) break;
      auto *reload = llvm::dyn_cast<llvm::LoadInst>(next);
      if (!reload || reload->getPointerOperand() != r) continue;
      if (reload->getType() != op->getType()) break;
      llvm::Intrinsic::ID arithmetic;
      switch (op->getOpcode()) {
        case llvm::Instruction::FAdd: arithmetic = llvm::Intrinsic::experimental_constrained_fadd; break;
        case llvm::Instruction::FSub: arithmetic = llvm::Intrinsic::experimental_constrained_fsub; break;
        case llvm::Instruction::FMul: arithmetic = llvm::Intrinsic::experimental_constrained_fmul; break;
        case llvm::Instruction::FDiv: arithmetic = llvm::Intrinsic::experimental_constrained_fdiv; break;
        default: continue;
      }
      llvm::IRBuilder<> builder(op);
      auto *value = builder.CreateConstrainedFPBinOp(arithmetic, l1, l2, nullptr, "", nullptr,
                                                     llvm::RoundingMode::Dynamic, llvm::fp::ebStrict);
      op->replaceAllUsesWith(value);
      op->eraseFromParent();
      for (auto *store : {s1, s2, result}) store->setVolatile(false);
      for (auto *load : {l1, l2, reload}) load->setVolatile(false);
      first->eraseFromParent();
      last->eraseFromParent();
      break;
    }
  }
}

int main(int argc, char **argv) {
  // --speed (profile v18, opt-in; the default output is unchanged):
  //  * the entry poll goes after the entry block's static allocas, so they stay
  //    static and SROA/mem2reg still apply (no ___chkstk_ms dynamic allocas);
  //  * plain guest loads are inlined as volatile, align-1 loads;
  //  * FPSR is unobserved: exception clear/test fold to nothing/0. Valid only
  //    for images with no MRS/MSR FPSR (record the scan per build).
  //  Checked binary arithmetic uses dynamic rounding directly where its
  //  private operand and result slots match the exception-collection sequence.
  // The parts can be selected separately (for bisecting): --static-allocas,
  // --inline-reads, --fpsr-unobserved. --speed selects all three.
  // --inline-poll (separate opt-in): each poll site first reads this thread's
  // poll binding inline (cooperative_poll.h PollFastView) and calls
  // __bunker_guest_poll_requested only when its request epoch is undelivered.
  // The binding is the host's thread_local __bunker_poll_binding; Windows
  // x86-64 hosts use --inline-poll=teb (TEB TLS slot, no MinGW emutls call).
  // The IR triple names the guest, so the host is chosen by flag. Sites,
  // ordinals and the slow path are unchanged.
  // --direct-calls (Windows x86-64 host, opt-in): a lifted direct call site
  // (__bunker_guest_call with a constant lifted callee) links its
  // GuestCallFrame inline through this thread's ExecutionContext (TEB slot at
  // __bunker_exec_tls_offset), tests the override filter inline, and calls the
  // callee directly. A filter hit, or no inline context, takes the original
  // call; a pending transfer or a return elsewhere relinks the same frame in
  // __bunker_guest_call_finish. __bunker_guest_transfer_pending is read inline
  // too. Layouts are fixed by static_asserts in runtime.cpp.
  // --inline-stores (opt-in): plain guest stores become volatile, align-1
  // stores in the IR, as tools/inline_lifted_helpers.py --inline-stores already
  // does after link (so the store-side reservation invalidation is dropped the
  // same way). The identity helpers __remill_compare_*, the carry flag and
  // __remill_function_return are inlined too.
  bool speed = false, static_allocas = false, inline_reads = false, fpsr_unobserved = false, inline_poll = false, inline_poll_teb = false,
       direct_calls = false, inline_stores = false;
  for (int i = 1; i + 2 < argc; ++i) {
    const llvm::StringRef flag(argv[i]);
    if (flag == "--speed") speed = static_allocas = inline_reads = fpsr_unobserved = true;
    else if (flag == "--static-allocas") static_allocas = true;
    else if (flag == "--inline-reads") inline_reads = true;
    else if (flag == "--fpsr-unobserved") fpsr_unobserved = true;
    else if (flag == "--inline-poll") inline_poll = true;
    else if (flag == "--inline-poll=teb") inline_poll = inline_poll_teb = true;
    else if (flag == "--direct-calls") direct_calls = true;
    else if (flag == "--inline-stores") inline_stores = true;
    else { llvm::errs() << "unknown flag " << flag << '\n'; return 2; }
  }
  if (argc < 3) {
    llvm::errs() << "usage: poll-instrument [--speed|--static-allocas|--inline-reads|--fpsr-unobserved|--inline-poll[=teb]|--direct-calls]... input.ll output.ll\n";
    return 2;
  }
  const char *input_path = argv[argc - 2], *output_path = argv[argc - 1];
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(input_path, diagnostic, context);
  if (!module) { diagnostic.print(argv[0], llvm::errs()); return 1; }
  if (module->getFunction("__bunker_guest_poll_requested") || module->getFunction("__bunker_guest_poll_slow")) {
    llvm::errs() << "module already contains cooperative poll hooks\n"; return 1;
  }
  auto requested = module->getOrInsertFunction("__bunker_guest_poll_requested", llvm::FunctionType::get(llvm::Type::getInt32Ty(context), false));
  llvm::FunctionCallee poll_check = requested;
  if (inline_poll) {
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto external = [&](const char *name, llvm::Type *type) {
      if (module->getNamedValue(name)) { llvm::errs() << "module already defines " << name << '\n'; std::exit(1); }
      // Statically linked into the same executable: dso_local avoids a
      // MinGW .refptr indirection on every poll.
      auto *global = new llvm::GlobalVariable(*module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
      global->setDSOLocal(true);
      return global;
    };
    auto *function = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt32Ty(context), false),
        llvm::GlobalValue::InternalLinkage, "__bunker_guest_poll_fast", *module);
    function->addFnAttr(llvm::Attribute::AlwaysInline);
    auto *entry = llvm::BasicBlock::Create(context, "", function), *bound = llvm::BasicBlock::Create(context, "", function),
         *call = llvm::BasicBlock::Create(context, "", function), *idle = llvm::BasicBlock::Create(context, "", function);
    llvm::IRBuilder<> builder(entry);
    // Volatile loads: re-read at every site, never hoisted out of loops.
    auto load = [&](llvm::Type *type, llvm::Value *address, bool atomic = false) {
      auto *value = builder.CreateAlignedLoad(type, address, llvm::MaybeAlign(8), true);
      if (atomic) value->setAtomic(llvm::AtomicOrdering::Monotonic);
      return value;
    };
    llvm::Value *view;
    if (inline_poll_teb) {
      // Windows x86-64 host: TEB TLS slot read through GS (address space 256).
      auto *offset = load(i64, external("__bunker_poll_tls_offset", i64));
      auto *teb = llvm::BasicBlock::Create(context, "", function, bound);
      builder.CreateCondBr(builder.CreateICmpEQ(offset, builder.getInt64(0)), call, teb);
      builder.SetInsertPoint(teb);
      view = load(ptr, builder.CreateIntToPtr(offset, llvm::PointerType::get(context, 256)));
    } else {
      auto *slot = external("__bunker_poll_binding", ptr);
      slot->setThreadLocal(true);
      slot->setDSOLocal(false);  // the TLS model is left to the target/linker
      view = load(ptr, builder.CreateThreadLocalAddress(slot));
    }
    builder.CreateCondBr(builder.CreateIsNull(view), idle, bound);
    // PollFastView { const atomic<uint64_t> *requested; uint64_t delivered; }
    builder.SetInsertPoint(bound);
    auto *requested_epoch = load(i64, load(ptr, view), true);
    auto *delivered_epoch = load(i64, builder.CreateConstInBoundsGEP1_64(builder.getInt8Ty(), view, 8));
    builder.CreateCondBr(builder.CreateICmpEQ(requested_epoch, delivered_epoch), idle, call);
    llvm::IRBuilder<>(idle).CreateRet(llvm::IRBuilder<>(idle).getInt32(0));
    llvm::IRBuilder<> slow_check(call);
    slow_check.CreateRet(slow_check.CreateCall(requested));
    poll_check = function;
  }
  if (direct_calls) {
    auto *guest_call = module->getFunction("__bunker_guest_call");
    auto *pending = module->getFunction("__bunker_guest_transfer_pending");
    auto *i8 = llvm::Type::getInt8Ty(context), *i32 = llvm::Type::getInt32Ty(context), *i64 = llvm::Type::getInt64Ty(context);
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto external = [&](const char *name, llvm::Type *type) {
      if (module->getNamedValue(name)) { llvm::errs() << "module already defines " << name << '\n'; std::exit(1); }
      auto *global = new llvm::GlobalVariable(*module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
      global->setDSOLocal(true);
      return global;
    };
    llvm::GlobalVariable *tls_offset = nullptr;
    auto internal = [&](const char *name, llvm::FunctionType *type) {
      auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, *module);
      function->addFnAttr(llvm::Attribute::AlwaysInline);
      return function;
    };
    // The thread's ExecutionContext pointer; 0 offset or null context takes `slow`.
    auto context_of = [&](llvm::IRBuilder<> &builder, llvm::Function *function, llvm::BasicBlock *slow) {
      if (!tls_offset) tls_offset = external("__bunker_exec_tls_offset", i64);
      auto *offset = builder.CreateAlignedLoad(i64, tls_offset, llvm::MaybeAlign(8));
      auto *teb = llvm::BasicBlock::Create(context, "ctx.teb", function), *bound = llvm::BasicBlock::Create(context, "ctx.bound", function);
      builder.CreateCondBr(builder.CreateICmpEQ(offset, builder.getInt64(0)), slow, teb);
      builder.SetInsertPoint(teb);
      auto *value = builder.CreateAlignedLoad(ptr, builder.CreateIntToPtr(offset, llvm::PointerType::get(context, 256)), llvm::MaybeAlign(8), true);
      builder.CreateCondBr(builder.CreateIsNull(value), slow, bound);
      builder.SetInsertPoint(bound);
      return value;
    };
    // transfer.active (ctx+16) | *terminal_pending (ctx+8)
    auto pending_of = [&](llvm::IRBuilder<> &builder, llvm::Value *ctx) {
      auto *terminal = builder.CreateAlignedLoad(ptr, builder.CreateConstInBoundsGEP1_64(i8, ctx, 8), llvm::MaybeAlign(8));
      auto *active = builder.CreateAlignedLoad(i8, builder.CreateConstInBoundsGEP1_64(i8, ctx, 16), llvm::MaybeAlign(1));
      return builder.CreateOr(active, builder.CreateAlignedLoad(i8, terminal, llvm::MaybeAlign(1)));
    };
    llvm::Function *call_fast = nullptr, *pending_fast = nullptr;
    if (guest_call) {
      if (guest_call->arg_size() != 6 || !guest_call->getReturnType()->isPointerTy()) {
        llvm::errs() << "__bunker_guest_call differs from runtime contract\n"; return 1;
      }
      auto *finish_type = llvm::FunctionType::get(ptr, {ptr, ptr, ptr}, false);
      auto finish = module->getOrInsertFunction("__bunker_guest_call_finish", finish_type);
      call_fast = internal("__bunker_guest_call_inline", guest_call->getFunctionType());
      auto *state = call_fast->getArg(0), *target = call_fast->getArg(1), *memory = call_fast->getArg(2),
           *expected = call_fast->getArg(3), *caller_sp = call_fast->getArg(4), *direct = call_fast->getArg(5);
      auto *entry = llvm::BasicBlock::Create(context, "", call_fast);
      auto *slow = llvm::BasicBlock::Create(context, "call.slow", call_fast);
      llvm::IRBuilder<> builder(entry);
      // GuestCallFrame { previous, expected_pc, caller_sp, root }
      auto *frame = builder.CreateAlloca(llvm::ArrayType::get(i64, 4), nullptr, "guest.frame");
      frame->setAlignment(llvm::Align(16));
      auto *ctx = context_of(builder, call_fast, slow);
      // Override filter: bit (target >> 2) & 65535 of __bunker_guest_override_filter.
      auto *filter = external("__bunker_guest_override_filter", llvm::ArrayType::get(i64, 1024));
      auto *bit = builder.CreateAnd(builder.CreateLShr(target, 2), builder.getInt64(65535));
      auto *word = builder.CreateAlignedLoad(i64, builder.CreateInBoundsGEP(i64, filter, builder.CreateLShr(bit, 6)), llvm::MaybeAlign(8));
      auto *hit = builder.CreateTrunc(builder.CreateLShr(word, builder.CreateAnd(bit, 63)), builder.getInt1Ty());
      auto *fast = llvm::BasicBlock::Create(context, "call.fast", call_fast);
      builder.CreateCondBr(builder.CreateOr(hit, builder.CreateIsNull(direct)), slow, fast);
      builder.SetInsertPoint(fast);
      auto *previous = builder.CreateAlignedLoad(ptr, ctx, llvm::MaybeAlign(8));
      builder.CreateAlignedStore(previous, frame, llvm::MaybeAlign(16));
      builder.CreateAlignedStore(expected, builder.CreateConstInBoundsGEP1_64(i8, frame, 8), llvm::MaybeAlign(8));
      builder.CreateAlignedStore(caller_sp, builder.CreateConstInBoundsGEP1_64(i8, frame, 16), llvm::MaybeAlign(16));
      builder.CreateAlignedStore(builder.getInt64(0), builder.CreateConstInBoundsGEP1_64(i8, frame, 24), llvm::MaybeAlign(8));  // root=false
      builder.CreateAlignedStore(frame, ctx, llvm::MaybeAlign(8));
      builder.CreateAlignedStore(builder.getInt8(1), builder.CreateConstInBoundsGEP1_64(i8, ctx, 48), llvm::MaybeAlign(1));  // call_protocol_used
      auto *lifted = llvm::FunctionType::get(ptr, {ptr, i64, ptr}, false);
      auto *result = builder.CreateCall(lifted, direct, {state, target, memory});
      builder.CreateAlignedStore(previous, ctx, llvm::MaybeAlign(8));
      auto *pc = builder.CreateAlignedLoad(i64, builder.CreateConstInBoundsGEP1_64(i8, state, 1056), llvm::MaybeAlign(8));
      auto *sp = builder.CreateAlignedLoad(i64, builder.CreateConstInBoundsGEP1_64(i8, state, 1040), llvm::MaybeAlign(8));
      auto *normal = builder.CreateAnd(builder.CreateICmpEQ(pending_of(builder, ctx), builder.getInt8(0)),
          builder.CreateAnd(builder.CreateICmpEQ(pc, expected), builder.CreateICmpEQ(sp, caller_sp)));
      auto *done = llvm::BasicBlock::Create(context, "call.done", call_fast);
      auto *finish_block = llvm::BasicBlock::Create(context, "call.finish", call_fast);
      builder.CreateCondBr(normal, done, finish_block);
      llvm::IRBuilder<>(done).CreateRet(result);
      llvm::IRBuilder<> finishing(finish_block);
      finishing.CreateRet(finishing.CreateCall(finish, {state, result, frame}));
      llvm::IRBuilder<> original(slow);
      llvm::SmallVector<llvm::Value *, 6> arguments;
      for (auto &argument : call_fast->args()) arguments.push_back(&argument);
      original.CreateRet(original.CreateCall(guest_call, arguments));
    }
    if (pending) {
      if (pending->arg_size() != 1 || !pending->getReturnType()->isIntegerTy(32)) {
        llvm::errs() << "__bunker_guest_transfer_pending differs from runtime contract\n"; return 1;
      }
      pending_fast = internal("__bunker_guest_transfer_pending_inline", pending->getFunctionType());
      auto *entry = llvm::BasicBlock::Create(context, "", pending_fast);
      auto *slow = llvm::BasicBlock::Create(context, "pending.slow", pending_fast);
      llvm::IRBuilder<> builder(entry);
      auto *ctx = context_of(builder, pending_fast, slow);
      builder.CreateRet(builder.CreateZExt(builder.CreateICmpNE(pending_of(builder, ctx), builder.getInt8(0)), i32));
      llvm::IRBuilder<> original(slow);
      original.CreateRet(original.CreateCall(pending, {pending_fast->getArg(0)}));
    }
    size_t rewritten = 0, pending_sites = 0;
    for (auto &function : *module) {
      if (function.isDeclaration() || !function.getName().starts_with("sub_")) continue;
      for (auto &block : function) for (auto &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call) continue;
        if (call_fast && call->getCalledFunction() == guest_call && llvm::isa<llvm::Function>(call->getArgOperand(5))) {
          call->setCalledFunction(call_fast); call->setAttributes(llvm::AttributeList()); ++rewritten;
        } else if (pending_fast && call->getCalledFunction() == pending) {
          call->setCalledFunction(pending_fast); call->setAttributes(llvm::AttributeList()); ++pending_sites;
        }
      }
    }
    llvm::outs() << "Direct calls: " << rewritten << " inline call sites, " << pending_sites << " inline pending checks\n";
  }
  auto slow = module->getOrInsertFunction("__bunker_guest_poll_slow", llvm::FunctionType::get(llvm::Type::getVoidTy(context),
      {llvm::PointerType::getUnqual(context), llvm::Type::getInt64Ty(context), llvm::Type::getInt64Ty(context), llvm::Type::getInt64Ty(context)}, false));
  // These exact runtime helpers return their first boolean argument. Inline
  // them so a tight loop cannot rely on accidental opaque helper barriers.
  std::vector<const char *> identities{"__remill_flag_computation_sign", "__remill_flag_computation_zero",
                                       "__remill_flag_computation_overflow", "__remill_compare_neq"};
  if (speed || inline_stores)
    for (auto name : {"__remill_flag_computation_carry", "__remill_compare_eq", "__remill_compare_sle",
                      "__remill_compare_slt", "__remill_compare_sge", "__remill_compare_sgt", "__remill_compare_ule",
                      "__remill_compare_ult", "__remill_compare_uge", "__remill_compare_ugt"})
      identities.push_back(name);
  for (auto name : identities) {
    auto *function = module->getFunction(name);
    if (!function) continue;
    if (!function->isDeclaration() || function->arg_empty() || !function->getReturnType()->isIntegerTy(1) ||
        !function->getArg(0)->getType()->isIntegerTy(1)) {
      llvm::errs() << "identity intrinsic differs from runtime contract\n"; return 1;
    }
    function->setAttributes(llvm::AttributeList());
    function->setLinkage(llvm::GlobalValue::InternalLinkage);
    function->addFnAttr(llvm::Attribute::AlwaysInline);
    llvm::IRBuilder<>(llvm::BasicBlock::Create(context, "", function)).CreateRet(function->getArg(0));
  }
  const llvm::Attribute::AttrKind effects[] = {llvm::Attribute::Memory, llvm::Attribute::NoSync, llvm::Attribute::NoFree,
      llvm::Attribute::NoCallback, llvm::Attribute::WillReturn, llvm::Attribute::Speculatable,
      llvm::Attribute::NoUnwind, llvm::Attribute::MustProgress};
  const llvm::Attribute::AttrKind state_effects[] = {llvm::Attribute::NoCapture, llvm::Attribute::ReadOnly,
      llvm::Attribute::ReadNone, llvm::Attribute::WriteOnly};
  // Imported trace declarations and existing direct-call sites must also lose
  // old effect/nounwind assertions now that the callee can invoke a poll hook.
  for (auto &function : *module) {
    if (function.getName().starts_with("sub_")) {
      for (auto attr : effects) function.removeFnAttr(attr);
      if (!function.arg_empty()) for (auto attr : state_effects) function.getArg(0)->removeAttr(attr);
    }
    for (auto &block : function) for (auto &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
        if (auto *callee = call->getCalledFunction(); callee && callee->getName().starts_with("sub_")) {
          for (auto attr : effects) call->removeFnAttr(attr);
          if (call->arg_size()) for (auto attr : state_effects) call->removeParamAttr(0, attr);
        }
  }
  {
    auto define = [&](llvm::Function *function) {
      function->setAttributes(llvm::AttributeList());
      function->setLinkage(llvm::GlobalValue::InternalLinkage);
      function->addFnAttr(llvm::Attribute::AlwaysInline);
      return llvm::IRBuilder<>(llvm::BasicBlock::Create(context, "", function));
    };
    const std::pair<const char *, llvm::Type *> loads[] = {
        {"__remill_read_memory_8", llvm::Type::getInt8Ty(context)},
        {"__remill_read_memory_16", llvm::Type::getInt16Ty(context)},
        {"__remill_read_memory_32", llvm::Type::getInt32Ty(context)},
        {"__remill_read_memory_64", llvm::Type::getInt64Ty(context)},
        {"__remill_read_memory_f32", llvm::Type::getFloatTy(context)},
        {"__remill_read_memory_f64", llvm::Type::getDoubleTy(context)}};
    for (auto &[name, type] : loads) {
      if (!inline_reads) break;
      auto *function = module->getFunction(name);
      if (!function) continue;
      if (!function->isDeclaration() || function->arg_size() != 2 || function->getReturnType() != type ||
          !function->getArg(1)->getType()->isIntegerTy(64)) {
        llvm::errs() << name << " differs from runtime contract\n"; return 1;
      }
      auto builder = define(function);
      auto *address = builder.CreateIntToPtr(function->getArg(1), llvm::PointerType::getUnqual(context));
      builder.CreateRet(builder.CreateAlignedLoad(type, address, llvm::MaybeAlign(1), true));
    }
    const std::pair<const char *, llvm::Type *> stores[] = {
        {"__remill_write_memory_8", llvm::Type::getInt8Ty(context)},
        {"__remill_write_memory_16", llvm::Type::getInt16Ty(context)},
        {"__remill_write_memory_32", llvm::Type::getInt32Ty(context)},
        {"__remill_write_memory_64", llvm::Type::getInt64Ty(context)},
        {"__remill_write_memory_f32", llvm::Type::getFloatTy(context)},
        {"__remill_write_memory_f64", llvm::Type::getDoubleTy(context)}};
    for (auto &[name, type] : stores) {
      if (!inline_stores) break;
      auto *function = module->getFunction(name);
      if (!function) continue;
      if (!function->isDeclaration() || function->arg_size() != 3 || !function->getReturnType()->isPointerTy() ||
          !function->getArg(0)->getType()->isPointerTy() || !function->getArg(1)->getType()->isIntegerTy(64) ||
          function->getArg(2)->getType() != type) {
        llvm::errs() << name << " differs from runtime contract\n"; return 1;
      }
      auto builder = define(function);
      auto *address = builder.CreateIntToPtr(function->getArg(1), llvm::PointerType::getUnqual(context));
      builder.CreateAlignedStore(function->getArg(2), address, llvm::MaybeAlign(1), true);
      builder.CreateRet(function->getArg(0));
    }
    if (auto *function = (speed || inline_stores) ? module->getFunction("__remill_function_return") : nullptr) {
      if (!function->isDeclaration() || function->arg_size() != 3 || !function->getReturnType()->isPointerTy()) {
        llvm::errs() << "__remill_function_return differs from runtime contract\n"; return 1;
      }
      define(function).CreateRet(function->getArg(2));
    }
    if (auto *function = fpsr_unobserved ? module->getFunction("__remill_fpu_exception_clear") : nullptr) {
      if (!function->isDeclaration() || !function->getReturnType()->isVoidTy()) {
        llvm::errs() << "__remill_fpu_exception_clear differs from runtime contract\n"; return 1;
      }
      define(function).CreateRetVoid();
    }
    if (auto *function = fpsr_unobserved ? module->getFunction("__remill_fpu_exception_test") : nullptr) {
      if (!function->isDeclaration() || !function->getReturnType()->isIntegerTy()) {
        llvm::errs() << "__remill_fpu_exception_test differs from runtime contract\n"; return 1;
      }
      define(function).CreateRet(llvm::ConstantInt::get(function->getReturnType(), 0));
    }
    // The allocas become static below, so stack coloring would start sharing
    // slots between temporaries whose lifetime markers do not overlap. The v17
    // dynamic allocas never shared storage; drop the markers to keep that
    // memory model (a store through a shared slot faulted).
    llvm::SmallVector<llvm::Instruction *, 256> markers;
    if (static_allocas)
      for (auto &function : *module) for (auto &block : function) for (auto &instruction : block)
        if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction); intrinsic && intrinsic->isLifetimeStartOrEnd())
          markers.push_back(intrinsic);
    for (auto *marker : markers) marker->eraseFromParent();
    if (fpsr_unobserved) unpinUncheckedArithmetic(*module);
  }
  size_t traces = 0, cycle_blocks = 0;
  for (auto &function : *module) {
    if (function.isDeclaration() || !function.getName().starts_with("sub_")) continue;
    if (function.arg_size() != 3 || !function.getArg(0)->getType()->isPointerTy() ||
        !function.getArg(1)->getType()->isIntegerTy(64) || !function.getArg(2)->getType()->isPointerTy() ||
        !function.getReturnType()->isPointerTy()) {
      llvm::errs() << "unexpected lifted signature\n"; return 1;
    }
    uint64_t entry;
    if (function.getName().drop_front(4).getAsInteger(16, entry)) return 1;
    // Removing all DFS backedges makes a directed graph acyclic, including
    // irreducible multi-entry loops. Poll once before each source terminator.
    // Search every component as well as the entry-reachable CFG; output block
    // order, not pointer ordering, determines stable site ordinals.
    std::map<llvm::BasicBlock *, unsigned> color;
    std::set<llvm::BasicBlock *> cuts;
    for (auto &root : function) if (!color[&root]) {
      std::vector<std::pair<llvm::BasicBlock *, unsigned>> stack{{&root, 0}};
      color[&root] = 1;
      while (!stack.empty()) {
        auto &[block, successor] = stack.back();
        auto *terminator = block->getTerminator();
        if (successor == terminator->getNumSuccessors()) {
          color[block] = 2; stack.pop_back(); continue;
        }
        auto *next = terminator->getSuccessor(successor++);
        if (color[next] == 1) cuts.insert(block);
        else if (!color[next]) { color[next] = 1; stack.emplace_back(next, 0); }
      }
    }
    // Hook calls synchronize, may throw, and escape State. Inferred attributes
    // of uninstrumented code must not survive those new observable effects.
    llvm::SmallVector<llvm::Instruction *, 16> sites;
    if (static_allocas) {
      // Static allocas first, then the entry poll, so splitting the entry block
      // leaves every one of them in the (static) entry block.
      auto &entry_block = function.getEntryBlock();
      llvm::SmallVector<llvm::AllocaInst *, 32> allocas;
      for (auto &instruction : entry_block)
        if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction); alloca && alloca->isStaticAlloca()) allocas.push_back(alloca);
      // Dynamic allocas were rounded to 16 bytes and 16-byte aligned. Keep that
      // layout: some semantics access a 12-byte temporary with 16-byte vector
      // operations, harmless inside the rounded slot but a clobber of the next
      // slot once static allocas are packed (seen in sub_7085f44c).
      const auto &layout = module->getDataLayout();
      for (auto *alloca : allocas) {
        const uint64_t bytes = alloca->getAllocationSize(layout)->getFixedValue();
        const uint64_t padded = (bytes + 15) & ~uint64_t(15);
        if (padded != bytes && !alloca->isArrayAllocation()) alloca->setAllocatedType(llvm::ArrayType::get(llvm::Type::getInt8Ty(context), padded));
        if (alloca->getAlign() < llvm::Align(16)) alloca->setAlignment(llvm::Align(16));
      }
      auto *first = &*entry_block.getFirstInsertionPt();
      for (auto *alloca : allocas) if (alloca != first) alloca->moveBefore(first);
      auto position = entry_block.getFirstInsertionPt();
      while (llvm::isa<llvm::AllocaInst>(&*position)) ++position;
      sites.push_back(&*position);
    } else {
      sites.push_back(&*function.getEntryBlock().getFirstInsertionPt());
    }
    for (auto &block : function) if (cuts.count(&block)) { sites.push_back(block.getTerminator()); ++cycle_blocks; }
    uint64_t ordinal = 0;
    for (auto *site : sites) {
      llvm::IRBuilder<> builder(site);
      auto *check = builder.CreateCall(poll_check);
      // Split manually so the lifted block name and its PHIs retain their
      // normal semantics. LLVM updates successor PHIs on splitBasicBlock.
      auto *before = site->getParent();
      auto *continuation = before->splitBasicBlock(site, "poll.continue");
      before->getTerminator()->eraseFromParent();
      auto *delivery = llvm::BasicBlock::Create(context, "poll.deliver", &function, continuation);
      llvm::IRBuilder<> branch(before);
      branch.CreateCondBr(branch.CreateICmpNE(check, branch.getInt32(0)), delivery, continuation);
      llvm::IRBuilder<> handler(delivery);
      llvm::Value *incoming_pc = function.getArg(1);
      if (ordinal) incoming_pc = handler.getInt64(0);
      handler.CreateCall(slow, {function.getArg(0), handler.getInt64(entry), handler.getInt64(ordinal++), incoming_pc});
      handler.CreateBr(continuation);
    }
    ++traces;
  }
  if (llvm::verifyModule(*module, &llvm::errs())) return 1;
  std::error_code error;
  llvm::raw_fd_ostream output(output_path, error);
  if (error) { llvm::errs() << error.message() << '\n'; return 1; }
  module->print(output, nullptr);
  llvm::outs() << "Cooperative polls: " << traces << " trace entries, " << cycle_blocks << " cycle-cut blocks\n";
}
