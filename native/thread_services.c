// SPDX-License-Identifier: MIT
// Adapted from d2r-mobile dm/host/hle_thread.c, revision 685d3ddef8bf75768fd4c51228566f2e8d2a5182.
// Copyright (c) 2026 jhaizhou. See third_party/d2r-mobile-LICENSE.
// Android adaptation: remove Mach diagnostics/QoS and D2R profiling; enter through guest_entry.S.
#include "thread_services.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <string.h>
#define LOG ot2_host_log
#define die ot2_host_die
/* guest.cpp 里的最近宿主服务环形记录（C++ 侧实现，extern "C"）。 */
extern void ot2_gate_ring_log(void);
/* 协程 state 探针（guest.cpp 的 island 直接写）。 */
extern uint32_t ot2_coro_probe_state(void);
extern uint32_t ot2_coro_probe_calls(void);
extern uint32_t ot2_exc_probe_calls(void);
extern uint64_t ot2_coro_ptr(void);
extern uint32_t ot2_coro_ptr_calls(void);
extern uint64_t ot2_cb_reg(void);
extern uint32_t ot2_cb_reg_calls(void);
extern uint64_t ot2_ex_probe(void);
extern uint32_t ot2_ex_probe_calls(void);
extern uint32_t ot2_post_probe_calls(void);
extern uint32_t ot2_prefs_ex_calls(void);
extern uint64_t ot2_prefs_ex_ptr(void);
extern uint64_t ot2_nre_ptr(void);
extern uint32_t ot2_nre_calls(void);
static uint64_t now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static uint64_t guest_ns(void) { return now_ns(); }
static int dm_detclock_enabled(void) { return 0; }
#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_TRANSFER };
typedef struct transfer { void* source;uint64_t size;int fd; } transfer_t;
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr;
    uint8_t guest_context[0x320];
    int guest_context_valid, paused;
    volatile uintptr_t sample_pc;   // SIGPROF 采样：被打断时的 PC
    volatile uintptr_t sample_sp;   // SIGPROF 采样：被打断时的 SP
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    /* 最近 SVC 环形记录：看主线程在卡住时到底在循环做什么（id / 返回地址 / 参数）。 */
    /* Recent-SVC ring: shows what the main thread actually loops on while stalled (id / return address / args). */
    struct { uint64_t id, lr, sp, x0, x1; } svc_ring[16];
    unsigned svc_ring_pos;
    uint64_t wait_count;       // 0x1C 等待次数：一直涨说明在循环等，不是死锁 | 0x1C wait count: growing means it loops, not deadlocks
    uint64_t wait_x0, wait_x1; // 最近一次 0x1C 的锁地址与条件键 | lock address and condition key of the latest 0x1C
    uint32_t mport;         // 宿主线程端口（取 CPU 时间） | Host thread port (to get CPU time)
    uint32_t qos;
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_context_ready = PTHREAD_COND_INITIALIZER;
static uintptr_t g_G_holder;
#define LOCK_G() do { pthread_mutex_lock(&G); thr_t* _gt=(thr_t*)pthread_getspecific(g_keythr); __atomic_store_n(&g_G_holder,_gt?(uintptr_t)_gt->handle:0,__ATOMIC_RELEASE); } while(0)
#define UNLOCK_G() do { __atomic_store_n(&g_G_holder,0,__ATOMIC_RELEASE); pthread_mutex_unlock(&G); } while(0)
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static pthread_once_t g_key_once=PTHREAD_ONCE_INIT;
static void create_thread_key(void){if(pthread_key_create(&g_keythr,NULL))die("thread key creation");}
static void init_condition(pthread_cond_t* cv){pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);pthread_cond_init(cv,&attr);pthread_condattr_destroy(&attr);}
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}
int svc_thread_capture_context(const void* context,uint64_t size) {
    if (!context || size!=0x320) return 0;
    LOCK_G();
    thr_t *t=cur();
    int installed=!t->guest_context_valid;
    if(installed) {
        memcpy(t->guest_context,context,sizeof(t->guest_context)); t->guest_context_valid=1;
        if(t->paused)pthread_cond_broadcast(&g_context_ready);
    }
    UNLOCK_G();
    return installed;
}
void svc_thread_release_context(void) {
    LOCK_G();
    thr_t *t=cur();
    while(t->paused) pthread_cond_wait(&t->cv,&G);
    t->guest_context_valid=0;
    UNLOCK_G();
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_MONOTONIC, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = pthread_cond_wait(&t->cv, &G);
        } else rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取当前游戏拍；diag.c 初始化时登记（单测不登记，拍号记 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每线程 TSD 槽（门、x18、TLS、TLR）唯一设置点 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
