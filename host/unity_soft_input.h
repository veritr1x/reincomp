#pragma once
#include "jni_host.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace bunker::windows {
// The audited UnityPlayer.hideSoftInput null-dialog path. UI posting is always
// deferred; its two f jobs use the original native-loaded/finishing gates and
// are drained at Java NEXT_FRAME's executeGLThreadJobs point. This supplies no
// general Handler, dialog, keyboard, or Android thread implementation.
class UnitySoftInputSession {
 public:
  using Runner=std::function<uint64_t(addr_t,const std::vector<addr_t>&)>;
  UnitySoftInputSession(jni::Host& host,addr_t player,Runner run)
      : state_(std::make_shared<Shared>(host,player,std::move(run))) {
    host.DefineField(kPlayer,"mSoftInputDialog",kDialog);
    // A newly supplied instance field has Java's null default. Never overwrite
    // an existing dialog: a second field/session definition is rejected.
    {Local dialog(host,host.GetObjectFieldValue(player,"mSoftInputDialog",kDialog));
      RequireNoException(host,"initial dialog field");
      if(dialog.value)throw std::runtime_error("soft input: initial dialog is not null");}
    host.BindVoidMethod(kPlayer,"hideSoftInput","()V",
      [s=state_](addr_t receiver,const jni::Host::MethodArguments&) {
        RequireNoException(s->host,"hideSoftInput entry");
        Local owner(s->host,s->host.LocalValue(s->player));
        State call{};call.gpr.x0.qword=s->host.EnvAddress();
        call.gpr.x1.qword=receiver;call.gpr.x2.qword=owner.value;
        s->host.InvokeEnv(24,call);
        RequireNoException(s->host,"receiver identity");
        if(!call.gpr.x0.qword)throw std::runtime_error("soft input: unowned UnityPlayer");
        // postOnUiThread always Handler.post, even for the current UI thread.
        std::lock_guard lock(s->mutex);s->ui_jobs.push_back(0);s->keyboard=false;
      });
    // Host text entry in place of the Android SoftInputDialog: the window's
    // keyboard edits the text; Enter/Escape finish it. Results reach Unity the
    // way the dialog's reportSoftInputStr does, as GL-thread jobs calling the
    // original nativeSetInputString/nativeSoftInputClosed/Canceled.
    host.BindVoidMethod(kPlayer,"setHideInputField","(Z)V",[](addr_t,const jni::Host::MethodArguments&){});
    host.BindVoidMethod(kPlayer,"showSoftInput","(Ljava/lang/String;IZZZZLjava/lang/String;IZ)V",
      [s=state_](addr_t,const jni::Host::MethodArguments&a){
        std::u16string initial;if(a[0])if(auto v=s->host.StringValue(a[0]))initial=*v;
        const int32_t limit=int32_t(a[7]);
        std::lock_guard lock(s->mutex);
        s->keyboard=true;s->text=initial;s->limit=limit>0?size_t(limit):0;
        s->gl_jobs.push_back({Job::Visible,{},true});
        std::fprintf(stderr,"Game soft input: keyboard opened (initial length=%zu limit=%d); type, then Enter to submit or Escape to cancel\n",initial.size(),limit);
        if(const char*preset=std::getenv("BUNKER_SOFT_INPUT_TEXT");preset&&*preset&&!s->preset_used){
          s->preset_used=true;
          s->text.clear();for(const char*p=preset;*p;++p)s->text.push_back(char16_t(static_cast<unsigned char>(*p)));
          QueueFinish(*s,false);
        }
      });
    host.BindVoidMethod(kPlayer,"setSelection","(II)V",[](addr_t,const jni::Host::MethodArguments&){});
    host.BindVoidMethod(kPlayer,"setCharacterLimit","(I)V",[s=state_](addr_t,const jni::Host::MethodArguments&a){
      std::lock_guard lock(s->mutex);s->limit=int32_t(a[0])>0?size_t(int32_t(a[0])):0;
    });
    host.BindVoidMethod(kPlayer,"setSoftInputStr","(Ljava/lang/String;)V",[s=state_](addr_t,const jni::Host::MethodArguments&a){
      std::u16string value;if(a[0])if(auto v=s->host.StringValue(a[0]))value=*v;
      std::lock_guard lock(s->mutex);s->text=value;
    });
  }
  bool KeyboardActive() const { std::lock_guard lock(state_->mutex);return state_->keyboard; }
  // One printable character, backspace (u'\b'), Enter (u'\r') or Escape (0x1b).
  void TypeCharacter(char16_t c) {
    std::lock_guard lock(state_->mutex);auto &s=*state_;
    if(!s.keyboard)return;
    if(c==u'\r'){QueueFinish(s,false);return;}
    if(c==0x1b){QueueFinish(s,true);return;}
    if(c==u'\b'){if(s.text.empty())return;s.text.pop_back();}
    else{if(c<0x20||(s.limit&&s.text.size()>=s.limit))return;s.text.push_back(c);}
    s.gl_jobs.push_back({Job::Text,s.text,false});
  }
  UnitySoftInputSession(const UnitySoftInputSession&)=delete;
  UnitySoftInputSession& operator=(const UnitySoftInputSession&)=delete;
  void SetNativeLoaded(bool value) { std::lock_guard lock(state_->mutex);state_->native_loaded=value; }
  // isFinishing returns true immediately once mQuitting has become true.
  void SetFinishing(bool value) { std::lock_guard lock(state_->mutex);state_->finishing=state_->finishing||value; }
  void SetUnityMainCurrent() {
    std::lock_guard lock(state_->mutex);
    const auto current=std::this_thread::get_id();
    if(state_->main!=std::thread::id{}&&state_->main!=current)
      throw std::runtime_error("soft input: UnityMain owner changed");
    state_->main=current;
  }
  void DrainUiJobs() {
    if(std::this_thread::get_id()!=state_->ui)throw std::runtime_error("soft input: foreign UI pump");
    RequireNoException(state_->host,"UI pump entry");
    for(;;) {
      {std::lock_guard lock(state_->mutex);if(state_->ui_jobs.empty())return;state_->ui_jobs.pop_front();}
      Report({Job::Area});Report({Job::Visible});
      Local player(state_->host,state_->host.LocalValue(state_->player));
      Local dialog(state_->host,state_->host.GetObjectFieldValue(player.value,"mSoftInputDialog",kDialog));
      RequireNoException(state_->host,"dialog field");
      if(dialog.value)throw std::runtime_error("soft input: original dialog dismissal/configuration is not implemented");
    }
  }
  void DrainGlThreadJobs() {
    {std::lock_guard lock(state_->mutex);if(state_->main!=std::this_thread::get_id())
      throw std::runtime_error("soft input: foreign GL job pump");}
    for(;;) {
      Job job;
      {std::lock_guard lock(state_->mutex);if(state_->gl_jobs.empty())return;job=state_->gl_jobs.front();state_->gl_jobs.pop_front();}
      Execute(job);
    }
  }
  // Audit only at a genuinely quiescent shutdown. Never flush on the wrong
  // thread or discard a queued original report to make teardown look idle.
  void CheckIdle() const {
    std::lock_guard lock(state_->mutex);
    if(!state_->ui_jobs.empty()||!state_->gl_jobs.empty())throw std::runtime_error("soft input: undrained jobs");
  }
 private:
  static constexpr const char* kPlayer="com/unity3d/player/UnityPlayer";
  static constexpr const char* kDialog="Lcom/unity3d/player/l;";
  struct Job {
    enum Kind { Area,Visible,Text,Closed,Canceled } kind;
    std::u16string text{};bool flag=false;
  };
  static void RequireNoException(jni::Host& host,const char* operation) {
    if(!host.PendingException().empty())throw std::runtime_error(std::string("soft input: pending Java exception at ")+operation);
  }
  struct Local {
    jni::Host& host;addr_t value;
    Local(jni::Host& h,addr_t v):host(h),value(v){}
    ~Local(){host.DeleteLocalReference(value);}
  };
  struct Shared {
    jni::Host& host;jni::Host::ObjectValue player;Runner run;
    const std::thread::id ui=std::this_thread::get_id();std::thread::id main;
    std::mutex mutex;bool native_loaded=false,finishing=false;
    std::deque<int> ui_jobs;std::deque<Job> gl_jobs;
    bool keyboard=false,preset_used=false;std::u16string text;size_t limit=0;
    Shared(jni::Host& h,addr_t p,Runner r):host(h),run(std::move(r)) {
      if(!run)throw std::invalid_argument("soft input: missing original native runner");
      RequireNoException(h,"session construction");
      if(!p)throw std::invalid_argument("soft input: null UnityPlayer");
      State call{};call.gpr.x0.qword=h.EnvAddress();call.gpr.x1.qword=p;
      h.InvokeEnv(31,call);
      Local owner(h,call.gpr.x0.qword);
      RequireNoException(h,"player class");
      const auto type=h.ClassNameValue(owner.value);
      RequireNoException(h,"player validation");
      if(!type||*type!=kPlayer)throw std::invalid_argument("soft input: expected live UnityPlayer instance");
      player=h.CaptureValue(p);
      RequireNoException(h,"player retention");
    }
  };
  // Caller holds the mutex. Mirrors reportSoftInputStr(text, 1, canceled)
  // followed by the dialog's dismissal visibility report.
  static void QueueFinish(Shared& s,bool canceled) {
    s.keyboard=false;
    if(canceled)s.gl_jobs.push_back({Job::Canceled});
    else{s.gl_jobs.push_back({Job::Text,s.text,false});s.gl_jobs.push_back({Job::Closed});}
    s.gl_jobs.push_back({Job::Visible,{},false});
    std::fprintf(stderr,"Game soft input: keyboard %s (length=%zu)\n",canceled?"canceled":"submitted",s.text.size());
  }
  void Report(Job job) {
    bool inline_main;
    {std::lock_guard lock(state_->mutex);
      if(state_->finishing||!state_->native_loaded)return;
      inline_main=state_->main==std::this_thread::get_id();
      if(!inline_main)state_->gl_jobs.push_back(job);
    }
    if(inline_main)Execute(job);
  }
  void Execute(Job job) {
    {std::lock_guard lock(state_->mutex);if(state_->finishing)return;}
    RequireNoException(state_->host,"native report entry");
    const char* name="";const char* sig="()V";
    switch(job.kind){
      case Job::Area:name="nativeSetInputArea";sig="(IIII)V";break;
      case Job::Visible:name="nativeSetKeyboardIsVisible";sig="(Z)V";break;
      case Job::Text:name="nativeSetInputString";sig="(Ljava/lang/String;)V";break;
      case Job::Closed:name="nativeSoftInputClosed";break;
      case Job::Canceled:name="nativeSoftInputCanceled";break;
    }
    const auto entry=state_->host.NativeEntry(kPlayer,name,sig);
    if(!entry)throw std::runtime_error(std::string("soft input: original native not registered: ")+name);
    Local player(state_->host,state_->host.LocalValue(state_->player));
    RequireNoException(state_->host,"native report player");
    std::vector<addr_t> args={state_->host.EnvAddress(),player.value};
    if(job.kind==Job::Area)args.insert(args.end(),4,0);
    if(job.kind==Job::Visible)args.push_back(job.flag?1:0);
    addr_t text=0;
    if(job.kind==Job::Text){text=state_->host.NewString(job.text);args.push_back(text);}
    try{state_->run(entry,args);}catch(...){if(text)state_->host.DeleteLocalReference(text);throw;}
    if(text)state_->host.DeleteLocalReference(text);
    if(!state_->host.PendingException().empty())throw std::runtime_error("soft input: native report left Java exception");
  }
  std::shared_ptr<Shared> state_;
};
}
