#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t svc_thread_main_init(void);
int svc_thread(int number, uint64_t* registers);
// A guest trapped at an SVC is quiescent until this capture is released.
// The returned token is nonzero only when this call installed the snapshot.
int svc_thread_capture_context(const void* context, uint64_t size);
void svc_thread_release_context(void);
int ot2_signal_event(uint32_t handle);
void ot2_host_log(const char* format, ...);
__attribute__((noreturn)) void ot2_host_die(const char* format, ...);
void guest_thread_init(void);
__attribute__((noreturn)) void guest_thread_exit(void);
__attribute__((noreturn)) void svc_thread_exit_on_host(void);
void crash_thread_init(void);
void dm_thread_enter(uint64_t entry,uint64_t argument,uint64_t stack_top);
void ot2_thread_dump_state(void);
void ot2_thread_record_sample(uintptr_t pc);
void ot2_thread_record_sample_sp(uintptr_t sp);
void ot2_thread_record_svc(uint64_t id,uintptr_t sp,uintptr_t lr,uint64_t x0,uint64_t x1);
uint32_t ot2_thread_handle_current(void);
uint64_t ot2_call_guest(const char* symbol,const uint64_t* arguments);
uint64_t ot2_call_guest_address(uint64_t address,const uint64_t* arguments);
uint64_t ot2_guest_symbol_address(const char* symbol);
// Android library calls may exceed small SDK thread stacks. Preserve the
// suspended guest stack for callbacks while running this service on its host stack.
uint64_t ot2_call_on_host_stack(uint64_t (*function)(uint64_t*),uint64_t* arguments);
void* ot2_native_gate(const char* name,void* function);
#ifdef __cplusplus
}
#endif
