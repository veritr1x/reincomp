#include <cstring>
static std::wstring playerString(P text){
    if(!text)return L"";
    int n=static_cast<int>(invoke("il2cpp_string_length",text));
    auto *chars=reinterpret_cast<const wchar_t *>(invoke("il2cpp_string_chars",text));
    return std::wstring(chars,n);
}
static void inspectPlayerScene(){
    static unsigned round=0;
    auto file=std::fopen("scene-objects.log",round?"a":"w");
    if(!file)return;
    std::fprintf(file,"Scene inspection %u\n",++round);
    auto find=reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Object::FindObjectsOfType"));
    auto getName=reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Object::GetName"));
    size_t assemblies=0;
    auto images=static_cast<P *>(il2cpp_domain_get_assemblies(il2cpp_domain_get(),&assemblies));
    struct Type {const char *ns,*name;};
    const Type wanted[]={{"Dark.View.Title","SplashScreen"},{"Dark.View.Title","TitleScreen"},{"Dark.View.Movie","PreTitleMovie"},
        {"Dark.View.UserInterface.Parts","DarkImage"},{"RenderHeads.Media.AVProVideo","MediaPlayer"},{"RenderHeads.Media.AVProVideo","DisplayUGUI"},{"UnityEngine","Camera"},
        {"UnityEngine","CanvasGroup"},{"UnityEngine.Video","VideoPlayer"},{"Dark.View.UserInterface.Parts","DarkButton"},{"UnityEngine.UI","Button"}};
    for(const auto &t:wanted){
        for(size_t i=0;i<assemblies;++i){
            auto image=il2cpp_assembly_get_image(images[i]);
            auto klass=il2cpp_class_from_name(image,t.ns,t.name);
            if(!klass)continue;
            auto items=find(il2cpp_type_get_object(il2cpp_class_get_type(klass)));
            auto n=items?invoke("il2cpp_array_length",items):0;
            std::fprintf(file,"%s.%s active objects=%llu\n",t.ns,t.name,(unsigned long long)n);
            auto header=il2cpp_array_object_header_size();
            for(size_t j=0;j<n && (j<48||!std::strcmp(t.name,"DarkImage"));++j){
                auto object=reinterpret_cast<P *>(static_cast<char *>(items)+header)[j];
                if(!object)continue;
                auto objName=playerString(getName(object));
                if(!std::strcmp(t.name,"DarkImage") && objName!=L"title_background" && objName!=L"dark_logo" && objName!=L"dark_logo_black" && objName!=L"title_white")continue;
                std::fprintf(file,"  %ls",objName.c_str());
                if(!std::strcmp(t.name,"DarkImage")){
                    for(auto property:{"get_sprite","get_mainTexture","get_materialForRendering"}){
                        auto m=il2cpp_class_get_method_from_name(klass,property,0);
                        P error=nullptr;
                        P value=m?reinterpret_cast<P>(invoke("il2cpp_runtime_invoke",m,object,nullptr,&error)):nullptr;
                        std::fprintf(file," %s=%ls",property,value&&!error?playerString(getName(value)).c_str():L"(null)");
                        if(value&&!error&&!std::strcmp(property,"get_materialForRendering")&&objName!=L"title_white"){
                            auto id=[&](const char *s){
                                return reinterpret_cast<int(*)(P)>(registry.at("UnityEngine.Shader::PropertyToID"))(reinterpret_cast<P>(invoke("il2cpp_string_new",s)));};
                            auto scalar=reinterpret_cast<float(*)(P,int)>(registry.at("UnityEngine.Material::GetFloatImpl"));
                            auto texture=reinterpret_cast<P(*)(P,int)>(registry.at("UnityEngine.Material::GetTextureImpl"));
                            for(auto k:{"_Height","_InverseMaskHeight"})std::fprintf(file," %s=%.3f",k,scalar(value,id(k)));
                            if(auto t=texture(value,id("_MaskTex")))std::fprintf(file," mask=%ls",playerString(getName(t)).c_str());
                        }
                    }
                    auto prop=[&](const char *s){P error=nullptr;auto m=il2cpp_class_get_method_from_name(klass,s,0);return m?reinterpret_cast<P>(invoke("il2cpp_runtime_invoke",m,object,nullptr,&error)):nullptr;};
                    if(auto boxed=prop("get_color")){
                        auto rgba=reinterpret_cast<float *>(invoke("il2cpp_object_unbox",boxed));
                        std::fprintf(file," tint=%.2f,%.2f,%.2f,%.2f",rgba[0],rgba[1],rgba[2],rgba[3]);
                    }
                    if(auto renderer=prop("get_canvasRenderer")){
                        auto alpha=reinterpret_cast<float(*)(P)>(registry.at("UnityEngine.CanvasRenderer::GetInheritedAlpha"));
                        auto cull=reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.CanvasRenderer::get_cull"));
                        std::fprintf(file," rendererAlpha=%.3f culled=%u",alpha(renderer),unsigned(cull(renderer)));
                    }
                }
                if(!std::strcmp(t.name,"CanvasGroup")){
                    auto alpha=reinterpret_cast<float(*)(P)>(registry.at("UnityEngine.CanvasGroup::get_alpha"));
                    std::fprintf(file," alpha=%.2f",alpha(object));
                }else if(!std::strcmp(t.name,"VideoPlayer")){
                    auto playing=reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.Video.VideoPlayer::get_isPlaying"));
                    auto prepared=reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.Video.VideoPlayer::get_isPrepared"));
                    std::fprintf(file," playing=%u prepared=%u",unsigned(playing(object)),unsigned(prepared(object)));
                }
                std::fputc('\n',file);
                if(!std::strcmp(t.name,"SplashScreen") || !std::strcmp(t.name,"TitleScreen")){
                    P it=nullptr;
                    while(auto f=reinterpret_cast<P>(invoke("il2cpp_class_get_fields",klass,&it))){
                        auto flags=invoke("il2cpp_field_get_flags",f);
                        if(flags&0x10)continue;
                        auto key=reinterpret_cast<const char *>(invoke("il2cpp_field_get_name",f));
                        auto ft=reinterpret_cast<P>(invoke("il2cpp_field_get_type",f));
                        auto kind=static_cast<int>(invoke("il2cpp_type_get_type",ft));
                        auto fc=reinterpret_cast<P>(invoke("il2cpp_class_from_type",ft));
                        auto offset=invoke("il2cpp_field_get_offset",f);
                        std::fprintf(file,"    field %s %s offset=%llu",il2cpp_class_get_name(fc),key,(unsigned long long)offset);
                        const char *value=static_cast<char *>(object)+offset;
                        if(kind==2)std::fprintf(file," value=%u",unsigned(*reinterpret_cast<const uint8_t *>(value)));
                        if(kind==8)std::fprintf(file," value=%d",*reinterpret_cast<const int32_t *>(value));
                        if(kind==12)std::fprintf(file," value=%.3f",*reinterpret_cast<const float *>(value));
                        if(kind==18||kind==28||kind==29)std::fprintf(file," assigned=%d",*reinterpret_cast<const P *>(value)!=nullptr);
                        std::fputc('\n',file);
                    }
                    P itMethod=nullptr;
                    while(auto m=il2cpp_class_get_methods(klass,&itMethod)){
                        auto name=il2cpp_method_get_name(m);
                        auto count=il2cpp_method_get_param_count(m);
                        std::fprintf(file,"    method %s params=%u pc=%llx\n",name,count,(unsigned long long)*static_cast<uintptr_t *>(m));
                        static bool movieTapped=false;
                        if(!movieTapped && round>=4 && !std::strcmp(t.name,"TitleScreen") && !std::strcmp(name,"OnTapMovieButton") && !count &&
                           GetEnvironmentVariableW(L"REINCOMP_TEST_TITLE_MOVIE",nullptr,0)){
                            movieTapped=true;
                            P error=nullptr;
                            invoke("il2cpp_runtime_invoke",m,object,nullptr,&error);
                            std::fprintf(file,"    Title movie replay requested, exception=%d\n",!!error);
                        }
                    }
                }
            }
            break;
        }
    }
    std::fclose(file);
}
