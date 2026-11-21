static void inspectMovieMetadata(){
    auto out=std::fopen("movie-metadata.tsv","w");
    if(!out)boundary("could not open movie metadata diagnostics");
    size_t count=0;
    auto all=static_cast<P *>(il2cpp_domain_get_assemblies(il2cpp_domain_get(),&count));
    for(size_t a=0;a<count;++a){
        auto image=il2cpp_assembly_get_image(all[a]);
        for(size_t i=0,n=il2cpp_image_get_class_count(image);i<n;++i){
            auto cls=il2cpp_image_get_class(image,i);
            if(!cls)continue;
            std::string name=il2cpp_class_get_name(cls);
            auto parent=cls;
            while(auto owner=il2cpp_class_get_declaring_type(parent)){
                name=std::string(il2cpp_class_get_name(owner))+"/"+name;
                parent=owner;
            }
            std::string ns=il2cpp_class_get_namespace(parent);
            if(ns.compare(0,4,"Dark") && ns.compare(0,10,"RenderHeads"))continue;
            if(name.find("Title")==name.npos && name.find("Movie")==name.npos &&
               name!="DarkImage" && name!="DarkStill" && name!="DisplayUGUI" && name!="AndroidMediaPlayer")continue;
            P it=nullptr;
            while(auto m=il2cpp_class_get_methods(cls,&it))
                std::fprintf(out,"%s.%s\t%s\t%llx\t%u\n",ns.c_str(),name.c_str(),il2cpp_method_get_name(m),
                    (unsigned long long)*static_cast<uintptr_t *>(m),il2cpp_method_get_param_count(m));
        }
    }
    std::fclose(out);ExitProcess(0);
}
