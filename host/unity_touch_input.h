#pragma once
// Pointer and key input for the original Unity player: the window's left
// mouse button becomes a single-finger android.view.MotionEvent and keyboard
// keys become android.view.KeyEvents, each passed to the translated
// UnityPlayer.nativeInjectEvent as UnityPlayer.onTouchEvent/onKeyDown do.
// Header-only and platform-neutral: callers supply client coordinates from
// their own window system (Win32 here, X11/Wayland on Linux).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

#include "jni_host.h"

namespace bunker::windows::touch_input {

// Android KeyEvent.KEYCODE_* for a Win32 virtual-key code (the window
// backends' common key identity); 0 when the key has no Android equivalent.
// Escape maps to KEYCODE_BACK, which Unity reports as KeyCode.Escape.
inline int32_t AndroidKeyCode(uint32_t vk){
  if(vk>=0x41&&vk<=0x5A)return int32_t(29+(vk-0x41));   // A..Z
  if(vk>=0x30&&vk<=0x39)return int32_t(7+(vk-0x30));    // 0..9
  if(vk>=0x60&&vk<=0x69)return int32_t(144+(vk-0x60));  // numpad 0..9
  if(vk>=0x70&&vk<=0x7B)return int32_t(131+(vk-0x70));  // F1..F12
  switch(vk){
    case 0x1B:return 4;    // Escape -> BACK
    case 0x0D:return 66;   // Enter
    case 0x20:return 62;   // Space
    case 0x08:return 67;   // Backspace -> DEL
    case 0x09:return 61;   // Tab
    case 0x25:return 21;case 0x26:return 19;case 0x27:return 22;case 0x28:return 20;  // arrows L/U/R/D
    case 0x10:case 0xA0:return 59;case 0xA1:return 60;    // Shift
    case 0x11:case 0xA2:return 113;case 0xA3:return 114;  // Ctrl
    case 0x12:case 0xA4:return 57;case 0xA5:return 58;    // Alt
    case 0x21:return 92;case 0x22:return 93;case 0x23:return 123;case 0x24:return 122;  // PgUp/PgDn/End/Home
    case 0x2D:return 124;case 0x2E:return 112;            // Insert, Delete (FORWARD_DEL)
    case 0xBA:return 74;case 0xBB:return 70;case 0xBC:return 55;case 0xBD:return 69;case 0xBE:return 56;  // ; = , - .
    case 0xBF:return 76;case 0xC0:return 68;case 0xDB:return 71;case 0xDC:return 73;case 0xDD:return 72;case 0xDE:return 75;  // / ` [ \ ] '
    case 0x6A:return 155;case 0x6B:return 157;case 0x6D:return 156;case 0x6E:return 158;case 0x6F:return 154;  // numpad * + - . /
  }
  return 0;
}
// Android META_* bits for held modifiers.
inline int32_t AndroidMetaState(bool shift,bool ctrl,bool alt){
  return (shift?0x41:0)|(ctrl?0x3000:0)|(alt?0x12:0);
}
// Unicode character for a KEYCODE under the given shift state (US layout), or 0.
inline int32_t AndroidUnicode(int32_t code,bool shift){
  if(code>=29&&code<=54)return (shift?'A':'a')+(code-29);
  if(code>=7&&code<=16)return shift?int32_t(")!@#$%^&*("[code-7]):'0'+(code-7);
  if(code>=144&&code<=153)return '0'+(code-144);
  switch(code){
    case 62:return ' ';case 66:return '\n';case 61:return '\t';
    case 74:return shift?':':';';case 70:return shift?'+':'=';case 55:return shift?'<':',';case 69:return shift?'_':'-';
    case 56:return shift?'>':'.';case 76:return shift?'?':'/';case 68:return shift?'~':'`';case 71:return shift?'{':'[';
    case 73:return shift?'|':'\\';case 72:return shift?'}':']';case 75:return shift?'"':'\'';
    case 155:return '*';case 157:return '+';case 156:return '-';case 158:return '.';case 154:return '/';
  }
  return 0;
}

class Injector {
 public:
  using Runner=std::function<addr_t(addr_t,const std::vector<addr_t>&)>;
  enum Action:int32_t {Down=0,Up=1,Move=2,Cancel=3};

