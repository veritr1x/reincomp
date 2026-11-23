#include "fsr31-pass.h"
#include "fsr31-interop.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace fsr31 {
namespace {
template<class F> F entry(const char *name){
    auto fn=reinterpret_cast<uintptr_t>(wglGetProcAddress(name));
    if(fn<=3 || fn==UINTPTR_MAX)throw std::runtime_error(std::string("missing ")+name);
    return reinterpret_cast<F>(fn);
}
struct Commands {
#define FSR_GL(type,member,symbol) type member=entry<type>(#symbol)
    FSR_GL(PFNGLCREATEFRAMEBUFFERSPROC,framebuffers,glCreateFramebuffers);
    FSR_GL(PFNGLDELETEFRAMEBUFFERSPROC,deleteFramebuffers,glDeleteFramebuffers);
    FSR_GL(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC,attach,glNamedFramebufferTexture);
    FSR_GL(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC,drawBuffers,glNamedFramebufferDrawBuffers);
    FSR_GL(PFNGLCOPYIMAGESUBDATAPROC,copy,glCopyImageSubData);
    FSR_GL(PFNGLBLITNAMEDFRAMEBUFFERPROC,blit,glBlitNamedFramebuffer);
    FSR_GL(PFNGLGETTEXTURELEVELPARAMETERIVPROC,level,glGetTextureLevelParameteriv);
    FSR_GL(PFNGLGETTEXTURESUBIMAGEPROC,sample,glGetTextureSubImage);
    FSR_GL(PFNGLCREATESHADERPROC,createShader,glCreateShader);
    FSR_GL(PFNGLSHADERSOURCEPROC,source,glShaderSource);
    FSR_GL(PFNGLCOMPILESHADERPROC,compile,glCompileShader);
    FSR_GL(PFNGLGETSHADERIVPROC,shaderInfo,glGetShaderiv);
    FSR_GL(PFNGLGETSHADERINFOLOGPROC,shaderLog,glGetShaderInfoLog);
    FSR_GL(PFNGLDELETESHADERPROC,deleteShader,glDeleteShader);
    FSR_GL(PFNGLCREATEPROGRAMPROC,createProgram,glCreateProgram);
    FSR_GL(PFNGLATTACHSHADERPROC,attachShader,glAttachShader);
    FSR_GL(PFNGLLINKPROGRAMPROC,link,glLinkProgram);
    FSR_GL(PFNGLGETPROGRAMIVPROC,programInfo,glGetProgramiv);
    FSR_GL(PFNGLDELETEPROGRAMPROC,deleteProgram,glDeleteProgram);
    FSR_GL(PFNGLUSEPROGRAMPROC,use,glUseProgram);
    FSR_GL(PFNGLPROGRAMUNIFORMMATRIX4FVPROC,matrix,glProgramUniformMatrix4fv);
    FSR_GL(PFNGLPROGRAMUNIFORM2FPROC,offset,glProgramUniform2f);
    FSR_GL(PFNGLCREATEVERTEXARRAYSPROC,arrays,glCreateVertexArrays);
    FSR_GL(PFNGLDELETEVERTEXARRAYSPROC,deleteArrays,glDeleteVertexArrays);
    FSR_GL(PFNGLBINDVERTEXARRAYPROC,bindArray,glBindVertexArray);
    FSR_GL(PFNGLCREATESAMPLERSPROC,samplers,glCreateSamplers);
    FSR_GL(PFNGLDELETESAMPLERSPROC,deleteSamplers,glDeleteSamplers);
    FSR_GL(PFNGLSAMPLERPARAMETERIPROC,samplerParam,glSamplerParameteri);
    FSR_GL(PFNGLBINDSAMPLERPROC,bindSampler,glBindSampler);
    FSR_GL(PFNGLBINDTEXTUREUNITPROC,bindUnit,glBindTextureUnit);
    FSR_GL(PFNGLACTIVETEXTUREPROC,activeTexture,glActiveTexture);
    FSR_GL(PFNGLGETINTEGERI_VPROC,integer,glGetIntegeri_v);
    FSR_GL(PFNGLBINDFRAMEBUFFERPROC,bindFramebuffer,glBindFramebuffer);
#undef FSR_GL
};
Commands& gl(){static Commands commands;return commands;}
GLuint shader(GLenum type,const char *code){
    auto& g=gl();auto obj=g.createShader(type);g.source(obj,1,&code,nullptr);g.compile(obj);
    GLint ok=0;g.shaderInfo(obj,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[1024]{};g.shaderLog(obj,sizeof(log),nullptr,log);g.deleteShader(obj);throw std::runtime_error(log);}
    return obj;
}
struct Pass {
    Interop bridge;
    uint32_t width{},height{},outW{},outH{};
    GLuint fb{}, program{}, sampler{}, vao{};
    void init(const SceneFrame& frame,uint32_t w,uint32_t h){
        width=w;height=h;outW=frame.width;outH=frame.height;
        const auto lib=std::filesystem::path(_wgetenv(L"REINCOMP_INSTALL_ROOT"))/L"game/amd_fidelityfx_vk.dll";
        bridge.open(lib.c_str(),width,height,outW,outH,frame.reversed);
        auto& g=gl();g.framebuffers(1,&fb);
        g.attach(fb,GL_COLOR_ATTACHMENT0,bridge.texture(Interop::Depth),0);
        g.attach(fb,GL_COLOR_ATTACHMENT1,bridge.texture(Interop::Motion),0);
        g.attach(fb,GL_COLOR_ATTACHMENT2,bridge.texture(Interop::Color),0);
        const GLenum buffers[]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1,GL_COLOR_ATTACHMENT2};
        g.drawBuffers(fb,3,buffers);
        const char *vertex=R"(#version 450
        out vec2 uv;
        void main(){uv=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(uv*2.-1.,0.,1.);})";
        const char *fragment=R"(#version 450
        layout(binding=14) uniform sampler2D depthMap;
        layout(binding=15) uniform sampler2D sceneColor;
        layout(location=0) uniform mat4 worldFromClip;
        layout(location=4) uniform mat4 previousClipFromWorld;
        layout(location=8) uniform vec2 jitterUV;
        in vec2 uv;
        layout(location=0) out float depth;
        layout(location=1) out vec2 motion;
        layout(location=2) out vec4 color;
        void main(){
            color=texelFetch(sceneColor,ivec2(gl_FragCoord.xy),0);
            depth=texture(depthMap,uv).r;
            vec2 sampleUV=uv+jitterUV;
            vec4 world=worldFromClip*vec4(sampleUV*2.-1.,depth*2.-1.,1.);
            vec4 prev=previousClipFromWorld*world;
            motion=prev.w>1e-6 ? clamp((prev.xy/prev.w*.5+.5)-sampleUV,vec2(-1.),vec2(1.)) : vec2(0.);
        })";
        auto vs=shader(GL_VERTEX_SHADER,vertex),fs=shader(GL_FRAGMENT_SHADER,fragment);
        program=g.createProgram();g.attachShader(program,vs);g.attachShader(program,fs);g.link(program);
        g.deleteShader(vs);g.deleteShader(fs);
        GLint ok=0;g.programInfo(program,GL_LINK_STATUS,&ok);if(!ok)throw std::runtime_error("could not link FSR depth and motion pass");
        g.arrays(1,&vao);g.samplers(1,&sampler);g.samplerParam(sampler,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        g.samplerParam(sampler,GL_TEXTURE_MAG_FILTER,GL_NEAREST);g.samplerParam(sampler,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        status("AMD FSR 3.1 ready: %ux%u -> %ux%u",width,height,outW,outH);
    }
    ~Pass(){
        if(fb)gl().deleteFramebuffers(1,&fb);
        if(program)gl().deleteProgram(program);
        if(vao)gl().deleteArrays(1,&vao);
        if(sampler)gl().deleteSamplers(1,&sampler);
    }
};
// A plugin event may follow any of the game's post-processing materials.
// Preserve its GL state so screen-space UI can render immediately afterward.
struct DrawState {
    GLint fbo{},program{},vao{},viewport[4]{},activeTexture{},textures[2]{},samplers[2]{};
    GLboolean mask[4]{},enabled[5]{};
    const GLenum caps[5]={GL_DEPTH_TEST,GL_BLEND,GL_SCISSOR_TEST,GL_CULL_FACE,GL_FRAMEBUFFER_SRGB};
    DrawState() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fbo);glGetIntegerv(GL_CURRENT_PROGRAM,&program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_VIEWPORT,viewport);
        glGetIntegerv(GL_ACTIVE_TEXTURE,&activeTexture);
        for(unsigned i=0;i<2;++i){
            gl().integer(GL_TEXTURE_BINDING_2D,14+i,&textures[i]);
            gl().integer(GL_SAMPLER_BINDING,14+i,&samplers[i]);
        }
        glGetBooleanv(GL_COLOR_WRITEMASK,mask);
        for(unsigned i=0;i<5;++i){enabled[i]=glIsEnabled(caps[i]);glDisable(caps[i]);}
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    }
    ~DrawState(){
        gl().bindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);gl().use(program);gl().bindArray(vao);
        for(unsigned i=0;i<2;++i){
            // Binding zero with glBindTextureUnit would clear every texture target on this unit.
            gl().activeTexture(GL_TEXTURE0+14+i);
            glBindTexture(GL_TEXTURE_2D,textures[i]);
            gl().bindSampler(14+i,samplers[i]);
        }
        gl().activeTexture(activeTexture);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        glColorMask(mask[0],mask[1],mask[2],mask[3]);
        for(unsigned i=0;i<5;++i)if(enabled[i])glEnable(caps[i]);
    }
};
// The render thread alone owns these resources. Unity may destroy its GL
// context before CRT global destructors run. Retire the old pass on resize.
Pass *active{};
// Publish the GL name with its target generation. Drivers can recycle names
// on resize, including when the window returns to an earlier size.
std::atomic<uint64_t> publishedOutput{};
SceneFrame previous{};
bool haveHistory=false, stopped=false;
auto lastFrame=std::chrono::steady_clock::time_point{};
void fallback(const SceneFrame& f, GLuint destination){
    // Frames already recorded by Unity must still have a valid final blit
    // when the device rejects external sharing or the AMD context.
    auto& g=gl();GLuint pair[2]{};g.framebuffers(2,pair);
    g.attach(pair[0],GL_COLOR_ATTACHMENT0,f.sceneColor,0);g.attach(pair[1],GL_COLOR_ATTACHMENT0,destination,0);
    const auto scissor=glIsEnabled(GL_SCISSOR_TEST);if(scissor)glDisable(GL_SCISSOR_TEST);
    g.blit(pair[0],pair[1],0,0,f.renderWidth,f.renderHeight,0,0,f.width,f.height,GL_COLOR_BUFFER_BIT,GL_LINEAR);
    if(scissor)glEnable(GL_SCISSOR_TEST);
    g.deleteFramebuffers(2,pair);
}
void fallback(const SceneFrame& f) {
    if(f.copyOutput)fallback(f,f.destination);
    if(f.directOutput)fallback(f,f.directOutput);
}
}
uint32_t sharedOutput(uint32_t generation) {
    const auto value=publishedOutput.load(std::memory_order_acquire);
    return uint32_t(value>>32)==generation ? uint32_t(value) : 0;
}
void __stdcall releasePass(int) {
    publishedOutput.store(0,std::memory_order_release);
    delete active;
    active=nullptr;
}
void __stdcall renderEvent(int,void *data){
    std::unique_ptr<SceneFrame> owner(static_cast<SceneFrame*>(data));
    auto& frame=*owner;
    if(stopped){fallback(frame);return;}
    try{
        // Unity and the plugin share a GL context. In particular, swapping
        // Unity's backbuffer can leave an error set before this event runs.
        // Check only errors from the FSR pass when deciding to disable it.
        for(unsigned i=0;i<16;++i){
            const auto prior=glGetError();
            if(prior==GL_NO_ERROR)break;
            static bool logged=false;
            if(!logged){status("OpenGL error %u preceding the FSR event",prior);logged=true;}
        }
        auto& g=gl();
        const auto w=frame.renderWidth,h=frame.renderHeight;
        if(!sharedOutput(frame.generation)){
            GLint textureWidth=0,textureHeight=0;
            g.level(frame.sceneColor,0,GL_TEXTURE_WIDTH,&textureWidth);g.level(frame.sceneColor,0,GL_TEXTURE_HEIGHT,&textureHeight);
            if(textureWidth!=int(w) || textureHeight!=int(h))throw std::runtime_error("Unity scene texture size mismatch");
            publishedOutput.store(0,std::memory_order_release);
            delete active;active=nullptr;haveHistory=false;
            auto next=std::make_unique<Pass>();next->init(frame,w,h);active=next.release();
            publishedOutput.store((uint64_t(frame.generation)<<32)|active->bridge.texture(Interop::Output),
                                  std::memory_order_release);
        }
        {
            DrawState state;
            g.bindFramebuffer(GL_DRAW_FRAMEBUFFER,active->fb);g.bindArray(active->vao);g.use(active->program);
            glViewport(0,0,w,h);g.bindUnit(14,frame.sceneDepth);g.bindSampler(14,active->sampler);
            g.bindUnit(15,frame.sceneColor);g.bindSampler(15,active->sampler);
            g.matrix(active->program,0,1,GL_FALSE,frame.worldFromClip.data());
            bool continuous=haveHistory && previous.scene==frame.scene && previous.camera==frame.camera && frame.index==previous.index+1;
            g.matrix(active->program,4,1,GL_FALSE,continuous?previous.clipFromWorld.data():frame.clipFromWorld.data());
            g.offset(active->program,8,frame.jitterX/w,frame.jitterY/h);
            glDrawArrays(GL_TRIANGLES,0,3);
        }
        if(auto error=glGetError())throw std::runtime_error("FSR input preparation GL error "+std::to_string(error));
        auto now=std::chrono::steady_clock::now();
        float ms=std::chrono::duration<float,std::milli>(now-lastFrame).count();
        float matrixChange=0;
        for(unsigned i=0;i<16;++i)matrixChange=std::max(matrixChange,std::abs(frame.clipFromWorld[i]-previous.clipFromWorld[i]));
        bool reset=!haveHistory || previous.scene!=frame.scene || previous.camera!=frame.camera || frame.index!=previous.index+1 || ms>200 || matrixChange>3;
        active->bridge.dispatch(frame.jitterX,frame.jitterY,reset?8.f:ms,frame.nearClip,frame.farClip,frame.verticalFov,frame.sharpness,reset);
        const auto output=active->bridge.texture(Interop::Output);
        if(frame.copyOutput)
            g.copy(output,GL_TEXTURE_2D,0,0,0,0,frame.destination,GL_TEXTURE_2D,0,0,0,0,frame.width,frame.height,1);
        static const bool trace=std::getenv("REINCOMP_TRACE_FSR")!=nullptr;
        static uint32_t sampled=0;
        if(trace && (sampled==0 || frame.index-sampled>380)){
            float color[4],out[4],depth;
            g.sample(frame.sceneColor,0,w/2,h/2,0,1,1,1,GL_RGBA,GL_FLOAT,sizeof(color),color);
            g.sample(frame.sceneDepth,0,w/2,h/2,0,1,1,1,GL_RED,GL_FLOAT,sizeof(depth),&depth);
            g.sample(frame.copyOutput?frame.destination:output,0,frame.width/2,frame.height/2,0,1,1,1,GL_RGBA,GL_FLOAT,sizeof(out),out);
            status("Frame %u camera=%p color %.4f %.4f %.4f depth %.5f reconstructed %.4f %.4f %.4f output=%s",
                frame.index,reinterpret_cast<void*>(frame.camera),color[0],color[1],color[2],depth,out[0],out[1],out[2],
                frame.copyOutput?"copied":"direct");
            sampled=frame.index;
        }
        if(auto err=glGetError())throw std::runtime_error("OpenGL scene pass error "+std::to_string(err));
        if(!haveHistory)status("First game frame reconstructed: %u",frame.index);
        previous=frame;lastFrame=now;haveHistory=true;
    }catch(const std::exception& error){
        stopped=true;disable(error.what());publishedOutput.store(0,std::memory_order_release);
        // Already submitted draws can still sample the external image.
        fallback(frame);
    }
}
}
