#include "../host_services.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void ot2_host_log(const char*,...) {}
extern "C" [[noreturn]] void ot2_host_die(const char*,...) {throw std::runtime_error("Unexpected host failure");}
extern "C" uint64_t ot2_call_guest(const char*,const uint64_t*) {throw std::runtime_error("Unexpected guest call");}
extern "C" uint64_t ot2_call_guest_address(uint64_t,const uint64_t*) {throw std::runtime_error("Unexpected guest address call");}
extern "C" int ot2_signal_event(uint32_t){return 0;}
extern "C" void vkhost_init(){}
extern "C" uint64_t vkhost_frame_count(){return 0;}
extern "C" void* ot2_nvn_lookup(const char*){return nullptr;}
namespace ot2 {void set_audio_active(bool) {}}

template<class T> uint64_t addr(T* p){return reinterpret_cast<uint64_t>(p);}
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static uint64_t call(const char* name,std::initializer_list<uint64_t> values){
    uint64_t r[8]{};std::copy(values.begin(),values.end(),r);
    for(const auto& s:ot2::host_services())if(!strcmp(s.name,name))return s.call(r);
    throw std::runtime_error(std::string("Missing service: ")+name);
}
static void mount(){require(!call("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE",{addr("save"),0}),"mount save");}
static void unmount(){require(!call("_ZN2nn2fs7UnmountEPKc",{addr("save")}),"unmount save");}
static uint64_t commit(){return call("_ZN2nn2fs6CommitEPKc",{addr("save")});}
static uint64_t open(const char* path){uint64_t h=0;require(!call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{addr(&h),addr(path),3}),"open save");return h;}
static uint64_t write(uint64_t h,const char* value){uint32_t flush=1;return call("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",{h,0,addr(value),strlen(value),addr(&flush)});}
static void close(uint64_t h){require(!call("_ZN2nn2fs9CloseFileENS0_10FileHandleE",{h}),"close save");}
static void replace(const char* path,const char* value){auto h=open(path);require(!write(h,value),"write save");close(h);}
using Tree=std::map<std::string,std::string>;
static Tree tree(const std::string& path){
    Tree result;
    for(const auto& e:std::filesystem::recursive_directory_iterator(path))if(e.is_regular_file()){
        std::ifstream f(e.path(),std::ios::binary);result[e.path().lexically_relative(path).string()]={std::istreambuf_iterator<char>(f),{}};
    }
    return result;
}
static void seed(const std::string& base){
    std::filesystem::create_directories(base+"/save/nested");
    std::ofstream(base+"/save/a")<<"old-A";std::ofstream(base+"/save/nested/b")<<"old-B";
}
static void configure(const std::string& base){ot2::configure_services(base+"/rom",base);}
static void child_status(pid_t pid,int expected){int status=0;require(waitpid(pid,&status,0)==pid,"wait child");require(WIFEXITED(status)&&WEXITSTATUS(status)==expected,"crash injection must execute");}

