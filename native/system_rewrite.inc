// Full-state host gates for elapsed-time reads and SDK service entry points.
// The tiny island never overwrites a second guest instruction. x16/x17/LR are
// saved before entering host code; every other register and FP state is saved
// by ot2_system_entry. Slow BRK sites remain available for negative controls.
alignas(4) uint32_t fast_system_enabled=1;
struct SystemService {const char* name;uint64_t (*call)(uint64_t*);};
std::vector<SystemService> system_services;
struct SystemSyscall {unsigned id;uintptr_t pc;};
std::vector<SystemSyscall> system_syscalls;
std::atomic<uint64_t> system_fast_calls{0};
extern "C" void ot2_system_entry();
void svc(unsigned,ucontext_t*,GuestThread*);
extern "C" void ot2_system_dispatch(uint64_t selector,uint64_t* registers){
    // Assembly stores its own return link separately at word 31.
    auto* saved=registers+100;
    registers[16]=saved[0];registers[17]=saved[1];registers[30]=saved[2];
    if(selector&0x8000){
        const auto& service=system_services.at(selector&0x7fff);
        /* 记录最近命中的宿主服务（跳过 nvn*）：卡住时游戏还在轮询哪些 SDK 服务。 */
        /* Record recently hit host services (skip nvn*): which SDK services the game still polls while stalled. */
        if(strncmp(service.name,"nvn",3)!=0)
            service_ring[service_pos.fetch_add(1,std::memory_order_relaxed)%service_ring_capacity]=service.name;
        if(service.call)registers[0]=service.call(registers);
    }
    else if(selector&0x4000){
        const auto& site=system_syscalls.at(selector&0x3fff);
        ucontext_t context{};memcpy(context.uc_mcontext.regs,registers,31*8);
        context.uc_mcontext.sp=reinterpret_cast<uintptr_t>(saved+4);context.uc_mcontext.pc=site.pc;
        int captured=0;
        if(thread_system_ready){
            GuestThreadContextImage snapshot{};
            memcpy(snapshot.gprs,registers,sizeof(snapshot.gprs));
            snapshot.fp=registers[29];snapshot.lr=saved[2];
            snapshot.sp=context.uc_mcontext.sp;snapshot.pc=site.pc+4;
            snapshot.psr=uint32_t(registers[32]);
            memcpy(snapshot.fpu_gprs,registers+36,sizeof(snapshot.fpu_gprs));
            snapshot.fpcr=uint32_t(registers[33]);snapshot.fpsr=uint32_t(registers[34]);
            snapshot.tpidr=current()->guest_tls;
            captured=svc_thread_capture_context(&snapshot,sizeof(snapshot));
        }
        auto* previous=active_signal;active_signal=&context;svc(site.id,&context,current());active_signal=previous;
        if(captured)svc_thread_release_context();
        memcpy(registers,context.uc_mcontext.regs,31*8);
    }
    else {const auto target=selector&31;if(target!=31)registers[target]=(selector&32)?19200000:ticks();}
    saved[0]=registers[16];saved[1]=registers[17];saved[2]=registers[30];
    system_fast_calls.fetch_add(1,std::memory_order_relaxed);
}
// 只记录、不打断的入口钩子：把一条 SP 相对的首指令搬到 island 里执行，
// 期间把 guest 的 x0（对象指针）解引用到 slot[0]（状态字）并给 slot[1] 计数。
// 只 clobber x16/x17（保存/恢复），不调用宿主函数，因此不会破坏 guest 寄存器。
// Log-only entry hook: move one SP-relative first instruction into an island, record
// the guest x0 object's state word into slot[0] and bump slot[1]. Only x16/x17 are
// clobbered (saved/restored); no host call, so guest registers stay intact.
bool patch_state_hook(uintptr_t pc,volatile uint32_t* slot,size_t first_island,uint32_t state_offset){
    uint32_t first;memcpy(&first,reinterpret_cast<void*>(pc),4);
    // 只接受 SP 相对、无 PC 依赖的首指令（str/stp 预索引或 sub sp,sp,#imm）。
    const uint32_t kind=first&0xffc00000u;
    if(kind!=0xf8000000u&&kind!=0xa9000000u&&kind!=0xa9800000u&&kind!=0xd1000000u)return false;
    constexpr size_t capacity=112;TlsIsland* island=nullptr;
    for(size_t i=first_island;i<tls_islands.size();++i){auto& c=tls_islands[i];if(c.bytes-c.used>=capacity&&tls_branch_reachable(pc,uintptr_t(c.memory)+c.used)&&tls_branch_reachable(uintptr_t(c.memory)+c.used+capacity,pc+4)){island=&c;break;}}
    if(!island){const size_t bytes=std::max<size_t>(16384,sysconf(_SC_PAGESIZE));auto* memory=tls_allocate_near(pc,bytes);if(!memory)return false;tls_islands.push_back({memory,bytes,0});island=&tls_islands.back();}
    auto* base=island->memory+island->used;
    std::vector<uint32_t> code;auto emit=[&](uint32_t word){code.push_back(word);return code.size()-1;};
    emit(0xa9be47f0);                 // stp x16,x17,[sp,#-32]!
    emit(0xf9000be0);                 // str x0,[sp,#16]
    auto literal_at=emit(0x58000010); // ldr x16,literal
    emit(0xb9400000u|((state_offset/4)<<10)|17); // ldr w17,[x0,#state_offset]
    emit(0xb9000211);                 // str w17,[x16]
    emit(0xb9400611);                 // ldr w17,[x16,#4]
    emit(0x11000631);                 // add w17,w17,#1
    emit(0xb9000611);                 // str w17,[x16,#4]
    emit(0xf9400be0);                 // ldr x0,[sp,#16]
    emit(0xa8c247f0);                 // ldp x16,x17,[sp],#32
    emit(first);                      // 被搬走的首指令 | the displaced first instruction
    auto back=emit(tls_branch(uintptr_t(base)+code.size()*4,pc+4));
    if(code.size()&1)emit(0xd503201f);
    auto literal=[&](uintptr_t value){auto at=code.size();emit(uint32_t(value));emit(uint32_t(value>>32));return at;};
    code[literal_at]|=(uint32_t(int32_t(literal(uintptr_t(slot)))-int32_t(literal_at))&0x7ffffu)<<5;
    if(code.size()*4>capacity)fail("State hook island capacity");
    memcpy(base,code.data(),code.size()*4);
    __builtin___clear_cache(reinterpret_cast<char*>(base),reinterpret_cast<char*>(base+code.size()*4));
    island->used+=(code.size()*4+15)&~size_t(15);
    auto branch=tls_branch(pc,uintptr_t(base));memcpy(reinterpret_cast<void*>(pc),&branch,4);
    (void)back;
    return true;
}
// 指针记录钩子：把 x0（协程 iterator）写到 slot[0..7]，slot[8] 计数。
// Pointer hook: store x0 (the coroutine iterator) at slot[0..7], count at slot[8].
bool patch_pointer_hook(uintptr_t pc,volatile void* slot,size_t first_island){
    uint32_t first;memcpy(&first,reinterpret_cast<void*>(pc),4);
    const uint32_t kind=first&0xffc00000u;
    const bool movz=(first&0xffe00000u)==0x52800000u;
    if(kind!=0xf8000000u&&kind!=0xa9000000u&&kind!=0xa9800000u&&kind!=0xd1000000u&&!movz)return false;
    constexpr size_t capacity=112;TlsIsland* island=nullptr;
    for(size_t i=first_island;i<tls_islands.size();++i){auto& c=tls_islands[i];if(c.bytes-c.used>=capacity&&tls_branch_reachable(pc,uintptr_t(c.memory)+c.used)&&tls_branch_reachable(uintptr_t(c.memory)+c.used+capacity,pc+4)){island=&c;break;}}
    if(!island){const size_t bytes=std::max<size_t>(16384,sysconf(_SC_PAGESIZE));auto* memory=tls_allocate_near(pc,bytes);if(!memory)return false;tls_islands.push_back({memory,bytes,0});island=&tls_islands.back();}
    auto* base=island->memory+island->used;
    std::vector<uint32_t> code;auto emit=[&](uint32_t word){code.push_back(word);return code.size()-1;};
    emit(0xa9be47f0);                 // stp x16,x17,[sp,#-32]!
    emit(0xf9000be0);                 // str x0,[sp,#16]
    auto literal_at=emit(0x58000010); // ldr x16,literal
    emit(0xf9000200);                 // str x0,[x16]
    emit(0xb9400a11);                 // ldr w17,[x16,#8]
    emit(0x11000631);                 // add w17,w17,#1
    emit(0xb9000a11);                 // str w17,[x16,#8]
    emit(0xf9400be0);                 // ldr x0,[sp,#16]
    emit(0xa8c247f0);                 // ldp x16,x17,[sp],#32
    emit(first);
    emit(tls_branch(uintptr_t(base)+code.size()*4,pc+4));
    if(code.size()&1)emit(0xd503201f);
    auto literal=[&](uintptr_t value){auto at=code.size();emit(uint32_t(value));emit(uint32_t(value>>32));return at;};
    code[literal_at]|=(uint32_t(int32_t(literal(uintptr_t(slot)))-int32_t(literal_at))&0x7ffffu)<<5;
    if(code.size()*4>capacity)fail("Pointer hook island capacity");
    memcpy(base,code.data(),code.size()*4);
    __builtin___clear_cache(reinterpret_cast<char*>(base),reinterpret_cast<char*>(base+code.size()*4));
    island->used+=(code.size()*4+15)&~size_t(15);
    auto branch=tls_branch(pc,uintptr_t(base));memcpy(reinterpret_cast<void*>(pc),&branch,4);
    return true;
}
// 记录任意 x 寄存器的入口钩子（slot[0..7]=值，slot[8]=计数）。
// Entry hook that records an arbitrary x register (slot[0..7]=value, slot[8]=count).
bool patch_register_hook(uintptr_t pc,volatile void* slot,size_t first_island,unsigned reg){
    uint32_t first;memcpy(&first,reinterpret_cast<void*>(pc),4);
    const uint32_t kind=first&0xffc00000u;
    const bool movz=(first&0xffe00000u)==0x52800000u;
    if(kind!=0xf8000000u&&kind!=0xa9000000u&&kind!=0xa9800000u&&kind!=0xd1000000u&&kind!=0xf9400000u&&!movz)return false;
    constexpr size_t capacity=112;TlsIsland* island=nullptr;
    for(size_t i=first_island;i<tls_islands.size();++i){auto& c=tls_islands[i];if(c.bytes-c.used>=capacity&&tls_branch_reachable(pc,uintptr_t(c.memory)+c.used)&&tls_branch_reachable(uintptr_t(c.memory)+c.used+capacity,pc+4)){island=&c;break;}}
    if(!island){const size_t bytes=std::max<size_t>(16384,sysconf(_SC_PAGESIZE));auto* memory=tls_allocate_near(pc,bytes);if(!memory)return false;tls_islands.push_back({memory,bytes,0});island=&tls_islands.back();}
    auto* base=island->memory+island->used;
    std::vector<uint32_t> code;auto emit=[&](uint32_t word){code.push_back(word);return code.size()-1;};
    emit(0xa9be47f0);                 // stp x16,x17,[sp,#-32]!
    emit(0xf9000be0);                 // str x0,[sp,#16]
    auto literal_at=emit(0x58000010); // ldr x16,literal
    emit(0xf9000200u|(reg&31u));      // str x<reg>,[x16]
    emit(0xb9400a11);                 // ldr w17,[x16,#8]
    emit(0x11000631);                 // add w17,w17,#1
    emit(0xb9000a11);                 // str w17,[x16,#8]
    emit(0xf9400be0);                 // ldr x0,[sp,#16]
    emit(0xa8c247f0);                 // ldp x16,x17,[sp],#32
    emit(first);
    emit(tls_branch(uintptr_t(base)+code.size()*4,pc+4));
    if(code.size()&1)emit(0xd503201f);
    auto literal=[&](uintptr_t value){auto at=code.size();emit(uint32_t(value));emit(uint32_t(value>>32));return at;};
    code[literal_at]|=(uint32_t(int32_t(literal(uintptr_t(slot)))-int32_t(literal_at))&0x7ffffu)<<5;
    if(code.size()*4>capacity)fail("Register hook island capacity");
    memcpy(base,code.data(),code.size()*4);
    __builtin___clear_cache(reinterpret_cast<char*>(base),reinterpret_cast<char*>(base+code.size()*4));
    island->used+=(code.size()*4+15)&~size_t(15);
    auto branch=tls_branch(pc,uintptr_t(base));memcpy(reinterpret_cast<void*>(pc),&branch,4);
    return true;
}
bool patch_system(uintptr_t pc,uint32_t op,size_t first_island,const char* name=nullptr,uint64_t (*call)(uint64_t*)=nullptr){
    const auto kind=op&~31u;const bool service=name!=nullptr;const bool syscall=(op&0xffe0001f)==0xd4000001;
    if(!service&&kind!=0xd53be020&&kind!=0xd53be000&&!syscall)return false;
    if(syscall&&system_syscalls.size()>=0x4000)return false;
    if(service&&system_services.size()>=0x8000)return false;
    constexpr size_t capacity=112;TlsIsland* island=nullptr;
    for(size_t i=first_island;i<tls_islands.size();++i){auto& c=tls_islands[i];if(c.bytes-c.used>=capacity&&tls_branch_reachable(pc,uintptr_t(c.memory)+c.used)&&tls_branch_reachable(uintptr_t(c.memory)+c.used+capacity,pc+4)){island=&c;break;}}
    if(!island){const size_t bytes=std::max<size_t>(16384,sysconf(_SC_PAGESIZE));auto* memory=tls_allocate_near(pc,bytes);if(!memory)return false;tls_islands.push_back({memory,bytes,0});island=&tls_islands.back();}
    auto* base=island->memory+island->used;
    const uint32_t selector=service?0x8000|uint32_t(system_services.size()):op==0xd40003c1?0u:syscall?0x4000|uint32_t(system_syscalls.size()):(op&31)|(kind==0xd53be000?32u:0u);
    std::vector<uint32_t> code;auto emit=[&](uint32_t word){code.push_back(word);return code.size()-1;};
    auto relative=[&](size_t at,size_t to){code[at]|=(uint32_t(int32_t(to)-int32_t(at))&0x7ffffu)<<5;};
    auto restore=[&]{emit(0xf9400bfe);emit(0xa8c247f0);}; // ldr LR,[sp,16]; ldp x16,x17,[sp],32
    auto resume=[&]{if(service)emit(0xd65f03c0);else emit(tls_branch(uintptr_t(base)+code.size()*4,pc+4));};
    emit(0xa9be47f0);emit(0xf9000bfe); // stp x16,x17,[sp,-32]!; str LR,[sp,16]
    auto flag=emit(0x58000010);emit(0x88dffe11); // ldr x16,literal; ldar w17,[x16]
    auto disabled=emit(0x34000011);
    emit(0x52800010|(selector<<5));auto entry=emit(0x58000011);emit(0xd63f0220);
    restore();resume();relative(disabled,code.size());restore();
    auto trap=emit(0xd4200000|((service?0x703:0x702)<<5));resume();
    if(code.size()&1)emit(0xd503201f);
    auto literal=[&](uintptr_t value){auto at=code.size();emit(uint32_t(value));emit(uint32_t(value>>32));return at;};
    relative(flag,literal(uintptr_t(&fast_system_enabled)));relative(entry,literal(uintptr_t(ot2_system_entry)));
    if(code.size()*4>capacity)fail("System gate island capacity");
    if(service)system_services.push_back({name,call});
    if(syscall)system_syscalls.push_back({(op>>5)&0xffffu,pc});
    traps.push_back({uintptr_t(base)+trap*4,op,name,call});
    memcpy(base,code.data(),code.size()*4);island->used+=(code.size()*4+15)&~size_t(15);
    auto branch=tls_branch(pc,uintptr_t(base));memcpy(reinterpret_cast<void*>(pc),&branch,4);return true;
}
