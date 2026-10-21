#include "../host_services.h"
#include "../graphics/backend.h"
#include "../graphics/nvn_dirty.h"
#include "../resource_reader.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/mman.h>
#include <fstream>
extern "C" void ot2_host_stream_profile(int);

extern "C" void ot2_host_log(const char*,...) {}
extern "C" uint64_t ot2_call_guest_address(uint64_t,const uint64_t*) {throw std::runtime_error("Unexpected guest address call");}
extern "C" [[noreturn]] void ot2_host_die(const char*,...) {throw std::runtime_error("Unexpected native failure");}
static uintptr_t created_event=0;static bool event_signaled=false;
extern "C" uint64_t ot2_call_guest(const char* name,const uint64_t* r) {
    if(!strcmp(name,"_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")){created_event=r[0];event_signaled=false;return 0;}
    if(!strcmp(name,"_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")&&created_event==r[0])return 42;
    throw std::runtime_error("Unexpected guest call in service test");
}
extern "C" int ot2_signal_event(uint32_t h){if(h!=42)return 0;event_signaled=true;return 1;}
extern "C" void vkhost_init(){}
extern "C" uint64_t vkhost_frame_count(){return 0;}
extern "C" void* ot2_nvn_lookup(const char*){return nullptr;}
namespace ot2 {void set_audio_active(bool) {}}

