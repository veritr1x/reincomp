template<class T> static T bundleApi(const char *name){
    return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),name));
}
static std::filesystem::path storagePath(const std::wstring &name);
static constexpr char desktopBundleDirectory[] = "desktop-bundles-v2";
struct VerifiedBundle {
    std::filesystem::path destination;
    uint64_t fileId=0,size=0,lastWrite=0,header=0;
    std::wstring mask;
    DWORD volume=0;
};
struct OctoFile {
    std::filesystem::path path;
    std::wstring id;
    VerifiedBundle identity;
};
static bool readBundleIdentity(const std::filesystem::path &path,VerifiedBundle &entry){
    HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok=GetFileInformationByHandle(file,&info);
    CloseHandle(file);
    if(!ok)return false;
    auto join=[](DWORD hi,DWORD lo){return (uint64_t(hi)<<32)|lo;};
    entry.volume=info.dwVolumeSerialNumber;
    entry.fileId=join(info.nFileIndexHigh,info.nFileIndexLow);
    entry.size=join(info.nFileSizeHigh,info.nFileSizeLow);
    entry.lastWrite=join(info.ftLastWriteTime.dwHighDateTime,info.ftLastWriteTime.dwLowDateTime);
    return entry.fileId!=0;
}
static auto &bundleIndex(){
    static auto index=[]{
        std::unordered_map<std::wstring,VerifiedBundle> result;
        auto folder=privateClient()/desktopBundleDirectory;
        for(bool learned:{false,true}){
            std::ifstream input(folder/(learned?"verified-streams.tsv":"verified-files.tsv"));
            std::string line;
            while(std::getline(input,line)){
                std::istringstream row(line);
                std::string id,key,mask;
                VerifiedBundle entry;
                if(!(row>>id>>key>>entry.fileId>>entry.size>>entry.lastWrite>>entry.header) ||
                   (learned && !(row>>entry.volume)) || row.get()!='\t' || !std::getline(row,mask))continue;
                if(id.size()!=32 || key.size()!=73 || key.substr(64)!="-00000000" ||
                   key.substr(0,64).find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
                entry.destination=folder/(key+".winbundle");
                entry.mask=std::filesystem::u8path(mask).wstring();
                result[std::wstring(id.begin(),id.end())]=std::move(entry);
            }
        }
        return result;
    }();
    return index;
}
static std::mutex bundleIndexMutex;
static bool sameBundleFile(const VerifiedBundle &a,const VerifiedBundle &b){
    return (!a.volume || a.volume==b.volume) && a.fileId==b.fileId && a.size==b.size && a.lastWrite==b.lastWrite;
}
static void rememberBundle(const OctoFile &source,const std::filesystem::path &destination){
    if(source.id.empty() || destination.empty())return;
    auto entry=source.identity;
    if(!readBundleIdentity(source.path,entry) || !sameBundleFile(source.identity,entry))return;
    entry.destination=destination;
    const auto mask=std::filesystem::path(entry.mask).u8string();
    if(mask.find_first_of("\t\r\n")!=std::string::npos)return;
    std::lock_guard lock(bundleIndexMutex);
    auto &map=bundleIndex();
    const auto it=map.find(source.id);
    if(it!=map.end() && sameBundleFile(it->second,entry) && it->second.header==entry.header &&
       it->second.mask==entry.mask && it->second.destination==destination)return;
    map[source.id]=entry;
    std::ofstream log(destination.parent_path()/"verified-streams.tsv",std::ios::app);
    log<<std::string(source.id.begin(),source.id.end())<<'\t'<<destination.stem().string()<<'\t'
       <<entry.fileId<<'\t'<<entry.size<<'\t'<<entry.lastWrite<<'\t'<<entry.header<<'\t'<<entry.volume<<'\t'<<mask<<'\n';
}
static std::filesystem::path verifiedBundle(P stream,P klass,int64_t length,OctoFile &candidate){
    static const auto getClass=bundleApi<P(*)(P)>("il2cpp_object_get_class");
    static const auto className=bundleApi<const char *(*)(P)>("il2cpp_class_get_name");
    static const auto nameSpace=bundleApi<const char *(*)(P)>("il2cpp_class_get_namespace");
    static const auto field=bundleApi<P(*)(P,const char *)>("il2cpp_class_get_field_from_name");
    static const auto offset=bundleApi<size_t(*)(P)>("il2cpp_field_get_offset");
    if(std::strcmp(className(klass),"MaskedHeaderStream") || std::strcmp(nameSpace(klass),"Dark.Asset.Cryptor"))return {};
    auto fieldOffset=[&](P type,const char *name){auto item=field(type,name);return item?offset(item):size_t(0);};
    static const auto baseSlot=fieldOffset(klass,"_baseStream"),maskSlot=fieldOffset(klass,"_maskString"),headerSlot=fieldOffset(klass,"_headerLength");
    if(!baseSlot || !maskSlot || !headerSlot)return {};
    auto reference=[](P value,size_t slot){P result;std::memcpy(&result,static_cast<const char *>(value)+slot,sizeof(result));return result;};
    auto source=reference(stream,baseSlot);
    if(!source)return {};
    auto type=getClass(source);
    if(std::strcmp(className(type),"FileStream") || std::strcmp(nameSpace(type),"System.IO"))return {};
    static const auto nameSlot=fieldOffset(type,"name");
    if(!nameSlot)return {};
    auto name=pathText(reference(source,nameSlot));
    if(name.find(L"/data/user/0/bunker/files/octo/")!=0)return {};
    const auto digest=std::filesystem::path(name).filename().wstring();
    if(digest.size()!=32 || digest.find_first_not_of(L"0123456789abcdef")!=std::wstring::npos)return {};
    int64_t header;std::memcpy(&header,static_cast<const char *>(stream)+headerSlot,sizeof(header));
    candidate.path=storagePath(name);
    auto &current=candidate.identity;
    if(!readBundleIdentity(candidate.path,current) || current.size!=uint64_t(length))return {};
    current.header=uint64_t((std::min)(length,header));
    current.mask=pathText(reference(stream,maskSlot));
    candidate.id=digest;
    std::lock_guard lock(bundleIndexMutex);
    auto &index=bundleIndex();
    const auto found=index.find(digest);
    if(found==index.end())return {};
    const auto &entry=found->second;
    std::error_code error;
    if(!sameBundleFile(entry,current) || entry.header!=current.header || entry.mask!=current.mask ||
       !std::filesystem::is_regular_file(entry.destination,error))return {};
    return entry.destination;
}
static void bundleLog(const char *action,const char *type,long long length,unsigned crc){
    if(auto f=std::fopen("bundle-streams.log","a")){
        std::fprintf(f,"%s %s bytes=%lld crc=%08x\n",action,type,length,crc);
        std::fclose(f);
    }
}
struct BundleNotification {
    HANDLE handle;
    explicit BundleNotification(const std::filesystem::path &folder)
        :handle(FindFirstChangeNotificationW(folder.c_str(),FALSE,FILE_NOTIFY_CHANGE_FILE_NAME)){}
    ~BundleNotification(){if(handle!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(handle);}
    void wait(DWORD milliseconds) {
        // A click into an inactive Unity window sends synchronous activation
        // messages. Service those while waiting for the converter, leaving
        // queued input and engine callbacks for Unity's normal player loop.
        DWORD count=handle==INVALID_HANDLE_VALUE?0:1;
        DWORD result=MsgWaitForMultipleObjectsEx(count,count?&handle:nullptr,milliseconds,
                                                QS_SENDMESSAGE,MWMO_INPUTAVAILABLE);
        if(count && result==WAIT_OBJECT_0){
            if(!FindNextChangeNotification(handle)){
                FindCloseChangeNotification(handle);
                handle=INVALID_HANDLE_VALUE;
            }
        }else if(result==WAIT_OBJECT_0+count){
            MSG message{};
            PeekMessageW(&message,nullptr,WM_NULL,WM_NULL,PM_NOREMOVE);
        }
    }
};
static std::filesystem::path prepareDesktopBundle(std::vector<unsigned char> &bytes,unsigned crc){
    if(bytes.size()<16 || std::memcmp(bytes.data(),"UnityFS",8))return {};
    struct Digest {
        BCRYPT_ALG_HANDLE algorithm=nullptr;
        BCRYPT_HASH_HANDLE value=nullptr;
        ~Digest(){if(value)BCryptDestroyHash(value);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    } hash;
    unsigned char digest[32]{};
    if(BCryptOpenAlgorithmProvider(&hash.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||
       BCryptCreateHash(hash.algorithm,&hash.value,nullptr,0,nullptr,0,0)<0||
       BCryptHashData(hash.value,bytes.data(),static_cast<ULONG>(bytes.size()),0)<0||
       BCryptFinishHash(hash.value,digest,sizeof digest,0)<0)throw std::runtime_error("bundle SHA-256");
    char key[80]{};
    for(size_t i=0;i<sizeof digest;++i)std::snprintf(key+2*i,3,"%02x",digest[i]);
    std::snprintf(key+64,16,"-%08x",crc);
    auto folder=privateClient()/desktopBundleDirectory;
    auto base=folder/key;
    auto destination=base;destination+=".winbundle";
    if(!std::filesystem::is_regular_file(destination)){
        auto inbox=folder/"pending";
        std::filesystem::create_directories(inbox);
        BundleNotification completion(folder);
        static std::atomic<unsigned> sequence{};
        auto partial=inbox/(std::to_string(GetCurrentProcessId())+"-"+std::to_string(sequence++)+".part");
        auto request=inbox/(std::string(key)+".request");
        if(!std::filesystem::exists(request)){
            std::ofstream out(partial,std::ios::binary);
            out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());out.close();
            auto error=out?(MoveFileW(partial.c_str(),request.c_str())?ERROR_SUCCESS:GetLastError()):ERROR_WRITE_FAULT;
            std::filesystem::remove(partial);
            if(error!=ERROR_SUCCESS&&error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS)throw std::runtime_error("publish bundle request");
        }
        auto failed=base;failed+=".error";
        auto deadline=GetTickCount64()+45000;
        while(!std::filesystem::exists(destination)&&!std::filesystem::exists(failed)&&GetTickCount64()<deadline)
            completion.wait(100);
    }
    if(std::filesystem::is_regular_file(destination)){
        return destination;
    }
    bundleLog("conversion unavailable",key,bytes.size(),crc);
    return {};
}
static std::filesystem::path convertBundleFile(const std::filesystem::path &path,unsigned crc){
    try{
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        auto n=input.tellg();
        if(n<16||n>(256LL<<20))return {};
        std::vector<unsigned char> bytes(static_cast<size_t>(n));
        input.seekg(0);
        if(input.read(reinterpret_cast<char *>(bytes.data()),n))return prepareDesktopBundle(bytes,crc);
    }catch(const std::exception &e){bundleLog(e.what(),"file",0,crc);}
    return {};
}
static std::filesystem::path convertBundleStream(P stream,unsigned crc){
    if(!stream)return {};
    static const auto getClass=bundleApi<P(*)(P)>("il2cpp_object_get_class");
    static const auto className=bundleApi<const char *(*)(P)>("il2cpp_class_get_name");
    static const auto method=bundleApi<P(*)(P,const char *,int)>("il2cpp_class_get_method_from_name");
    static const auto getVirtual=bundleApi<P(*)(P,P)>("il2cpp_object_get_virtual_method");
    static const auto call=bundleApi<P(*)(P,P,P *,P *)>("il2cpp_runtime_invoke");
    static const auto unbox=bundleApi<P(*)(P)>("il2cpp_object_unbox");
    static const auto byteClass=bundleApi<P(*)(P,const char *,const char *)>("il2cpp_class_from_name")(
        bundleApi<P(*)()>("il2cpp_get_corlib")(),"System","Byte");
    auto klass=getClass(stream);
    auto type=className(klass);
    auto run=[&](const char *name,int n,P *args)->P{
        auto m=method(klass,name,n);
        if(!m)throw std::runtime_error(name);
        P ex=nullptr;
        auto value=call(getVirtual(stream,m),stream,args,&ex);
        if(ex)throw std::runtime_error(name);
        return value;
    };
    int64_t length=0,position=-1;
    bool touched=false;
    auto restore=[&]{
        P args[]={&position};
        run("set_Position",1,args);
        touched=false;
    };
    try{
        if(!*static_cast<uint8_t *>(unbox(run("get_CanSeek",0,nullptr))))return {};
        length=*static_cast<int64_t *>(unbox(run("get_Length",0,nullptr)));
        position=*static_cast<int64_t *>(unbox(run("get_Position",0,nullptr)));
        if(position!=0 || length<16 || length>(256LL<<20))return {};
        OctoFile candidate;
        if(!crc){
            std::filesystem::path cached;
            try {cached=verifiedBundle(stream,klass,length,candidate);}
            catch(const std::filesystem::filesystem_error &e){bundleLog(e.what(),type,length,crc);}
            if(!cached.empty()){
                static const bool trace=std::getenv("REINCOMP_NATIVE_RENDER_TIMING")!=nullptr;
                if(trace){
                    static std::atomic<unsigned> hits{};
                    unsigned n=++hits;
                    if((n&(n-1))==0){
                        if(auto log=std::fopen("bundle-streams.log","a")){
                            std::fprintf(log,"Verified Octo file hits %u\n",n);
                            std::fclose(log);
                        }
                    }
                }
                return cached;
            }
        }
        std::vector<unsigned char> bytes(static_cast<size_t>(length));
        constexpr int chunk=256*1024;
        auto array=bundleApi<P(*)(P,size_t)>("il2cpp_array_new")(byteClass,(std::min<int64_t>)(chunk,length));
        struct Pin{
            uint32_t handle;
            ~Pin(){bundleApi<void(*)(uint32_t)>("il2cpp_gchandle_free")(handle);}
        } pin{bundleApi<uint32_t(*)(P,bool)>("il2cpp_gchandle_new")(array,true)};
        auto data=static_cast<unsigned char *>(array)+bundleApi<uint32_t(*)()>("il2cpp_array_object_header_size")();
        for(int64_t offset=0;offset<length;){
            int start=0,want=int((std::min<int64_t>)(chunk,length-offset));
            P args[]={array,&start,&want};
            touched=true;
            int got=*static_cast<int *>(unbox(run("Read",3,args)));
            if(got<=0||got>want)throw std::runtime_error("short stream");
            std::memcpy(bytes.data()+offset,data,got);
            offset+=got;
        }
        restore();
        auto path=prepareDesktopBundle(bytes,crc);
        rememberBundle(candidate,path);
        return path;
    }catch(const std::exception &e){
        if(touched)restore();
        bundleLog(e.what(),type,length,crc);
    }
    return {};
}
template<bool Async> static void loadDesktopStream(State &s,void *original){
    auto path=convertBundleStream(reinterpret_cast<P>(s.gpr.x0.qword),s.gpr.x1.dword);
    if(path.empty()){invoke<P,P,uint32_t,uint32_t>(s,original);return;}
    static auto load=[]{
        const char *name=Async?"UnityEngine.AssetBundle::LoadFromFileAsync_Internal":"UnityEngine.AssetBundle::LoadFromFile_Internal";
        auto m=std::lower_bound(std::begin(methods),std::end(methods),name,[](auto &m,const char *n){return std::strcmp(m.name,n)<0;});
        if(m==std::end(methods)||std::strcmp(m->name,name))std::abort();
        return reinterpret_cast<P(*)(P,uint32_t,uint64_t)>(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"))+m->rva);
    }();
    s.gpr.x0.qword=reinterpret_cast<uintptr_t>(load(managedPath(path.native()),0,0));
}