  Injector(jni::Host &java,Runner run,addr_t player,bool trace=false):java_(java),run_(std::move(run)),trace_(trace){
    entry_=java.NativeEntry("com/unity3d/player/UnityPlayer","nativeInjectEvent","(Landroid/view/InputEvent;)Z");
    if(!entry_)throw std::runtime_error("UnityPlayer.nativeInjectEvent is not registered");
    player_=java.CaptureValue(player);
    Define(java);
  }

  // Client-area pixel coordinates, scaled by the caller to the surface size.
  void Inject(Action action,float x,float y){
    if(action==Down){if(down_)return;down_=true;down_ms_=NowMs();}
    else if(!down_)return;
    auto state=std::make_shared<Motion>(Motion{action,x,y,down_ms_,NowMs()});
    if(action==Up||action==Cancel)down_=false;
    auto event=java_.NewHostInstance("android/view/MotionEvent",state);
    auto player=java_.LocalValue(player_);
    addr_t handled=0;
    try{handled=run_(entry_,{java_.EnvAddress(),player,event});}
    catch(...){java_.DeleteLocalReference(event);java_.DeleteLocalReference(player);throw;}
    java_.DeleteLocalReference(event);java_.DeleteLocalReference(player);
    if(trace_||action!=Move)std::fprintf(stderr,"Touch input: action=%d x=%.1f y=%.1f handled=%d\n",int(action),double(x),double(y),int(handled&0xff));
  }

  // One android.view.KeyEvent (ACTION_DOWN=0 / ACTION_UP=1) from the keyboard.
  void InjectKey(bool down,int32_t key_code,int32_t repeat,int32_t meta,int32_t unicode,int32_t scan,int64_t down_ms){
    Motion m{down?0:1,0,0,down_ms,NowMs()};
    m.source=0x101;m.key_code=key_code;m.repeat=repeat;m.meta=meta;m.unicode=unicode;m.scan=scan;
    auto event=java_.NewHostInstance("android/view/KeyEvent",std::make_shared<Motion>(m));
    auto player=java_.LocalValue(player_);
    addr_t handled=0;
    try{handled=run_(entry_,{java_.EnvAddress(),player,event});}
    catch(...){java_.DeleteLocalReference(event);java_.DeleteLocalReference(player);throw;}
    java_.DeleteLocalReference(event);java_.DeleteLocalReference(player);
    if(trace_||!repeat)std::fprintf(stderr,"Key input: action=%d keycode=%d repeat=%d meta=%#x unicode=%d handled=%d\n",down?0:1,key_code,repeat,unsigned(meta),unicode,int(handled&0xff));
  }
  static int64_t Now(){return NowMs();}

 private:
  // Shared host state of MotionEvent and KeyEvent (one InputEvent state type).
  struct Motion {int32_t action;float x,y;int64_t down_ms,event_ms;
    int32_t source=0x1002,key_code=0,repeat=0,meta=0,unicode=0,scan=0;};
  static int64_t NowMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
  static uint32_t Bits(float value){uint32_t bits;std::memcpy(&bits,&value,4);return bits;}