template<class T> uint64_t address(T* value){return reinterpret_cast<uint64_t>(value);}
uint64_t call(const char* name,std::initializer_list<uint64_t> args){
    uint64_t r[8]{};std::copy(args.begin(),args.end(),r);
    for(auto& service:ot2::host_services())if(!strcmp(service.name,name))return service.call(r);
    throw std::runtime_error(std::string("Service absent: ")+name);
}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    for(const auto& service:ot2::host_services())require(strcmp(service.name,"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb"),"Real SDK must initialize filter from mounted system data");
    for(const auto& service:ot2::host_services())require(strcmp(service.name,"_ZN2nn3ngc15ProfanityFilterC1Ev")&&strcmp(service.name,"_ZN2nn3ngc15ProfanityFilterD1Ev")&&strcmp(service.name,"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj"),"Polymorphic filter lifecycle and checks remain in SDK");
    char temporary[]="/tmp/ot2-filesystem-XXXXXX";auto* directory=mkdtemp(temporary);if(!directory)return 2;
    struct Cleanup {std::string path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    std::string base=directory;std::filesystem::create_directory(base+"/rom");std::filesystem::create_directory(base+"/user");
    std::filesystem::create_directories(base+"/AddOnContent/3");
    std::filesystem::create_directories(base+"/AddOnContent/1");
    std::filesystem::create_directories(base+"/AddOnContent/01");
    std::filesystem::create_directories(base+"/AddOnContent/2001");
    std::ofstream(base+"/AddOnContent/1/item")<<"owned";
    std::filesystem::create_directory_symlink(base+"/rom",base+"/AddOnContent/2");
    ot2::configure_services(base+"/rom",base+"/user");
    {uint64_t language=0;call("_ZN2nn8settings15GetLanguageCodeEPNS0_12LanguageCodeE",{address(&language)});
     require(language==call("_ZN2nn2oe18GetDesiredLanguageEv",{})&&!memcmp(&language,"zh-Hans",7),"System settings language agrees with configured guest language");}
    {
        uint64_t cache=99,h=0,read=0;char bytes[5]{};
        require(call("_ZN2nn2fs29QueryMountSystemDataCacheSizeEPmNS_3ncm12SystemDataIdE",{address(&cache),0x0100000000000823})==0x202,"Missing system archive is not invented");
        std::filesystem::create_directories(base+"/user/SystemData/0100000000000823");
        std::ofstream(base+"/user/SystemData/0100000000000823/version.dat")<<"test";
        require(!call("_ZN2nn2fs29QueryMountSystemDataCacheSizeEPmNS_3ncm12SystemDataIdE",{address(&cache),0x0100000000000823})&&cache==0,"Host directory mounting needs no guest cache allocation");
        require(!call("_ZN2nn2fs15MountSystemDataEPKcNS_3ncm12SystemDataIdEPvm",{address("ngc"),0x0100000000000823,0,0}),"Mount actual system archive read-only");
        require(!call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&h),address("ngc:/version.dat"),1}),"Read installed dictionary data");
        require(!call("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",{address(&read),h,0,address(bytes),4})&&read==4&&!strcmp(bytes,"test"),"System archive contents are not synthetic");
        call("_ZN2nn2fs9CloseFileENS0_10FileHandleE",{h});
        require(call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&h),address("ngc:/version.dat"),2})!=0,"System archive is never writable");
        call("_ZN2nn2fs7UnmountEPKc",{address("ngc")});
        std::filesystem::create_directory_symlink(base+"/rom",base+"/user/SystemData/0100000000000824");
        require(call("_ZN2nn2fs15MountSystemDataEPKcNS_3ncm12SystemDataIdE",{address("bad"),0x0100000000000824})==0x202,"System data symlink cannot escape archive root");
    }
    {
        int32_t count=99;std::array<uint8_t,32> names;names.fill(0x5a);
        call("_ZN2nn4bcat10InitializeEv",{});
        require(call("_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi",{address(&count),address(names.data()),1})==0xa7a&&count==0,"BCAT requires cache mount");
        require(!call("_ZN2nn4bcat25MountDeliveryCacheStorageEv",{}),"mount empty local BCAT cache");
        require(!call("_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi",{address(&count),address(names.data()),1})&&count==0&&names.front()==0x5a,"empty BCAT enumeration does not invent entries");
        require(call("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE",{})==0x47a,"BCAT cache miss returns NotFound");
        uint64_t progress=0;
        call("_ZN2nn4bcat21DeliveryCacheProgressC1Ev",{address(&progress)});
        require(call("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv",{address(&progress)})==0,"BCAT progress begins idle");
        require(call("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE",{address(&progress)})==0x107a,"offline BCAT sync is denied");
        require(call("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv",{address(&progress)})==9&&call("_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv",{address(&progress)})==0x107a,"BCAT terminal progress preserves failure");
        call("_ZN2nn4bcat21DeliveryCacheProgressD1Ev",{address(&progress)});
        call("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv",{});
    }
    require(call("_ZN2nn3aoc17CountAddOnContentEv",{})==2,"only installed canonical DLC directories are reported");
    int32_t indices[4]={-1,-1,-1,-1};
    require(call("_ZN2nn3aoc16ListAddOnContentEPiii",{address(indices),0,4})==2&&indices[0]==1&&indices[1]==3&&indices[2]==-1,"ordered bounded DLC list");
    require(call("_ZN2nn3aoc16ListAddOnContentEPiii",{address(indices),1,1})==1&&indices[0]==3,"DLC list pagination");
    require(call("_ZN2nn3aoc16ListAddOnContentEPiii",{address(indices),uint64_t(-1),4})==0,"negative DLC offset rejected");
    require(call("_ZN2nn2fs17MountAddOnContentEPKciPvm",{address("missing"),2,0,0})==0x7d402,"unowned DLC cannot be mounted");
    require(!call("_ZN2nn2fs17MountAddOnContentEPKciPvm",{address("dlc"),1,0,0}),"owned DLC mount");
    uint64_t dlc_handle=0,dlc_read=0;char dlc_bytes[6]{};
    require(!call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&dlc_handle),address("dlc:/item"),1}),"DLC file read open");
    require(!call("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",{address(&dlc_read),dlc_handle,0,address(dlc_bytes),5})&&dlc_read==5&&!strcmp(dlc_bytes,"owned"),"DLC content read through real filesystem path");
    call("_ZN2nn2fs9CloseFileENS0_10FileHandleE",{dlc_handle});
    require(call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&dlc_handle),address("dlc:/item"),2})!=0,"DLC mount is read only");

    {
        std::string contents(200000,'x');for(size_t i=0;i<contents.size();++i)contents[i]=char(i*31);
        std::ofstream(base+"/rom/resource",std::ios::binary).write(contents.data(),contents.size());
        require(!call("_ZN2nn2fs8MountRomEPKcPvm",{address("rom"),0,0}),"resource mount");
        for(int enabled:{0,1}){
            ot2_host_stream_profile(enabled);uint64_t h=0,count=0;
            require(!call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&h),address("rom:/resource"),1}),"resource open");
            std::array<char,65536> output{};
            for(uint64_t offset:{0,65536,196600}){
                require(!call("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",{address(&count),h,offset,address(output.data()),output.size()}),"resource pread");
                require(count==std::min<uint64_t>(output.size(),contents.size()-offset)&&!memcmp(output.data(),contents.data()+offset,count),"resource bytes and EOF identical with both fd paths");
            }
            require(!call("_ZN2nn2fs9CloseFileENS0_10FileHandleE",{h}),"resource close");
            require(call("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",{address(&count),h,0,address(output.data()),1})!=0,"closed guest handle unavailable immediately");
        }
        auto owner=std::make_shared<ResourceReadFile>(open((base+"/rom/resource").c_str(),O_RDONLY));require(owner->fd>=0,"resource fd lease");
        auto pending=owner;const int original_fd=owner->fd;owner.reset();char actual[32]{};
        require(pread(pending->fd,actual,sizeof(actual),65536)==sizeof(actual)&&!memcmp(actual,contents.data()+65536,sizeof(actual)),"pending read survives guest owner close without fd recycling");
        pending.reset();require(fcntl(original_fd,F_GETFD)<0&&errno==EBADF,"final read lease closes fd");
        ResourcePrefetch queue;owner=std::make_shared<ResourceReadFile>(open((base+"/rom/resource").c_str(),O_RDONLY));
        require(!queue.add(owner,0,3*1024*1024)&&!queue.add(owner,INT64_MAX,1),"prefetch bounds reject oversize and offset overflow");
        for(unsigned i=0;i<1000;++i)queue.add(owner,0,65536);queue.clear();
    }
    {
        std::array<uint8_t,64> context,token,event;context.fill(0xa5);token.fill(0x5a);event.fill(0x7b);auto original=context;uint64_t length=123;
        call("_ZN2nn7account12AsyncContextC1Ev",{address(context.data())});
        require(context==original,"Offline async constructor preserves unknown SDK object layout");
        require(call("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",{address(context.data()),0})!=0,"Offline token acquisition fails explicitly");
        require(call("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE",{address(context.data()),address(event.data())})==0&&event_signaled&&created_event==address(event.data()),"Offline account completion uses a real SDK-created signaled event");
        require(call("_ZN2nn7account12AsyncContext9GetResultEv",{address(context.data())})!=0,"Completion is not authentication success");
        require(call("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",{address(&length),address(token.data()),token.size(),0})!=0&&length==0&&token.front()==0x5a,"Token failure reports zero length without fabricating credentials");
        for(auto& service:ot2::host_services())if(strstr(service.name,"_ZN2nn7friends")&&!strstr(service.name,"10InitializeEv")){uint64_t r[8]{};require(service.call(r)!=0,"Remote friends operation cannot report success");}
        require(call("_ZN2nn6socket6SocketEiii",{2,1,0})==uint64_t(-1)&&call("_ZN2nn6socket12GetLastErrorEv",{})==50,"Offline socket has BSD failure and Horizon ENETDOWN");
        uint64_t resolved=0x123;require(call("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_",{0,0,0,address(&resolved)})==4&&resolved==0,"DNS failure clears result pointer");
    }
    uint32_t message=0;ot2::set_activity_active(false);
    require(call("_ZN2nn2oe20GetCurrentFocusStateEv",{})==2,"visible but unfocused");
    ot2::set_activity_focus(3);
    require(call("_ZN2nn2oe20GetCurrentFocusStateEv",{})==3,"background focus");
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==1&&message==15,"background notification");
    ot2::set_activity_focus(3);
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==0,"unchanged focus does not notify");
    ot2::set_activity_active(true);ot2::set_activity_active(false);ot2::set_activity_active(true);
    require(call("_ZN2nn2oe20GetCurrentFocusStateEv",{})==1,"foreground focus");
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==1&&message==15,"focus notification");
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==0,"focus notifications coalesce");
    call("_ZN2nn2oe28SetResumeNotificationEnabledEb",{1});
    ot2::set_activity_focus(2);ot2::set_activity_focus(1);
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==1&&message==15,"Resume retains focus-change notification");
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==1&&message==16,"Subscribed guest receives real resume after keyboard focus returns");
    ot2::set_activity_focus(1);
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==0,"Stable foreground does not invent resume events");
    ot2::set_activity_focus(2);ot2::set_activity_focus(1);
    call("_ZN2nn2oe28SetResumeNotificationEnabledEb",{0});
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==1&&message==15,"Disabling resume does not drop focus state");
    require(call("_ZN2nn2oe25TryPopNotificationMessageEPj",{address(&message)})==0,"Disabled resume notification is not delivered");
    require(!call("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE",{address("save"),0}),"mount save");
    require(!call("_ZN2nn2fs10CreateFileEPKcl",{address("save:/slot"),8}),"create save");
    uint64_t handle=0;require(!call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&handle),address("save:/slot"),2}),"open writable save");
    uint32_t flush=1;const char bytes[]="OT2SAVE";
    require(!call("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",{handle,0,address(bytes),8,address(&flush)}),"write save");
    require(!call("_ZN2nn2fs9CloseFileENS0_10FileHandleE",{handle}),"close written save");
    require(!call("_ZN2nn2fs10CreateFileEPKcl",{address("save:/other"),1}),"create rename target");
    require(call("_ZN2nn2fs10RenameFileEPKcS2_",{address("save:/other"),address("save:/slot")})!=0,"rename must not replace existing save");
    require(!call("_ZN2nn2fs14CommitSaveDataEPKc",{address("save")}),"commit save");
    require(!call("_ZN2nn2fs7UnmountEPKc",{address("save")}),"unmount save");
    require(!call("_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE",{address("read"),0x0100a3501946e000,0}),"reopen read-only save");
    require(call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&handle),address("read:/slot"),2})!=0,"read-only write rejected");
    require(!call("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",{address(&handle),address("read:/slot"),1}),"open read-only save");
    std::array<char,8> restored{};uint64_t read=0;
    require(!call("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",{address(&read),handle,0,address(restored.data()),restored.size()}),"read restored save");
    require(read==8&&!memcmp(bytes,restored.data(),8),"saved bytes survive remount");
    size_t page=sysconf(_SC_PAGESIZE);auto* tracked=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);require(tracked!=MAP_FAILED,"tracked read allocation");
    require(nvn_dirty_add_range(tracked,page)&&nvn_dirty_protect(tracked,page),"protect guest file destination");auto stamp=nvn_dirty_now();
    require(!call("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",{address(&read),handle,0,address(tracked),8}),"real ReadFile unprotects kernel destination");
    require(read==8&&!memcmp(bytes,tracked,8)&&nvn_dirty_since(tracked,page,stamp),"kernel file write dirties the texture page");
    nvn_dirty_remove_range(tracked);munmap(tracked,page);call("_ZN2nn2fs9CloseFileENS0_10FileHandleE",{handle});
    require(call("_ZN2nn2fs10CreateFileEPKcl",{address("read:/../escape"),1})!=0,"traversal rejected");
    require(!call("_ZN2nn2fs18CreateCacheStorageEill",{7,4096,2048}),"create cache");
    require(call("_ZN2nn2fs18CreateCacheStorageEill",{7,4096,2048})!=0,"duplicate cache rejected");
    int64_t size=0,journal=0;require(!call("_ZN2nn2fs19GetCacheStorageSizeEPlS1_i",{address(&size),address(&journal),7}),"cache size");
    require(size==4096&&journal==2048,"cache metadata persisted");
    uint64_t list=0;require(!call("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE",{address(&list)}),"open cache list");
    std::array<uint8_t,40> entries;entries.fill(0xff);int32_t count=-1;
    require(!call("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi",{address(&count),address(entries.data()),list,1}),"read cache list");
    int32_t index=-1;memcpy(&index,entries.data(),4);require(count==1&&index==7&&entries[31]==0&&entries[32]==255,"cache entry ABI and bounds");
    require(!call("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi",{address(&count),address(entries.data()),list,1})&&count==0,"cache list exhaustion");
    call("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE",{list});
    require(!call("_ZN2nn2fs17MountCacheStorageEPKci",{address("cache"),7}),"mount cache");
    require(call("_ZN2nn2fs18DeleteCacheStorageEi",{7})!=0,"mounted cache protected");
    require(!call("_ZN2nn2fs15CreateDirectoryEPKc",{address("cache:/nested")}),"cache directory");
    require(!call("_ZN2nn2fs10CreateFileEPKcl",{address("cache:/nested/file"),4}),"cache file");
    require(!call("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",{address("cache:/nested")}),"recursive cleanup");
    require(call("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",{address("cache:/")})!=0,"mount root protected");
    call("_ZN2nn2fs7UnmountEPKc",{address("cache")});require(!call("_ZN2nn2fs18DeleteCacheStorageEi",{7}),"delete unmounted cache");
    require(call("_ZN2nn2fs17MountCacheStorageEPKci",{address("cache"),7})!=0,"deleted cache not mounted");
    call("_ZN2nn2fs7UnmountEPKc",{address("read")});
    constexpr uint64_t other=0x0100ad401a2f0000ULL;
    ot2::configure_services(base+"/rom",base+"/other",other);
    require(ot2::configured_program_id()==other,"configured game identity");
    require(!call("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE",{other,0}),"other title cannot see previous title's existing save");
    std::filesystem::create_directories(base+"/other/save");
    require(call("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE",{other,0}),"selected title sees its own save");
    require(!call("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE",{ot2::legacy_ot2_program_id,0}),"cross-title save identity rejected");
    require(call("_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE",{address("wrong"),ot2::legacy_ot2_program_id,0})!=0,"cross-title read-only save rejected");
    std::cout<<"PASS filesystem: durable save remount, read-only boundary, cache lifecycle, title isolation\n";
}
