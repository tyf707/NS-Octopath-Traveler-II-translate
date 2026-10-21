#include "guest.h"
#include "game_profile.h"
#include "fault_decode.h"
#include "guest_clock.h"
#include "frame_rate_patch.h"
#include "guest_context.h"
#include "native_abi.h"
#include "thread_services.h"
#include "host_services.h"
#include "graphics/nvn_dirty.h"
#include <asm/sigcontext.h>
#include "graphics/backend.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <unordered_set>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

extern "C" {
alignas(4) __attribute__((visibility("hidden"))) uint32_t ot2_register_gates_enabled=0;
alignas(4) __attribute__((visibility("hidden"))) uint32_t ot2_native_abis[2048]{};
void ot2_signal_entry(int, siginfo_t*, void*);
uint64_t ot2_guest_call(uintptr_t, GuestThread*, const uint64_t*, uintptr_t);
void ot2_host_invoke(void*,uint64_t*,void*,uintptr_t);
void ot2_abi_fixture();
void ot2_native_entry();
uint64_t ot2_host_stack_call(uint64_t (*)(uint64_t*),uint64_t*,uintptr_t);
}
namespace {
struct Trap { uintptr_t pc; uint32_t instruction; const char* hook; uint64_t (*call)(uint64_t*)=nullptr; uint64_t hits=0; };
struct alignas(16) GuestThreadContextImage {
    uint64_t gprs[29];
    uint64_t fp,lr,sp,pc;
    uint32_t psr;
    alignas(16) uint8_t fpu_gprs[32][16];
    uint32_t fpcr,fpsr;
    uint64_t tpidr;
};
static_assert(sizeof(GuestThreadContextImage)==0x320);
static_assert(offsetof(GuestThreadContextImage,fpu_gprs)==0x110);
static_assert(offsetof(GuestThreadContextImage,tpidr)==0x318);
std::vector<Trap> traps;
alignas(4) uint32_t fast_tls_enabled=1;
std::atomic<uint64_t> tls_signal_count{0};
std::atomic<uint64_t> svc_signal_count{0},counter_signal_count{0},sdk_signal_count{0};
struct NativeGate {char name[160];void* function;uint64_t hits;bool present;};
NativeGate native_gates[2048]{};
std::atomic<unsigned> native_gate_count{0};
uint8_t* native_gate_code=nullptr;
std::mutex native_gate_mutex;
std::vector<ot2::ModuleView> images;
int log_fd=-1;
uint8_t* address_space{};
constexpr size_t reserve_size=128ULL<<30;
constexpr size_t alias_offset=0x10000000, alias_size=96ULL<<30;
constexpr size_t stack_offset=100ULL<<30, stack_capacity=512ULL<<20;
constexpr size_t heap_offset=101ULL<<30, heap_capacity=4ULL<<30;
size_t heap_size=0;
uint64_t image_size=0;
struct Mapping { uintptr_t base; size_t size; uint32_t state; uintptr_t source; };
std::vector<Mapping> mappings;
std::mutex mapping_mutex;
std::mutex setup_mutex;
bool thread_system_ready=false;
ot2::LoadedGame* active_game=nullptr;
thread_local ucontext_t* active_signal=nullptr;
thread_local uintptr_t service_callback_stack=0;
thread_local unsigned host_service_depth=0;

void log(const char* fmt, ...) {
    char buffer[2048]; va_list args; va_start(args,fmt);
    vsnprintf(buffer,sizeof(buffer),fmt,args); va_end(args);
    __android_log_write(ANDROID_LOG_INFO,"OT2Guest",buffer);
    if(log_fd>=0) { write(log_fd,buffer,strlen(buffer)); write(log_fd,"\n",1); }
}
[[noreturn]] void fail(const char* reason, const ucontext_t* uc=nullptr) {
    log("STOP %s",reason);
    if(uc) {
        const auto& c=uc->uc_mcontext;
        log("pc=%llx lr=%llx sp=%llx",c.pc,c.regs[30],c.sp);
        for(const auto& m:images) {
            auto b=reinterpret_cast<uintptr_t>(m.base);
            if(c.pc>=b && c.pc<b+m.size) log("PC %s+0x%llx",m.name.c_str(),c.pc-b);
            if(c.regs[30]>=b && c.regs[30]<b+m.size) log("LR %s+0x%llx",m.name.c_str(),c.regs[30]-b);
        }
        for(unsigned i=0;i<31;i+=3) log("x%u=%llx x%u=%llx x%u=%llx",i,c.regs[i],i+1,i+1<31?c.regs[i+1]:0,i+2,i+2<31?c.regs[i+2]:0);
        uintptr_t fp=c.regs[29];
        for(unsigned depth=0;depth<24 && fp;depth++) {
            bool readable=false;
            for(unsigned n=0;n<GuestThreadCapacity;n++) {
                const auto& t=ot2_threads[n];
                if(fp>=t.stack && fp+16<=t.stack+t.stack_size) readable=true;
            }
            if(!readable || (fp&15)) break;
            const auto* frame=reinterpret_cast<const uintptr_t*>(fp);
            for(const auto& m:images) {
                auto base=reinterpret_cast<uintptr_t>(m.base);
                if(frame[1]>=base && frame[1]<base+m.size) log("FRAME %u %s+0x%llx",depth,m.name.c_str(),(unsigned long long)(frame[1]-base));
            }
            if(frame[0]<=fp) break;
            fp=frame[0];
        }
    }
    if(log_fd>=0) fsync(log_fd);
    _exit(71);
}
GuestThread* current() { return ot2_guest_context_current(); }
ot2::GuestClock guest_clock;
uint64_t monotonic_ns() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts);
    return uint64_t(ts.tv_sec)*1000000000 + uint64_t(ts.tv_nsec);
}
uint64_t ticks() {
    const auto ns=guest_clock.read(monotonic_ns);
    return (ns/1000000000)*19200000 + (ns%1000000000)*12/625;
}
GuestThread* create_thread_context() { return ot2_guest_context_create(); }
void install_handlers() {
    struct sigaction action{}; action.sa_sigaction=ot2_signal_entry;
    action.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_NODEFER; sigemptyset(&action.sa_mask);
    for(int sig:{SIGTRAP,SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGPROF}) if(sigaction(sig,&action,nullptr)) fail("Signal handler setup failed");
}
bool special(uint32_t instruction) {
    auto op=instruction&~31U;
    return (instruction&0xffe0001f)==0xd4000001 || op==0xd53bd040 || op==0xd53bd060 ||
        op==0xd51bd040 || op==0xd53be020 || op==0xd53be000;
}
#include "tls_rewrite.inc"
/* 最近宿主服务（NVN/SDK 门）调用环形记录：看卡住时游戏还在做哪些系统调用。 */
/* Ring of the latest host-service (NVN/SDK gate) calls: shows which system calls the game still makes while stalled. */
static constexpr unsigned gate_ring_capacity=64;
static std::atomic<unsigned> gate_ring[gate_ring_capacity];
static std::atomic<unsigned> gate_ring_pos{0};
/* 宿主服务（陷阱表）最近命中环形：只存静态名字指针。 */
static constexpr unsigned service_ring_capacity=64;
static const char* service_ring[service_ring_capacity];
static std::atomic<unsigned> service_pos{0};
/* 协程状态探针的落点（island 直接写这里，不走函数调用）。 */
/* Landing zone for the coroutine state probe (the island writes here directly). */
alignas(8) static volatile uint32_t coro_probe_slot[2]={0,0};
/* 指针钩子落点：+0 = iterator 指针，+8 = 调用计数。 */
alignas(8) struct { volatile uint64_t ptr; volatile uint32_t calls; volatile uint32_t pad; } coro_ptr_probe{};
alignas(8) struct { volatile uint64_t ptr; volatile uint32_t calls; volatile uint32_t pad; } cb_reg_probe{};
alignas(8) struct { volatile uint64_t ptr; volatile uint32_t calls; volatile uint32_t pad; } ex_probe{};
alignas(8) struct { volatile uint64_t ptr; volatile uint32_t calls; volatile uint32_t pad; } post_probe{};
alignas(8) struct { volatile uint64_t ptr; volatile uint32_t calls; volatile uint32_t pad; } prefs_ex_probe{};
alignas(8) struct { volatile uint64_t ptr; volatile uint32_t calls; volatile uint32_t pad; } nre_probe{};
extern "C" uint32_t ot2_prefs_ex_calls(void){return prefs_ex_probe.calls;}
extern "C" uint64_t ot2_prefs_ex_ptr(void){return prefs_ex_probe.ptr;}
extern "C" uint64_t ot2_nre_ptr(void){return nre_probe.ptr;}
extern "C" uint32_t ot2_nre_calls(void){return nre_probe.calls;}
extern "C" uint64_t ot2_ex_probe(void){return ex_probe.ptr;}
extern "C" uint32_t ot2_ex_probe_calls(void){return ex_probe.calls;}
extern "C" uint32_t ot2_post_probe_calls(void){return post_probe.calls;}
extern "C" uint64_t ot2_cb_reg(void){return cb_reg_probe.ptr;}
extern "C" uint32_t ot2_cb_reg_calls(void){return cb_reg_probe.calls;}
extern "C" uint64_t ot2_coro_ptr(void){return coro_ptr_probe.ptr;}
extern "C" uint32_t ot2_coro_ptr_calls(void){return coro_ptr_probe.calls;}
alignas(8) static volatile uint32_t exc_probe_slot[2]={0,0};
extern "C" uint32_t ot2_coro_probe_state(void){return coro_probe_slot[0];}
extern "C" uint32_t ot2_coro_probe_calls(void){return coro_probe_slot[1];}
extern "C" uint32_t ot2_exc_probe_calls(void){return exc_probe_slot[1];}
extern "C" uint64_t ot2_host_set_start_time(uint64_t*);
#include "system_rewrite.inc"
extern "C" uint64_t ot2_call_guest_address(uint64_t,const uint64_t*);
static uintptr_t fade_material_address=0;
static uint64_t fade_apply(uint64_t* r,bool black) {
    auto* self=reinterpret_cast<uint8_t*>(r[0]);
    if(self) {
        const float power=black?1.0f:0.0f;
        const float zero=0.0f;
        memcpy(self+0x28,&power,4);
        memcpy(self+0x3c,&zero,4);
        const uint64_t arguments[8]={r[0],0,0,0,0,0,0,0};
        if(fade_material_address) ot2_call_guest_address(fade_material_address,arguments);
    }
    return 0;
}
static uint64_t fade_in_host(uint64_t* r){return fade_apply(r,false);}
static uint64_t fade_out_host(uint64_t* r){return fade_apply(r,true);}
void patch(uint8_t* base,size_t size) {
    const auto first_island=tls_islands.size();
    size_t tls_native=0,tls_fallback=0;
    for(size_t offset=0;offset+4<=size;offset+=4) {
        uint32_t op; memcpy(&op,base+offset,4);
        if(!special(op)) continue;
        if(patch_tls(reinterpret_cast<uintptr_t>(base+offset),op,first_island)){++tls_native;continue;}
        if(patch_system(reinterpret_cast<uintptr_t>(base+offset),op,first_island))continue;
        auto kind=op&~31u;if(kind==0xd53bd040||kind==0xd53bd060||kind==0xd51bd040)++tls_fallback;
        traps.push_back({reinterpret_cast<uintptr_t>(base+offset),op,nullptr});
        const uint32_t brk=0xd4200000 | (0x702<<5);
        memcpy(base+offset,&brk,4);
    }
    seal_tls_islands(first_island);
    if(size>4096)log("TLS_PATCH base=%p bytes=%zu native=%zu signal=%zu islands=%zu",base,size,tls_native,tls_fallback,tls_islands.size()-first_island);
}
/* 调试探针：把某个 IL2CPP 方法入口换成只打日志的宿主服务（命中即记，跳过原函数）。 */
/* Debug probe: replace an IL2CPP method entry with a log-only host service (records hits, skips the body). */
static uint64_t probe_hit(uint64_t* r) {
    log("PROBE_HIT thread=%#x x0=%llx x1=%llx x2=%llx x3=%llx",
        ot2_thread_handle_current(),(unsigned long long)r[0],(unsigned long long)r[1],(unsigned long long)r[2],(unsigned long long)r[3]);
    return 0;
}
void install_service_initializers(const ot2::LoadedGame& game) {
    const auto first_island=tls_islands.size();
    // This game's own SDK ships its direct-call audio renderer. Select it at
    // the manager seam instead of opening Horizon's audren IPC service.
    const auto audio_ipc=game.symbol("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv");
    const auto audio_dfc=game.symbol("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if(!audio_ipc || !audio_dfc || (int64_t(audio_dfc)-int64_t(audio_ipc))%4 ||
       std::abs(int64_t(audio_dfc)-int64_t(audio_ipc))>=(1LL<<27))fail("SDK direct audio entry unavailable");
    const uint32_t audio_branch=0x14000000U | (uint32_t((int64_t(audio_dfc)-int64_t(audio_ipc))/4)&0x03ffffffU);
    memcpy(reinterpret_cast<void*>(audio_ipc),&audio_branch,4);
    // Service clients are replaced by host APIs, so their Horizon IPC sessions
    // must not be created. Keep nn::os and libc initialization in the original SDK.
    static constexpr const char* names[]={
        "nndiagStartup", "_ZN2nn2oe10InitializeEv", "_ZN2nn3aoc10InitializeEv",
        "_ZN2nn3err26InitializeContextWriterApiEv", "_ZN2nn2fs24InitializeForApplicationEv",
        "_ZN2nn4pctl10InitializeEv", "_ZN2nn4diag34InitializeApplicationAbortObserverEv"
    };
    for(const auto* name:names) {
        const auto address=game.symbol(name);
        if(!address) fail("Service initialization export absent");
        uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
        if(patch_system(address,original,first_island,name))continue;
        traps.push_back({address,original,name});
        const uint32_t brk=0xd4200000 | (0x703<<5);
        memcpy(reinterpret_cast<void*>(address),&brk,4);
    }
    for(const auto* list:{&ot2::host_services(),&ot2::audio_services(),&ot2::input_services(),&ot2::movie_services()}) for(const auto& service:*list) {
        const auto address=game.symbol(service.name);
        if(!address) continue;
        std::erase_if(traps,[&](const auto& t){return t.pc==address;});
        uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
        if(patch_system(address,original,first_island,service.name,service.call))continue;
        traps.push_back({address,original,service.name,service.call});
        const uint32_t brk=0xd4200000 | (0x703<<5);
        memcpy(reinterpret_cast<void*>(address),&brk,4);
    }
    /* 服务覆盖自检：把游戏导出的 `_ZN2nn*` SDK 符号与我们的服务表对照，
     * 给新游戏接入一个可读的清单（hooked=已由宿主接管，unhooked=跑游戏自带 SDK 代码）。
     * debug.ot2.servicecheck=1 时额外打印未接管的符号名。 */
    {
        std::unordered_set<std::string> implemented;
        size_t table_entries=0;
        for(const auto* list:{&ot2::host_services(),&ot2::audio_services(),&ot2::input_services(),&ot2::movie_services()})
            for(const auto& service:*list){implemented.insert(service.name);++table_entries;}
        auto names=game.exported_names("_ZN2nn");
        size_t hooked=0,unhooked=0;
        std::vector<const std::string*> misses;
        for(const auto& name:names){
            if(implemented.count(name))++hooked;
            else{++unhooked;if(misses.size()<64)misses.push_back(&name);}
        }
        log("SERVICE_COVERAGE sdk_exports=%zu hooked=%zu native=%zu table_entries=%zu",
            names.size(),hooked,unhooked,table_entries);
        char check[PROP_VALUE_MAX]{};
        if(__system_property_get("debug.ot2.servicecheck",check)>0&&check[0]!='0'){
            std::string chunk;
            for(const auto* name:misses){
                if(chunk.size()+name->size()>900){log("SERVICE_UNHOOKED %s",chunk.c_str());chunk.clear();}
                if(!chunk.empty())chunk+=' ';
                chunk+=*name;
            }
            if(!chunk.empty())log("SERVICE_UNHOOKED %s",chunk.c_str());
            if(unhooked>misses.size())log("SERVICE_UNHOOKED truncated=%zu",unhooked-misses.size());
        }
    }
    /* 用宿主时钟实现 SystemTimeWork.SetTime：本作开场场景里 GameManager 尚未初始化，
     * GameManager.nowTime 是默认 DateTime(0001-01-01)，游戏自己的 SetTime 会对它调
     * DateTime.ToFileTime 抛 ArgumentOutOfRangeException，把命名回调整个展开掉，
     * 于是「命名成功后卡在博士场景」。这里用宿主时钟做同语义实现。
     * 默认对 010018e011d92000 启用；debug.ot2.fixtime=0 可关闭（调试用）。
     * TODO: 找到 GameManager.nowTime 未初始化的根因后可以移除本补丁。 */
    {
        char fix[PROP_VALUE_MAX]{};
        const bool has_property=__system_property_get("debug.ot2.fixtime",fix)>0;
        // 补丁开关：优先 profile 声明（game-build.json 的 patches），
        // 未声明时退回内置默认（Unity 标题默认启用）；debug.ot2.fixtime=0 可强制关闭。
        const bool enabled=has_property?(fix[0]!='0'):ot2::patch_enabled("settime-host",
            ot2::configured_program_id()==0x010018e011d92000ULL||ot2::engine_kind()==ot2::Engine::unity);
        if(enabled) {
            for(const auto& m:images) if(m.name=="main") {
                const auto address=reinterpret_cast<uintptr_t>(m.base)+0x1C177C0;
                uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
                if(patch_system(address,original,first_island,"SystemTimeWork_SetTime_host",ot2_host_set_start_time))
                    log("SETTIME_HOST installed");
                else log("SETTIME_HOST failed");
            }
        }
    }
    /* 过渡修复：Fader.OnUpdate 在场景切换后不再被 tick，遮罩材质停在
     * fillPower=1 的不透明状态，画面全黑、演出协程等 isBusy 卡死。
     * 这里把 FadeIn/FadeOut 变成瞬时完成（改 _fillPower/_speed 并立即 UpdateMaterial）。 */
    {
        char fix[PROP_VALUE_MAX]{};
        const bool has_property=__system_property_get("debug.ot2.fadefix",fix)>0;
        /* 默认关闭：实测它并不能解除黑屏，且可能干扰早期淡入流程；仅调试时用
         * debug.ot2.fadefix=1 打开。 */
        const bool enabled=has_property?(fix[0]!='0'):ot2::patch_enabled("fade-instant",false);
        if(enabled) for(const auto& m:images) if(m.name=="main") {
            const auto base=reinterpret_cast<uintptr_t>(m.base);
            fade_material_address=base+0x24D03A0;
            auto install=[&](uintptr_t address,uint64_t (*call)(uint64_t*),const char* name) {
                uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
                if(patch_system(address,original,first_island,name,call)) log("FADEFIX %s ok",name);
                else log("FADEFIX %s failed",name);
            };
            install(base+0x24D04C0,fade_in_host,"FadeIn_instant");
            install(base+0x24D05A0,fade_in_host,"FadeIn_duration_instant");
            install(base+0x24D0820,fade_out_host,"FadeOut_instant");
            install(base+0x24D0900,fade_out_host,"FadeOut_duration_instant");
        }
    }
    /* 诊断：打印模块基址，便于把栈上的地址映射回模块偏移。 */
    for(const auto& m:images) log("MODULE %s base=%#llx size=%#llx",
        m.name.c_str(),(unsigned long long)reinterpret_cast<uintptr_t>(m.base),(unsigned long long)m.size);
    /* debug.ot2.probe=1：命名回调探针；debug.ot2.probe=2：协程 state 记录钩子。 */
    {
        char probe[PROP_VALUE_MAX]{};
        if(__system_property_get("debug.ot2.probe",probe)>0) {
            if(!strcmp(probe,"30")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    struct SceneProbe { uint64_t rva; volatile void* slot; const char* name; } points[]={
                        {0x1E0F6A0,&coro_ptr_probe,"SceneSwitchNext"},
                        {0x1C1F710,&ex_probe,"TitleInitialize"},
                        {0x1C1F9B0,&nre_probe,"TitlePlayOpening"},
                        {0x240C8B0,&cb_reg_probe,"TitleLoadAssetNext"},
                        {0x240CBC0,&prefs_ex_probe,"TitleOpeningNext"},
                        {0x240AB70,&post_probe,"TitleUpdate"},
                    };
                    for(const auto& point:points)
                        log("SCENE_PROBE %s %s",point.name,
                            patch_register_hook(base+point.rva,point.slot,first_island,0)?"installed":"failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"29")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    struct SceneProbe { uint64_t rva; volatile void* slot; unsigned reg; const char* name; } points[]={
                        {0x44B34,&ex_probe,0,"RaiseExceptionObject"},
                        {0x9AE10,&nre_probe,30,"RaiseExceptionCaller"},
                        {0x1E19F30,&cb_reg_probe,0,"EventStartUpNext"},
                        {0x1F1CA60,&prefs_ex_probe,0,"DailyEventUpdate"},
                        {0x1F56E80,&post_probe,0,"BerryGrowUpdate"},
                        {0x1B65F40,&coro_ptr_probe,0,"LoadObjectCreate"},
                    };
                    for(const auto& point:points)
                        log("SCENE_PROBE %s %s",point.name,
                            patch_register_hook(base+point.rva,point.slot,first_island,point.reg)?"installed":"failed");
                }
                goto probe_done;
            }
            // Read-only scene-transition diagnostics. The islands preserve the
            // displaced instruction and registers; no coroutine state is changed.
            if(!strcmp(probe,"28")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    struct SceneProbe { uint64_t rva; volatile void* slot; const char* name; } points[]={
                        {0x1E0F6A0,&coro_ptr_probe,"SceneSwitchNext"},
                        {0x21B7820,&ex_probe,"FieldInitNext"},
                        {0x21B8600,&nre_probe,"FieldInitAfterNext"},
                        {0x1BC0C70,&cb_reg_probe,"FieldSetupNext"},
                        {0x1BC3BF0,&prefs_ex_probe,"FieldLoadEnd"},
                        {0x189E670,&post_probe,"ExceptionCtor"},
                    };
                    for(const auto& point:points)
                        log("SCENE_PROBE %s %s",point.name,
                            patch_register_hook(base+point.rva,point.slot,first_island,0)?"installed":"failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"27")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x21A4070,&ex_probe,first_island,0)) log("X1 LangOnUpdate");
                    else log("X1 failed");
                    if(patch_register_hook(base+0x21A5700,&nre_probe,first_island,0)) log("X2 OpOpenNext");
                    else log("X2 failed");
                    if(patch_register_hook(base+0x21A43D0,&cb_reg_probe,first_island,0)) log("X3 UpdateSelect");
                    else log("X3 failed");
                    if(patch_register_hook(base+0x21A48C0,&post_probe,first_island,0)) log("X4 OpenConfirm");
                    else log("X4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"26")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x1F346D0,&ex_probe,first_island,0)) log("W1 PlayCorNext");
                    else log("W1 failed");
                    if(patch_register_hook(base+0x1F32860,&nre_probe,first_island,0)) log("W2 Play");
                    else log("W2 failed");
                    if(patch_register_hook(base+0x1F345E0,&cb_reg_probe,first_island,0)) log("W3 OpPlayNext");
                    else log("W3 failed");
                    if(patch_register_hook(base+0x1F32F10,&post_probe,first_island,0)) log("W4 SetFadeExit");
                    else log("W4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"25")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x1F322D0,&ex_probe,first_island,0)) log("V1 DemoMyUpdate");
                    else log("V1 failed");
                    if(patch_register_hook(base+0x24D01B0,&nre_probe,first_island,0)) log("V2 FaderOnUpdate");
                    else log("V2 failed");
                    if(patch_register_hook(base+0x1F345E0,&cb_reg_probe,first_island,0)) log("V3 OpPlayNext");
                    else log("V3 failed");
                    if(patch_register_hook(base+0x1F346D0,&post_probe,first_island,0)) log("V4 PlayCorNext");
                    else log("V4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"24")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x24D01B0,&ex_probe,first_island,0)) log("U1 FaderOnUpdate");
                    else log("U1 failed");
                    if(patch_register_hook(base+0x24CFD80,&nre_probe,first_island,0)) log("U2 FaderIsBusy");
                    else log("U2 failed");
                    if(patch_register_hook(base+0x1F34F70,&cb_reg_probe,first_island,0)) log("U3 SceneStartNext");
                    else log("U3 failed");
                    if(patch_register_hook(base+0x24D04C0,&post_probe,first_island,0)) log("U4 FadeIn");
                    else log("U4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"23")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x1F34440,&ex_probe,first_island,0)) log("T1 OpLoadAndPlay");
                    else log("T1 failed");
                    if(patch_register_hook(base+0x1F34200,&nre_probe,first_island,0)) log("T2 OpLoad");
                    else log("T2 failed");
                    if(patch_register_hook(base+0x1F34F70,&cb_reg_probe,first_island,0)) log("T3 SceneStart");
                    else log("T3 failed");
                    if(patch_register_hook(base+0x2409280,&post_probe,first_island,0)) log("T4 MovieUpdate");
                    else log("T4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"22")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x1F329D0,&ex_probe,first_island,0)) log("S1 DemoSceneStart");
                    else log("S1 failed");
                    if(patch_register_hook(base+0x2408BB0,&nre_probe,first_island,0)) log("S2 PlayStreaming");
                    else log("S2 failed");
                    if(patch_register_hook(base+0x1BF8AC0,&cb_reg_probe,first_island,0)) log("S3 NowLoadingClose");
                    else log("S3 failed");
                    if(patch_register_hook(base+0x20C7ED0,&post_probe,first_island,0)) log("S4 NowLoadingUpdate");
                    else log("S4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"21")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x1C175E0,&ex_probe,first_island,0)) log("R1 SetStartTime");
                    else log("R1 failed");
                    if(patch_register_hook(base+0x1C176D8,&nre_probe,first_island,0)) log("R2 beforeSetTime");
                    else log("R2 failed");
                    if(patch_register_hook(base+0x2388B20,&cb_reg_probe,first_island,0)) log("R3 set_SystemData");
                    else log("R3 failed");
                    if(patch_register_hook(base+0x227AEE8,&post_probe,first_island,0)) log("R4 return");
                    else log("R4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"20")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x1E0A870,&ex_probe,first_island,0)) log("Q1 GameManager.Initialize");
                    else log("Q1 failed");
                    if(patch_register_hook(base+0x1E0EA00,&nre_probe,first_island,0)) log("Q2 OnetimeInit.MoveNext");
                    else log("Q2 failed");
                    if(patch_register_hook(base+0x1E0B1D0,&cb_reg_probe,first_island,0)) log("Q3 GameManager.OnUpdate");
                    else log("Q3 failed");
                    if(patch_register_hook(base+0x1E09910,&prefs_ex_probe,first_island,0)) log("Q4 set_nowTime");
                    else log("Q4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"19")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x26B6B40,&ex_probe,first_island,0)) log("P1 ThrowAOORE");
                    else log("P1 failed");
                    if(patch_register_hook(base+0x27E8AC0,&nre_probe,first_island,0)) log("P2 ToFileTimeUtc");
                    else log("P2 failed");
                    if(patch_register_hook(base+0x2388B20,&cb_reg_probe,first_island,0)) log("P3 set_SystemData");
                    else log("P3 failed");
                    if(patch_register_hook(base+0x189E670,&prefs_ex_probe,first_island,0)) log("P4 ExceptionCtor");
                    else log("P4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"18")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x27E8A70,&nre_probe,first_island,0)) log("E1 ToFileTime x0");
                    else log("E1 failed");
                    if(patch_register_hook(base+0x2388B20,&cb_reg_probe,first_island,0)) log("E2 set_SystemData");
                    else log("E2 failed");
                    if(patch_register_hook(base+0x227AEE8,&post_probe,first_island,0)) log("E3 return");
                    else log("E3 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"17")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x26BB2B0,&ex_probe,first_island,0)) log("D1 get_Local");
                    else log("D1 failed");
                    if(patch_register_hook(base+0x27E8AC0,&nre_probe,first_island,0)) log("D2 ToFileTimeUtc");
                    else log("D2 failed");
                    if(patch_register_hook(base+0x2388B20,&cb_reg_probe,first_island,0)) log("D3 set_SystemData");
                    else log("D3 failed");
                    if(patch_register_hook(base+0x227AEE8,&post_probe,first_island,0)) log("D4 return");
                    else log("D4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"16")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x238F000,&ex_probe,first_island,0)) log("C1 SetPlayerStatus");
                    else log("C1 failed");
                    if(patch_register_hook(base+0x1C175E0,&nre_probe,first_island,0)) log("C2 SetStartTime");
                    else log("C2 failed");
                    if(patch_register_hook(base+0x27E8A70,&prefs_ex_probe,first_island,0)) log("C3 ToFileTime");
                    else log("C3 failed");
                    if(patch_register_hook(base+0x1DEF080,&cb_reg_probe,first_island,0)) log("C4 ExistCheck");
                    else log("C4 failed");
                    if(patch_register_hook(base+0x227AEE8,&post_probe,first_island,0)) log("C5 return");
                    else log("C5 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"15")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x27E8A70,&nre_probe,first_island,0)) log("N1 ToFileTime");
                    else log("N1 failed");
                    if(patch_register_hook(base+0x19B7DA0,&prefs_ex_probe,first_island,0)) log("N2 AOORE");
                    else log("N2 failed");
                    if(patch_register_hook(base+0x1C175E0,&cb_reg_probe,first_island,0)) log("N3 SetStartTime");
                    else log("N3 failed");
                    if(patch_register_hook(base+0x238F2A8,&post_probe,first_island,0)) log("N4 late");
                    else log("N4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"14")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x237DF80,&nre_probe,first_island,0)) log("M1 get_SystemData");
                    else log("M1 failed");
                    if(patch_register_hook(base+0x1C176D8,&prefs_ex_probe,first_island,0)) log("M2 beforeSetTime");
                    else log("M2 failed");
                    if(patch_register_hook(base+0x2388B20,&cb_reg_probe,first_island,0)) log("M3 set_SystemData");
                    else log("M3 failed");
                    if(patch_register_hook(base+0x1C178D0,&post_probe,first_island,0)) log("M4 SetDailyEventLastTime");
                    else log("M4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"13")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x238F24C,&nre_probe,first_island,0)) log("K1 installed");
                    else log("K1 failed");
                    if(patch_register_hook(base+0x238F2A8,&prefs_ex_probe,first_island,0)) log("K2 installed");
                    else log("K2 failed");
                    if(patch_register_hook(base+0x238F2B0,&post_probe,first_island,0)) log("K3 installed");
                    else log("K3 failed");
                    if(patch_register_hook(base+0x1C175E0,&cb_reg_probe,first_island,0)) log("K4 installed");
                    else log("K4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"12")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x238F110,&nre_probe,first_island,0)) log("H110 installed");
                    else log("H110 failed");
                    if(patch_register_hook(base+0x1E039B0,&prefs_ex_probe,first_island,0)) log("HFlag installed");
                    else log("HFlag failed");
                    if(patch_register_hook(base+0x1F54020,&cb_reg_probe,first_island,0)) log("HItem installed");
                    else log("HItem failed");
                    if(patch_register_hook(base+0x238F1B4,&post_probe,first_island,0)) log("H1B4 installed");
                    else log("H1B4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"11")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x238F000,&nre_probe,first_island,0)) log("H1 SetPlayerStatus entry");
                    else log("H1 failed");
                    if(patch_register_hook(base+0x227AEE8,&cb_reg_probe,first_island,19)) log("H2 post-call");
                    else log("H2 failed");
                    if(patch_register_hook(base+0x1DEF080,&prefs_ex_probe,first_island,0)) log("H3 ExistCheck");
                    else log("H3 failed");
                    if(patch_register_hook(base+0x1F3E280,&post_probe,first_island,0)) log("H4 MoveNext control");
                    else log("H4 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"10")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x28C8BF0,&prefs_ex_probe,first_island,0)) log("PREFS_EX_HOOK installed");
                    else log("PREFS_EX_HOOK failed");
                    if(patch_register_hook(base+0x26990E0,&nre_probe,first_island,0)) log("NRE_HOOK installed");
                    else log("NRE_HOOK failed");
                    if(patch_register_hook(base+0x227AEE8,&cb_reg_probe,first_island,19)) log("REG_HOOK installed");
                    else log("REG_HOOK failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"9")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x1DEF080,&ex_probe,first_island,0)) log("EX_HOOK installed");
                    else log("EX_HOOK failed");
                    if(patch_register_hook(base+0x1D1AD34,&post_probe,first_island,0)) log("POST_HOOK installed");
                    else log("POST_HOOK failed");
                    if(patch_register_hook(base+0x227AEE8,&cb_reg_probe,first_island,19)) log("REG_HOOK installed");
                    else log("REG_HOOK failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"8")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_pointer_hook(base+0x227AE60,&coro_ptr_probe,first_island)) log("PTR_HOOK b10_0 installed");
                    else log("PTR_HOOK b10_0 failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"7")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_register_hook(base+0x227AEE8,&cb_reg_probe,first_island,19)) log("REG_HOOK x19@0x227AEE8 installed");
                    else log("REG_HOOK failed");
                    if(patch_pointer_hook(base+0x1F3C090,&coro_ptr_probe,first_island)) log("PTR_HOOK callback installed");
                    else log("PTR_HOOK callback failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"6")) {
                for(const auto& m:images) if(m.name=="main") {
                    if(patch_pointer_hook(reinterpret_cast<uintptr_t>(m.base)+0x1F3C090,&coro_ptr_probe,first_island))
                        log("PTR_HOOK callback installed");
                    else log("PTR_HOOK callback failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"5")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_pointer_hook(base+0x1F3E280,&coro_ptr_probe,first_island)) log("PTR_HOOK installed");
                    else log("PTR_HOOK failed");
                    uint32_t original;memcpy(&original,reinterpret_cast<void*>(base+0x238F000),4);
                    if(patch_system(base+0x238F000,original,first_island,"PlayerWork_SetPlayerStatus",probe_hit))
                        log("PROBE_INSTALLED PlayerWork_SetPlayerStatus");
                    else log("PROBE_FAILED PlayerWork_SetPlayerStatus");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"4")) {
                for(const auto& m:images) if(m.name=="main") {
                    if(patch_pointer_hook(reinterpret_cast<uintptr_t>(m.base)+0x1F3E280,&coro_ptr_probe,first_island))
                        log("PTR_HOOK installed");
                    else log("PTR_HOOK failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"3")) {
                for(const auto& m:images) if(m.name=="main") {
                    const auto base=reinterpret_cast<uintptr_t>(m.base);
                    if(patch_state_hook(base+0x1F3E280,coro_probe_slot,first_island,0x10))
                        log("STATE_HOOK coro installed");
                    else log("STATE_HOOK coro failed");
                    if(patch_state_hook(base+0x189E670,exc_probe_slot,first_island,0x10))
                        log("STATE_HOOK exc installed");
                    else log("STATE_HOOK exc failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"2")) {
                for(const auto& m:images) if(m.name=="main") {
                    /* Demo_Hakase.<PlayerSelect>d__17$$MoveNext：记录 <>1__state（+0x10）。 */
                    const auto address=reinterpret_cast<uintptr_t>(m.base)+0x1F3E280;
                    if(patch_state_hook(address,coro_probe_slot,first_island,0x10))
                        log("STATE_HOOK installed at %#llx",(unsigned long long)address);
                    else log("STATE_HOOK failed");
                }
                goto probe_done;
            }
            if(!strcmp(probe,"1")) for(const auto& m:images) if(m.name=="main") {
                struct Probe { uint64_t rva; const char* name; } probes[]={
                    {0x238F000,"PlayerWork_SetPlayerStatus"},
                };
                for(const auto& p:probes) {
                    const auto address=reinterpret_cast<uintptr_t>(m.base)+p.rva;
                    uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
                    if(patch_system(address,original,first_island,p.name,probe_hit))
                        log("PROBE_INSTALLED %s at %#llx",p.name,(unsigned long long)address);
                    else log("PROBE_FAILED %s",p.name);
                }
            }
        }
    probe_done:;
    }
    seal_tls_islands(first_island);
}
#include "guest_memory.inc"
std::atomic<int> requested_frame_rate{30},applied_frame_rate{0};
ot2::FrameRatePatch frame_rate_patch;
void update_frame_rate(){
    struct Memory {
        void* resolve(uintptr_t address,size_t length,bool write){
            if(!length||address>UINTPTR_MAX-length)return nullptr;
            const int need=PROT_READ|(write?PROT_WRITE:0);
            // All fields fit eight bytes; check both 4-KiB guest pages if a
            // pointer straddles the boundary, including borrowed source pages.
            if((memory_permission_at(address)&need)!=need||(memory_permission_at(address+length-1)&need)!=need)return nullptr;
            return reinterpret_cast<void*>(address);
        }
    } memory;
    std::lock_guard lock(mapping_mutex);
    const int target=requested_frame_rate.load(std::memory_order_relaxed);
    auto result=frame_rate_patch.apply(memory,target);
    if(result==ot2::FrameRatePatch::changed||result==ot2::FrameRatePatch::unchanged){
        if(applied_frame_rate.exchange(target)!=target)log("FRAME_RATE_POLICY applied=%d build=0d9649011312f62e clock=unchanged",target);
    }else if(applied_frame_rate.exchange(0)!=0)log("FRAME_RATE_POLICY unavailable result=%d",int(result));
}
/* SDK 在 TLS+0x1F8 存线程控制块指针，并读其 +0x1B0 作为线程标识（sdk+0x351890）。
 * guest 自己用 msr 设置 TLS 时不会经过 guest_context_create，这里按需补一块。
 * The SDK keeps a thread control block at TLS+0x1F8 and reads its +0x1B0 as the
 * thread id; a guest that sets TLS via msr bypasses guest_context_create. */
static void ot2_tls_ensure_control(GuestThread* c) {
    if(!c||!c->guest_tls) return;
    uint64_t cur=0; memcpy(&cur,reinterpret_cast<const void*>(c->guest_tls+0x1f8),8);
    if(cur) return;
    void* ctl=mmap(nullptr,0x400,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(ctl==MAP_FAILED) return;
    uint32_t tid=0x1001u+uint32_t(c->handle&0xffffu);
    memcpy(reinterpret_cast<uint8_t*>(ctl)+0x1b0,&tid,4);
    uint64_t ptr=reinterpret_cast<uintptr_t>(ctl);
    memcpy(reinterpret_cast<void*>(c->guest_tls+0x1f8),&ptr,8);
}
bool capture_guest_thread_context(const ucontext_t* uc,const GuestThread* c,GuestThreadContextImage& out) {
    const auto& m=uc->uc_mcontext;
    for(unsigned i=0;i<29;i++)out.gprs[i]=m.regs[i];
    out.fp=m.regs[29];out.lr=m.regs[30];out.sp=m.sp;
    out.pc=m.pc+4;out.psr=uint32_t(m.pstate);out.tpidr=c->guest_tls;
    const auto* records=m.__reserved;
    size_t offset=0;
    while(offset+sizeof(_aarch64_ctx)<=sizeof(m.__reserved)) {
        const auto* record=reinterpret_cast<const _aarch64_ctx*>(records+offset);
        if(record->size<sizeof(*record)||record->size>sizeof(m.__reserved)-offset)break;
        if(record->magic==FPSIMD_MAGIC && record->size>=sizeof(fpsimd_context)) {
            const auto* fp_context=reinterpret_cast<const fpsimd_context*>(record);
            memcpy(out.fpu_gprs,fp_context->vregs,sizeof(out.fpu_gprs));
            out.fpcr=fp_context->fpcr;out.fpsr=fp_context->fpsr;
            return true;
        }
        offset+=record->size;
    }
    return false;
}
void svc(unsigned id,ucontext_t* uc,GuestThread* c) {
    auto* r=uc->uc_mcontext.regs;
    /* 环形记录最近 SVC（纯内存写，无锁）：卡住时主线程到底在循环什么。 */
    /* Ring-record the latest SVCs (memory writes only): what does the main thread loop on while stalled. */
    ot2_thread_record_svc(id,uc->uc_mcontext.sp,r[30],r[0],r[1]);
    if(id==0x1c) ot2_thread_record_sample_sp(uc->uc_mcontext.sp);
    static std::atomic<unsigned long> svc_seq{0};
    const unsigned long svc_n=svc_seq.fetch_add(1,std::memory_order_relaxed);
    if(svc_n<400000) log("SVC#%lu 0x%x thread=%#llx tls=%#llx tlr=%#llx x0=%llx x1=%llx x2=%llx x3=%llx",svc_n,id,
        (unsigned long long)c->handle,(unsigned long long)c->guest_tls,(unsigned long long)c->guest_tlr,r[0],r[1],r[2],r[3]);
    if(id==0x1e) { r[0]=ticks(); return; }
    if(id==0x15 || id==0x51 || id==0x52) {
        std::lock_guard lock(mapping_mutex);
        const uintptr_t address=r[1];const size_t length=r[2];const uint32_t handle=r[0],permission=r[3];
        uint64_t transfer_serial=0;
        if(auto result=memory_validate(address,length,memory_invalid_current)){r[0]=result;return;}
        if(id!=0x15&&!memory_contains(reinterpret_cast<uintptr_t>(address_space),reserve_size,address,length)){r[0]=memory_invalid_current;return;}
        if(id!=0x52&&permission!=0&&permission!=1&&permission!=3){r[0]=0xd801;return;}
        if(id==0x15){
            if(!memory_source_available(address,length)){r[0]=memory_invalid_current;return;}
            transfer_sources.reserve(transfer_sources.size()+1);
        }else{
            auto owner=std::find_if(transfer_sources.begin(),transfer_sources.end(),[&](const auto& t){return t.handle==handle&&!t.closed;});
            if(owner==transfer_sources.end()){r[0]=0xe401;return;}
            transfer_serial=owner->serial;
            const auto base=reinterpret_cast<uintptr_t>(address_space);
            if(memory_overlaps(address,length,base+heap_offset,heap_capacity)||memory_overlaps(address,length,base+alias_offset,alias_size)){r[0]=memory_invalid_region;return;}
            if(owner->size!=length){r[0]=memory_invalid_size;return;}
            if(id==0x51){
                if(owner->permission!=permission||std::any_of(mappings.begin(),mappings.end(),[&](const auto& m){return (m.state==0xd||m.state==0xe)&&m.source==transfer_serial;})){r[0]=0xfa01;return;}
                if(!memory_destination_free(address,length)){r[0]=memory_invalid_current;return;}
                mappings.reserve(mappings.size()+1);
            }else{
                auto mapped=std::find_if(mappings.begin(),mappings.end(),[&](const auto& m){return m.base==address&&m.size==length&&(m.state==0xd||m.state==0xe)&&m.source==transfer_serial;});
                if(mapped==mappings.end()){r[0]=memory_invalid_current;return;}
            }
        }
        if(!svc_thread(id,reinterpret_cast<uint64_t*>(r)))fail("Transfer memory adapter absent",uc);
        if(!r[0])memory_registry_changed();
        if(!r[0]&&id==0x15){
            transfer_sources.push_back({address,length,uint32_t(r[1]),permission,false,++next_transfer_serial});memory_protect(address,length,permission);
        }
        if(!r[0]&&id==0x51)mappings.push_back({address,length,permission?0xeu:0xdu,transfer_serial});
        if(!r[0]&&id==0x52)std::erase_if(mappings,[&](const auto& m){return m.base==address&&m.size==length&&(m.state==0xd||m.state==0xe)&&m.source==transfer_serial;});
        return;
    }
    if(id==0x16){
        std::lock_guard lock(mapping_mutex);const auto handle=uint32_t(r[0]);
        auto owner=std::find_if(transfer_sources.begin(),transfer_sources.end(),[&](const auto& t){return t.handle==handle&&!t.closed;});
        if(owner!=transfer_sources.end()){
            const bool mapped=std::any_of(mappings.begin(),mappings.end(),[&](const auto& m){return (m.state==0xd||m.state==0xe)&&m.source==owner->serial;});
            if(!svc_thread(id,reinterpret_cast<uint64_t*>(r)))fail("Transfer close adapter absent",uc);
            if(!r[0]){
                if(mapped)owner->closed=true;
                else {const auto address=owner->base,length=owner->size;transfer_sources.erase(owner);memory_protect(address,length,PROT_READ|PROT_WRITE);}
            }
            return;
        }
    }
    /* WaitProcessWideKeyAtomic 的 sp 已在 svc() 入口记录（见上方 ot2_thread_record_svc 段）。 */
    /* WaitProcessWideKeyAtomic's sp is recorded at the svc() entry (see the
     * ot2_thread_record_svc section above). */
    if(thread_system_ready && svc_thread(id,reinterpret_cast<uint64_t*>(r))) return;
    if(id==0x10) { r[0]=0; return; }
    if(id==0x25) {
        /* GetThreadId(handle)：必须按**传入的句柄**返回对应线程的 id，
         * 否则 SDK 为别的线程建的线程对象会带上当前线程 id，
         * 导致「某对象是否是当前线程」的判断恒为真。
         * GetThreadId(handle) must report the thread named by the handle, otherwise
         * thread objects get the caller's id and "is this the current thread"
         * always evaluates true. */
        const uint32_t requested=static_cast<uint32_t>(r[1]);
        uint64_t out=c->handle;
        if(requested && requested!=0xffff8001u) {
            for(unsigned i=0;i<GuestThreadCapacity;i++)
                if(static_cast<uint32_t>(ot2_threads[i].handle)==requested){ out=ot2_threads[i].handle; break; }
        }
        r[0]=0; r[1]=out; return;
    }
    if(id==0x0c) { r[0]=0;r[1]=44;return; }
    if(id==0x0e) { r[0]=0;r[1]=0;r[2]=1;return; }
    if(id==0x27) { log("GUEST %.*s",int(std::min<uint64_t>(r[1],1600)),reinterpret_cast<char*>(r[0]));r[0]=0;return; }
    if(id==0x29) {
        std::lock_guard lock(mapping_mutex);
        auto base=reinterpret_cast<uintptr_t>(address_space); uint64_t value=0;
        switch(r[1]) {
        case 0:value=1;break; case 1:value=0xffffffffffffffffULL;break;
        case 2:value=base+alias_offset;break; case 3:value=alias_size;break;
        case 4:value=base+heap_offset;break; case 5:value=heap_capacity;break;
        case 6:case 21:value=0xcd500000;break;
        case 7:case 22:value=heap_size+image_size;break;
        case 8:value=0;break;
        case 11:if(getrandom(&value,8,0)!=8) fail("Random source failed",uc);break;
        case 12:value=base;break; case 13:value=reserve_size;break;
        case 14:value=base+stack_offset;break; case 15:value=stack_capacity;break;
        case 16:value=0x2000000;break; case 17:value=0x400000;break;
        case 18:value=ot2::configured_program_id();break; case 20:value=0;break;
        case 23:value=1;break;
        case 28:
            if(r[3]){r[0]=0xf001;return;} // Kernel InvalidEnumValue
            if(uint32_t(r[2])!=0xffff8001U){r[0]=0xe401;return;} // Current process handle only
            // The host reserves a fixed alias region (id 3); it does not add
            // the optional extra region selected by newer Horizon processes.
            // Report that actual layout, not an invented per-game size.
            value=0;break;
        default:log("Unhandled GetInfo id=%llu",r[1]);fail("GetInfo unavailable",uc);
        }
        r[0]=0;r[1]=value;return;
    }
    if(id==0x06) {
        std::lock_guard lock(mapping_mutex);
        const uintptr_t address=r[2]; uint64_t b=0,size=0; uint32_t state=0,permission=0,attribute=0;
        if(address>=c->stack && address<c->stack+c->stack_size) { b=c->stack;size=c->stack_size;state=0xb;permission=3; }
        else if(const auto image=memory_image_region(address);image.size){b=image.base;size=image.size;state=image.state;permission=image.permission;}
        if(!size) for(const auto& m:mappings) if(address>=m.base && address<m.base+m.size) {b=m.base;size=m.size;state=m.state;permission=3;break;}
        const auto heap=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
        if(!size && address>=heap && address<heap+heap_size) {b=heap;size=heap_size;state=5;permission=3;}
        memory_query_source(address,b,size,state,attribute,permission);
        if(!size && address>=(1ULL<<39)){b=1ULL<<39;size=0-b;state=0x10;permission=0;}
        if(!size) {
            // Horizon exposes holes as free ranges. SDK startup walks from address zero.
            uint64_t lo=0,hi=1ULL<<39;
            auto boundary=[&](uint64_t begin,uint64_t count) {
                if(begin+count<=address) lo=std::max(lo,begin+count);
                if(begin>address) hi=std::min(hi,begin);
            };
            boundary(c->stack,c->stack_size);
            for(const auto& m:images) boundary(reinterpret_cast<uintptr_t>(m.base),m.size);
            for(const auto& m:mappings) boundary(m.base,m.size);
            if(heap_size) boundary(reinterpret_cast<uintptr_t>(address_space)+heap_offset,heap_size);
            if(address>=hi) fail("QueryMemory exceeds 39-bit address space",uc);
            b=lo;size=hi-lo;
        }
        auto* out=reinterpret_cast<uint64_t*>(r[0]);out[0]=b;out[1]=size;
        auto* p=reinterpret_cast<uint32_t*>(out);p[4]=state;p[5]=attribute;p[6]=permission;p[7]=0;p[8]=0;p[9]=0;
        r[0]=0;r[1]=0;return;
    }
    if(id==0x2c || id==0x2d) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_physical(id==0x2c,r[0],r[1]);return;
    }
    if(id==0x04 || id==0x05) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_stack(id==0x04,r[0],r[1],r[2]);return;
    }
    if(id==0x01) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_resize_heap(r[1]);
        if(!r[0])r[1]=reinterpret_cast<uintptr_t>(address_space)+heap_offset;return;
    }
    /* Unity 的 SDK 在初始化时用 0x1f 取一个 32 位值（返回值放在 x1）。
     * The Unity SDK reads a 32-bit value through 0x1f during startup (result in x1). */
    if(id==0x1f) {
        auto* t=ot2_guest_context_current();
        ot2_tls_ensure_control(t);
        const char* name=reinterpret_cast<const char*>(r[1]?r[1]:0);
        log("CONNECT_NAMED_PORT name=%s", name?name:"?");
        r[0]=0; r[1]=0x200; return;
    }
    /* 线程属性查询：0x21 = 本线程、0x22 = 指定线程、0x23 返回值放 x1。
     * Thread attribute queries: 0x21 self, 0x22 handle, 0x23 returns in x1. */
    if(id==0x21) {
        /* sm: 服务管理器 HLE。请求区可能落在 TLR+0（服务名或 CMIF）或
         * TLR+0x20（域请求），两种都处理：
         *   1) 以 "SFCI" 开头 → CMIF 事务，回 SFCO + 域头
         *   2) 以可打印 ASCII 开头（如 "fsp-srv"）→ GetService，
         *      回一个移动句柄并登记；该句柄后续的请求一律成功返回。
         * sm: service-manager HLE covering both CMIF transactions and inline
         * GetService name requests. */
        auto* t=ot2_guest_context_current();
        if(t && t->guest_tlr) {
            uint8_t* b=reinterpret_cast<uint8_t*>(t->guest_tlr);
            if(svc_n<220) {
                uint32_t words[12]{};
                memcpy(words,b,sizeof(words));
                log("IPC_REQUEST thread=%#llx handle=%#llx hdr=%08x/%08x/%08x/%08x body=%08x/%08x/%08x/%08x tail=%08x/%08x/%08x/%08x",
                    (unsigned long long)t->handle,(unsigned long long)r[0],
                    words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7],words[8],words[9],words[10],words[11]);
            }
            uint8_t* m=b+0x20;                       /* 请求体固定落在 TLR+0x20 */
            const uint32_t* q8=reinterpret_cast<const uint32_t*>(m);
            const uint32_t* q=reinterpret_cast<const uint32_t*>(b);
            const uint32_t message_type=q[0]&0xffffu;
            if(message_type==5u || message_type==7u) {
                /* CMIF control requests use an aligned header at +0x10. */
                const uint32_t command=q[6];
                if(command!=0u && command!=3u) {
                    log("UNHANDLED_IPC_CONTROL handle=%#llx command=%u",(unsigned long long)r[0],command);
                    r[0]=0xe401; return;
                }
                uint32_t* w=reinterpret_cast<uint32_t*>(b);
                w[0]=0u; w[1]=9u; w[2]=0u; w[3]=0u;
                w[4]=0x4F434653u; w[5]=0u; w[6]=0u; w[7]=0u;
                w[8]=command==0u ? 1u : 0u; /* domain object ID / pointer-buffer size */
                log("IPC_CONTROL handle=%#llx command=%u value=%u",
                    (unsigned long long)r[0],command,w[8]);
                r[0]=0; return;
            }
            if(r[0]==0x800 && (message_type==4u || message_type==6u)) {
                size_t data_offset=8;
                if(q[1]&0x80000000u) {
                    const uint32_t special=q[2];
                    data_offset+=4;
                    if(special&1u) data_offset+=8;
                    data_offset+=4*((special>>1)&0xfu);
                    data_offset+=4*((special>>5)&0xfu);
                }
                data_offset=(data_offset+15u)&~size_t(15u);
                if(data_offset+32u<=0x200u) {
                    uint32_t* domain=reinterpret_cast<uint32_t*>(b+data_offset);
                    uint32_t* cmif=domain+4;
                    if((domain[0]&0xffu)==1u && domain[1]==1u && cmif[0]==0x49434653u) {
                        const uint32_t command=cmif[2];
                        if(command!=1u && command!=1005u) {
                            log("UNHANDLED_FSP_DOMAIN command=%u",command);
                            r[0]=0xe401; return;
                        }
                        uint32_t* w=reinterpret_cast<uint32_t*>(b);
                        /* With no special header the data starts at +8, is aligned
                         * to +16, and the word count covers the aligned payload. */
                        w[0]=0u; w[1]=command==1005u ? 13u : 12u;
                        w[2]=0u; w[3]=0u; /* HIPC alignment padding */
                        w[4]=0u; w[5]=0u; w[6]=0u; w[7]=0u; /* no returned domain objects */
                        w[8]=0x4F434653u; w[9]=0u; w[10]=0u; w[11]=0u;
                        w[12]=0u; /* GetGlobalAccessLogMode: disabled */
                        log("FSP_DOMAIN command=%u",command);
                        r[0]=0; return;
                    }
                }
                log("UNHANDLED_FSP_REQUEST type=%u offset=%zu",message_type,data_offset);
                r[0]=0xe401; return;
            }
            const bool is_cmif = q8[0]==0x49434653u;
            bool is_name=false; char name[9]={0};
            if(!is_cmif) {
                for(int i=0;i<8;i++){ uint8_t ch=m[i];
                    if(!ch) break;
                    if(ch<32||ch>126){ is_name=false; break; }
                    name[i]=char(ch); is_name=true; }
            }
            if(r[0]==0x200 && is_name) {
                static uint32_t next_handle=0x800;
                const uint32_t h=next_handle++;
                log("SM_GETSERVICE name=%s handle=%#x",name,h);
                uint32_t* hw=reinterpret_cast<uint32_t*>(b);
                hw[0]=0u;                 /* tag = 0（响应） */
                hw[1]=4u|0x80000000u;     /* CMIF out(4), has_special=1 */
                hw[2]=0x20u;              /* special: num_move_handles=1 */
                hw[3]=h;                  /* 移动出来的会话句柄 */
                uint32_t* c=reinterpret_cast<uint32_t*>(b+0x10);
                c[0]=0x4F434653u; c[1]=0u; c[2]=0u; c[3]=0u;
            } else {
                uint32_t req_ver=0;
                const uint32_t* r30=reinterpret_cast<const uint32_t*>(b+0x30);
                if(q8[0]==0x49434653u) req_ver=q8[1];
                else if(r30[0]==0x49434653u) req_ver=r30[1];
                else if(q[0]==0x49434653u) req_ver=q[1];
                uint32_t* w=reinterpret_cast<uint32_t*>(b);
                w[0]=0u; w[1]=8u|0x80000000u;   /* tag=0, ndata=8, has_special=1 */
                w[2]=0u;                         /* special: 无句柄移动 */
                uint32_t* c=reinterpret_cast<uint32_t*>(b+0x10);
                c[0]=0x4F434653u; c[1]=req_ver; c[2]=0u; c[3]=0u;
                uint32_t* d=reinterpret_cast<uint32_t*>(b+0x20);
                d[0]=1u; d[1]=0u; d[2]=0u; d[3]=0u;
            }
            log("REPLY_CHECK tlr=%#llx w10=%08x w14=%08x w20=%08x",
                (unsigned long long)t->guest_tlr,
                *reinterpret_cast<const uint32_t*>(b+0x10),
                *reinterpret_cast<const uint32_t*>(b+0x14),
                *reinterpret_cast<const uint32_t*>(b+0x20));
        }
        r[0]=0; return;
    }
    if(id==0x22) { r[0]=0; return; }
    if(id==0x23) { r[0]=0; r[1]=0; return; }
    /* 0x36 = GetThreadLocalRegion：把本线程的 TLS 地址写回给调用方。
     * 0x36 = GetThreadLocalRegion: report this thread's TLS address. */
    if(id==0x36) {
        /* 0x36 = GetThreadLocalRegion。SDK 在 TLS+0x1F8 存一个指向线程控制块的指针，
         * 其 +0x1B0 是当前线程标识（用于判断「某个对象是不是当前线程」）。
         * 运行时没初始化它，会让该判断恒为真而触发断言，这里补上。
         * The SDK keeps a thread-control pointer at TLS+0x1F8 whose +0x1B0 is the
         * current thread id; without it the "is this the current thread" check
         * degenerates to true and an assert fires. */
        if(c->guest_tls) {
            uintptr_t tls=c->guest_tls;
            uintptr_t scratch=tls+0x800;
            uint64_t cur=0; memcpy(&cur,reinterpret_cast<const void*>(tls+0x1f8),8);
            if(cur!=scratch) {
                memcpy(reinterpret_cast<void*>(tls+0x1f8),&scratch,8);
                uint32_t tid=uint32_t(c->handle&0xffffffffu);
                memcpy(reinterpret_cast<void*>(scratch+0x1b0),&tid,4);
            }
        }
        r[0]=c->guest_tls; return;
    }
    if(id==0x26) {
        /* 把 detail 指向的内容打印出来：SDK 的断言信息常常就放在那里。
         * Dump the detail bytes: the SDK assertion payload usually lives there. */
        log("Guest Break reason=%llx detail=%llx size=%llx",r[0],r[1],r[2]);
        if(r[1] && r[2] && r[2] <= 256) {
            const uint8_t* d=reinterpret_cast<const uint8_t*>(r[1]);
            char hex[3*64+1]={0}; char ascii[65]={0};
            size_t n=r[2]>48?48:size_t(r[2]);
            for(size_t i=0;i<n;i++){snprintf(hex+i*3,4,"%02x ",d[i]);ascii[i]=(d[i]>=32&&d[i]<127)?char(d[i]):'.';}
            log("Guest Break detail hex=%s",hex);log("Guest Break detail ascii=%s",ascii);
        }
        /* 诊断：用 sdk 模块的宿主基址读失败单例的 vtable 与方法。 */
        { uintptr_t base=0, size=0;
          for(const auto& m:images) if(m.name=="sdk"){ base=reinterpret_cast<uintptr_t>(m.base); size=m.size; }
          auto gp=[&](uint64_t a)->uint64_t{ uint64_t v=0;
              if(!base||a+8>size) return 0;
              memcpy(&v,reinterpret_cast<const void*>(base+a),8); return v; };
          auto hp=[&](uint64_t a)->uint64_t{ uint64_t v=0; if(!a) return 0;
              memcpy(&v,reinterpret_cast<const void*>(a),8); return v; };
          uint64_t slot=gp(0xd030d8ull);
          uint64_t obj=hp(slot);
          uint64_t vt=obj?hp(obj):0;
          uint64_t m2=vt?hp(vt+0x20):0;   /* 空指针保护：vt=0 时原来会二次崩溃，盖掉真正的断言信息 */
          log("SINGLETON base=%#llx slot=%#llx obj=%#llx vtable=%#llx method=%#llx",
              (unsigned long long)base,slot,obj,vt,m2); }
        /* 诊断：dump TLS+0x1F8 及其 +0x1B0 字段。 */
        { uint64_t tls=c->guest_tls, ptr=0, field=0;
          if(tls) memcpy(&ptr,reinterpret_cast<const void*>(tls+0x1f8),8);
          if(ptr) memcpy(&field,reinterpret_cast<const void*>(ptr+0x1b0),4);
          log("TLS_STATE tls=%#llx ptr=%#llx field=%#x",(unsigned long long)tls,(unsigned long long)ptr,(unsigned)field); }
        /* 诊断：dump 栈顶原始字，把落在模块内的返回地址列出来（比帧指针链可靠）。 */
        { uintptr_t sp=uc->uc_mcontext.sp;
          log("STACK_RAW sp=%#llx",(unsigned long long)sp);
          for(unsigned i=0;i<48;i++){ uint64_t v=0;
              memcpy(&v,reinterpret_cast<const void*>(sp+i*8),8);
              for(const auto& m:images){ auto b=reinterpret_cast<uintptr_t>(m.base);
                  if(v>=b&&v<b+m.size){ log("  [sp+%#x] %s+0x%llx",i*8,m.name.c_str(),(unsigned long long)(v-b)); break; } }
          } }
        fail("Guest SDK abort",uc);
    }
    log("Unimplemented SVC 0x%x",id); fail("Required system call unavailable",uc);
}
}
extern "C" void ot2_host_log(const char* format, ...) {
    char text[2048];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);log("%s",text);
}
extern "C" void ot2_host_die(const char* format, ...) {
    char text[2048];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);fail(text);
}
extern "C" void guest_thread_init() { create_thread_context(); }
#ifdef OT2_EXECUTION_TESTING
extern "C" int ot2_test_memory_permission(uintptr_t address){std::lock_guard lock(mapping_mutex);return memory_permission_at(address);}
extern "C" uint64_t ot2_test_memory_permission_scans(){std::lock_guard lock(mapping_mutex);return memory_permission_scans;}
extern "C" bool ot2_test_memory_init(uint64_t* regions){
    if(address_space)return false;
    address_space=static_cast<uint8_t*>(mmap(nullptr,reserve_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(address_space==MAP_FAILED){address_space=nullptr;return false;}
    regions[0]=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
    regions[1]=reinterpret_cast<uintptr_t>(address_space)+stack_offset;
    regions[2]=reinterpret_cast<uintptr_t>(address_space)+alias_offset;
    regions[3]=alias_size;regions[4]=heap_capacity;regions[5]=stack_capacity;
    svc_thread_main_init();thread_system_ready=true;
    return true;
}
extern "C" bool ot2_test_memory_page_size(size_t page){
    if(page<static_cast<size_t>(sysconf(_SC_PAGESIZE))||page<4096||(page&(page-1)))return false;
    memory_host_page=page;return true;
}
extern "C" uintptr_t ot2_test_memory_module(){
    auto* raw=static_cast<uint8_t*>(mmap(nullptr,5*memory_host_page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(raw==MAP_FAILED)return 0;
    const auto base=(reinterpret_cast<uintptr_t>(raw)+memory_host_page-1)&~uintptr_t(memory_host_page-1);
    if(mprotect(reinterpret_cast<void*>(base),memory_host_page,PROT_READ|PROT_EXEC)||mprotect(reinterpret_cast<void*>(base+memory_host_page),memory_host_page,PROT_READ))return 0;
    memory_registry_changed();images.push_back({"memory-contract-module",reinterpret_cast<uint8_t*>(base),4*memory_host_page,0,memory_host_page,memory_host_page,2*memory_host_page});
    return base;
}
extern "C" void ot2_test_memory_svc(unsigned id,uint64_t* registers){
    ucontext_t context{};GuestThread thread{};
    memcpy(context.uc_mcontext.regs,registers,31*sizeof(uint64_t));
    svc(id,&context,&thread);
    memcpy(registers,context.uc_mcontext.regs,31*sizeof(uint64_t));
}
extern "C" bool ot2_test_context_fresh(){
    auto* c=create_thread_context();
    uintptr_t callback=0,reused=0;size_t size=0,reused_size=0;
    ot2_guest_context_callback_stack(c,callback,size);
    ot2_guest_context_callback_stack(c,reused,reused_size);
    if(!callback||callback!=reused||size!=1024*1024||size!=reused_size)return false;
    *reinterpret_cast<volatile uint8_t*>(callback)=1;
    *reinterpret_cast<volatile uint8_t*>(callback+size-1)=2;
    bool fresh=c->guest_tls!=0x12340000&&c->guest_tlr!=0x56780000;
    c->guest_tls=0x12340000;c->guest_tlr=0x56780000;
    return fresh;
}
extern "C" size_t ot2_test_context_live(){return ot2_guest_context_live();}
extern "C" size_t ot2_test_context_bytes(){return ot2_guest_context_bytes();}
extern "C" bool ot2_test_context_exit(){
    auto* main=create_thread_context();install_handlers();main->handle=svc_thread_main_init();thread_system_ready=true;
    auto* code=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(code==MAP_FAILED)return false;
    const uint32_t instructions[]={0x94000008,0xd4000141,0xd65f03c0},host_break=0xd420e060;
    memcpy(code,instructions,sizeof(instructions));memcpy(code+32,&host_break,4);memcpy(code+64,instructions+1,8);patch(code,4096);
    traps.push_back({reinterpret_cast<uintptr_t>(code+32),host_break,"context_exit_fixture",[](uint64_t* r)->uint64_t{
        auto* c=current();c->guest_tls=0x12340000;c->guest_tlr=0x56780000;
        if(r[0]){const uint64_t args[8]{};ot2_call_guest_address(r[0],args);}
        return 0;
    }});
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+4096));
    if(mprotect(code,4096,PROT_READ|PROT_EXEC))return false;
    const auto baseline=ot2_guest_context_live(),bytes=ot2_guest_context_bytes();
    for(unsigned i=0;i<400;i++){
        const size_t size=128*1024;auto* stack=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(stack==MAP_FAILED)return false;
        {std::lock_guard lock(mapping_mutex);memory_registry_changed();mappings.push_back({reinterpret_cast<uintptr_t>(stack),size,5,0});}
        uint64_t r[32]{};r[1]=reinterpret_cast<uintptr_t>(code);r[2]=(i&1)?reinterpret_cast<uintptr_t>(code+64):0;
        r[3]=reinterpret_cast<uintptr_t>(stack)+size;r[4]=44;svc_thread(0x08,r);if(r[0])return false;
        uint32_t handle=r[1];r[0]=handle;svc_thread(0x09,r);if(r[0])return false;
        r[1]=reinterpret_cast<uintptr_t>(&handle);r[2]=1;r[3]=2000000000;svc_thread(0x18,r);if(r[0])return false;
        // Completion must already be on the host stack; SDK is free to release this now.
        {std::lock_guard lock(mapping_mutex);memory_registry_changed();std::erase_if(mappings,[&](const Mapping& m){return m.base==reinterpret_cast<uintptr_t>(stack);});}
        munmap(stack,size);r[0]=handle;svc_thread(0x16,r);if(r[0])return false;
        for(unsigned retry=0;retry<10000 && ot2_guest_context_live()!=baseline;retry++)usleep(100);
        if(ot2_guest_context_live()!=baseline||ot2_guest_context_bytes()!=bytes)return false;
    }
    std::erase_if(traps,[&](const Trap& t){return t.pc>=reinterpret_cast<uintptr_t>(code)&&t.pc<reinterpret_cast<uintptr_t>(code+4096);});munmap(code,4096);return true;
}
#endif
extern "C" void crash_thread_init() {}
uintptr_t callback_stack(GuestThread* c){uintptr_t sp;asm volatile("mov %0,sp":"=r"(sp));
    if(sp>=c->stack&&sp<c->stack+c->stack_size)return (sp-1024)&~uintptr_t(15);
    if(service_callback_stack)return service_callback_stack;
    return active_signal?(active_signal->uc_mcontext.sp-256)&~uintptr_t(15):0;
}
extern "C" uint64_t ot2_call_on_host_stack(uint64_t (*function)(uint64_t*),uint64_t* args){
    auto* c=current();if(!c||!c->host_frame)fail("Host service outside guest call");
    struct Invocation {uint64_t (*function)(uint64_t*);uint64_t* args;uintptr_t callback;};
    Invocation invocation{function,args,callback_stack(c)};
    auto invoke=+[](uint64_t* raw)->uint64_t{
        auto& call=*reinterpret_cast<Invocation*>(raw);auto* c=current();
        const auto previous=service_callback_stack,base=c->stack,size=c->stack_size;
        if(host_service_depth==0){
            ot2_guest_context_callback_stack(c,c->stack,c->stack_size);
            service_callback_stack=c->stack+c->stack_size;
        }else service_callback_stack=call.callback;
        ++host_service_depth;auto result=call.function(call.args);--host_service_depth;
        c->stack=base;c->stack_size=size;service_callback_stack=previous;return result;
    };
    // host_frame is the suspended ot2_guest_call frame. A nested callback
    // establishes a new frame below the current native service automatically.
    return ot2_host_stack_call(invoke,reinterpret_cast<uint64_t*>(&invocation),(c->host_frame-1024)&~uintptr_t(15));
}
extern "C" uint64_t ot2_call_guest(const char* name,const uint64_t* args) {
    auto* c=current();if(!c || !active_game) fail("Guest callback outside active game");
    auto entry=active_game->symbol(name);if(!entry) fail("Guest callback export absent");
    uintptr_t top=callback_stack(c);
    if(top && top<c->stack+4096) fail("Nested guest callback stack exhausted");
    return ot2_guest_call(entry,c,args,top);
}
extern "C" uint64_t ot2_guest_symbol_address(const char* name) {
    if(!active_game)fail("Guest symbol outside active game");
    return active_game->symbol(name);
}
extern "C" uint64_t ot2_call_guest_address(uint64_t address,const uint64_t* args) {
    auto* c=current();if(!c || !address)fail("Invalid guest callback");
    uintptr_t top=callback_stack(c);
    if(top && top<c->stack+4096)fail("Nested guest callback stack exhausted");
    return ot2_guest_call(address,c,args,top);
}
extern "C" void* ot2_native_gate(const char* name,void* function) {
    std::lock_guard lock(native_gate_mutex);
    unsigned count=native_gate_count.load();
    for(unsigned i=0;i<count;i++)if(!strcmp(native_gates[i].name,name))return native_gate_code+i*32;
    if(count>=2048 || strlen(name)>=sizeof(native_gates[0].name))fail("Native gate capacity exceeded");
    if(!native_gate_code) {
        native_gate_code=static_cast<uint8_t*>(mmap(nullptr,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(native_gate_code==MAP_FAILED)fail("Native gate allocation failed");
        for(unsigned i=0;i<2048;i++){
            auto* stub=native_gate_code+i*32;uint32_t code[]={0x52800010U|(i<<5),0x58000071,0xd61f0220,0xd503201f};
            memcpy(stub,code,sizeof(code));auto target=reinterpret_cast<uintptr_t>(ot2_native_entry);memcpy(stub+16,&target,8);
        }
        __builtin___clear_cache(reinterpret_cast<char*>(native_gate_code),reinterpret_cast<char*>(native_gate_code+65536));
        if(mprotect(native_gate_code,65536,PROT_READ|PROT_EXEC))fail("Native gate protection failed");
    }
    strcpy(native_gates[count].name,name);native_gates[count].function=function;native_gates[count].present=!strcmp(name,"nvnQueuePresentTexture");
    __atomic_store_n(&ot2_native_abis[count],native_register_abi(name),__ATOMIC_RELEASE);
    native_gate_count.store(count+1,std::memory_order_release);
    return native_gate_code+count*32;
}
extern "C" void ot2_gate_ring_log(void){
    const unsigned pos=gate_ring_pos.load(std::memory_order_relaxed);
    const unsigned count=std::min<unsigned>(pos,gate_ring_capacity);
    const unsigned total=native_gate_count.load(std::memory_order_acquire);
    log("GATE_RING count=%u pos=%u",count,pos);
    for(unsigned i=0;i<count;i++){
        const unsigned index=gate_ring[(pos-count+i)%gate_ring_capacity].load(std::memory_order_relaxed);
        log("  GATE %s",index<total?native_gates[index].name:"?");
    }
    const unsigned spos=service_pos.load(std::memory_order_relaxed);
    const unsigned scount=std::min<unsigned>(spos,service_ring_capacity);
    log("SERVICE_RING count=%u pos=%u",scount,spos);
    for(unsigned i=0;i<scount;i++){
        const char* name=service_ring[(spos-scount+i)%service_ring_capacity];
        log("  SERVICE %s",name?name:"?");
    }
}
extern "C" void* ot2_native_function(unsigned index){
    if(index>=native_gate_count.load(std::memory_order_acquire))fail("Native gate index invalid");
    auto& gate=native_gates[index];
    /* 环形里跳过 NVN 图形调用：每帧上万条会把真正要看的系统服务（hid/oe/fs/applet）冲掉。 */
    /* Skip NVN graphics calls in the ring: tens of thousands per frame would flush out the system services (hid/oe/fs/applet) we want to see. */
    if(strncmp(gate.name,"nvn",3)!=0)
        gate_ring[gate_ring_pos.fetch_add(1,std::memory_order_relaxed)%gate_ring_capacity].store(index,std::memory_order_relaxed);
    if(__atomic_fetch_add(&gate.hits,1,__ATOMIC_RELAXED)==0)log("NATIVE %s",gate.name);
    if(gate.present)update_frame_rate();
    if(!gate.function){log("Missing native API: %s",gate.name);fail("Required native API unavailable");}return gate.function;
}
extern "C" void dm_thread_enter(uint64_t entry,uint64_t argument,uint64_t stack_top) {
    auto* c=current();
    if(!c) fail("Worker thread has no execution context");
    bool found=false;
    {
        std::lock_guard lock(mapping_mutex);
        for(const auto& m:mappings) if(stack_top>m.base && stack_top<=m.base+m.size) {
            ot2_guest_context_use_stack(c,m.base,stack_top-m.base);found=true;break;
        }
    }
    if(!found) fail("Worker stack has not been mapped");
    const uint64_t args[]={argument,0,0,0,0,0,0,0};
    ot2_guest_call(entry,c,args,0);
    fail("Worker returned without ExitThread");
}
extern "C" void ot2_signal_handler(int sig,siginfo_t* info,void* raw) {
    auto* uc=static_cast<ucontext_t*>(raw);
    if(sig==SIGPROF){
        const uintptr_t pc=uc->uc_mcontext.pc;
        bool in_module=false;
        for(const auto& m:images){ auto b=reinterpret_cast<uintptr_t>(m.base);
            if(pc>=b&&pc<b+m.size){ in_module=true; break; } }
        /* 只有在 guest 代码里采样时才记录 sp：否则采到的是宿主栈。 */
        ot2_thread_record_sample(pc);
        if(in_module) ot2_thread_record_sample_sp(uc->uc_mcontext.sp);
        return; }
    if(sig==SIGSEGV && (info->si_code==SEGV_ACCERR||info->si_code==SEGV_MAPERR)){
        // Linux exposes the ARM64 data-abort ESR in the signal context. Only
        // write faults may be handled by dirty tracking; never swallow execute
        // faults or unrelated guest memory errors.
        auto* records=uc->uc_mcontext.__reserved;size_t offset=0;
        while(offset+sizeof(_aarch64_ctx)<=sizeof(uc->uc_mcontext.__reserved)){
            auto* record=reinterpret_cast<const _aarch64_ctx*>(records+offset);
            if(record->size<sizeof(*record)||record->size>sizeof(uc->uc_mcontext.__reserved)-offset)break;
            if(record->magic==ESR_MAGIC&&record->size>=sizeof(esr_context)){
                const auto esr=reinterpret_cast<const esr_context*>(record)->esr;
                int saved_errno=errno;
                if(dirty_write_fault_candidate(sig,info->si_code,esr)&&nvn_dirty_fault(info->si_addr)){errno=saved_errno;return;}
                break;
            }
            offset+=record->size;
        }
    }
    auto* c=current();
    if(sig!=SIGTRAP) {
        log("Signal %d code=%d address=%p tracked=%d",sig,info->si_code,info->si_addr,nvn_dirty_tracked(info->si_addr,1));
        size_t offset=0;while(offset+sizeof(_aarch64_ctx)<=sizeof(uc->uc_mcontext.__reserved)){
            auto* record=reinterpret_cast<const _aarch64_ctx*>(uc->uc_mcontext.__reserved+offset);
            if(record->size<sizeof(*record)||record->size>sizeof(uc->uc_mcontext.__reserved)-offset)break;
            if(record->magic==ESR_MAGIC&&record->size>=sizeof(esr_context))log("FAULT_ESR value=%llx",(unsigned long long)reinterpret_cast<const esr_context*>(record)->esr);
            offset+=record->size;
        }
        fail(c?"Guest fault":"Signal outside guest thread",uc);
    }
    if(!c) fail("Signal outside guest thread",uc);
    Trap* found=nullptr;
    static std::array<std::atomic<uint32_t>,32768> trap_lookup{};
    auto& cached=trap_lookup[(uc->uc_mcontext.pc>>2)&(trap_lookup.size()-1)];const auto index=cached.load(std::memory_order_relaxed);
    if(index&&index<=traps.size()&&traps[index-1].pc==uc->uc_mcontext.pc)found=&traps[index-1];
    else for(size_t i=0;i<traps.size();++i)if(traps[i].pc==uc->uc_mcontext.pc){found=&traps[i];cached.store(i+1,std::memory_order_relaxed);break;}
    if(!found) fail("Unregistered breakpoint",uc);
    if(found->hook) {
        sdk_signal_count.fetch_add(1,std::memory_order_relaxed);
        /* 记录最近命中的宿主服务（跳过 nvn*，那是图形门）：看卡住时游戏还在轮询哪些 SDK 服务。 */
        /* Record recently hit host services (skip nvn*, those are graphics gates): shows which SDK services the game still polls while stalled. */
        if(strncmp(found->hook,"nvn",3)!=0)
            service_ring[service_pos.fetch_add(1,std::memory_order_relaxed)%service_ring_capacity]=found->hook;
        if(__atomic_fetch_add(&found->hits,1,__ATOMIC_RELAXED)==0) log("HOST %s",found->hook);
        if(found->call) {
            auto* previous=active_signal;active_signal=uc;
            uc->uc_mcontext.regs[0]=found->call(reinterpret_cast<uint64_t*>(uc->uc_mcontext.regs));
            active_signal=previous;
        }
        uc->uc_mcontext.pc=uc->uc_mcontext.regs[30];
        return;
    }
    auto op=found->instruction;auto target=op&31;auto kind=op&~31U;
    if(kind==0xd53bd040||kind==0xd53bd060||kind==0xd51bd040)tls_signal_count.fetch_add(1,std::memory_order_relaxed);
    auto* r=uc->uc_mcontext.regs;
    if((op&0xffe0001f)==0xd4000001) {
        svc_signal_count.fetch_add(1,std::memory_order_relaxed);
        int captured=0;
        if(thread_system_ready) {
            GuestThreadContextImage snapshot{};
            if(capture_guest_thread_context(uc,c,snapshot))
                captured=svc_thread_capture_context(&snapshot,sizeof(snapshot));
        }
        svc((op>>5)&0xffff,uc,c);
        if(captured)svc_thread_release_context();
    }
    else if(kind==0xd51bd040) { c->guest_tls=target==31?0:r[target]; ot2_tls_ensure_control(c); }
    else if(target!=31) {
        switch(kind) {
        case 0xd53bd040:r[target]=c->guest_tls;break;
        case 0xd53bd060:r[target]=c->guest_tlr;break;
        case 0xd53be020:counter_signal_count.fetch_add(1,std::memory_order_relaxed);r[target]=ticks();break;
        case 0xd53be000:counter_signal_count.fetch_add(1,std::memory_order_relaxed);r[target]=19200000;break;
        default:fail("Unknown patched instruction",uc);
        }
    }
    uc->uc_mcontext.pc+=4;
}
namespace ot2 {
void freeze_guest_clock(unsigned reason,bool on){guest_clock.freeze(reason,on,monotonic_ns);}
void set_frame_rate_target(int fps){requested_frame_rate.store(fps==60?60:30,std::memory_order_relaxed);}
int frame_rate_applied(){return applied_frame_rate.load(std::memory_order_relaxed);}
static double host_abi_fixture(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,double j,double k) {
    return a+b+c+d+e+f+g+h+i+j+k;
}
std::string execution_probe() {
    std::lock_guard lock(setup_mutex);
    auto* c=create_thread_context();install_handlers();
    // Deliberately overwrite guest x18; cross TLS and SVC gates, then return.
    uint32_t instructions[]={0xd280f692,0xd53bd040,0xd40003c1,0xaa1203e0,0xd65f03c0};
    auto* code=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(code==MAP_FAILED) throw std::runtime_error("Execution probe allocation failed");
    memcpy(code,instructions,sizeof(instructions));patch(code,sizeof(instructions));
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+sizeof(instructions)));
    if(mprotect(code,4096,PROT_READ|PROT_EXEC)) throw std::runtime_error("Execution probe mprotect failed");
    const uint64_t args[8]{};const auto value=ot2_guest_call(reinterpret_cast<uintptr_t>(code),c,args,0);
    if(value!=1972) throw std::runtime_error("Guest x18 did not survive host trap");
    auto* nested=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(nested==MAP_FAILED) throw std::runtime_error("Nested probe allocation failed");
    const uint32_t outer[]={0xa9bf7bfd,0xd280f692,0x9400000e,0x8b120000,0xa8c17bfd,0xd65f03c0};
    std::vector<uint32_t> inner{0xd11003ff,0xa9007bfd};
    for(unsigned offset=16;offset<1024;offset+=8)inner.push_back(0xf90003ff|((offset/8)<<10));
    for(uint32_t instruction:{0xd280f6b2U,0xd40003c1U,0xaa1203e0U,0xa9407bfdU,0x911003ffU,0xd65f03c0U})inner.push_back(instruction);
    const uint32_t host_break=0xd420e060;
    memcpy(nested,outer,sizeof(outer));memcpy(nested+64,&host_break,4);memcpy(nested+128,inner.data(),inner.size()*4);
    patch(nested+128,inner.size()*4);
    traps.push_back({reinterpret_cast<uintptr_t>(nested+64),host_break,"nested_execution_fixture",[](uint64_t* r)->uint64_t{
        volatile uint64_t sentinels[16];for(unsigned i=0;i<16;++i)sentinels[i]=0x5a5a0000+i;
        const uint64_t arguments[8]{};auto* thread=current();
        const auto top=(active_signal->uc_mcontext.sp-256)&~uintptr_t(15);
        auto result=ot2_guest_call(r[0],thread,arguments,top);
        for(unsigned i=0;i<16;++i)if(sentinels[i]!=0x5a5a0000+i)fail("Nested signal overwrote host frame");
        return result;
    }});
    __builtin___clear_cache(reinterpret_cast<char*>(nested),reinterpret_cast<char*>(nested+4096));
    if(mprotect(nested,4096,PROT_READ|PROT_EXEC)) throw std::runtime_error("Nested probe mprotect failed");
    const uint64_t nested_args[]={reinterpret_cast<uintptr_t>(nested+128),0,0,0,0,0,0,0};
    const auto nested_value=ot2_guest_call(reinterpret_cast<uintptr_t>(nested),c,nested_args,0);
    if(nested_value!=3945) throw std::runtime_error("Nested guest register restoration failed");
    auto* direct=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(direct==MAP_FAILED)throw std::runtime_error("Direct callback probe allocation failed");
    memcpy(direct,outer,sizeof(outer));const uint32_t jump[]={0x58000050,0xd61f0200};memcpy(direct+64,jump,sizeof(jump));
    auto callback=+[](uint64_t entry)->uint64_t{volatile uint64_t guard[16];for(unsigned i=0;i<16;i++)guard[i]=0x12340000+i;const uint64_t arguments[8]{};auto value=ot2_call_guest_address(entry,arguments);for(unsigned i=0;i<16;i++)if(guard[i]!=0x12340000+i)fail("Direct callback overwrote native stack");return value;};
    auto gate=reinterpret_cast<uintptr_t>(ot2_native_gate("direct_nested_fixture",reinterpret_cast<void*>(callback)));memcpy(direct+72,&gate,8);
    __builtin___clear_cache(reinterpret_cast<char*>(direct),reinterpret_cast<char*>(direct+4096));if(mprotect(direct,4096,PROT_READ|PROT_EXEC))throw std::runtime_error("Direct callback probe protection failed");
    const auto direct_value=ot2_guest_call(reinterpret_cast<uintptr_t>(direct),c,nested_args,0);if(direct_value!=3945)throw std::runtime_error("Direct native callback register restoration failed");
    const uint64_t abi_args[]={reinterpret_cast<uint64_t>(ot2_native_gate("abi_fixture",reinterpret_cast<void*>(host_abi_fixture))),0,0,0,0,0,0,0};
    auto abi_value=ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_abi_fixture),c,abi_args,0);
    if(abi_value!=49)throw std::runtime_error("Native floating-point/stack ABI failed");
    struct {alignas(16) uint8_t view[16];uint64_t guards[2];} view_fixture{};
    view_fixture.guards[0]=0x12345678abcd0001;view_fixture.guards[1]=0x12345678abcd0002;
    const uint64_t view_args[]={reinterpret_cast<uint64_t>(view_fixture.view),3,2,0,0,0,0,0};
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureViewSetDefaults")),c,view_args,0);
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureViewSetLevels")),c,view_args,0);
    if(view_fixture.guards[0]!=0x12345678abcd0001 || view_fixture.guards[1]!=0x12345678abcd0002)throw std::runtime_error("NVN view exceeds game ABI size");
    struct {alignas(16) uint8_t texture[128];tex_builder_t builder;} texture_fixture{};
    texture_fixture.builder.d.target=NVN_TEXTURE_TARGET_BUFFER;texture_fixture.builder.d.format=NVN_FORMAT_R32UI;
    texture_fixture.builder.d.width=32;texture_fixture.builder.d.height=texture_fixture.builder.d.depth=texture_fixture.builder.d.levels=1;
    const auto saved_builder=texture_fixture.builder;
    const uint64_t texture_args[]={reinterpret_cast<uint64_t>(texture_fixture.texture),reinterpret_cast<uint64_t>(&texture_fixture.builder),0,0,0,0,0,0};
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureInitialize")),c,texture_args,0);
    if(memcmp(&saved_builder,&texture_fixture.builder,sizeof(saved_builder)))throw std::runtime_error("NVN texture writes beyond 128-byte game object");
    alignas(16) std::array<uint8_t,256> source_memory{},copy_memory{};cmd_t source_cmd{},copy_cmd{};
    auto call_nvn=[&](const char* name,std::initializer_list<uint64_t> values){uint64_t a[8]{};std::copy(values.begin(),values.end(),a);return ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup(name)),c,a,0);};
    for(auto item:{std::pair{&source_cmd,source_memory.data()},std::pair{&copy_cmd,copy_memory.data()}}){
        call_nvn("nvnCommandBufferInitialize",{reinterpret_cast<uint64_t>(item.first),0});
        pool_t pool{MAGIC('P','O','O','L'),0,item.second,256};
        call_nvn("nvnCommandBufferAddCommandMemory",{reinterpret_cast<uint64_t>(item.first),reinterpret_cast<uint64_t>(&pool),0,256});
        call_nvn("nvnCommandBufferBeginRecording",{reinterpret_cast<uint64_t>(item.first)});
    }
    call_nvn("nvnCommandBufferSetCopyRowStride",{reinterpret_cast<uint64_t>(&source_cmd),1234});
    uint64_t handle=call_nvn("nvnCommandBufferEndRecording",{reinterpret_cast<uint64_t>(&source_cmd)});
    call_nvn("nvnCommandBufferCopyCommands",{reinterpret_cast<uint64_t>(&copy_cmd),1,reinterpret_cast<uint64_t>(&handle)});
    call_nvn("nvnCommandBufferEndRecording",{reinterpret_cast<uint64_t>(&copy_cmd)});
    source_memory.fill(0xdd);op_hdr copied{};int64_t stride=0;memcpy(&copied,copy_memory.data()+8,8);memcpy(&stride,copy_memory.data()+16,8);
    if(copied.op!=OP_COPY_STRIDE || stride!=1234)throw std::runtime_error("NVN CopyCommands borrowed reusable source memory");
    alignas(16) std::array<uint8_t,64> keyboard;keyboard.fill(0xa5);uint64_t keyboard_args[]={reinterpret_cast<uint64_t>(keyboard.data()),0,0,0,0,0,0,0};
    bool keyboard_called=false;for(auto& service:input_services())if(!strcmp(service.name,"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE")){service.call(keyboard_args);keyboard_called=true;}
    if(!keyboard_called)throw std::runtime_error("Keyboard state service absent");
    for(size_t i=8;i<keyboard.size();i++)if(keyboard[i]!=(i<48?0:0xa5))throw std::runtime_error("Keyboard state ABI size mismatch");
    return "{\"direct_native_callback\":true,\"guest_x18\":"+std::to_string(value)+",\"tls_and_svc_traps\":true,\"nested_return\":"+std::to_string(nested_value)+",\"native_abi_return\":"+std::to_string(abi_value)+",\"nvn_view_16_bytes\":true,\"nvn_texture_128_bytes\":true,\"nvn_command_copy_owned\":true,\"keyboard_state_48_bytes\":true}";
}
extern "C" void ot2_guest_batch_profile(int enabled){
    ot2::set_io_prefetch(enabled!=0);__atomic_store_n(&fast_system_enabled,enabled!=0,__ATOMIC_RELEASE);
    ot2_host_log("SYSTEM_GATES enabled=%d calls=%llu",enabled,(unsigned long long)system_fast_calls.exchange(0));
}
extern "C" void ot2_guest_register_gates(int enabled){
    __atomic_store_n(&ot2_register_gates_enabled,enabled!=0,__ATOMIC_RELEASE);
}
extern "C" void ot2_guest_execution_profile(int enable_tls){
    log("EXECUTION_BOUNDARIES tls_fast=%u tls_traps=%llu svc_traps=%llu counter_traps=%llu sdk_traps=%llu",
        __atomic_load_n(&fast_tls_enabled,__ATOMIC_ACQUIRE),(unsigned long long)tls_signal_count.exchange(0),
        (unsigned long long)svc_signal_count.exchange(0),(unsigned long long)counter_signal_count.exchange(0),(unsigned long long)sdk_signal_count.exchange(0));
    __atomic_store_n(&fast_tls_enabled,enable_tls!=0,__ATOMIC_RELEASE);
}
extern "C" void ot2_guest_permission_profile(int enabled){
    std::lock_guard lock(mapping_mutex);
    log("MEMORY_PERMISSION enabled=%u scans=%llu hits=%llu mappings=%zu",memory_permission_cache_enabled,(unsigned long long)memory_permission_scans,(unsigned long long)memory_permission_hits,mappings.size());
    memory_permission_cache_enabled=enabled!=0;memory_permission_scans=memory_permission_hits=0;
}
void boot_game(const std::string& directory,const std::string& log_path,uint64_t program_id) {
    log_fd=open(log_path.c_str(),O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0600);
    log("Dedicated native host: ProgramId=%016llx",(unsigned long long)program_id);
    auto user_path=(std::filesystem::path(log_path).parent_path()/"userdata").string();
    std::filesystem::create_directories(user_path);
    vkhost_configure_package((std::filesystem::path(log_path).parent_path()/"shaders").c_str(),(std::filesystem::path(directory).parent_path().parent_path()/"shaders").c_str());
    configure_services((std::filesystem::path(directory).parent_path()/"RomFS").string(),user_path,program_id);
    auto* c=create_thread_context();install_handlers();
    address_space=static_cast<uint8_t*>(mmap(nullptr,reserve_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(address_space==MAP_FAILED) fail("Address space reserve failed");
    log("ENGINE adapter=%s",ot2::engine_name());
    log("PATCHES configured=%d list=%s",ot2::patch_list_configured()?1:0,ot2::patch_list_text());
    log("MAIN_INIT_BEGIN");c->handle=svc_thread_main_init();log("MAIN_INIT_DONE h=%#x",c->handle);thread_system_ready=true;
    LoadedGame game(directory);active_game=&game;memory_registry_changed();images=game.modules();
    // 引擎适配层：UE4 的帧率通过 CVar 运行库补丁实现（按构建 ID 精确匹配）；
    // 非 UE4 引擎没有这条路径，跳过并明确记录，避免“静默无效”。
    for(const auto& m:images)if(m.name=="main"){
        if(ot2::engine_kind()==ot2::Engine::ue4){
            frame_rate_patch.configure(reinterpret_cast<uintptr_t>(m.base),m.size,m.build_id);
            log("FRAMERATE_PATCH engine=ue4 build=%s",frame_rate_patch.supported()?"supported":"unmatched");
        }else{
            log("FRAMERATE_PATCH skipped engine=%s patch=none",ot2::engine_name());
        }
    }
    for(auto& m:images) { image_size+=m.size;patch(m.base+m.text_offset,m.text_size);log("Module %s base=%p size=%zu",m.name.c_str(),m.base,m.size); }
    install_service_initializers(game);
    game.make_executable();log("Patched %zu system boundaries; running ARM64 directly",traps.size());
    auto call=[&](const char* name,uint64_t arg0=0,uint64_t arg1=0) {
        auto address=game.symbol(name);if(!address) {log("Missing %s",name);fail("Required entry absent");}
        const uint64_t args[]={arg0,arg1,0,0,0,0,0,0};
        log("ENTER %s",name);auto result=ot2_guest_call(address,c,args,0);log("RETURN %s = %llx",name,(unsigned long long)result);
    };
    call("_init_libc0");call("nnosInitialize",c->handle,0);call("_init_libc1");
    call("nninitInitializeSdkModule");call("nninitStartup");call("_init_libc2");
    for(auto address:game.initializers()) {const uint64_t args[8]{};log("DT_INIT %p",reinterpret_cast<void*>(address));ot2_guest_call(address,c,args,0);}
    call("nnMain");
}
#ifdef OT2_EXECUTION_TESTING
#include "tests/guest_sdk_contract.inc"
#include "tests/register_contract.inc"
#include "tests/tls_contract.inc"
#include "tests/native_gate_contract.inc"
#include "tests/host_stack_contract.inc"
#endif
}
