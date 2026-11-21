// This opt-in startup diagnostic uses the managed metadata shipped with the game.
#include <fstream>
#include <set>
static std::string enumStorage(P type){
    if(!type || il2cpp_type_get_type(type)!=0x11 || il2cpp_type_is_byref(type))return "-";
    auto klass=il2cpp_class_from_type(type);
    if(!klass || !il2cpp_class_is_enum(klass))return "-";
    auto base=il2cpp_class_enum_basetype(klass);
    if(!base)return "-";
    switch(il2cpp_type_get_type(base)){
        case 8:return "int32_t";
        case 9:return "uint32_t";
        default:return "other-enum";
    }
}
static void inspectEnumLayouts(){
    std::ifstream input("enum-requests.txt");
    std::set<std::string> targets,classes;
    std::string key;
    while(std::getline(input,key)){
        auto sep=key.find("::");
        if(sep==key.npos)continue;
        targets.insert(key);
        classes.insert(key.substr(0,sep));
    }
    if(targets.empty())boundary("enum signature inventory missing requests");
    std::ofstream out("managed-enums.partial",std::ios::trunc);
    size_t assemblies=0,found=0;
    auto all=static_cast<P *>(il2cpp_domain_get_assemblies(il2cpp_domain_get(),&assemblies));
    for(size_t a=0;a<assemblies;++a){
        auto image=il2cpp_assembly_get_image(all[a]);
        if(std::string(il2cpp_image_get_name(image)).find("UnityEngine")!=0)continue;
        auto count=il2cpp_image_get_class_count(image);
        for(size_t i=0;i<count;++i){
            auto klass=il2cpp_image_get_class(image,i);
            if(!klass)continue;
            std::string cname=il2cpp_class_get_name(klass);
            auto parent=klass;
            while(auto declaring=il2cpp_class_get_declaring_type(parent)){
                cname=std::string(il2cpp_class_get_name(declaring))+"/"+cname;
                parent=declaring;
            }
            auto qualified=std::string(il2cpp_class_get_namespace(parent))+"."+cname;
            if(!classes.count(qualified))continue;
            P it=nullptr;
            while(auto method=il2cpp_class_get_methods(klass,&it)){
                auto name=qualified+"::"+il2cpp_method_get_name(method);
                if(!targets.count(name))continue;
                uint32_t iflags=0;
                il2cpp_method_get_flags(method,&iflags);
                if(!(iflags&0x1000))continue;
                out<<name<<'\t'<<enumStorage(il2cpp_method_get_return_type(method));
                if(il2cpp_method_is_instance(method))out<<"\t-";
                auto params=il2cpp_method_get_param_count(method);
                for(uint32_t p=0;p<params;++p)out<<'\t'<<enumStorage(il2cpp_method_get_param(method,p));
                out<<'\n';
                ++found;
            }
        }
    }
    out.close();
    if(!found || !out || !MoveFileExW(L"managed-enums.partial",L"managed-enums.tsv",MOVEFILE_REPLACE_EXISTING))
        boundary("enum signature inventory failed to publish");
    ExitProcess(0);
}
