// The pass plugin is optional. Binding happens once while Unity registers its
// internal calls, so the normal renderer pays no per-frame integration cost.
namespace fsr_engine {
using BeforeCull = void(*)(P);
using SetTexture = P(*)(P,int,P);
using BlitSource = P(*)(P,P);
using Execute = void(*)(P);
using FinalViewport = void(*)(P);
using DrawProperties = P(*)(P,P);
static BeforeCull beforeCull;
static SetTexture setTexture;
static BlitSource blitSource;
static Execute execute;
static FinalViewport finalViewport;
static DrawProperties drawProperties;
static Call bind(const char *key) {
    static const bool ready = [] {
        if (!std::getenv("REINCOMP_FSR31")) return false;
        const auto root = _wgetenv(L"REINCOMP_INSTALL_ROOT");
        if (!root || !*root) return false;
        auto path=std::filesystem::path(root)/L"game"/L"Fsr31.dll";
        auto library=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return false;
        beforeCull=reinterpret_cast<BeforeCull>(GetProcAddress(library,"FsrBeforeCull"));
        setTexture=reinterpret_cast<SetTexture>(GetProcAddress(library,"FsrSetTexture"));
        blitSource=reinterpret_cast<BlitSource>(GetProcAddress(library,"FsrBlitSource"));
        execute=reinterpret_cast<Execute>(GetProcAddress(library,"FsrExecute"));
        finalViewport=reinterpret_cast<FinalViewport>(GetProcAddress(library,"FsrFinalViewport"));
        drawProperties=reinterpret_cast<DrawProperties>(GetProcAddress(library,"FsrDrawProperties"));
        return beforeCull && setTexture && blitSource && execute && finalViewport;
    }();
    if (!ready) return nullptr;
    if (!std::strcmp(key,"UnityEngine.Camera::GetCullingParameters_Internal")) return [](State& s, void *fn) {
        beforeCull(reinterpret_cast<P>(s.gpr.x0.qword));
        invoke<uint8_t,P,uint8_t,P,int32_t>(s,fn);
    };
    if (!std::strcmp(key,"UnityEngine.Rendering.CommandBuffer::SetGlobalTexture_Impl")) return [](State& s, void *fn) {
        auto original=s.gpr.x2.qword;
        s.gpr.x2.qword=reinterpret_cast<uintptr_t>(setTexture(reinterpret_cast<P>(s.gpr.x0.qword),s.gpr.x1.dword,reinterpret_cast<P>(original)));
        invoke<void,P,int32_t,P,int32_t>(s,fn);
        s.gpr.x2.qword=original;
    };
    if (!std::strcmp(key,"UnityEngine.Rendering.CommandBuffer::Blit_Identifier_Injected")) return [](State& s,void *fn){
        auto original=s.gpr.x1.qword;
        s.gpr.x1.qword=reinterpret_cast<uintptr_t>(blitSource(reinterpret_cast<P>(s.gpr.x0.qword),reinterpret_cast<P>(original)));
        invoke<void,P,P,P,P,int32_t,P,P,int32_t,int32_t>(s,fn);
        s.gpr.x1.qword=original;
    };
    if (!std::strcmp(key,"UnityEngine.Rendering.CommandBuffer::Internal_DrawMesh_Injected")) return [](State& s,void *fn){
        auto cmd=reinterpret_cast<P>(s.gpr.x0.qword);
        finalViewport(cmd);
        auto block=s.gpr.x6.qword;
        if(drawProperties)s.gpr.x6.qword=uintptr_t(drawProperties(cmd,reinterpret_cast<P>(block)));
        invoke<void,P,P,P,P,int32_t,int32_t,P>(s,fn);
        s.gpr.x6.qword=block;
    };
    if (!std::strcmp(key,"UnityEngine.Rendering.ScriptableRenderContext::ExecuteCommandBuffer_Internal_Injected")) return [](State& s, void *fn) {
        execute(reinterpret_cast<P>(s.gpr.x1.qword));
        invoke<void,P,P>(s,fn);
    };
    if (!std::strcmp(key,"UnityEngine.Rendering.ScriptableRenderContext::Submit_Internal_Injected")) return [](State& s, void *fn) {
        invoke<void,P>(s,fn);
        execute(nullptr);
    };
    return nullptr;
}
}
