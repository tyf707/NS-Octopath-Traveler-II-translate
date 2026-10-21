#include "guest_context.h"
#include <cstring>
extern "C" void ot2_host_log(const char* format, ...);
#include "thread_services.h"
#include <atomic>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" { __attribute__((visibility("hidden"))) GuestThread ot2_threads[GuestThreadCapacity]{}; }
namespace {
pthread_key_t key;
pthread_once_t once=PTHREAD_ONCE_INIT;
std::atomic<size_t> live{},mapped_bytes{};
struct Owned {
    GuestThread* context;
    void* tls{};size_t tls_size=16384;
    void* stack{};size_t stack_size{};
    void* signal{};size_t signal_size=2*1024*1024;
    void* callback{};size_t callback_size{};
};
void* allocate(size_t size,int protection){
    auto* memory=mmap(nullptr,size,protection,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory==MAP_FAILED)ot2_host_die("Guest thread mapping failed");
    mapped_bytes.fetch_add(size);return memory;
}
void release(void* memory,size_t size){if(memory){if(munmap(memory,size))ot2_host_die("Guest thread unmap failed");mapped_bytes.fetch_sub(size);}}
void destroy(void* value){
    auto* owned=static_cast<Owned*>(value);auto* c=owned->context;
    // ExitThread first switches to the original pthread stack. Never unmap a
    // signal/guest stack while pthread destructors are still executing on it.
    uintptr_t sp;asm volatile("mov %0,sp":"=r"(sp));
    auto inside=[&](void* p,size_t size){return p&&sp>=uintptr_t(p)&&sp<uintptr_t(p)+size;};
    if(inside(owned->stack,owned->stack_size)||inside(owned->signal,owned->signal_size)||inside(owned->callback,owned->callback_size))ot2_host_die("Guest context destroyed on an owned stack");
    stack_t disabled{};disabled.ss_flags=SS_DISABLE;if(sigaltstack(&disabled,nullptr))ot2_host_die("Guest signal stack disable failed");
    // Free immutable allocation addresses, never guest-modifiable TLS/TLR.
    release(owned->tls,owned->tls_size);release(owned->stack,owned->stack_size);release(owned->signal,owned->signal_size);
    release(owned->callback,owned->callback_size);
    c->host_x18=c->host_frame=c->guest_tls=c->guest_tlr=c->stack=c->stack_size=c->handle=0;
    __atomic_store_n(&c->host_tp,0,__ATOMIC_RELEASE);live.fetch_sub(1);delete owned;
}
void initialize(){if(pthread_key_create(&key,destroy))ot2_host_die("Guest context key creation failed");}
}
GuestThread* ot2_guest_context_current(){
    const auto tp=reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
    for(auto& c:ot2_threads)if(__atomic_load_n(&c.host_tp,__ATOMIC_ACQUIRE)==tp)return &c;
    return nullptr;
}
GuestThread* ot2_guest_context_create(){
    if(auto* current=ot2_guest_context_current())return current;
    pthread_once(&once,initialize);GuestThread* c=nullptr;unsigned index=0;
    for(;index<GuestThreadCapacity;++index){uintptr_t free=0;if(__atomic_compare_exchange_n(&ot2_threads[index].host_tp,&free,UINTPTR_MAX,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){c=&ot2_threads[index];break;}}
    if(!c)ot2_host_die("Guest live thread capacity exhausted");
    auto* owned=new Owned{c};owned->tls=allocate(owned->tls_size,PROT_READ|PROT_WRITE);
    /* Horizon 上 tpidrro_el0 指向的 TLS 与 svcGetThreadLocalRegion 返回的 TLR 是
     * 同一块 0x200 区域（消息缓冲区在 +0，disable_count 在 +0x100）。
     * 早期实现把它们分成两块，导致 SDK 读 TLS+0x1F8 永远是 0。
     * On Horizon the TLS at tpidrro_el0 and the TLR are the same 0x200 region;
     * splitting them made the SDK read a zeroed TLS+0x1F8. */
    c->guest_tls=reinterpret_cast<uintptr_t>(owned->tls);c->guest_tlr=c->guest_tls;
    /* SDK 在 TLS+0x1F8 存线程控制块指针，并读其 +0x1B0 作为线程标识（sdk+0x351890）。
     * TLS+0x200 之后是 guest 自己的 TLS 变量区，会被覆盖，所以单独分配一块。
     * The SDK keeps a thread control block pointer at TLS+0x1F8 and reads its
     * +0x1B0 as the thread id; the area past TLS+0x200 belongs to the guest's
     * own TLS variables, so allocate a dedicated block. */
    { void* ctl=allocate(0x400,PROT_READ|PROT_WRITE);
      *reinterpret_cast<uintptr_t*>(c->guest_tlr+0x1f8)=reinterpret_cast<uintptr_t>(ctl);
      uint32_t tid=uint32_t(0x1001u+index);
      std::memcpy(static_cast<uint8_t*>(ctl)+0x1b0,&tid,sizeof(tid)); }
    /* SDK 通过 tpidrro_el0（就是 guest_tls）读 TLS+0x1F8 的线程控制指针，
     * 其 +0x1B0 需是与 0 不同的线程标识，否则「是否是当前线程」会误判为真。
     * The SDK reads the thread-control pointer from TLS+0x1F8 (tpidrro_el0 is
     * guest_tls), so mirror it there with a non-zero thread id. */
    { uintptr_t tls=c->guest_tls, scratch=tls+0x200;
      *reinterpret_cast<uintptr_t*>(tls+0x1f8)=scratch;
      uint32_t tid=uint32_t(0x1001u+index);
      std::memcpy(reinterpret_cast<void*>(scratch+0x1b0),&tid,sizeof(tid));
      ot2_host_log("TLS_INIT tls=%p scratch=%p tid=%x",reinterpret_cast<void*>(tls),
                   reinterpret_cast<void*>(scratch),(unsigned)tid); }
    const size_t page=sysconf(_SC_PAGESIZE);c->stack_size=8*1024*1024;owned->stack_size=c->stack_size+2*page;
    owned->stack=allocate(owned->stack_size,PROT_NONE);c->stack=reinterpret_cast<uintptr_t>(owned->stack)+page;
    if(mprotect(reinterpret_cast<void*>(c->stack),c->stack_size,PROT_READ|PROT_WRITE))ot2_host_die("Guest stack protection failed");
    owned->signal=allocate(owned->signal_size,PROT_READ|PROT_WRITE);
    stack_t alternate{};alternate.ss_flags=static_cast<int>(1U<<31); // Linux SS_AUTODISARM, needed for nested guest traps.
    alternate.ss_sp=owned->signal;alternate.ss_size=owned->signal_size;
    if(sigaltstack(&alternate,nullptr))ot2_host_die("Guest signal stack setup failed");
    asm volatile("mov %0,x18":"=r"(c->host_x18));c->handle=0x1001+index;
    if(pthread_setspecific(key,owned))ot2_host_die("Guest context key assignment failed");
    live.fetch_add(1);
    __atomic_store_n(&c->host_tp,reinterpret_cast<uintptr_t>(__builtin_thread_pointer()),__ATOMIC_RELEASE);
    return c;
}
void ot2_guest_context_use_stack(GuestThread* c,uintptr_t base,size_t size){
    auto* owned=static_cast<Owned*>(pthread_getspecific(key));if(!owned||owned->context!=c)ot2_host_die("Guest stack assignment on wrong thread");
    release(owned->stack,owned->stack_size);owned->stack=nullptr;owned->stack_size=0;
    c->stack=base;c->stack_size=size; // SDK-owned mapping; the SDK releases it after thread completion.
}
void ot2_guest_context_callback_stack(GuestThread* c,uintptr_t& base,size_t& size){
    auto* owned=static_cast<Owned*>(pthread_getspecific(key));if(!owned||owned->context!=c)ot2_host_die("Callback stack on wrong thread");
    // Only services using the native-stack boundary allocate this. Reuse it
    // until thread exit, with guards on both ends and a fixed 1 MiB upper bound.
    const size_t page=sysconf(_SC_PAGESIZE);size=1024*1024;
    if(!owned->callback){
        owned->callback_size=size+2*page;owned->callback=allocate(owned->callback_size,PROT_NONE);
        if(mprotect(static_cast<uint8_t*>(owned->callback)+page,size,PROT_READ|PROT_WRITE))ot2_host_die("Callback stack protection failed");
    }
    base=uintptr_t(owned->callback)+page;
}
size_t ot2_guest_context_live(){return live.load();}
size_t ot2_guest_context_bytes(){return mapped_bytes.load();}