int main(){try {
    std::string tmp=(getenv("TMPDIR")?getenv("TMPDIR"):"/tmp");tmp+="/ot2-save-contract-XXXXXX";
    require(mkdtemp(tmp.data()),"temporary save fixture");
    struct Cleanup {std::string p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{tmp};
    const Tree old{{"a","old-A"},{"nested/b","old-B"}},next{{"a","new-A"},{"nested/b","new-B"}};
    auto base=tmp+"/visibility";seed(base);configure(base);mount();
    replace("save:/a","new-A");
    require(tree(base+"/save")==old,"uncommitted writes must not change the committed save");
    require(!call("_ZN2nn2fs10DeleteFileEPKc",{addr("save:/nested/b")}),"delete working file");
    require(!call("_ZN2nn2fs10CreateFileEPKcl",{addr("save:/new-file"),12}),"create working file");
    unmount();mount();
    require(tree(base+"/save")==old,"unmount must discard an uncommitted transaction");
    replace("save:/a","new-A");replace("save:/nested/b","new-B");require(!commit(),"commit whole tree");
    require(tree(base+"/save")==next,"both files publish together");
    // A commit copies the work tree; it must not strand a still-open file on an old generation.
    auto h=open("save:/a");require(!write(h,"held1"),"write before commit");require(!commit(),"commit with open handle");
    require(!write(h,"held2"),"write through retained handle");close(h);require(!commit(),"commit retained handle update");
    require(tree(base+"/save").at("a")=="held2","handle remains on the working tree");unmount();
    std::cout<<"PASS save visibility, rollback, multi-file commit, retained handle\n";

    for(const char* point:{"before_commit","snapshot_file","snapshot_ready","before_exchange","after_exchange","parent_synced"}){
        base=tmp+"/crash-"+point;seed(base);pid_t pid=fork();require(pid>=0,"fork crash fixture");
        if(!pid){configure(base);mount();replace("save:/a","new-A");
            if(!strcmp(point,"before_commit"))_exit(91);
            replace("save:/nested/b","new-B");setenv("OT2_SAVE_TEST_KILL",point,1);commit();_exit(92);
        }
        child_status(pid,91);
        const bool published=!strcmp(point,"after_exchange")||!strcmp(point,"parent_synced");
        require(tree(base+"/save")== (published?next:old),"killed transaction must expose a complete generation");
        configure(base);mount();require(!commit(),"remount cleans abandoned transaction");unmount();
        require(tree(base+"/save")== (published?next:old),"recovery must not publish stale work");
        std::cout<<"PASS save crash "<<point<<'\n';
    }
    base=tmp+"/failure";seed(base);configure(base);mount();replace("save:/a","new-A");
    h=open("save:/nested/b");setenv("OT2_SAVE_TEST_FAIL","write",1);
    require(write(h,"new-B")!=0,"injected ENOSPC must reach WriteFile");close(h);
    require(commit()!=0,"a failed write must poison the whole transaction");
    require(ot2::take_save_notice()==ot2::SaveCommitFailed,"rejected commit reaches frontend notification");
    require(!ot2::take_save_notice(),"frontend notification is consumed once");
    require(tree(base+"/save")==old,"failed write cannot publish mixed files");
    replace("save:/a","new-A");replace("save:/nested/b","new-B");
    setenv("OT2_SAVE_TEST_FAIL","snapshot_file",1);require(commit()!=0,"snapshot failure reported");
    require(tree(base+"/save")==old,"failed snapshot preserves committed generation");
    require(!commit(),"retry complete snapshot");require(tree(base+"/save")==next,"recovery permits the next transaction");
    h=open("save:/a");setenv("OT2_SAVE_TEST_FAIL","resize",1);
    require(call("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl",{h,0})!=0,"injected resize failure");
    setenv("OT2_SAVE_TEST_FAIL","reclone",1);
    require(commit()!=0,"resize failure poisons commit");require(write(h,"stale")!=0,"rollback invalidates stale handles");
    require(tree(base+"/save")==next,"resize failure preserves committed save");
    require(commit()!=0,"failed rollback must stay poisoned until the retry completes");
    replace("save:/a","fixed");require(!commit(),"writes recover after a failed rollback");
    require(tree(base+"/save").at("a")=="fixed","next transaction publishes after rollback retry");unmount();
    std::cout<<"PASS save ENOSPC, snapshot retry, resize failure, stale handle rejection, rollback retry\n";
    base=tmp+"/read-only-fallback";seed(base);configure(base);
    ot2::take_save_notice();
    setenv("OT2_SAVE_TEST_FAIL","mountclone",1);mount();
    require(ot2::take_save_notice()==ot2::SaveReadOnly,"read-only fallback reaches frontend notification");
    uint64_t readonly=0;
    require(!call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{addr(&readonly),addr("save:/a"),1}),"failed work clone must still load committed saves");
    char data[5]{};uint64_t count=0;
    require(!call("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",{addr(&count),readonly,0,addr(data),sizeof(data)})&&count==5&&!memcmp(data,"old-A",5),"read-only fallback reads last commit");
    require(write(readonly,"oops!")!=0,"read-only handle rejects writes");close(readonly);
    require(call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{addr(&readonly),addr("save:/a"),2})!=0,"read-only fallback rejects writable opens");
    require(call("_ZN2nn2fs10CreateFileEPKcl",{addr("save:/a"),5})==0x402,"existing read-only file reports already exists");
    require(call("_ZN2nn2fs15CreateDirectoryEPKc",{addr("save:/nested")})==0x402,"existing read-only directory reports already exists");
    require(call("_ZN2nn2fs10CreateFileEPKcl",{addr("save:/new"),5})!=0,"read-only fallback rejects creation");
    require(call("_ZN2nn2fs10DeleteFileEPKc",{addr("save:/a")})!=0,"read-only fallback rejects deletion");
    require(call("_ZN2nn2fs10RenameFileEPKcS2_",{addr("save:/a"),addr("save:/new")})!=0,"read-only fallback rejects rename");
    require(!commit(),"D2R read-only fallback commit is an empty operation");
    require(tree(base+"/save")==old,"read-only session never changes committed data");unmount();
    mount();replace("save:/a","fixed");require(!commit(),"remount can recover writable save");unmount();
    require(tree(base+"/save").at("a")=="fixed","recovered mount publishes save");
    std::cout<<"PASS save read-only fallback, existing-path errors, protected commit, writable remount\n";
    base=tmp+"/durability";seed(base);configure(base);mount();
    replace("save:/a","new-A");replace("save:/nested/b","new-B");
    setenv("OT2_SAVE_TEST_FAIL","parent_sync",1);require(commit()!=0,"parent sync failure must be reported after publication");
    require(tree(base+"/save")==next,"published generation remains complete after sync failure");
    require(ot2::take_save_notice()==ot2::SaveDurabilityUncertain,"frontend must distinguish publication from confirmed durability");
    require(!commit(),"durability failure permits retry");unmount();
    std::cout<<"PASS save published-but-unsynced notification and retry\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL save contract: "<<e.what()<<'\n';return 1;}}
