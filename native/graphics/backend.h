#pragma once
#include "nvn_obj.h"
#ifdef __cplusplus
extern "C" {
#endif
void vkhost_init(void);
int vkhost_device_limit(NVNdeviceInfo property,int guest_maximum);
uint64_t nvn_new_serial(void);
void vkhost_configure(const char* shader_directory);
void vkhost_configure_package(const char* cache_directory,const char* offline_directory);
void vkhost_program(const prog_t* program);
struct ANativeWindow;
void vkhost_set_window(struct ANativeWindow* window);
void vkhost_set_aspect_mode(int mode);
void vkhost_set_present_interval(int value);
void* ot2_nvn_lookup(const char* name);
void vkhost_pool_add(const void* memory,uint64_t size);
void vkhost_pool_del(const void* memory);
void vkhost_set_tex_pool_n(int n);
void vkhost_descriptor_pool_del(const dpool_t*,int sampler);
void vkhost_reg_tex(const dpool_t*,int id,const tex_t* texture,const view_t* view);
void vkhost_reg_samp(const dpool_t*,int id,const samp_par* sampler);
void vkhost_tex_created(const tex_t* texture);
void vkhost_tex_release(tex_t* texture);
void vkhost_tex_retain(uint64_t serial);
void vkhost_tex_unref(uint64_t serial);
void vkhost_prog_release(prog_t* program);
void vkhost_write_texels(const tex_t*,const view_t*,const NVNcopyRegion*,const void*,int64_t,int64_t);
void vkhost_read_texels(const tex_t*,unsigned,unsigned,const NVNcopyRegion*,uint8_t*,size_t,size_t);
uint64_t vkhost_last_fence(void);
int vkhost_wait(uint64_t fence,uint64_t timeout);
void vkhost_finish(void);
void vkhost_submit(int n,const uint64_t* handles);
uint64_t vkhost_present(const tex_t*,int interval,const int* crop);
void vkhost_suspend(int on);
uint64_t vkhost_frame_count(void);
void vkhost_frame_stats(double out[4]);
int vkhost_probe_transfers(void);
int vkhost_probe_descriptor_reuse(void);
int vkhost_probe_resource_lifecycle(void);
int vkhost_probe_texture_coherence(void);
int vkhost_probe_deswizzle(void);
#ifdef __cplusplus
}
#endif