static void *thread_main(void *p) {
    thr_t *t = p;
    LOG("THREAD_MAIN enter h=%#x", t->handle);
    pthread_setspecific(g_keythr, t);
    crash_thread_init();
    guest_thread_init();
    LOG("THREAD_MAIN init done h=%#x started=%d", t->handle, t->started);
    LOCK_G(); while (!t->started) pthread_cond_wait(&t->cv, &G); UNLOCK_G();
    LOG("THREAD_MAIN enter guest h=%#x entry=%#llx", t->handle, (unsigned long long)t->entry);
    dm_thread_enter(t->entry, t->arg, t->stack_top);   // 不返回：客体须经 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
void svc_thread_exit_on_host(void) {
    thr_t *t=cur();
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    LOCK_G();
    t->exited = 1;
    pthread_cond_broadcast(&g_context_ready);
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    UNLOCK_G();
    pthread_exit(NULL);
}
void dm_thread_returned(void) { die("客体线程入口函数返回（Horizon 要求 ExitThread）"); }

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
static void* watchdog_main(void* arg) {
    (void)arg;
    for (int i = 0; i < 180; i++) {
        struct timespec ts = {5, 0}; nanosleep(&ts, NULL);
        if (pthread_mutex_trylock(&G) != 0) {
            LOG("=== WATCHDOG %d G_BUSY holder=%#llx ===", i, (unsigned long long)__atomic_load_n(&g_G_holder,__ATOMIC_ACQUIRE));
            continue;
        }
        LOG("=== WATCHDOG %d ===", i);
        pthread_mutex_unlock(&G);
        /* 采一次 PC：ot2_thread_dump_state 会广播 SIGPROF 并打印 pc=。 */
        ot2_thread_dump_state();
        continue;
    }
    return NULL;
}
/* sp 所在可读区间的末端（按 /proc/self/maps）；0 = 不在可读映射里。
 * 直接按 256 字读可能越过栈映射末端撞上 PROT_NONE 保护页，在 watchdog 里
 * 触发 SIGSEGV（那不是客体线程，「Signal outside guest thread」会直接杀进程）。
 * End of the readable mapping containing sp (from /proc/self/maps); 0 = not in
 * a readable mapping. Reading a fixed 256 words can run past the end of a stack
 * mapping into a PROT_NONE guard page and SIGSEGV on the watchdog thread, which
 * kills the process ("Signal outside guest thread"). */
static uintptr_t readable_end(uintptr_t address) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512]; uintptr_t end = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long b = 0, e = 0; char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &b, &e, perms) != 3) continue;
        if (address >= (uintptr_t)b && address < (uintptr_t)e) {
            if (perms[0] == 'r') end = (uintptr_t)e;
            break;
        }
    }
    fclose(f);
    return end;
}
void ot2_thread_dump_state(void) {
    static int calls;
    /* Java 每帧也会调进来（getNativeFrameStats），按次数限流会被它耗尽；
     * 改为按时间限流：4 秒内只 dump 一次，watchdog 的 5 秒周期仍能轮到。 */
    /* Java also calls this every frame (getNativeFrameStats), so a count-based
     * limit gets exhausted by it; throttle by time instead: at most one dump per
     * 4 seconds, which still lets the 5-second watchdog cycle through. */
    static uint64_t last_ns;
    const uint64_t now = now_ns();
    if (last_ns && now - last_ns < 4000000000ULL) return;
    last_ns = now;
    calls++;
    if (pthread_mutex_trylock(&G) != 0) {
        LOG("=== STATE_DUMP %d G_BUSY holder=%#x ===", calls, (unsigned)__atomic_load_n(&g_G_holder,__ATOMIC_ACQUIRE));
        return;
    }
    for (thr_t *t = g_all; t; t = t->all) if (t->pt) pthread_kill(t->pt, SIGPROF);
    pthread_mutex_unlock(&G);
    usleep(8000);
    if (pthread_mutex_trylock(&G) != 0) { LOG("=== STATE_DUMP %d G_BUSY2 ===", calls); return; }
    LOG("=== STATE_DUMP %d ===", calls);
    /* 等锁线程的 waddr 附近内存：条件变量/互斥量结构里通常有 owner 线程标识。 */
    for (thr_t *t = g_all; t; t = t->all) if (t->pt && t->waddr && t->wkind) {
        const uintptr_t base = t->waddr - 0x20;
        LOG("WAITOBJ h=%#x waddr=%#llx", t->handle, (unsigned long long)t->waddr);
        const uintptr_t *w = (const uintptr_t *)base;
        for (unsigned i = 0; i < 8; i++) LOG("   [%+03x] %#llx", (int)(i * 8 - 0x20), (unsigned long long)w[i]);
    }
    for (thr_t *t = g_all; t; t = t->all)
        LOG("THR h=%#x id=%llu st=%d ex=%d wk=%d wkey=%#llx waddr=%#llx wtag=%#x svc=%#x lr=%#llx pc=%#llx n=%llu",
            t->handle, (unsigned long long)t->id, t->started, t->exited, t->wkind,
            (unsigned long long)t->wkey, (unsigned long long)t->waddr, t->wtag,
            t->last_svc, (unsigned long long)t->last_lr, (unsigned long long)t->sample_pc, (unsigned long long)t->nsvc);
    for (thr_t *t = g_all; t; t = t->all) if (t->pt && t->sample_sp) {
        const uintptr_t sp = t->sample_sp;
        const uintptr_t end = readable_end(sp);
        unsigned words = end > sp ? (unsigned)((end - sp) / sizeof(uintptr_t)) : 0;
        if (words > 256) words = 256;
        LOG("STACK h=%#x sp=%#llx words=%u", t->handle, (unsigned long long)sp, words);
        const uintptr_t *w = (const uintptr_t *)sp;
        for (unsigned i = 0; i < words; i++) LOG("   [%02u] %#llx", i, (unsigned long long)w[i]);
    }
    LOG("CORO_STATE state=%u calls=%u exc=%u ptr=%#llx ptrcalls=%u cbreg=%#llx cbregcalls=%u exist=%#llx excalls=%u postcalls=%u",
        ot2_coro_probe_state(), ot2_coro_probe_calls(), ot2_exc_probe_calls(),
        (unsigned long long)ot2_coro_ptr(), ot2_coro_ptr_calls(),
        (unsigned long long)ot2_cb_reg(), ot2_cb_reg_calls(),
        (unsigned long long)ot2_ex_probe(), ot2_ex_probe_calls(), ot2_post_probe_calls());
    LOG("EXCEPTIONS prefs=%u nre=%u nreptr=%#llx prefsptr=%#llx", ot2_prefs_ex_calls(), ot2_nre_calls(), (unsigned long long)ot2_nre_ptr(), (unsigned long long)ot2_prefs_ex_ptr());
    /* 主线程最近的 SVC 序列：id 循环 = 每帧固定系统调用；0x1c 计数的增长速度可区分「循环等」与「死等」。 */
    /* The main thread's latest SVC sequence: a repeating id pattern = fixed per-frame syscalls; the 0x1c count growth distinguishes "looping wait" from "dead wait". */
    for (thr_t *t = g_all; t; t = t->all) if (t->handle == 0x1001) {
        LOG("SVCRING h=%#x waits=%llu lastwait-x0=%#llx lastwait-x1=%#llx",
            t->handle, (unsigned long long)t->wait_count,
            (unsigned long long)t->wait_x0, (unsigned long long)t->wait_x1);
        const unsigned n = t->svc_ring_pos < 16 ? t->svc_ring_pos : 16;
        for (unsigned i = 0; i < n; i++) {
            const unsigned k = (t->svc_ring_pos - n + i) % 16;
            LOG("   SVC id=%#llx lr=%#llx sp=%#llx x0=%#llx x1=%#llx",
                (unsigned long long)t->svc_ring[k].id, (unsigned long long)t->svc_ring[k].lr,
                (unsigned long long)t->svc_ring[k].sp, (unsigned long long)t->svc_ring[k].x0,
                (unsigned long long)t->svc_ring[k].x1);
        }
    }
    ot2_gate_ring_log();
    pthread_mutex_unlock(&G);
}
void ot2_thread_record_sample(uintptr_t pc) {
    /* 早期（线程键尚未创建）也可能被调用：先确保键存在，否则
     * pthread_getspecific 拿到的是未初始化键，会读错槽位。 */
    /* May be called very early, before the thread key exists: create it first,
     * otherwise pthread_getspecific reads an uninitialised slot. */
    pthread_once(&g_key_once,create_thread_key);
    thr_t* t = (thr_t*)pthread_getspecific(g_keythr);
    if (t) t->sample_pc = pc;
}
void ot2_thread_record_sample_sp(uintptr_t sp) {
    /* SVC 0x1C（WaitProcessWideKeyAtomic）在极早期就可能出现，此时 TLS 键还未建立；
     * 必须先用 pthread_once 建键，否则启动即崩（已实测）。 */
    /* SVC 0x1C (WaitProcessWideKeyAtomic) can happen very early, before the
     * TLS key is created; create it with pthread_once first or startup crashes
     * (measured). */
    pthread_once(&g_key_once,create_thread_key);
    thr_t* t = (thr_t*)pthread_getspecific(g_keythr);
    if (t) t->sample_sp = sp;
}
/* 每条 SVC 记一条环形记录（只在宿主 svc() 里调一次，纯内存写）。 */
/* Record one ring entry per SVC (called once from the host svc(), memory writes only). */
void ot2_thread_record_svc(uint64_t id, uintptr_t sp, uintptr_t lr, uint64_t x0, uint64_t x1) {
    pthread_once(&g_key_once,create_thread_key);
    thr_t* t = (thr_t*)pthread_getspecific(g_keythr);
    if (!t) return;
    unsigned i = t->svc_ring_pos++ % 16;
    t->svc_ring[i].id = id; t->svc_ring[i].sp = sp; t->svc_ring[i].lr = lr;
    t->svc_ring[i].x0 = x0; t->svc_ring[i].x1 = x1;
    if (id == 0x1c) { t->wait_count++; t->wait_x0 = x0; t->wait_x1 = x1; }
}
uint32_t ot2_thread_handle_current(void) {
    pthread_once(&g_key_once,create_thread_key);
    thr_t* t=(thr_t*)pthread_getspecific(g_keythr);
    return t?t->handle:0;
}
uint32_t svc_thread_main_init(void) {
    static int watchdog_started;
    if (!watchdog_started) {
        watchdog_started = 1; pthread_t w;
        int wrc = pthread_create(&w, NULL, watchdog_main, NULL);
        if (!wrc) pthread_detach(w);
        LOG("WATCHDOG_START rc=%d", wrc);
    }
    pthread_once(&g_key_once,create_thread_key);
    if(pthread_getspecific(g_keythr))return cur()->handle;
    thr_t *t = calloc(1, sizeof *t);
    init_condition(&t->cv);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 1; t->pt = pthread_self();
    LOCK_G(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; UNLOCK_G();
    pthread_setspecific(g_keythr, t);
    return t->handle;
}

static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    LOCK_G();
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    UNLOCK_G();
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
int ot2_signal_event(uint32_t handle) {
    LOCK_G();
    event_t* event=h_peek(handle,O_EVENT_W);
    if(event){event->signaled=1;wake_sync_waiters();}
    UNLOCK_G();
    return event!=NULL;
}
int svc_thread(int num, uint64_t *regs) {
    { thr_t *me = pthread_getspecific(g_keythr); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->nsvc++; } }
    if (0 && num != 0x0B) { thr_t *me = pthread_getspecific(g_keythr); LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, regs[0], regs[1], regs[2], regs[3]); }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        init_condition(&t->cv);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 1;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        LOCK_G(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; UNLOCK_G();
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CREATE_THREAD h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle, t->entry, t->arg, t->stack_top, t->prio, t->core);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        LOCK_G();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        LOG("START_THREAD h=%#x found=%d", (uint32_t)regs[0], t ? 1 : 0);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        UNLOCK_G();
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: guest_thread_exit(); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else sched_yield();
        return 1;
    }
    case 0x0C: { LOCK_G();thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; UNLOCK_G();return 1; }
    case 0x0D: { LOCK_G();thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE; UNLOCK_G();return 1; }
    case 0x0E: { LOCK_G();thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; UNLOCK_G();return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        LOCK_G();thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2]; }
        regs[0] = t ? 0 : R_BAD_HANDLE; UNLOCK_G();return 1;
    }
    case 0x10: regs[0] = (uint64_t)(uint32_t)cur()->core; return 1;
    case 0x25: { LOCK_G();thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; UNLOCK_G();return 1; }
    case 0x32: {  // SetThreadActivity: wait for a truthful guest boundary snapshot.
        LOCK_G();
        const uint32_t handle=(uint32_t)regs[0];
        thr_t *t=h_get(handle,O_THREAD);
        uint32_t activity=(uint32_t)regs[1];
        if(!t) regs[0]=R_BAD_HANDLE;
        else if(activity>1 || t==cur() || t->exited) regs[0]=R_BAD_STATE;
        else {
            const uint64_t identity=t->id;
            t->paused=activity==1;
            if(!t->paused){pthread_cond_signal(&t->cv);pthread_cond_broadcast(&g_context_ready);}
            // The target may be in guest code or a hosted graphics call.
            // Queue the pause and let its next SVC publish a register image;
            // never turn this ordinary scheduling race into BadState.
            while(t && activity==1 && t->paused && !t->guest_context_valid && !t->exited){
                pthread_cond_wait(&g_context_ready,&G);
                t=h_peek(handle,O_THREAD);
                if(t && t->id!=identity)t=NULL;
            }
            regs[0]=!t?R_BAD_HANDLE:(t->exited || (activity==1&&!t->paused))?R_BAD_STATE:0;
        }
        UNLOCK_G();
        LOG("SET_THREAD_ACTIVITY handle=%#x activity=%u result=%#llx",handle,activity,regs[0]);
        return 1;
    }
    case 0x33: {  // GetThreadContext3: return only a real, paused guest SVC snapshot.
        LOCK_G();
        thr_t *t=h_get((uint32_t)regs[1],O_THREAD);
        if(!t) regs[0]=R_BAD_HANDLE;
        else if(!regs[0] || !t->paused || !t->guest_context_valid) regs[0]=R_BAD_STATE;
        else { memcpy((void*)regs[0],t->guest_context,sizeof(t->guest_context)); regs[0]=0; }
        UNLOCK_G();
        return 1;
    }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        LOCK_G();
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        UNLOCK_G();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: LOCK_G(); lock_handoff(regs[0]); UNLOCK_G(); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        LOCK_G();
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        UNLOCK_G();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        LOCK_G();
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        UNLOCK_G();
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        LOCK_G();
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        UNLOCK_G();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        LOCK_G();
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        UNLOCK_G();
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        LOCK_G(); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); UNLOCK_G();
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        LOCK_G();
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        UNLOCK_G();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        LOCK_G();
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        UNLOCK_G();
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        LOCK_G();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        UNLOCK_G();
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x15: { // Same-process transfer memory with real shared backing.
        if(!regs[2] || ((regs[1]|regs[2])&4095)){regs[0]=0xd401;return 1;}
        transfer_t* m=calloc(1,sizeof(*m));m->source=(void*)regs[1];m->size=regs[2];
        m->fd=(int)syscall(__NR_memfd_create,"ot2-transfer",1);
        if(m->fd<0 || ftruncate(m->fd,m->size))die("Transfer-memory backing failed");
        if(pwrite(m->fd,m->source,m->size,0)!=(ssize_t)m->size)die("Transfer-memory initialization failed");
        if(mmap(m->source,m->size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,m->fd,0)==MAP_FAILED)die("Transfer-memory source map failed");
        LOCK_G();regs[1]=h_new(O_TRANSFER,m);UNLOCK_G();regs[0]=0;return 1;
    }
    case 0x51: case 0x52: {
        LOCK_G();transfer_t* m=h_peek((uint32_t)regs[0],O_TRANSFER);
        if(!m || regs[2]!=m->size || (regs[1]&4095)){UNLOCK_G();regs[0]=m?0xd401:R_BAD_HANDLE;return 1;}
        int flags=num==0x51?MAP_SHARED|MAP_FIXED:MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED;
        void* result=mmap((void*)regs[1],m->size,num==0x51?PROT_READ|PROT_WRITE:PROT_NONE,flags,num==0x51?m->fd:-1,0);
        UNLOCK_G();regs[0]=result==MAP_FAILED?R_OUT_OF_RES:0;return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        LOCK_G();
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_TRANSFER) {
            transfer_t* m=g_h[i].obj;close(m->fd);free(m);g_h[i].type=O_FREE;g_h[i].obj=NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            t->paused = 0;pthread_cond_signal(&t->cv);
            pthread_cond_broadcast(&g_context_ready);
            if (t->exited) thr_free(t);
        }
        UNLOCK_G();
        regs[0] = 0; return 1;
    }
    }
    return 0;
}


#ifdef OT2_THREAD_TESTING
size_t ot2_thread_test_objects(void){LOCK_G();size_t n=0;for(thr_t* t=g_all;t;t=t->all)n++;UNLOCK_G();return n;}
size_t ot2_thread_test_waiters(int kind,uint64_t address){LOCK_G();size_t n=0;for(thr_t* t=g_wait;t;t=t->next)if(t->wkind==kind&&(!address||t->waddr==address))n++;UNLOCK_G();return n;}
size_t ot2_thread_test_pending_pauses(void){LOCK_G();size_t n=0;for(thr_t* t=g_all;t;t=t->all)if(t->paused&&!t->guest_context_valid)n++;UNLOCK_G();return n;}
#endif