  static void Define(jni::Host &java){
    using Args=jni::Host::MethodArguments;
    java.DefineClass("android/view/InputEvent",{},"java/lang/Object",true);
    java.DefineClass("android/view/MotionEvent",{},"android/view/InputEvent",true);
    java.DefineClass("android/view/KeyEvent",{},"android/view/InputEvent",true);
    java.MarkClassInitialized("android/view/KeyEvent");
    java.MarkClassInitialized("android/view/InputEvent");
    java.MarkClassInitialized("android/view/MotionEvent");
    auto read=[&java](addr_t object){auto m=java.ReadHostState<Motion>(object);if(!m)throw std::runtime_error("InputEvent host state missing");return *m;};
    auto pointer=[&java](const Args &a){if(int32_t(a[0])!=0){java.ThrowException("java/lang/IllegalArgumentException",u"pointerIndex out of range");return false;}return true;};
    // InputEvent.
    java.DefineIntMethod("android/view/InputEvent","getSource","()I",[read](addr_t o,const Args&){return read(o).source;});  // TOUCHSCREEN / KEYBOARD
    java.DefineIntMethod("android/view/InputEvent","getDeviceId","()I",[](addr_t,const Args&){return int32_t(1);});
    java.DefineLongMethod("android/view/InputEvent","getEventTime","()J",[read](addr_t o,const Args&){return read(o).event_ms;});
    java.DefineLongMethod("android/view/InputEvent","getEventTimeNano","()J",[read](addr_t o,const Args&){return read(o).event_ms*1000000;});
    // KeyEvent: one key from the keyboard device; characters via getUnicodeChar.
    const char *K="android/view/KeyEvent";
    java.DefineConstructor(K,"(Landroid/view/KeyEvent;)V",[&java](const Args &a)->addr_t{
      auto source=a[0]?java.ReadHostState<Motion>(a[0]):nullptr;
      if(!source){java.ThrowException("java/lang/NullPointerException",u"KeyEvent(null)");return 0;}
      return java.NewHostInstance("android/view/KeyEvent",std::make_shared<Motion>(*source));});
    java.DefineIntMethod(K,"getKeyCode","()I",[read](addr_t o,const Args&){return read(o).key_code;});
    java.DefineIntMethod(K,"getAction","()I",[read](addr_t o,const Args&){return read(o).action;});
    java.DefineIntMethod(K,"getRepeatCount","()I",[read](addr_t o,const Args&){return read(o).repeat;});
    java.DefineIntMethod(K,"getMetaState","()I",[read](addr_t o,const Args&){return read(o).meta;});
    java.DefineIntMethod(K,"getModifiers","()I",[read](addr_t o,const Args&){return read(o).meta&0x77ff7;});
    java.DefineIntMethod(K,"getScanCode","()I",[read](addr_t o,const Args&){return read(o).scan;});
    java.DefineIntMethod(K,"getFlags","()I",[](addr_t,const Args&){return int32_t(0x8);});  // FLAG_FROM_SYSTEM
    java.DefineLongMethod(K,"getDownTime","()J",[read](addr_t o,const Args&){return read(o).down_ms;});
    java.DefineIntMethod(K,"getUnicodeChar","()I",[read](addr_t o,const Args&){return read(o).unicode;});
    java.DefineIntMethod(K,"getUnicodeChar","(I)I",[read](addr_t o,const Args &a){return AndroidUnicode(read(o).key_code,(int32_t(a[0])&1)!=0);});
    java.DefineObjectMethod(K,"getCharacters","()Ljava/lang/String;",[](addr_t,const Args&)->addr_t{return 0;});
    java.DefineBooleanMethod(K,"isSystem","()Z",[read](addr_t o,const Args&){return read(o).key_code==4;});
    java.DefineBooleanMethod(K,"isCanceled","()Z",[](addr_t,const Args&){return false;});
    java.DefineBooleanMethod(K,"isLongPress","()Z",[](addr_t,const Args&){return false;});
    java.DefineBooleanMethod(K,"isShiftPressed","()Z",[read](addr_t o,const Args&){return (read(o).meta&0x1)!=0;});
    java.DefineBooleanMethod(K,"isCtrlPressed","()Z",[read](addr_t o,const Args&){return (read(o).meta&0x1000)!=0;});
    java.DefineBooleanMethod(K,"isAltPressed","()Z",[read](addr_t o,const Args&){return (read(o).meta&0x2)!=0;});
    // MotionEvent: one finger, pointer id 0, no history.
    const char *M="android/view/MotionEvent";
    // Unity copies the event (MotionEvent.obtain) to queue it for UnityMain.
    java.DefineStaticObjectMethod(M,"obtain","(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;",[&java](addr_t,const Args &a)->addr_t{
      auto source=a[0]?java.ReadHostState<Motion>(a[0]):nullptr;
      if(!source){java.ThrowException("java/lang/NullPointerException",u"MotionEvent.obtain(null)");return 0;}
      return java.NewHostInstance("android/view/MotionEvent",std::make_shared<Motion>(*source));});
    java.DefineVoidMethod(M,"recycle","()V",[](addr_t,const Args&){});
    java.DefineLongMethod(M,"getDownTime","()J",[read](addr_t o,const Args&){return read(o).down_ms;});
    for(const char *name:{"getAction","getActionMasked"})java.DefineIntMethod(M,name,"()I",[read](addr_t o,const Args&){return read(o).action;});
    java.DefineIntMethod(M,"getActionIndex","()I",[](addr_t,const Args&){return int32_t(0);});
    java.DefineIntMethod(M,"getPointerCount","()I",[](addr_t,const Args&){return int32_t(1);});
    java.DefineIntMethod(M,"getHistorySize","()I",[](addr_t,const Args&){return int32_t(0);});
    for(const char *name:{"getButtonState","getMetaState","getFlags","getEdgeFlags","getActionButton"})java.DefineIntMethod(M,name,"()I",[](addr_t,const Args&){return int32_t(0);});
    java.DefineIntMethod(M,"getPointerId","(I)I",[pointer](addr_t,const Args &a){return pointer(a)?int32_t(0):int32_t(-1);});
    java.DefineIntMethod(M,"findPointerIndex","(I)I",[](addr_t,const Args &a){return int32_t(a[0])==0?int32_t(0):int32_t(-1);});
    java.DefineIntMethod(M,"getToolType","(I)I",[pointer](addr_t,const Args &a){return pointer(a)?int32_t(1):int32_t(0);});  // TOOL_TYPE_FINGER
    for(const char *name:{"getX","getRawX"})java.DefineFloatBitsMethod(M,name,"()F",[read](addr_t o,const Args&){return Bits(read(o).x);});
    for(const char *name:{"getY","getRawY"})java.DefineFloatBitsMethod(M,name,"()F",[read](addr_t o,const Args&){return Bits(read(o).y);});
    java.DefineFloatBitsMethod(M,"getX","(I)F",[read,pointer](addr_t o,const Args &a){return pointer(a)?Bits(read(o).x):0u;});
    java.DefineFloatBitsMethod(M,"getY","(I)F",[read,pointer](addr_t o,const Args &a){return pointer(a)?Bits(read(o).y):0u;});
    for(const char *name:{"getPressure","getSize"}){
      java.DefineFloatBitsMethod(M,name,"()F",[](addr_t,const Args&){return Bits(1.0f);});
      java.DefineFloatBitsMethod(M,name,"(I)F",[pointer](addr_t,const Args &a){return pointer(a)?Bits(1.0f):0u;});
    }
    for(const char *name:{"getTouchMajor","getTouchMinor","getToolMajor","getToolMinor"}){
      java.DefineFloatBitsMethod(M,name,"()F",[](addr_t,const Args&){return Bits(1.0f);});
      java.DefineFloatBitsMethod(M,name,"(I)F",[pointer](addr_t,const Args &a){return pointer(a)?Bits(1.0f):0u;});
    }
    java.DefineFloatBitsMethod(M,"getOrientation","()F",[](addr_t,const Args&){return Bits(0.0f);});
    java.DefineFloatBitsMethod(M,"getOrientation","(I)F",[pointer](addr_t,const Args &a){return pointer(a)?Bits(0.0f):0u;});
    // AXIS_X=0, AXIS_Y=1, AXIS_PRESSURE=2, AXIS_SIZE=3; other axes report 0.
    auto axis=[read](addr_t o,int32_t which){auto m=read(o);return which==0?m.x:which==1?m.y:(which==2||which==3)?1.0f:0.0f;};
    java.DefineFloatBitsMethod(M,"getAxisValue","(I)F",[axis](addr_t o,const Args &a){return Bits(axis(o,int32_t(a[0])));});
    java.DefineFloatBitsMethod(M,"getAxisValue","(II)F",[axis,pointer](addr_t o,const Args &a){Args b{a[1]};return pointer(b)?Bits(axis(o,int32_t(a[0]))):0u;});
  }

  jni::Host &java_;Runner run_;addr_t entry_=0;jni::Host::ObjectValue player_;bool trace_=false;
  bool down_=false;int64_t down_ms_=0;
};

}  // namespace bunker::windows::touch_input
