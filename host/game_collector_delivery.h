#pragma once
#include <chrono>
#include <thread>
#include "game_collector_mask.h"
#include "game_socket_wait_context.h"
// Included inside the harness namespace, after its checked Read/Require helpers.
// Creator-certified original finalizer and managed-worker condition profiles.
// Their role-specific wait certificates share the original stop/restart path.
class GameCollectorDelivery {
  using Pthreads=bunker::windows::Pthreads;
  using SignalHost=bunker::windows::SignalHost;
  using SignalDelivery=bunker::windows::SignalDelivery;
  using Lifecycle=bunker::windows::WorkerSignalLifecycle;
 public:
  // Optional composition at the already installed lifecycle/wait hooks. The
  // extension owns its admission policy; the finalizer profile stays separate.
  struct WorkerExtension {
    std::function<void(const Pthreads::WorkerInfo&,Lifecycle&)> prepare;
    std::function<void(const Pthreads::WorkerInfo&)> enter,before_tls;
    std::function<void(const Pthreads::WorkerInfo&,Pthreads::WorkerRetirement)> retire;
    std::function<bool(uint64_t)> owns;
    std::function<bool(const Pthreads::WaitContext&)> select_mutex;
    std::function<void(const Pthreads::WaitContext&)> checkpoint;
    // Optional external UnityMain contended-mutex admission (not a worker).
    std::function<bool(uint64_t)> main_owns;
    std::function<bool(const Pthreads::WaitContext&)> main_select;
    std::function<void(const Pthreads::WaitContext&)> main_checkpoint;
    // An embedding host can retain the parked parent invocation registers and
    // stack for the full stop/scan/restart cycle. Ordinary workers have none.
    std::function<bool(State *const *,size_t)> sender_parent_roots;
  };
 private:
  struct Channel {
    std::mutex mutex;std::condition_variable condition;uint64_t generation=0;
    uint64_t Generation(){std::lock_guard lock(mutex);return generation;}
    void Notify(){std::lock_guard lock(mutex);Require(generation!=UINT64_MAX,"collector wake generation exhausted");++generation;condition.notify_all();}
    void Wait(uint64_t expected,uint64_t relative){std::unique_lock lock(mutex);if(relative)condition.wait_for(lock,std::chrono::nanoseconds(relative),[&]{return generation!=expected;});else condition.wait(lock,[&]{return generation!=expected;});}
  };
  static std::shared_ptr<const Pthreads::WaitWake> MainWaitWake(){
    // A mutex unlock may still be delivering a notification after the waiter
    // returns. Only reuse the channel once the last enrollment releases it.
    static thread_local std::shared_ptr<const Pthreads::WaitWake> cached;
    if(cached && cached.use_count()==1)return cached;
    auto channel=std::make_shared<Channel>();
    auto wake=std::make_shared<const Pthreads::WaitWake>(Pthreads::WaitWake{
      [channel]{return channel->Generation();},[channel]{channel->Notify();},
      [channel](uint64_t generation,uint64_t){channel->Wait(generation,1000000);}});
    if(!cached)cached=wake;
    return wake;
  }
  enum class Role {unknown,finalizer,managed_worker};
  enum class ManagedWait {unknown,event_timed,event_untimed,condition_timed,condition_untimed};
  // Select by the proven caller, then validate all phase/argument/root fields.
  // A malformed known caller must not escape validation as an ordinary wait.
  static ManagedWait ManagedSite(const State&state){
    switch(state.gpr.x30.qword){
      case 0x82550574:return ManagedWait::event_timed;
      case 0x825505d8:return ManagedWait::event_untimed;
      case 0x82550164:return ManagedWait::condition_timed;
      case 0x82550174:return ManagedWait::condition_untimed;
      default:return ManagedWait::unknown;
    }
  }
  static unsigned ManagedMutexSite(const State&state){
    switch(state.gpr.x30.qword){case 0x8255045c:return 1;case 0x825bf1cc:return 2;case 0x825bf018:return 3;case 0x8258a538:return 4;default:break;}
    // Any other direct pthread_mutex_lock from original libil2cpp text: the stop
    // is delivered at the import boundary, as Android interrupts the futex wait
    // inside pthread_mutex_lock. Roots are the full import-entry State.
    const auto lr=state.gpr.x30.qword;return lr>=0x80000000&&lr<0x88000000&&!(lr&3)?5:0;
  }
  static bool ManagedContext(State&state){
    if(OpaqueDepth()||recomp::CurrentExecutionState()!=&state)return false;
    const auto frames=recomp::CurrentExecutionFrames(state);
    return frames.size()==1&&frames[0]==&state;
  }
  struct Owner {
    Lifecycle::ContextRef context;Role role=Role::unknown;
    std::shared_ptr<Channel> channel=std::make_shared<Channel>();
    std::unique_ptr<recomp::PollBinding> binding;
    std::unique_ptr<SignalDelivery> delivery;
    std::atomic<bool> wait_live{false},stop_live{false},queue_blocked{false},retiring{false};
    std::atomic<addr_t> record{0};std::atomic<uint64_t> stop_epoch{0};
    std::atomic<uint64_t> certified_epoch{0},begun_epoch{0};
    std::atomic<bool> certified_new_epoch{false};uint64_t pump_calls=0;
    const Pthreads::WaitContext *checkpoint=nullptr;State *wait_state=nullptr;
    // Original IL2CPP GC_lock backoff NanoSleep (see BackoffSleepSite).
    std::atomic<bool> backoff{false};const SignalHost::Boundary *backoff_boundary=nullptr;SignalHost::Dispatch backoff_selected{};
    // NanoSleep entry PC/LR/X0/X1. Before the exit drain revalidates, the
    // import has written its result to X0 and returned (PC = LR), so the site
    // and arguments are checked as admitted.
    uint64_t backoff_request=0,backoff_remaining=0;addr_t backoff_pc=0,backoff_lr=0;
    std::atomic<GameSocketWaitContext*> socket_wait{nullptr};std::mutex socket_sender_mutex;
    const SignalHost::Boundary *socket_boundary=nullptr;SignalHost::Dispatch socket_selected{};
    State interrupted{};addr_t frame_base=0;unsigned depth=0,posts=0;
    ~Owner(){if(binding||delivery){std::fputs("Game GC: owner destroyed without same-worker retirement\n",stderr);std::abort();}}
  };
  static bool ConditionRole(const Owner&owner){return owner.role==Role::finalizer||owner.role==Role::managed_worker;}
  struct Witness {Role role=Role::unknown;};
  struct ProviderDeferred {}; // Caught only by the direct C++ provider site.
  Pthreads &threads_;SignalHost &signals_;bunker::windows::Semaphores &semaphores_;recomp::Runtime &runtime_;
  Lifecycle lifecycle_;WorkerExtension extension_;std::mutex mutex_;std::map<uint64_t,std::shared_ptr<Owner>> owners_;bool closed_=false;
  static Owner *&Current(){static thread_local Owner *owner=nullptr;return owner;}
  static const Witness *&Creating(){static thread_local const Witness *witness=nullptr;return witness;}
  static unsigned &OpaqueDepth(){static thread_local unsigned depth=0;return depth;}
  static constexpr uint64_t kProviderOrdinal=UINT64_MAX,kManagedProviderOrdinal=UINT64_MAX-4;
  static uint64_t ProviderOrdinal(const Owner&owner){return owner.role==Role::managed_worker?kManagedProviderOrdinal:kProviderOrdinal;}
  static constexpr addr_t kReturn=0x825aada0,kAck=0x87cb1e68,kEpoch=0x879a6b08,kStopped=0x879a6b10;
  [[noreturn]] static void Fail(const char *message){std::fprintf(stderr,"Game GC: unsupported bounded profile: %s; execution stopped without host-generated collector acknowledgment\n",message);std::fflush(stderr);std::abort();}
  static void Check(bool value,const char *message){if(!value)Fail(message);}
  // Return addresses of all ten direct BL 8258a510 sites in original libil2cpp.
  static bool ClearStateCaller(addr_t caller){
    switch(caller){case 0x82561fcc:case 0x825622bc:case 0x825625d0:case 0x825820a8:case 0x8258210c:
      case 0x82582198:case 0x8258223c:case 0x8258a708:case 0x8258aaf0:case 0x8258abc8:return true;default:return false;}
  }
  // Thread::Sleep (libil2cpp 82562060) sets WaitSleepJoin, sleeps, then
  // tail-calls ClearState(thread, 0x20) at 8256209c, so ClearState's saved LR
  // is the return address of whoever called Sleep. Qualify it by decoding the
  // BL immediately before that return address: its target must be Sleep, the
  // icall thunk 825625ac (b 82562060), or managed Thread.Sleep(int) 843edde8,
  // which tail-branches to the thunk for every valid timeout.
  static bool SleepTailCaller(addr_t caller){
    if(caller&3||caller<0x80000004||caller>=0x88000000)return false;
    const auto insn=Read<uint32_t>(caller-4);if((insn>>26)!=0x25)return false; // BL imm26
    const auto target=caller-4+addr_t(int64_t(int32_t(insn<<6)>>6)*4);
    return target==0x82562060||target==0x825625ac||target==0x843edde8;
  }
  static void ManagedMutexCheck(bool value,unsigned site,const State&state,const Pthreads::WorkerInfo&info,addr_t mutex,const char*message,const char*group){
    if(!value&&site==4){
      const auto sp=state.gpr.sp.qword,object=state.gpr.x19.qword;
      const bool aligned=!(sp&15),above_low=sp>=info.stack_low,frame_capacity=sp<=info.stack_high&&info.stack_high-sp>=0x30;
      const bool owned_frame=aligned&&above_low&&frame_capacity;
      // The rejection path must not inspect an unowned/malformed stack or add
      // heap dereferences. Only a complete owned frame permits its saved LR read.
      const auto caller=owned_frame?Read<addr_t>(sp+0x28):addr_t(0);
      std::fprintf(stderr,"Game GC: ClearState rejection group=%s native_tid=%lu prepared_tid=%u token=%#llx PC=%#llx LR=%#llx SP=%#llx FP=%#llx stack_low=%#llx stack_high=%#llx X19=%#llx X20=%#llx X21=%#llx X0=%#llx mutex=%#llx aligned=%u above_low=%u frame_capacity=%u fp_shape=%u object_nonzero=%u object_range=%u mask_zero=%u saved_caller_read=%u saved_caller=%#llx; diagnostic only, admission unchanged\n",group,GetCurrentThreadId(),info.native_tid,(unsigned long long)info.token,(unsigned long long)state.gpr.pc.qword,(unsigned long long)state.gpr.x30.qword,(unsigned long long)sp,(unsigned long long)state.gpr.x29.qword,(unsigned long long)info.stack_low,(unsigned long long)info.stack_high,(unsigned long long)object,(unsigned long long)state.gpr.x20.qword,(unsigned long long)state.gpr.x21.qword,(unsigned long long)state.gpr.x0.qword,(unsigned long long)mutex,unsigned(aligned),unsigned(above_low),unsigned(frame_capacity),unsigned(frame_capacity&&state.gpr.x29.qword==sp+0x20),unsigned(object!=0),unsigned(object<=UINT64_MAX-0x18),unsigned(state.gpr.x21.qword==0),unsigned(owned_frame),(unsigned long long)caller);
    }
    Check(value,message);
  }
  std::shared_ptr<Owner> Lookup(uint64_t token){std::lock_guard lock(mutex_);auto i=owners_.find(token);return i==owners_.end()?nullptr:i->second;}
  bool Pending(const Owner &owner){uint64_t blocked=0;Check(!signals_.Pending(owner.context->PreparedWorker().token,&blocked),"signal owner is not live");return blocked||SignalHost::NeedsPoll(owner.context->SignalThread());}
  void RetainManagedKnown(Owner &owner){
    const auto result=signals_.DispatchOneAdmittedSubset(owner.context->PreparedWorker().token,SignalHost::Bit(30)|SignalHost::Bit(24),0,{});
    Check(!result.error,"unknown eligible signal outside a managed delivery phase");
  }
  bool SelectMutex(const Pthreads::WaitContext &context){
    if(extension_.select_mutex&&extension_.select_mutex(context))return true;
    if(extension_.main_owns&&extension_.main_select&&extension_.main_owns(context.token))return extension_.main_select(context);
    auto *owner=Current();return owner&&owner->role==Role::managed_worker&&!owner->socket_wait.load()&&!owner->depth&&
      context.phase==Pthreads::WaitPhase::MutexLock&&context.active_state&&owner->wait_state==context.active_state&&
      ManagedMutexSite(*context.active_state)&&(context.active_state->gpr.pc.qword==0x7ffc60000030ULL||ManagedMutexSite(*context.active_state)==5)&&ManagedContext(*context.active_state);
  }
  addr_t Record(uint64_t token){
    Check(Read<uint32_t>(0x879a62a0)==1,"original GC registry is not initialized");
    uint32_t hash=uint32_t(token>>8)^uint32_t(token);hash^=hash>>16;
    addr_t cursor=Read<addr_t>(0x879a6b28+(hash&255)*8);std::array<addr_t,256> seen{};size_t count=0;
    while(cursor){Check(!(cursor&7)&&cursor<=UINT64_MAX-0x60&&count<seen.size(),"original GC record chain shape/capacity");for(size_t i=0;i<count;++i)Check(seen[i]!=cursor,"original GC record cycle");seen[count++]=cursor;if(Read<uint64_t>(cursor+8)==token)return cursor;cursor=Read<addr_t>(cursor);}
    return 0;
  }
  void Dispositions(){
    Check(Read<int32_t>(0x87984e14)==30&&Read<int32_t>(0x87984e18)==24&&Read<int32_t>(0x87984e10)==1,"original configured signals/retry differ");
    const uint64_t expected=~(SignalHost::Bit(2)|SignalHost::Bit(3)|SignalHost::Bit(6)|SignalHost::Bit(15)|SignalHost::Bit(9)|SignalHost::Bit(19));
    for(int signal:{30,24}){bunker::windows::AndroidSignalAction action{};Check(!signals_.Action(signal,nullptr,&action),"original disposition query failed");Check(action.handler==0x825beb3c&&action.flags==uint32_t(signal==30?0x10000004:0x10000000)&&action.mask==expected,"original disposition/thunk/mask differs");}
    Check(Read<addr_t>(0x879a7788)==0x825b2af4&&Read<addr_t>(0x879a7758)==0x825b2c70,"original handler table differs");
    int32_t count=0;Check(!semaphores_.GetValue(kAck,&count),"original acknowledgment semaphore is not initialized");
  }
  void ValidateWait(Owner &owner,const Pthreads::WaitContext &context,const SignalHost::Delivery *selected=nullptr){
    const auto &info=owner.context->PreparedWorker();auto *state=recomp::CurrentExecutionState();
    Check(ConditionRole(owner)&&owner.context->CurrentPhase()==Lifecycle::Phase::entered&&Current()==&owner,"wait has no entered creator-certified condition owner");
    Check(context.token==info.token&&context.active_state&&context.active_state==state&&threads_.Self()==info.token&&owner.context->NativeThreadId()==GetCurrentThreadId(),"wait active State/native identity differs");
    const auto frames=recomp::CurrentExecutionFrames(*state);Check(frames.size()==1&&frames[0]==state&&!OpaqueDepth(),"parent/native opaque root phase is not qualified");
    Check(context.phase==Pthreads::WaitPhase::ConditionReleased||context.phase==Pthreads::WaitPhase::MutexReacquire||(owner.role==Role::managed_worker&&context.phase==Pthreads::WaitPhase::MutexLock),"unknown native wait phase");
    const addr_t condition=reinterpret_cast<addr_t>(context.condition),mutex=reinterpret_cast<addr_t>(context.mutex);
    if(owner.role==Role::finalizer){
      Check(!context.original_deadline&&!state->gpr.x2.qword&&state->gpr.x30.qword==0x825505d8&&state->gpr.x0.qword==reinterpret_cast<addr_t>(context.condition)&&state->gpr.x1.qword==reinterpret_cast<addr_t>(context.mutex),"original untimed condition arguments/return differ");
      Check(condition>=0x2c&&mutex==condition-0x28&&Read<addr_t>(0x879a7a40)==condition-0x2c&&state->gpr.pc.qword==0x7ffc60000020ULL,"actual finalizer event/import identity or condition/mutex geometry differs");
    }else if(context.phase==Pthreads::WaitPhase::MutexLock){
      const auto site=ManagedMutexSite(*state);Check(owner.role==Role::managed_worker&&site,"managed mutex caller is unqualified");
      ManagedMutexCheck(!context.condition&&!context.original_deadline&&mutex&&state->gpr.x0.qword==mutex&&(state->gpr.pc.qword==0x7ffc60000030ULL||(site==5&&(state->gpr.pc.qword==0x7ffc60000038ULL||state->gpr.pc.qword==0x7ffc60000040ULL))),site,*state,info,mutex,"managed mutex actual import/context differs","context");
      if(site==5){}else{
      const uint64_t frame_size=site==1?0x70:site==4?0x30:0x60,frame_pointer=site==1?0x60:site==4?0x20:0x50;
      ManagedMutexCheck(state->gpr.sp.qword<=info.stack_high&&info.stack_high-state->gpr.sp.qword>=frame_size&&state->gpr.x29.qword==state->gpr.sp.qword+frame_pointer,site,*state,info,mutex,"original managed mutex helper frame differs","frame");
      if(site==1)Check(state->gpr.x21.qword<=UINT64_MAX-4&&state->gpr.x19.qword==mutex&&state->gpr.x21.qword+4==mutex,"original event mutex geometry differs");
      else if(site==4){
        // Original ClearState 8258a510 (lock; state&=~W1; unlock). W21 holds the
        // zero-extended W1. Every direct BL site in libil2cpp passes the constant
        // 0 or 0x20 (WaitSleepJoin), including the ThreadStateSetter destructor
        // 825625b8 whose four constructors all store 0x20. The Thread::Sleep
        // tail-call site 8256209c is qualified through SleepTailCaller.
        // X20 is a wrapper pointer; the tail forwarder loads its stored mutex.
        const auto sp=state->gpr.sp.qword,object=state->gpr.x19.qword,clear_mask=state->gpr.x21.qword;
        ManagedMutexCheck(!(sp&15)&&sp>=info.stack_low&&object&&object<=UINT64_MAX-0x18&&(clear_mask==0||clear_mask==0x20),site,*state,info,mutex,"original managed ClearState object/mask/stack differs","object-mask-stack");
        const auto caller=Read<addr_t>(sp+0x28);
        ManagedMutexCheck(ClearStateCaller(caller)||(clear_mask==0x20&&SleepTailCaller(caller)),site,*state,info,mutex,"original managed ClearState caller differs","caller");
        const auto inner=Read<addr_t>(object+0x10);
        ManagedMutexCheck(inner&&inner<=UINT64_MAX-0xa8,site,*state,info,mutex,"original managed ClearState inner pointer differs","inner");
        const auto wrapper=Read<addr_t>(inner+0xa0);
        ManagedMutexCheck(wrapper&&wrapper<=UINT64_MAX-8&&state->gpr.x20.qword==wrapper&&mutex==Read<addr_t>(wrapper),site,*state,info,mutex,"original managed ClearState wrapper/mutex identity differs","wrapper-mutex");
      }
      else{const auto queue=Read<addr_t>(0x879a78c0);Check(queue&&queue<=UINT64_MAX-0x20&&mutex==Read<addr_t>(queue+0x20),"original managed queue mutex identity differs");}
      }
    }else{
      Check(owner.role==Role::managed_worker,"condition owner has no certified role");
      const auto site=ManagedSite(*state);Check(site!=ManagedWait::unknown,"managed-worker condition caller is unqualified");
      const bool timed=site==ManagedWait::event_timed||site==ManagedWait::condition_timed;
      if(timed)Check(context.original_deadline&&reinterpret_cast<addr_t>(context.original_deadline)==state->gpr.sp.qword&&state->gpr.x2.qword==state->gpr.sp.qword&&state->gpr.pc.qword==0x7ffc60000028ULL,"original managed-worker timed condition phase/deadline differs");
      else Check(!context.original_deadline&&state->gpr.pc.qword==0x7ffc60000020ULL,"original managed-worker untimed condition phase differs"); // X2 is not an untimed ABI argument.
      Check(condition&&mutex&&state->gpr.x0.qword==condition&&state->gpr.x1.qword==mutex,"original managed-worker condition/mutex arguments differ");
      const bool event=site==ManagedWait::event_timed||site==ManagedWait::event_untimed;
      if(event)Check(condition>=0x2c&&mutex==condition-0x28,"original managed-worker event geometry differs");
      const uint64_t frame_size=event?0x70:0x50,frame_pointer=event?0x60:0x40;
      Check(state->gpr.sp.qword<=info.stack_high&&info.stack_high-state->gpr.sp.qword>=frame_size&&state->gpr.x29.qword==state->gpr.sp.qword+frame_pointer,"original managed-worker helper frame differs");
    }
    Check(!(state->gpr.sp.qword&15)&&state->gpr.sp.qword>=info.stack_low&&state->gpr.sp.qword<=info.stack_high&&state->gpr.sp.qword-info.stack_low>=9504&&state->sr.tpidr_el0.qword==info.tls_address,"normal stack/TLS or two-frame capacity differs");
    const auto stack=signals_.AlternateStack(info.token).configured;Check(stack.flags==2&&!stack.pointer&&!stack.size,"target alternate stack is not fresh disabled");
    uint64_t mask=0;Check(!signals_.Mask(info.token,0,nullptr,&mask),"wait signal mask query failed");
    Check(selected?GameCollectorWaitMask::SelectedStop(info.token,mask,*selected):GameCollectorWaitMask::Ordinary(mask),selected?"selected stop action/saved/current mask differs":"ordinary wait blocks original stop/restart signals");
    if(selected && !GameQuietTrace())std::fprintf(stderr,"Game GC: selected outer mask verified target=%#llx saved=%#llx current=%#llx action=%#llx signal=%d; no host mask mutation\n",(unsigned long long)info.token,(unsigned long long)selected->saved_mask,(unsigned long long)mask,(unsigned long long)selected->action.mask,selected->signal);
  }
  // The original GC_lock slow path (libil2cpp 825a7180) spins, sched_yields,
  // then sleeps with a stack timespec and null remainder while the allocation
  // lock is held, e.g. by a collecting thread that is stopping the world. It
  // holds no collector state across the sleep, so the stop is delivered there
  // exactly as Android's interrupted nanosleep would.
  static constexpr addr_t kNanoSleep=0x7ff600300008ULL,kBackoffReturn=0x825a7230;
  void ValidateBackoff(Owner &owner,const SignalHost::Delivery *selected=nullptr){
    const auto &info=owner.context->PreparedWorker();auto *state=recomp::CurrentExecutionState();
    Check(ConditionRole(owner)&&owner.backoff&&owner.wait_state&&owner.context->CurrentPhase()==Lifecycle::Phase::entered&&Current()==&owner,"backoff has no entered creator-certified condition owner");
    Check(state==owner.wait_state&&threads_.Self()==info.token&&owner.context->NativeThreadId()==GetCurrentThreadId()&&!OpaqueDepth(),"backoff active State/native identity differs");
    const auto frames=recomp::CurrentExecutionFrames(*state);Check(frames.size()==1&&frames[0]==state,"backoff has parent Runtime roots");
    const auto sp=state->gpr.sp.qword;
    const bool exact=state->gpr.x30.qword==kBackoffReturn;
    State entry=*state;entry.gpr.pc.qword=owner.backoff_pc;
    const bool live_pc=state->gpr.pc.qword==owner.backoff_pc||state->gpr.pc.qword==owner.backoff_lr;
    if(!live_pc||state->gpr.x30.qword!=owner.backoff_lr||!BackoffSleepSite(entry)||(exact&&(owner.backoff_request!=sp||owner.backoff_remaining))){
      std::fprintf(stderr,"Game GC: backoff site PC=%#llx LR=%#llx SP=%#llx entry_X0=%#llx entry_X1=%#llx live_X0=%#llx\n",(unsigned long long)state->gpr.pc.qword,(unsigned long long)state->gpr.x30.qword,(unsigned long long)sp,(unsigned long long)owner.backoff_request,(unsigned long long)owner.backoff_remaining,(unsigned long long)state->gpr.x0.qword);
      Fail("original GC_lock backoff NanoSleep import/arguments differ");
    }
    Check(!(sp&15)&&sp>=info.stack_low&&sp<=info.stack_high&&info.stack_high-sp>=(exact?0x40:0x10)&&sp-info.stack_low>=9504&&state->sr.tpidr_el0.qword==info.tls_address,"backoff helper frame/stack/TLS or two-frame capacity differs");
    const auto stack=signals_.AlternateStack(info.token).configured;Check(stack.flags==2&&!stack.pointer&&!stack.size,"backoff alternate stack is not fresh disabled");
    uint64_t mask=0;Check(!signals_.Mask(info.token,0,nullptr,&mask),"backoff signal mask query failed");
    Check(selected?GameCollectorWaitMask::SelectedStop(info.token,mask,*selected):GameCollectorWaitMask::Ordinary(mask),selected?"backoff selected stop mask differs":"backoff blocks original stop/restart signals");
  }
  bool Admit(const recomp::PollAdmissionContext &context) noexcept {
    if(auto*owner=Current())if(const auto*socket=owner->socket_wait.load())return !OpaqueDepth()&&socket->Admit(context);
    auto *owner=Current();if(!owner||context.site.ordinal!=ProviderOrdinal(*owner))return false;
    const State *expected=owner->checkpoint?owner->checkpoint->active_state:owner->backoff.load()&&owner->backoff_boundary?owner->wait_state:nullptr;
    return expected&&owner->wait_live.load()&&!OpaqueDepth()&&context.frames.size()==1&&context.frames[0]==&context.state&&expected==&context.state&&context.site.trace==context.state.gpr.pc.qword&&context.program_counter==context.state.gpr.pc.qword;
  }
  void Pump(Owner &owner,State &state){
    if(auto*socket=owner.socket_wait.load()){
      Check(Current()==&owner&&owner.context->CurrentPhase()==Lifecycle::Phase::entered&&owner.context->NativeThreadId()==GetCurrentThreadId()&&owner.wait_state==&state&&!OpaqueDepth()&&owner.socket_boundary,"socket phase has no actual entered bounded owner");
      socket->Validate(threads_,signals_);++owner.pump_calls;if(!Pending(owner))return;
      const auto epoch=Read<uint64_t>(kEpoch);
      if(owner.certified_epoch!=epoch||!owner.certified_new_epoch||owner.begun_epoch==epoch||Read<uint64_t>(kStopped)!=1||Read<uint8_t>(0x879a62a8)!=1)throw ProviderDeferred{};
      const auto record=Record(owner.context->PreparedWorker().token);Check(record&&record==owner.record,"socket certified record changed");socket->ValidateRecord(record);Dispositions();
      owner.socket_selected=signals_.DispatchOneAdmittedSubset(socket->Token(),SignalHost::Bit(30)|SignalHost::Bit(24),SignalHost::Bit(30),*owner.socket_boundary);Check(!owner.socket_selected.error,"socket eligible signal outside controlled subset");return;
    }
    // This handler is reached only through the separately qualified controlled
    // provider site. It is not a compiler-inserted trace/backedge site.
    const bool backoff=!owner.checkpoint&&owner.backoff&&owner.backoff_boundary;
    if(backoff){Check(owner.wait_live,"backoff poll has no live certificate");ValidateBackoff(owner);}
    else{Check(owner.checkpoint&&owner.wait_live,"provider poll has no live certificate");ValidateWait(owner,*owner.checkpoint);}
    ++owner.pump_calls;
    if(!Pending(owner))return; // Only a transport hint, never a collector ack.
    Check(SignalHost::NeedsPoll(owner.context->SignalThread()),"masked pending signal outside an owned stop frame");
    const auto epoch=Read<uint64_t>(kEpoch);
    if(owner.certified_epoch!=epoch||!owner.certified_new_epoch||owner.begun_epoch==epoch||Read<uint64_t>(kStopped)!=1||Read<uint8_t>(0x879a62a8)!=1)throw ProviderDeferred{};
    const auto record=Record(owner.context->PreparedWorker().token);Check(record&&record==owner.record&&Read<addr_t>(record+0x28)==owner.context->PreparedWorker().stack_high,"pending request has no matching original record");Dispositions();
    // Selection and eligibility intersection are atomic inside SignalHost.
    // A late24 is not cleared by this outer30-only provider boundary.
    if(backoff){
      owner.backoff_selected=owner.role==Role::managed_worker?
        signals_.DispatchOneAdmittedSubset(owner.context->PreparedWorker().token,SignalHost::Bit(30)|SignalHost::Bit(24),SignalHost::Bit(30),*owner.backoff_boundary):
        signals_.DispatchOneAllowed(owner.context->PreparedWorker().token,SignalHost::Bit(30),*owner.backoff_boundary);
      Check(!owner.backoff_selected.error,"backoff original signal dispatch failed");return;
    }
    auto result=owner.role==Role::managed_worker?
      signals_.DispatchOneAdmittedSubset(owner.context->PreparedWorker().token,SignalHost::Bit(30)|SignalHost::Bit(24),SignalHost::Bit(30),[&](const auto&delivery){return Deliver(delivery);}):
      signals_.DispatchOneAllowed(owner.context->PreparedWorker().token,SignalHost::Bit(30),[&](const auto&delivery){return Deliver(delivery);});
    Check(!result.error,"admitted original signal dispatch failed");(void)state;
  }
  void Retire(const Pthreads::WorkerInfo &info,const Pthreads::WorkerLease &lease,Pthreads::WorkerRetirement reason,const Pthreads::WorkerLifecycleHooks &hooks){
    if(extension_.retire)extension_.retire(info,reason);
    auto owner=Lookup(info.token);
    if(owner&&owner->role==Role::managed_worker){std::lock_guard claim(owner->socket_sender_mutex);owner->retiring=true;Check(!Pending(*owner),"managed owner retires with queued collector delivery");}
    if(owner){Check(!owner->depth&&!owner->wait_live&&!owner->stop_live,"worker retires through active provider/signal frame");if(owner->binding||owner->delivery){Check(owner->context->NativeThreadId()==GetCurrentThreadId()&&(!owner->binding||Current()==owner.get()),"poll binding retirement on wrong worker");Check(!Pending(*owner),"worker retires with an original pending signal");owner->binding.reset();owner->delivery.reset();Current()=nullptr;}}
    hooks.retire(info,lease,reason);
    {std::lock_guard lock(mutex_);owners_.erase(info.token);}
    GameWorkerObservation::Retire(info,reason);
  }
  void NestedSuspend(Owner &owner,State &state){
    const auto frames=recomp::CurrentExecutionFrames(state);const auto record=owner.record.load();
    Check(Current()==&owner&&owner.depth==1&&owner.stop_live&&owner.posts==1&&frames.size()==2&&frames[0]==&state&&frames[1]==owner.wait_state&&state.gpr.x30.qword==0x825b2c24&&state.gpr.x0.qword==0x879a6b18&&state.gpr.sp.qword==owner.frame_base-0x60&&state.gpr.x29.qword==owner.frame_base-0x30,"sigsuspend is outside the exact original stop frame");
    Check(record&&Read<addr_t>(record+0x18)==state.gpr.x29.qword&&Read<uint64_t>(record+0x10)==owner.stop_epoch&&owner.stop_epoch==Read<uint64_t>(kEpoch),"original stop FP/epoch is not published before sigsuspend");
    const auto expected=~(SignalHost::Bit(2)|SignalHost::Bit(3)|SignalHost::Bit(6)|SignalHost::Bit(15)|SignalHost::Bit(24));
    Check(Read<uint64_t>(0x879a6b18)==expected,"original temporary mask can select an unqualified handler");Dispositions();
    // An early/old24 may run while worldStopped is still1. The original body
    // itself tests that flag after EINTR and waits again; no host early resume.
  }
 public:
  struct Opaque {bool active;explicit Opaque(bool enabled):active(enabled){if(active)++OpaqueDepth();}~Opaque(){if(active)--OpaqueDepth();}};
  static constexpr uint32_t kVirtualRealUid=10000; // Explicit sandbox, not Windows/device identity.
  GameCollectorDelivery(Pthreads &threads,SignalHost &signals,bunker::windows::Semaphores &sem,recomp::Runtime &runtime,WorkerExtension extension={}):threads_(threads),signals_(signals),semaphores_(sem),runtime_(runtime),lifecycle_(signals),extension_(std::move(extension)){
    const bool any_extension=extension_.prepare||extension_.enter||extension_.before_tls||extension_.retire||extension_.owns||extension_.checkpoint||extension_.select_mutex;
    Check(!any_extension||(extension_.prepare&&extension_.enter&&extension_.before_tls&&extension_.retire&&extension_.owns&&extension_.checkpoint),"worker extension hooks are incomplete");
    Check(Translated(kReturn)&&Executable(kReturn)&&Translated(0x825beb3c)&&Translated(0x825b2af4)&&Translated(0x825b2c70),"original normal-return/handler heads missing from final cohort");
    auto hooks=lifecycle_.TakeHooks();auto installed=hooks;
    installed.prepare=[this,hooks](const auto &info){auto lease=hooks.prepare(info);auto owner=std::make_shared<Owner>();owner->context=lifecycle_.Lookup(info.token);owner->role=Creating()?Creating()->role:Role::unknown;{std::lock_guard lock(mutex_);Check(!closed_&&owners_.emplace(info.token,owner).second,"duplicate/closed worker owner");}if(extension_.prepare)extension_.prepare(info,lifecycle_);GameWorkerObservation::Lifecycle("prepare",info);return lease;};
    installed.enter=[this,hooks](const auto &info,const auto &lease){hooks.enter(info,lease);auto owner=Lookup(info.token);Check(owner&&Current()==nullptr,"worker owner entry is not cold");owner->delivery=std::make_unique<SignalDelivery>(runtime_,Translated,kReturn,recomp::signal_frame::Bounds{info.stack_low,info.stack_high},signals_,info.token);owner->binding=std::make_unique<recomp::PollBinding>(owner->context->PollRequests(),[this,owner](State &s,recomp::PollSite,const auto&){try{Pump(*owner,s);}catch(const ProviderDeferred&){throw;}catch(const std::exception&e){Fail(e.what());}catch(...){Fail("unknown original delivery exception");}},[this](const auto &context){return Admit(context);});Current()=owner.get();if(extension_.enter)extension_.enter(info);GameWorkerObservation::Enter(info);};
    installed.before_tls=[this,hooks](const auto &info,const auto &lease){if(extension_.before_tls)extension_.before_tls(info);auto owner=Lookup(info.token);
      if(owner&&owner->role==Role::managed_worker){std::lock_guard claim(owner->socket_sender_mutex);owner->retiring=true;Check(!owner->depth&&!owner->wait_live&&!Pending(*owner),"managed worker reaches TLS with active/pending stop");hooks.before_tls(info,lease);GameWorkerObservation::Lifecycle("before_tls",info);return;}
      Check(owner&&!owner->depth&&!owner->wait_live&&!Pending(*owner),"worker reaches TLS with active/pending stop");hooks.before_tls(info,lease);GameWorkerObservation::Lifecycle("before_tls",info);};
    installed.retire=[this,hooks](const auto &i,const auto&l,auto r){Retire(i,l,r,hooks);};threads_.SetWorkerLifecycleHooks(std::move(installed));
    threads_.SetWaitHooks({[this](uint64_t token){
      if(extension_.select_mutex&&extension_.owns(token)){
        auto wake=signals_.Wake(token);
        Check(bool(wake),"worker wait has no live signal channel");
        return std::make_shared<const Pthreads::WaitWake>(Pthreads::WaitWake{
          [wake]{return wake->Generation();},[wake]{wake->Notify();},
          [wake](uint64_t generation,uint64_t relative){
            if(relative)wake->WaitRelative(generation,std::chrono::nanoseconds(relative));
            else wake->Wait(generation);
          }});
      }
      if(extension_.main_owns&&extension_.main_owns(token))return MainWaitWake();
      auto owner=Lookup(token);auto channel=owner?owner->channel:std::make_shared<Channel>();return std::make_shared<const Pthreads::WaitWake>(Pthreads::WaitWake{[channel]{return channel->Generation();},[channel]{channel->Notify();},[channel](auto generation,auto relative){channel->Wait(generation,relative);}});},[this](const auto &context){Checkpoint(context);},[this](const auto &context){Finish(context);},[this](const auto &context){return SelectMutex(context);}});
    std::fprintf(stderr,"Game GC: creator-certified finalizer untimed and managed-worker timed/untimed condition profiles configured; virtual Android real UID=%u; no Windows/device UID claim\n",kVirtualRealUid);
  }
  ~GameCollectorDelivery(){Check(closed_,"collector adapter destroyed before worker drain");}
  void CloseAfterShutdown(){std::lock_guard lock(mutex_);Check(owners_.empty(),"collector close has live workers");lifecycle_.CloseAfterShutdown();closed_=true;}
  void Notify(uint64_t token){
    // SignalHost normally calls its request_poll callback only while eligible
    // pending bits exist. A concurrent retirement/mask change may already have
    // cleared that condition; an obsolete wake is not another GC request.
    if(!SignalHost::NeedsPoll(signals_.Lookup(token)))return;
    if(auto owner=Lookup(token))if(owner->socket_wait.load()){owner->context->PollRequests().Request();return;}
    auto owner=Lookup(token);
    if(owner&&owner->role==Role::managed_worker){Check(owner->context->CurrentPhase()==Lifecycle::Phase::entered&&!owner->retiring&&!owner->queue_blocked,"queued managed target has no supported live owner");owner->context->PollRequests().Request();owner->channel->Notify();return;}
    Check(owner&&ConditionRole(*owner)&&(owner->wait_live.load()||owner->stop_live.load()),"queued target has left its qualified wait/stop lifetime");owner->context->PollRequests().Request();owner->channel->Notify();
  }
  void WithCreate(State &state,const std::function<void(State&)> &invoke){
    GameWorkerObservation::BeforeCreate(state);Witness witness;
    if(state.gpr.x30.qword==0x82556a64&&state.gpr.x2.qword==0x82556a94&&state.gpr.x3.qword){const auto record=state.gpr.x3.qword;Check(record<=UINT64_MAX-0x20,"original creator prefix overflow");if(Read<addr_t>(record+0x10)==0x82557c80){const auto payload=Read<addr_t>(record+0x18);Check(payload&&payload<=UINT64_MAX-24,"original creator payload overflow");const auto owner=Read<addr_t>(payload),function=Read<addr_t>(payload+8),argument=Read<addr_t>(payload+16);if(owner&&function==0x825c3ed0&&!argument)witness.role=Role::finalizer;else if(owner&&(function==0x8258aa94||function==0x82562248)&&argument)witness.role=Role::managed_worker;}}
    struct Scope {const Witness *previous;explicit Scope(const Witness &w):previous(Creating()){Creating()=&w;}~Scope(){Creating()=previous;}} scope(witness);invoke(state);
  }
  // Diagnostic: the blocking import each native thread is currently inside.
  // Every blocking import records itself, so this must not take a global lock:
  // each native thread owns one slot, written only by that thread and read
  // (best effort, diagnostics only) by DescribeActiveWait.
  struct ActiveWaitSlot {std::atomic<DWORD> tid{0};std::atomic<bool> active{false};char name[48]{};std::atomic<addr_t> lr{0},x0{0};};
  static constexpr size_t kActiveWaitSlots=4096;
  static ActiveWaitSlot *ActiveWaitSlots(){static ActiveWaitSlot slots[kActiveWaitSlots];return slots;}
  struct ActiveWaitSlotTag;
  static ActiveWaitSlot *CurrentActiveWaitSlot(){
    static constexpr recomp::ThreadLocalRef<ActiveWaitSlot*,ActiveWaitSlotTag> mine{};
    ActiveWaitSlot *slot=mine;
    if(!slot){static std::atomic<size_t> next{0};const auto index=next.fetch_add(1,std::memory_order_relaxed);
      if(index>=kActiveWaitSlots)return nullptr; // Diagnostics only: stop recording.
      slot=&ActiveWaitSlots()[index];slot->tid.store(GetCurrentThreadId(),std::memory_order_release);mine=slot;}
    return slot;
  }
  // Returns the slot to clear, or null when not recorded (nested or no slot).
  static ActiveWaitSlot *EnterActiveWait(const std::string &name,const State &state){
    auto *slot=CurrentActiveWaitSlot();if(!slot||slot->active.load(std::memory_order_relaxed))return nullptr;
    const auto size=(std::min)(name.size(),sizeof(slot->name)-1);
    std::memcpy(slot->name,name.data(),size);
    slot->name[size]='\0';
    slot->lr.store(addr_t(state.gpr.x30.qword),std::memory_order_relaxed);slot->x0.store(addr_t(state.gpr.x0.qword),std::memory_order_relaxed);
    slot->active.store(true,std::memory_order_release);return slot;
  }
  struct ActiveWaitScope {ActiveWaitSlot *slot;~ActiveWaitScope(){if(slot)slot->active.store(false,std::memory_order_release);}};
  static std::string DescribeActiveWait(DWORD tid){
    auto *slots=ActiveWaitSlots();
    for(size_t i=0;i<kActiveWaitSlots;++i){auto &slot=slots[i];
      if(slot.tid.load(std::memory_order_acquire)!=tid||!slot.active.load(std::memory_order_acquire))continue;
      char name[sizeof slot.name];std::memcpy(name,slot.name,sizeof name);name[sizeof name-1]=0;
      char text[160];std::snprintf(text,sizeof text,"%s LR=%#llx X0=%#llx",name,(unsigned long long)slot.lr.load(),(unsigned long long)slot.x0.load());return text;}
    return "none (running guest code or an unhooked import)";
  }
  void WithWait(const std::string &name,State &state,const std::function<void()> &invoke){
    ActiveWaitScope scope{EnterActiveWait(name,state)};
    WithWaitInner(name,state,invoke);
  }
  void WithWaitInner(const std::string &name,State &state,const std::function<void()> &invoke){
    auto *owner=Current();
    if(owner&&owner->socket_wait.load()&&name=="sigsuspend"){NestedSuspend(*owner,state);invoke();return;}
    if(owner&&owner->role==Role::managed_worker&&!owner->socket_wait.load()&&!owner->depth){
      const bool condition=(name=="pthread_cond_wait"||name=="pthread_cond_timedwait")&&ManagedSite(state)!=ManagedWait::unknown;
      const bool mutex=(name=="pthread_mutex_lock"&&ManagedMutexSite(state))||((name=="pthread_rwlock_rdlock"||name=="pthread_rwlock_wrlock")&&ManagedMutexSite(state)==5);
      if((condition||mutex)&&ManagedContext(state)){
        Check(!owner->wait_state,"nested managed provider invocation is unqualified");owner->wait_state=&state;
        struct Phase {Owner &owner;~Phase(){std::lock_guard claim(owner.socket_sender_mutex);owner.wait_live=false;owner.checkpoint=nullptr;owner.wait_state=nullptr;}}phase{*owner};
        invoke();std::lock_guard claim(owner->socket_sender_mutex);owner->wait_live=false;RetainManagedKnown(*owner);return;
      }
      // A live owner may retain a request between certified providers, but an
      // unsupported blocking/native continuation cannot promise progress.
      {
        static std::mutex seen_mutex;static std::map<std::pair<std::string,addr_t>,uint64_t> seen;uint64_t count;
        {std::lock_guard lock(seen_mutex);count=++seen[{name,addr_t(state.gpr.x30.qword)}];}
        if(count<=4||!(count&(count-1)))std::fprintf(stderr,"Game GC: managed unqualified wait name=%s token=%#llx native_tid=%lu LR=%#llx SP=%#llx X0=%#llx X1=%#llx count=%llu; queue blocked for its duration\n",name.c_str(),(unsigned long long)owner->context->PreparedWorker().token,GetCurrentThreadId(),(unsigned long long)state.gpr.x30.qword,(unsigned long long)state.gpr.sp.qword,(unsigned long long)state.gpr.x0.qword,(unsigned long long)state.gpr.x1.qword,(unsigned long long)count);
      }
      {std::lock_guard claim(owner->socket_sender_mutex);owner->wait_live=false;
        if(Pending(*owner)){uint64_t blocked=0;signals_.Pending(owner->context->PreparedWorker().token,&blocked);
          std::fprintf(stderr,"Game GC: pending delivery at unqualified managed wait name=%s token=%#llx native_tid=%lu LR=%#llx X0=%#llx blocked=%#llx needs_poll=%d epoch=%llu stopped=%llu\n",name.c_str(),(unsigned long long)owner->context->PreparedWorker().token,GetCurrentThreadId(),(unsigned long long)state.gpr.x30.qword,(unsigned long long)state.gpr.x0.qword,(unsigned long long)blocked,int(SignalHost::NeedsPoll(owner->context->SignalThread())),(unsigned long long)Read<uint64_t>(kEpoch),(unsigned long long)Read<uint64_t>(kStopped));
          Fail("unqualified managed wait has pending collector delivery");}
        owner->queue_blocked=true;}
      struct Blocked {Owner &owner;~Blocked(){std::lock_guard claim(owner.socket_sender_mutex);owner.queue_blocked=false;}}blocked{*owner};
      invoke();Check(!Pending(*owner),"pending collector delivery raced an unqualified managed wait");return;
    }
    if(name=="sigsuspend"){Check(owner&&ConditionRole(*owner),"sigsuspend has no owned original stop");NestedSuspend(*owner,state);invoke();return;}
    if(owner&&ConditionRole(*owner)&&(name=="sem_wait"||name=="sem_timedwait"))Fail("unqualified semaphore wait cannot consume a collector signal");
    if(owner&&ConditionRole(*owner)&&(name=="pthread_cond_wait"||name=="pthread_cond_timedwait")){
      Check(!owner->wait_state&&!owner->depth,"nested condition provider is unqualified");owner->wait_state=&state;invoke();owner->wait_live=false;Check(!Pending(*owner),"original pending signal raced provider exit");owner->wait_state=nullptr;
    }else invoke();
  }
  void Checkpoint(const Pthreads::WaitContext &context){
    GameWorkerObservation::NativeEnvironment native;
    if(extension_.owns&&extension_.owns(context.token)){extension_.checkpoint(context);return;}
    if(extension_.main_owns&&extension_.main_checkpoint&&extension_.main_owns(context.token)){extension_.main_checkpoint(context);return;}
    auto owner=Lookup(context.token);if(!owner||!ConditionRole(*owner)){if(owner)Check(!Pending(*owner),"unknown-role worker has pending delivery");return;}
    if(owner->role==Role::managed_worker&&(!context.active_state||(context.phase==Pthreads::WaitPhase::MutexLock?!ManagedMutexSite(*context.active_state):ManagedSite(*context.active_state)==ManagedWait::unknown)||!ManagedContext(*context.active_state))){
      owner->wait_live=false;
      Check(!Pending(*owner),"unqualified managed checkpoint has pending collector delivery");return;
    }
    // Basic wait publication does not walk the original registry. It can run
    // while unrelated original registrations/unlinks mutate their buckets.
    ValidateWait(*owner,context);Check(owner->wait_state==context.active_state,"checkpoint is outside actual observed import invocation");
    if(owner->role==Role::managed_worker)RetainManagedKnown(*owner);
    auto use=lifecycle_.AcquireUse(context.token);owner->wait_live=true;owner->checkpoint=&context;
    struct Scope {Owner &owner;~Scope(){owner.checkpoint=nullptr;}} scope{*owner};
    for(unsigned count=0;__bunker_guest_poll_requested();++count){
      Check(count<16,"provider transport hint drain budget exhausted");
      if(Pending(*owner)){
        const auto epoch=Read<uint64_t>(kEpoch);
        // A late standard30 may outlive resume. Keep both the model bit and
        // transport epoch pending until a new genuine stop send certifies a
        // stable registry cycle. Do not interpret an unrelated lockword1 as it.
        // Once this target has begun the epoch, the coordinator can complete
        // and release its lock. Do not walk buckets again for late retries.
        if(owner->certified_epoch!=epoch||!owner->certified_new_epoch||owner->begun_epoch==epoch||Read<uint64_t>(kStopped)!=1||Read<uint8_t>(0x879a62a8)!=1)return;
        const auto record=Record(context.token);Check(record&&record==owner->record,"certified target record changed");
        if((Read<uint64_t>(record+0x10)&~uint64_t(1))==epoch)return;
      }
      const auto previous=owner->pump_calls;
      try{__bunker_guest_poll_slow(context.active_state,context.active_state->gpr.pc.qword,ProviderOrdinal(*owner),context.active_state->gpr.pc.qword);}
      catch(const ProviderDeferred&){return;}
      if(owner->pump_calls==previous)return; // Admission deferred: keep epoch.
    }
  }
  void Finish(const Pthreads::WaitContext &context){
    GameWorkerObservation::NativeEnvironment native;
    auto owner=Lookup(context.token);
    if(!owner||!ConditionRole(*owner)||owner->socket_wait.load())return;
    const bool managed=owner->role==Role::managed_worker;
    // A finalizer send holds the claim from its wait_live check through queue
    // and notify. Withdraw a published wait under that same claim while the
    // condition provider still has a valid State and can run the stop handler.
    if(!managed&&(!owner->wait_live.load()||context.phase==Pthreads::WaitPhase::MutexLock))return;
    if(managed&&(!context.active_state||(context.phase==Pthreads::WaitPhase::MutexLock?!ManagedMutexSite(*context.active_state):ManagedSite(*context.active_state)==ManagedWait::unknown)||!ManagedContext(*context.active_state))){
      std::lock_guard claim(owner->socket_sender_mutex);owner->wait_live=false;
      Check(!Pending(*owner),"unqualified managed retirement has pending collector delivery");return;
    }
    ValidateWait(*owner,context);Check(owner->wait_state==context.active_state,"retirement is outside actual observed import invocation");
    auto use=lifecycle_.AcquireUse(context.token);
    for(unsigned count=0;;++count){
      Check(count<16,"wait retirement could not drain the qualified pending request");
      std::unique_lock claim(owner->socket_sender_mutex);
      if(!Pending(*owner)){owner->wait_live=false;return;}
      const auto epoch=Read<uint64_t>(kEpoch);
      if(owner->certified_epoch!=epoch||!owner->certified_new_epoch||owner->begun_epoch==epoch||Read<uint64_t>(kStopped)!=1||Read<uint8_t>(0x879a62a8)!=1){
        Check(managed,"finalizer wait exit has an uncertified pending collector delivery");
        RetainManagedKnown(*owner);owner->wait_live=false;return;
      }
      // This actual retirement/claim callback is itself a fresh live context,
      // even when an earlier finish callback already withdrew its certificate.
      owner->wait_live=true;
      // Send owns this same claim through BeforeKill -> real Queue -> Notify.
      // Never hold it while the original stop handler waits for restart24.
      claim.unlock();Checkpoint(context);
    }
  }
  bool SenderRootsCovered(State &state) const {
    const auto frames=recomp::CurrentExecutionFrames(state);
    if(frames.empty()||frames.front()!=&state)return false;
    return frames.size()==1 || (extension_.sender_parent_roots && extension_.sender_parent_roots(frames.data(),frames.size()));
  }
  void BeforeKill(const State &state){
    if(auto owner=Lookup(state.gpr.x0.qword))if(auto*socket=owner->socket_wait.load()){
      const auto signal=state.gpr.x1.dword;Check((signal==30||signal==24)&&state.gpr.x30.qword==(signal==30?0x825b2d1cULL:0x825b2f74ULL),"socket signal sender is outside original coordinator");
      Check(recomp::CurrentExecutionState()==&state&&!OpaqueDepth(),"socket sender is opaque or foreign State");Check(SenderRootsCovered(*recomp::CurrentExecutionState()),"socket sender has unqualified parent Runtime roots");
      const auto epoch=Read<uint64_t>(kEpoch);Check(epoch&&!(epoch&1)&&Read<uint8_t>(0x879a62a8)==1&&threads_.Self()!=socket->Token(),"socket original sender epoch/lock/self differs");
      Check(signal==30?Read<uint64_t>(kStopped)==1:owner->stop_live&&owner->stop_epoch==epoch&&!Read<uint64_t>(kStopped),"socket stop/restart phase differs");Dispositions();
      const auto record=Record(socket->Token());Check(record&&(signal==30?state.gpr.x26.qword:state.gpr.x27.qword)==record,"socket original sender traversal differs");socket->ValidateRecord(record);owner->record=record;
      if(signal==30){owner->certified_new_epoch=(Read<uint64_t>(record+0x10)&~uint64_t(1))!=epoch;owner->certified_epoch=epoch;}return;
    }
    const auto token=state.gpr.x0.qword;const auto signal=state.gpr.x1.dword;Check(signal==30||signal==24,"only original stop/restart nonzero sends are supported");auto owner=Lookup(token);Check(owner&&ConditionRole(*owner)&&owner->context->CurrentPhase()==Lifecycle::Phase::entered,"send target is not a captured live creator-certified condition owner");
    Check(state.gpr.x30.qword==(signal==30?0x825b2d1cULL:0x825b2f74ULL),"nonzero send is outside audited original caller");
    auto *active=recomp::CurrentExecutionState();Check(active==&state&&!OpaqueDepth(),"collector sender is inside an opaque native continuation");Check(SenderRootsCovered(*active),"collector sender has unqualified parent Runtime roots");
    const auto epoch=Read<uint64_t>(kEpoch);Check(epoch&&!(epoch&1)&&Read<uint8_t>(0x879a62a8)==1&&token!=threads_.Self(),"original epoch/registry-lock consistency differs");
    if(owner->role==Role::managed_worker&&owner->queue_blocked.load()&&!owner->retiring.load()){
      // Short non-checkpointed provider waits (rwlock/once/unlisted locks):
      // the sender waits a bounded time for the target to resume guest code,
      // where its ordinary poll checkpoint observes the stop.
      const auto begin=std::chrono::steady_clock::now();
      while(owner->queue_blocked.load()&&!owner->retiring.load()&&std::chrono::steady_clock::now()-begin<std::chrono::seconds(10))std::this_thread::sleep_for(std::chrono::microseconds(200));
      std::fprintf(stderr,"Game GC: managed stop target=%#llx waited %lld us for unqualified wait exit (still_blocked=%d)\n",(unsigned long long)token,(long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count(),int(owner->queue_blocked.load()));
    }
    if(owner->role==Role::managed_worker)Check(!owner->retiring&&!owner->queue_blocked,"managed stop target is retiring or inside an unsupported blocking phase");
    if(signal==30)Check(state.gpr.x30.qword==0x825b2d1c&&(owner->role==Role::managed_worker||owner->wait_live)&&Read<uint64_t>(kStopped)==1,"original stop caller/live wait cycle differs");
    else Check(state.gpr.x30.qword==0x825b2f74&&owner->stop_live&&owner->stop_epoch==epoch&&!Read<uint64_t>(kStopped),"original restart caller/owned stop differs");
    // Only the audited sender paths, paused under their original collector
    // registry lock, may authorize this host bucket walk.
    Dispositions();
    const auto record=Record(token);const auto actual=signal==30?state.gpr.x26.qword:state.gpr.x27.qword;
    Check(record&&actual==record&&Read<uint64_t>(record+8)==token&&Read<addr_t>(record+0x28)==owner->context->PreparedWorker().stack_high,"actual original traversal record/owned stack differs");
    if(signal==30){Check(state.gpr.x30.qword==0x825b2d1c&&(owner->role==Role::managed_worker||owner->wait_live)&&Read<uint64_t>(kStopped)==1&&!(Read<uint8_t>(record+0x20)&1)&&!Read<uint8_t>(record+0x21),"original stop sender/eligible wait differs");}
    else Check(state.gpr.x30.qword==0x825b2f74&&owner->stop_live&&owner->stop_epoch==epoch&&!Read<uint64_t>(kStopped),"original restart sender/owned stop differs");
    owner->record=record;
    if(signal==30){owner->certified_new_epoch=(Read<uint64_t>(record+0x10)&~uint64_t(1))!=epoch;owner->certified_epoch=epoch;}
    if(!GameQuietTrace())std::fprintf(stderr,"Game GC: original request signal=%u target=%#llx record=%#llx epoch=%llu LR=%#llx; source validated before actual pthread_kill\n",signal,(unsigned long long)token,(unsigned long long)record,(unsigned long long)epoch,(unsigned long long)state.gpr.x30.qword);
  }
  void Send(State &state,const std::function<void()> &invoke){
    auto owner=Lookup(state.gpr.x0.qword);Check(bool(owner),"nonzero send has no real worker owner");
    std::unique_lock socket_guard(owner->socket_sender_mutex);
    if(!owner->socket_wait.load()&&owner->role==Role::finalizer&&state.gpr.x1.dword==30){
      // Android interrupts the finalizer wherever it runs. Here it can only stop
      // in a certified wait, so the sender (holding the original allocation and
      // registry locks) waits until the target reaches one: its untimed event
      // wait, or the GC_lock backoff it must enter to take the lock we hold.
      // The claim then spans send/Queue/Notify; backoff exit takes it too.
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
      for(unsigned spins=0;!owner->wait_live.load()&&std::chrono::steady_clock::now()<deadline;++spins){
        socket_guard.unlock();if(spins<64)SwitchToThread();else Sleep(1);socket_guard.lock();
      }
    }else if(!owner->socket_wait.load()&&owner->role!=Role::managed_worker)socket_guard.unlock();
    // Shared Owner/Context plus this claim span real Queue/Notify. Managed
    // before_tls/retire take the same claim. No receiver State is read here.
    BeforeKill(state);invoke();
  }
  void BeforeNonlocal(){auto *owner=Current();if(owner&&owner->depth)Fail("nonlocal capture/transfer attempted inside an active collector frame");}
  std::optional<uint64_t> Deliver(const SignalHost::Delivery &delivery){
    if(auto*owner=Current())if(auto*socket=owner->socket_wait.load()){
      Check(delivery.token==socket->Token()&&owner->context->NativeThreadId()==GetCurrentThreadId(),"socket delivery has foreign owner");Dispositions();const auto token=socket->Token();auto use=lifecycle_.AcquireUse(token);const auto record=owner->record.load();Check(record!=0,"socket delivery lacks sender certificate");socket->ValidateRecord(record);
      if(!owner->depth){
        Check(delivery.signal==30&&owner->socket_boundary,"socket outer delivery is not controlled stop30");socket->Validate(threads_,signals_,&delivery);const auto epoch=Read<uint64_t>(kEpoch);const bool duplicate=(Read<uint64_t>(record+0x10)&~uint64_t(1))==epoch;Check(epoch&&!(epoch&1)&&(duplicate||Read<uint64_t>(kStopped)==1),"socket stopped epoch differs");
        owner->interrupted=*recomp::CurrentExecutionState();owner->frame_base=owner->interrupted.gpr.sp.qword-4704;owner->posts=0;owner->stop_epoch=epoch;owner->begun_epoch=epoch;owner->stop_live=!duplicate;owner->depth=1;
        auto result=owner->delivery->Deliver(delivery);Check(owner->posts==(duplicate?0u:2u),"socket original handler omitted genuine posts");if(!duplicate)Check(Read<uint64_t>(record+0x10)==(epoch|1),"socket original resume epoch store absent");owner->depth=0;owner->stop_live=false;
        if(!GameQuietTrace())std::fprintf(stderr,"Game GC socket wait: genuine stop returned token=%#llx epoch=%llu actual_posts=%u; no collection/frame claim\n",(unsigned long long)token,(unsigned long long)epoch,owner->posts);
        return result;
      }
      Check(delivery.signal==24,"socket nested delivery is not restart24");NestedSuspend(*owner,*recomp::CurrentExecutionState());owner->depth=2;auto result=owner->delivery->Deliver(delivery);owner->depth=1;return result;
    }
    auto *owner=Current();Check(owner&&ConditionRole(*owner)&&delivery.token==owner->context->PreparedWorker().token,"delivery has wrong original worker");Dispositions();
    const auto token=delivery.token;auto use=lifecycle_.AcquireUse(token);const auto record=owner->record.load();Check(record&&Read<uint64_t>(record+8)==token,"delivery record was not the validated original record");
    if(!owner->depth){
      if(!owner->checkpoint&&owner->backoff&&owner->backoff_boundary){Check(delivery.signal==30,"backoff outer delivery is not stop30");ValidateBackoff(*owner,&delivery);}
      else{Check(delivery.signal==30&&owner->checkpoint,"outer delivery is not qualified stop30");ValidateWait(*owner,*owner->checkpoint,&delivery);}
      const auto epoch=Read<uint64_t>(kEpoch);const bool duplicate=(Read<uint64_t>(record+0x10)&~uint64_t(1))==epoch;
      Check(epoch&&!(epoch&1)&&(duplicate||Read<uint64_t>(kStopped)==1),"new stop has no live original stopped epoch");
      owner->interrupted=*recomp::CurrentExecutionState();owner->frame_base=owner->interrupted.gpr.sp.qword-4704;owner->posts=0;owner->stop_epoch=epoch;owner->begun_epoch=epoch;owner->stop_live=!duplicate;owner->depth=1;
      auto result=owner->delivery->Deliver(delivery);
      Check(owner->posts==(duplicate?0u:2u),"original stop path returned without its own expected post calls");if(!duplicate)Check(Read<uint64_t>(record+0x10)==(epoch|1),"original resume epoch store was not observed");
      owner->depth=0;owner->stop_live=false;
      if(!GameQuietTrace())std::fprintf(stderr,"Game GC: original stop handler returned target=%#llx epoch=%llu duplicate=%u actual_posts=%u observed_record_epoch=%llu; no collection/init/frame claim\n",(unsigned long long)token,(unsigned long long)epoch,unsigned(duplicate),owner->posts,(unsigned long long)Read<uint64_t>(record+0x10));
      return result;
    }
    auto *state=recomp::CurrentExecutionState();Check(delivery.signal==24,"nested signal is not original restart24");NestedSuspend(*owner,*state);owner->depth=2;auto result=owner->delivery->Deliver(delivery);owner->depth=1;
    if(!GameQuietTrace())std::fprintf(stderr,"Game GC: original nested restart handler returned target=%#llx epoch=%llu\n",(unsigned long long)token,(unsigned long long)owner->stop_epoch.load());
    return result;
  }
  void BeforePost(const State &state){
    auto *owner=Current();if(!owner||!owner->depth)return;
    if(auto*socket=owner->socket_wait.load()){Check(owner->depth==1&&owner->stop_live&&state.gpr.x0.qword==kAck&&state.gpr.x30.qword==(owner->posts?0x825b2c50ULL:0x825b2c04ULL)&&owner->posts<2,"socket acknowledgment caller differs");socket->PhysicalRoots(owner->interrupted,owner->frame_base,owner->record.load());return;}
    Check(owner->depth==1&&owner->stop_live&&state.gpr.x0.qword==kAck&&state.gpr.x30.qword==(owner->posts?0x825b2c50ULL:0x825b2c04ULL)&&owner->posts<2,"unqualified acknowledgment path");
    const auto record=owner->record.load();Check(Read<addr_t>(record+0x18)==owner->frame_base-0x30,"original published FP differs from real handler frame");
    const addr_t saved=owner->frame_base+recomp::signal_frame::kMachineOffset+8;
    for(unsigned i=0;i<31;++i){uint64_t value;std::memcpy(&value,reinterpret_cast<const uint8_t*>(&owner->interrupted.gpr)+i*16+8,8);Check(Read<uint64_t>(saved+i*8)==value,"interrupted GPR is not physically in original scan range");}
    std::array<uint8_t,512> vectors{};recomp::ReadGuestMemory(vectors.data(),owner->frame_base+recomp::signal_frame::kFpsimdOffset+16,vectors.size());Check(!std::memcmp(vectors.data(),&owner->interrupted.simd,vectors.size()),"interrupted SIMD is not physically in original scan range");
    if(!GameQuietTrace())std::fprintf(stderr,"Game GC: physical interrupted roots verified target=%#llx frame=%#llx published_fp=%#llx original_sp=%#llx stack_high=%#llx GPR=31 Q=32; before original sem_post\n",(unsigned long long)owner->context->PreparedWorker().token,(unsigned long long)owner->frame_base,(unsigned long long)Read<addr_t>(record+0x18),(unsigned long long)owner->interrupted.gpr.sp.qword,(unsigned long long)owner->context->PreparedWorker().stack_high);
  }
  void AfterPost(const State &state){auto *owner=Current();if(!owner||!owner->depth)return;Check(!state.gpr.x0.dword,"original acknowledgment sem_post failed");++owner->posts;if(!GameQuietTrace())std::fprintf(stderr,"Game GC: original sem_post returned target=%#llx ordinal=%u LR=%#llx; epoch store follows in original code\n",(unsigned long long)owner->context->PreparedWorker().token,owner->posts,(unsigned long long)state.gpr.x30.qword);}
  bool Owns(uint64_t token){auto owner=Lookup(token);return owner&&owner->context->CurrentPhase()==Lifecycle::Phase::entered;}
  // The exact GC_lock backoff, plus any other direct nanosleep from original
  // libil2cpp text (Thread::Sleep and friends): the stop is delivered inside
  // the sleep as Android's interrupted nanosleep would, with the full
  // import-entry State as roots.
  static bool BackoffSleepSite(const State &state)noexcept{
    const auto lr=state.gpr.x30.qword;
    return state.gpr.pc.qword==kNanoSleep&&(lr==kBackoffReturn||(lr>=0x80000000&&lr<0x88000000&&!(lr&3)));
  }
  bool BackoffOwner(uint64_t token){auto *owner=Current();return owner&&ConditionRole(*owner)&&Owns(token)&&owner->context->PreparedWorker().token==token&&!owner->socket_wait.load();}
  uint64_t NanoSleepAdmission(uint64_t token)noexcept{
    auto *owner=Current();
    return owner&&owner->backoff.load()&&owner->wait_live.load()&&!owner->depth&&!OpaqueDepth()&&owner->context->PreparedWorker().token==token?SignalHost::Bit(30):0;
  }
  // One provider pass: the real nanosleep wait loop, or the syscall-exit drain.
  SignalHost::Dispatch DispatchBackoff(Owner &owner){
    const auto token=owner.context->PreparedWorker().token;const auto known=SignalHost::Bit(30)|SignalHost::Bit(24);
    owner.backoff_selected={};const SignalHost::Boundary boundary=[this](const auto&delivery){return Deliver(delivery);};owner.backoff_boundary=&boundary;
    struct Scope{Owner&owner;~Scope(){owner.backoff_boundary=nullptr;}}scope{owner};
    if(SignalHost::NeedsPoll(owner.context->SignalThread()))owner.context->PollRequests().Request();
    try{if(__bunker_guest_poll_requested())__bunker_guest_poll_slow(owner.wait_state,owner.wait_state->gpr.pc.qword,ProviderOrdinal(owner),owner.wait_state->gpr.pc.qword);}
    catch(const ProviderDeferred&){return signals_.DispatchOneAdmittedSubset(token,known,0,{});}
    return owner.backoff_selected;
  }
  // The remainder pointer is validated null, so the provider's remainder-copy
  // boundary has nothing to copy; the collector's own boundary is used.
  SignalHost::Dispatch DispatchNanoSleep(uint64_t token,uint64_t allowed,const SignalHost::Boundary&){
    auto *owner=Current();const auto known=SignalHost::Bit(30)|SignalHost::Bit(24);
    Check(owner&&owner->context->PreparedWorker().token==token&&(allowed==0||allowed==SignalHost::Bit(30)),"backoff dispatcher is outside its owner/admission profile");
    if(!allowed||!owner->backoff)return signals_.DispatchOneAdmittedSubset(token,known,0,{});
    return DispatchBackoff(*owner);
  }
  template<class F>void WithBackoffSleep(State &state,F&&invoke){
    auto owner=Lookup(threads_.Self());
    Check(owner&&Current()==owner.get()&&ConditionRole(*owner)&&owner->context->CurrentPhase()==Lifecycle::Phase::entered&&owner->context->NativeThreadId()==GetCurrentThreadId()&&!owner->wait_state&&!owner->checkpoint&&!owner->depth&&!owner->socket_wait.load()&&!owner->backoff,"backoff NanoSleep has no idle entered condition owner");
    auto use=lifecycle_.AcquireUse(owner->context->PreparedWorker().token);
    if(state.gpr.x1.qword&&state.gpr.x0.qword){uint64_t request[2];recomp::ReadGuestMemory(request,state.gpr.x0.qword,sizeof(request));recomp::WriteGuestMemory(state.gpr.x1.qword,request,sizeof(request));}
    {std::lock_guard claim(owner->socket_sender_mutex);owner->wait_state=&state;owner->backoff=true;owner->backoff_request=state.gpr.x0.qword;owner->backoff_remaining=state.gpr.x1.qword;owner->backoff_pc=state.gpr.pc.qword;owner->backoff_lr=state.gpr.x30.qword;}
    struct Scope{Owner&owner;~Scope(){std::lock_guard claim(owner.socket_sender_mutex);owner.wait_live=false;owner.backoff=false;owner.wait_state=nullptr;owner.backoff_boundary=nullptr;}}scope{*owner};
    ValidateBackoff(*owner);owner->wait_live=true;
    invoke();
    // A stop certified against this live phase after the provider's final
    // dispatch is delivered on return, as the kernel does on syscall exit.
    for(unsigned count=0;;++count){
      Check(count<16,"backoff exit could not drain its certified collector request");
      std::unique_lock claim(owner->socket_sender_mutex);
      if(!Pending(*owner)){owner->wait_live=false;return;}
      claim.unlock();
      const auto before=owner->begun_epoch.load();DispatchBackoff(*owner);
      if(owner->begun_epoch.load()==before&&Pending(*owner)){
        // Not a fresh certified stop (e.g. a late duplicate): managed workers
        // retain known requests between providers; a finalizer may not.
        std::lock_guard relock(owner->socket_sender_mutex);owner->wait_live=false;
        if(owner->role==Role::managed_worker){RetainManagedKnown(*owner);return;}
        Fail("finalizer backoff exit has an uncertified pending collector delivery");
      }
    }
  }
  template<class F>void WithSocketWait(const std::string&name,State&state,F&&invoke,const GameSocketWaitContext::SitePolicy&policy={}){
    auto owner=Lookup(threads_.Self());Check(owner&&Current()==owner.get()&&owner->context->CurrentPhase()==Lifecycle::Phase::entered&&owner->context->NativeThreadId()==GetCurrentThreadId()&&!owner->wait_state&&!owner->depth&&!owner->socket_wait.load(),"socket import has no idle actual entered worker");
    const auto&info=owner->context->PreparedWorker();auto use=lifecycle_.AcquireUse(info.token);GameSocketWaitContext socket(state,name,info.token,info.stack_low,info.stack_high,info.tls_address,false,policy);
    {std::lock_guard lock(owner->socket_sender_mutex);owner->wait_state=&state;owner->socket_wait=&socket;}
    struct Scope{Owner&owner;~Scope(){std::lock_guard lock(owner.socket_sender_mutex);owner.socket_wait=nullptr;owner.wait_state=nullptr;owner.socket_boundary=nullptr;}}scope{*owner};invoke();socket.Complete();
  }
  GameSocketWaitContext::Decision ObserveSocketWait(const GameSocketWaitContext::Observation&observation){
    auto*owner=Current();if(!owner||!owner->socket_wait.load())return{};auto*socket=owner->socket_wait.load();
    return socket->Observe(observation,[&]{
      Check(!OpaqueDepth(),"socket checkpoint is opaque");socket->Validate(threads_,signals_);const auto epoch=Read<uint64_t>(kEpoch);
      if(!SignalHost::NeedsPoll(owner->context->SignalThread())||owner->certified_epoch!=epoch||!owner->certified_new_epoch||owner->begun_epoch==epoch||Read<uint64_t>(kStopped)!=1||Read<uint8_t>(0x879a62a8)!=1)return GameSocketWaitContext::Decision{};
      const auto record=Record(socket->Token());Check(record&&record==owner->record,"socket sender-certified worker record changed");GameSocketWaitContext::PollCertificate certificate(*socket,record,epoch);
      owner->socket_selected={};const SignalHost::Boundary boundary=[this](const auto&delivery){return Deliver(delivery);};owner->socket_boundary=&boundary;
      struct Scope{Owner&owner;~Scope(){owner.socket_boundary=nullptr;}}scope{*owner};
      if(SignalHost::NeedsPoll(owner->context->SignalThread()))owner->context->PollRequests().Request();
      try{if(__bunker_guest_poll_requested())__bunker_guest_poll_slow(&socket->StateRef(),socket->StateRef().gpr.pc.qword,GameSocketWaitContext::kOrdinal,socket->StateRef().gpr.pc.qword);}
      catch(const ProviderDeferred&){return GameSocketWaitContext::Decision{};}
      return GameSocketWaitContext::Decision{owner->socket_selected.handled,owner->socket_selected.restart};
    });
  }
};
