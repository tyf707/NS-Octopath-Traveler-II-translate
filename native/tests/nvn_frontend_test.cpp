// D2R t_nvn contract cases, using the real exported lookup and command recorder.
#include "../graphics/backend.h"
#include "../graphics/nvn_protos.h"
#include "../graphics/nvn_commands.h"
#include "../graphics/nvn_deswizzle.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <memory>
#include <future>
#include <chrono>
#ifndef OT2_FRONTEND_ONLY
extern "C" int vkhost_test_precise_preparation();
extern "C" int vkhost_test_filter_formats();
extern "C" int vkhost_test_histogram(const char*);
extern "C" int vkhost_test_pixel_id(const char*);
extern "C" int vkhost_test_present_formats();
extern "C" int vkhost_test_texel_formats();
extern "C" int vkhost_test_storage_formats();
extern "C" int vkhost_test_layered_render(const char*,const char*);
extern "C" void vkhost_test_precision_enabled(int);
#include "../graphics/nvn_dirty.h"
#include <signal.h>
#include <cerrno>
#include <unistd.h>
static void dirty_test_fault(int,siginfo_t* info,void*){int saved=errno;if(nvn_dirty_fault(info->si_addr)){errno=saved;return;}_exit(99);}
#endif

template<class T>T proc(const char* name){auto p=ot2_nvn_lookup(name);if(!p)throw std::runtime_error(name);return reinterpret_cast<T>(p);}
#define P(name) proc<NVNT_##name>(#name)
static void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
static NVNdevice device{};
#include "nvn_capabilities_contract.inc"
struct Commands {
    alignas(4096) std::array<uint8_t,131072> memory{};
    NVNmemoryPool pool{};NVNcommandBuffer buffer{};size_t used=0;unsigned callbacks=0;
    static void refill(NVNcommandBuffer* cb,NVNcommandBufferMemoryEvent,size_t size,void* opaque){
        auto& self=*static_cast<Commands*>(opaque);size=std::max<size_t>(256,size);
        require(self.used+size<=self.memory.size(),"test command memory capacity");
        P(nvnCommandBufferAddCommandMemory)(cb,&self.pool,self.used,size);self.used+=size;++self.callbacks;
    }
    Commands(){NVNmemoryPoolBuilder pb{};P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,memory.data(),memory.size());require(P(nvnMemoryPoolInitialize)(&pool,&pb),"command pool");
        P(nvnCommandBufferInitialize)(&buffer,&device);P(nvnCommandBufferSetMemoryCallback)(&buffer,refill);P(nvnCommandBufferSetMemoryCallbackData)(&buffer,this);}
    ~Commands(){P(nvnCommandBufferFinalize)(&buffer);P(nvnMemoryPoolFinalize)(&pool);}
    void begin(){P(nvnCommandBufferBeginRecording)(&buffer);}
    uint64_t end(){return P(nvnCommandBufferEndRecording)(&buffer);}
};
static std::vector<const op_hdr*> records(uint64_t handle){
    std::vector<const op_hdr*> out;auto p=reinterpret_cast<const uint8_t*>(handle);
    for(unsigned i=0;i<100000;++i){auto h=reinterpret_cast<const op_hdr*>(p);require(h->size>=8&&h->size%8==0,"valid record size");
        if(h->op==OP_END)return out;if(h->op==OP_JUMP){memcpy(&p,p+8,8);continue;}if(h->op!=OP_BEGIN)out.push_back(h);p+=h->size;}
    throw std::runtime_error("command list must end");
}
template<class T>const T& payload(const op_hdr* header){return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(header)+8);}

#ifndef OT2_FRONTEND_ONLY
extern "C" void vkhost_test_queue_gate(int);
extern "C" void vkhost_test_present_gate(int);
extern "C" bool vkhost_test_is_suspended();
extern "C" void vkhost_test_wait_pipelines();
extern "C" uint64_t vkhost_test_pipeline_stat(int);
extern "C" int vkhost_test_pipeline_singleflight();
extern "C" int vkhost_test_descriptor_capacity();
extern "C" int vkhost_test_descriptor_working_set();
static void queued_contract(NVNqueue& queue){
    using namespace std::chrono_literals;
    Commands command;uint32_t result=0,input=0xaabbccdd;NVNsync sync{},inside{},never{};
    for(auto* s:{&sync,&inside,&never})P(nvnSyncInitialize)(s,&device);
    command.begin();P(nvnCommandBufferUpdateUniformBuffer)(&command.buffer,reinterpret_cast<uint64_t>(&result),4,0,4,&input);
    P(nvnCommandBufferFenceSync)(&command.buffer,&inside,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);auto handle=command.end();
    vkhost_test_queue_gate(1);P(nvnQueueSubmitCommands)(&queue,1,&handle);
    P(nvnQueueFenceSync)(&queue,&sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    require(result==0,"async submit returns before queued work executes");
    require(P(nvnSyncWait)(&sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"queued fence cannot signal before execution");
    require(P(nvnSyncWait)(&inside,1000000)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"command fence is published at enqueue and honors timeout");
    require(P(nvnSyncWait)(&never,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"unfenced sync times out");
    auto waiting=std::async(std::launch::async,[&]{return P(nvnSyncWait)(&sync,2000000000);});
    require(waiting.wait_for(20ms)==std::future_status::timeout,"long fence wait blocks on incomplete work");
    vkhost_test_queue_gate(0);require(waiting.get()==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED,"long wait wakes at GPU completion");
    require(result==input&&P(nvnSyncWait)(&inside,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED,"completed fence makes ordered writes visible");
    vkhost_test_queue_gate(1);auto suspension=std::async(std::launch::async,[]{vkhost_suspend(1);});
    auto until=std::chrono::steady_clock::now()+2s;while(!vkhost_test_is_suspended()&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
    require(vkhost_test_is_suspended(),"suspend closes admission");
    require(suspension.wait_for(20ms)==std::future_status::timeout,"suspend drains in-flight work before return");
    vkhost_test_queue_gate(0);suspension.get();
    auto blocked=std::async(std::launch::async,[&]{P(nvnQueueSubmitCommands)(&queue,1,&handle);});
    require(blocked.wait_for(20ms)==std::future_status::timeout,"suspend blocks new submissions");vkhost_suspend(0);blocked.get();P(nvnQueueFinish)(&queue);
    // No Android surface: present is dropped and still retires its ticket.
    NVNtextureBuilder tb{};NVNtexture t{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,8,4);P(nvnTextureInitialize)(&t,&tb);
    NVNwindowBuilder wb{};NVNwindow window{};NVNtexture* textures[]={&t};P(nvnWindowBuilderSetDefaults)(&wb);P(nvnWindowBuilderSetTextures)(&wb,1,textures);P(nvnWindowInitialize)(&window,&wb);
    int index=-1;P(nvnWindowAcquireTexture)(&window,nullptr,&index);require(index==0,"Acquire accepts a null sync");
    P(nvnQueuePresentTexture)(&queue,&window,0);P(nvnWindowAcquireTexture)(&window,nullptr,&index);require(index==0,"surface loss releases presentation ticket");
    vkhost_test_present_gate(1);P(nvnQueuePresentTexture)(&queue,&window,0);
    // This command fence excludes the presenter, unlike QueueFenceSync below.
    P(nvnQueueSubmitCommands)(&queue,1,&handle);
    require(P(nvnSyncWait)(&inside,2000000000)==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED||P(nvnSyncWait)(&inside,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED,"translation progresses while presenter is held");
    P(nvnQueueFenceSync)(&queue,&sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);require(P(nvnSyncWait)(&sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"queue fence orders pending presents too");
    vkhost_test_present_gate(0);P(nvnQueueFinish)(&queue);
    P(nvnWindowAcquireTexture)(&window,&sync,&index);
    command.begin();P(nvnCommandBufferWaitSync)(&command.buffer,&sync);auto wait_present=command.end();P(nvnQueueSubmitCommands)(&queue,1,&wait_present);P(nvnQueueFinish)(&queue);
    P(nvnTextureFinalize)(&t);
    puts("PASS asynchronous frontend fences, timeout, completion visibility, suspend/drain/resume, headless present tickets");
}
#include "nvn_cpu_write_contract.inc"
#include "nvn_draw_contract.inc"
#include "nvn_attachment_contract.inc"
#include "nvn_buffer_contract.inc"
#include "nvn_gpu_ring_contract.inc"
#include "nvn_command_state_contract.inc"
#include "nvn_depth_bounds_contract.inc"
#include "nvn_null_contract.inc"
#include "nvn_uniform_range_contract.inc"
#include "nvn_texel_view_contract.inc"
#include "nvn_cube_view_contract.inc"
#include "nvn_integer_2d_contract.inc"
extern "C" int vkhost_test_constant_decode();
extern "C" int vkhost_test_integer_selections();
extern "C" uint64_t vkhost_test_gpu_profile(int,int);
extern "C" uint64_t vkhost_test_descriptor_templates(int);
extern "C" int vkhost_test_descriptor_template_order();
extern "C" int vkhost_test_descriptor_delta_order();
extern "C" int vkhost_test_persistent_uploads();
extern "C" int vkhost_test_descriptor_delta();
extern "C" int vkhost_test_descriptor_blocks();
extern "C" void vkhost_test_batch_optimizations(int);
extern "C" uint64_t vkhost_test_batch_work(unsigned);
extern "C" int vkhost_test_stream_uploads();
extern "C" int vkhost_test_resource_descriptors();
extern "C" int vkhost_test_cached_readback();
extern "C" int vkhost_test_native_null();
extern "C" int vkhost_test_bc();
extern "C" uint64_t vkhost_test_command_recycling(int);
static void gpu_contract(){
    if(getenv("OT2_CPU_WRITE_ONLY")){cpu_write_contract();return;}
    if(getenv("OT2_INTEGER_2D_ONLY")){
        NVNqueueBuilder qb{};NVNqueue queue{};P(nvnQueueBuilderSetDefaults)(&qb);P(nvnQueueInitialize)(&queue,&qb);
        integer_2d_contract(queue);depth_alias_contract(queue);lighting_mask_contract(queue);P(nvnQueueFinalize)(&queue);return;
    }
    if(getenv("OT2_CUBE_VIEWS_ONLY")){
        NVNqueueBuilder qb{};NVNqueue queue{};P(nvnQueueBuilderSetDefaults)(&qb);P(nvnQueueInitialize)(&queue,&qb);
        cube_view_contract(queue);P(nvnQueueFinalize)(&queue);return;
    }
    if(getenv("OT2_SOFTWARE_SAMPLES_ONLY")){
        NVNqueueBuilder qb{};NVNqueue queue{};P(nvnQueueBuilderSetDefaults)(&qb);P(nvnQueueInitialize)(&queue,&qb);
        // Includes repeated resets/reports, cross-submission intervals, GPU
        // holding fences, descriptor reuse and the hardware depth-test path.
        draw_contract(queue,0);draw_contract(queue,1);P(nvnQueueFinalize)(&queue);return;
    }
    if(getenv("OT2_BC_ONLY")){require(vkhost_test_bc(),"BC texture fallback");return;}
    if(getenv("OT2_NULL_ONLY")){
        NVNqueueBuilder qb{};NVNqueue queue{};P(nvnQueueBuilderSetDefaults)(&qb);P(nvnQueueInitialize)(&queue,&qb);
        null_resource_contract(queue);P(nvnQueueFinalize)(&queue);return;
    }
    require(vkhost_test_present_formats(),"display encoded pixels, crop, scale, channel ordering and alpha");
    require(vkhost_test_texel_formats(),"integer texel specialization");
    require(vkhost_test_integer_selections(),"bounded integer selection cache, type and stage changes");
    require(vkhost_test_storage_formats(),"integer texel read/write specialization");
    require(vkhost_probe_descriptor_reuse(),"cold application startup descriptor probe counts sets independently of pools");
    require(vkhost_test_bc(),"BC texture fallback");
    const bool baseline=getenv("OT2_BATCH_BASELINE")!=nullptr;vkhost_test_batch_optimizations(!baseline);
    const auto recycled_before=vkhost_test_command_recycling(!baseline);
    require(vkhost_test_descriptor_blocks(),"32768 sets share 512 pools, partial retirement preserves live sets, OOM retries, every block reclaimed");
    require(vkhost_test_persistent_uploads(),"persistent uploads: real GPU held versions, mutation, reuse, guards, pool lifetime");
    require(vkhost_test_stream_uploads(),"resource uploads preserve held versions and only update dirty pages when idle");
    require(vkhost_test_resource_descriptors(),"persistent resources retain descriptor reuse, held GPU versions and eviction invalidates stale descriptor handles");
    require(vkhost_test_cached_readback(),"cached readback respects fences, invalidation and protected guest destination");
    require(vkhost_test_precise_preparation(),"tracked dependencies retain unrelated writes and invalidate related writes and pool replacement");
    require(vkhost_test_descriptor_delta(),"descriptor delta changes one binding, preserves others, guards and disabled full-update control");
    require(vkhost_test_descriptor_template_order(),"descriptor templates preserve different input orders sharing one layout, changed offsets, values and guards");
    require(vkhost_test_descriptor_delta_order(),"linear descriptor deltas preserve changed offsets, permuted input orders, mode switches and GPU guards");
    const auto template_before=vkhost_test_descriptor_templates(1);
    require(vkhost_test_descriptor_working_set(),"retired descriptor working set avoids cyclic eviction");
    require(vkhost_test_descriptor_capacity(),"descriptor working set survives GPU fences without pool churn");
    require(vkhost_probe_resource_lifecycle(),"cached image/descriptor lookups preserve pool identity, replacement and retirement");
    require(vkhost_test_constant_decode(),"direct storage descriptor decoding");
    require(vkhost_probe_deswizzle(),"GPU/CPU deswizzle format/mip/layer/3D/version matrix");
    puts("PASS GPU/CPU deswizzle byte comparison, odd tails, guards and within-batch source versions");
    NVNqueueBuilder qb{};NVNqueue queue{};P(nvnQueueBuilderSetDefaults)(&qb);P(nvnQueueInitialize)(&queue,&qb);
    auto submit=[&](uint64_t handle){P(nvnQueueSubmitCommands)(&queue,1,&handle);P(nvnQueueFinish)(&queue);};
    queued_contract(queue);
    command_state_contract(queue,0);command_state_contract(queue,1);
    if(!getenv("OT2_PIPELINE_TEST_ROOT"))depth_bounds_contract(queue,0,1);
    depth_bounds_contract(queue,1,1);depth_bounds_contract(queue,1,0);
    null_resource_contract(queue);
    uniform_range_contract(queue);
    texel_view_contract(queue);texel_view_contract(queue,true);cube_view_contract(queue);integer_2d_contract(queue);depth_alias_contract(queue);lighting_mask_contract(queue);
    const auto profile_before=vkhost_test_gpu_profile(1,0);
    draw_contract(queue,0);draw_contract(queue,1);
    attachment_contract(queue,0);attachment_contract(queue,1);
    attachment_contract(queue,0,1024,512);attachment_contract(queue,1,1024,512);
    attachment_contract(queue,0,1280,720,0);attachment_contract(queue,1,1280,720,0);
    buffer_contract(queue,0);buffer_contract(queue,1);
    gpu_ring_contract(queue,0);gpu_ring_contract(queue,1);
    const auto recycled_after=vkhost_test_command_recycling(!baseline);
    require(baseline?recycled_after==recycled_before:recycled_after>recycled_before+8,"command recycling exercised across held GPU fences; disabled control does not reuse");
    printf("COMMAND_RECYCLE enabled=%d reused=%llu\n",!baseline,(unsigned long long)(recycled_after-recycled_before));
    require(vkhost_test_gpu_profile(0,0)>profile_before&&vkhost_test_gpu_profile(0,1)>0&&vkhost_test_gpu_profile(0,2)==0&&vkhost_test_gpu_profile(0,4)>0,"GPU timestamps retire with held command/query/arena slots");
    const auto profile_off=vkhost_test_gpu_profile(0,0);buffer_contract(queue,1);
    require(vkhost_test_gpu_profile(0,0)==profile_off,"disabled GPU profiling records no queries");
    puts("PASS GPU profile timestamps, wrap, held slot ownership and disabled negative control");
    printf("BATCH_WORK groups=%llu delta=%llu folded=%llu pipeline_versions=%llu depth_pruned=%llu dead_stores=%llu baseline=%d\n",
        (unsigned long long)vkhost_test_batch_work(0),(unsigned long long)vkhost_test_batch_work(1),(unsigned long long)vkhost_test_batch_work(2),(unsigned long long)vkhost_test_batch_work(3),(unsigned long long)vkhost_test_batch_work(4),(unsigned long long)vkhost_test_batch_work(5),baseline);
    if(!baseline)require(vkhost_test_batch_work(0)>0&&vkhost_test_batch_work(2)>0&&(!vkhost_test_native_null()||vkhost_test_batch_work(3)>0)&&vkhost_test_batch_work(5)>0,"batch fast paths exercised by actual GPU output assertions; version-only pipeline lookup requires native null descriptors");
    const auto template_after=vkhost_test_descriptor_templates(0);
    require(template_after>template_before,"real GPU contract exercises descriptor templates");
    null_resource_contract(queue);uniform_range_contract(queue);texel_view_contract(queue);draw_contract(queue,1);
    require(vkhost_test_descriptor_templates(0)==template_after,"descriptor template disabled negative control uses regular updates");
    printf("PASS descriptor templates: mixed resources, nulls, dynamic offsets, input versions and pending GPU leases; updates=%llu disabled=0\n",(unsigned long long)(template_after-template_before));
    alignas(4096) std::array<uint8_t,16384> backing{};
    NVNmemoryPoolBuilder pb{};NVNmemoryPool pool{};P(nvnMemoryPoolBuilderSetDefaults)(&pb);P(nvnMemoryPoolBuilderSetStorage)(&pb,backing.data(),backing.size());P(nvnMemoryPoolInitialize)(&pool,&pb);
    NVNtextureBuilder tb{};NVNtexture texture{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,8,4);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);P(nvnTextureBuilderSetStorage)(&tb,&pool,0);P(nvnTextureInitialize)(&texture,&tb);
    NVNcopyRegion full{0,0,0,8,4,1};float blue[4]{0,0,1,1},green[4]{0,1,0,1};std::array<uint8_t,160> result;result.fill(0xcc);
    {
        Commands clear,delayed,copied;
        clear.begin();P(nvnCommandBufferClearTexture)(&clear.buffer,&texture,nullptr,&full,blue,15);submit(clear.end());
        delayed.begin();P(nvnCommandBufferCopyTextureToBuffer)(&delayed.buffer,&texture,nullptr,&full,reinterpret_cast<uint64_t>(result.data()),0);auto old=delayed.end();
        copied.begin();P(nvnCommandBufferCopyCommands)(&copied.buffer,1,&old);auto copy=copied.end();
        P(nvnTextureFinalize)(&texture);P(nvnTextureBuilderSetStorage)(&tb,&pool,4096);P(nvnTextureInitialize)(&texture,&tb);
        clear.begin();P(nvnCommandBufferClearTexture)(&clear.buffer,&texture,nullptr,&full,green,15);submit(clear.end());
        // Re-grant the original record memory. The copied list still owns the
        // first texture's metadata and its already-rendered GPU contents.
        P(nvnCommandBufferAddCommandMemory)(&delayed.buffer,&delayed.pool,0,delayed.memory.size());memset(delayed.memory.data(),0xee,delayed.memory.size());
        submit(copy);
        for(unsigned pixel=0;pixel<32;pixel++)require(result[pixel*4]==0&&result[pixel*4+1]==0&&result[pixel*4+2]==255&&result[pixel*4+3]==255,"finalized old texture retains original GPU contents");
        for(unsigned i=128;i<result.size();i++)require(result[i]==0xcc,"GPU readback keeps destination guard");
        P(nvnTextureFinalize)(&texture);
    }
    puts("PASS Vulkan frontend pixel readback across Finalize/rebuild/Copy/source-memory reuse");
    {
        Commands command;NVNtexture image{};NVNtextureView view{};P(nvnTextureBuilderSetSize2D)(&tb,8,4);P(nvnTextureBuilderSetLevels)(&tb,2);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
        P(nvnTextureInitialize)(&image,&tb);P(nvnTextureViewSetDefaults)(&view);P(nvnTextureViewSetFormat)(&view,NVN_FORMAT_RGBA8_SRGB);P(nvnTextureViewSetLevels)(&view,1,1);
        NVNcopyRegion mip{0,0,0,4,2,1};float half[4]{0.5f,0.5f,0.5f,1};std::array<uint8_t,36> pixels;pixels.fill(0xcc);
        command.begin();P(nvnCommandBufferClearTexture)(&command.buffer,&image,&view,&mip,half,15);
        P(nvnCommandBufferCopyTextureToBuffer)(&command.buffer,&image,&view,&mip,reinterpret_cast<uint64_t>(pixels.data()),0);submit(command.end());
        for(unsigned i=0;i<8;i++)require(pixels[i*4]>=187&&pixels[i*4]<=189&&pixels[i*4+3]==255,"SRGB attachment view must encode linear clear at selected mip");
        require(pixels[32]==0xcc,"mip readback bounds");P(nvnTextureFinalize)(&image);P(nvnTextureBuilderSetLevels)(&tb,1);
    }
    puts("PASS Vulkan SRGB attachment view and mip extent through actual frontend");
    {
        Commands command;std::array<uint32_t,320> destination{};uint32_t data[16]{};
        NVNeventBuilder eb{};NVNevent event{};P(nvnEventBuilderSetDefaults)(&eb);P(nvnEventBuilderSetStorage)(&eb,&pool,8192);P(nvnEventInitialize)(&event,&eb);
        NVNcounterData timestamp{};command.begin();
        for(unsigned k=0;k<20;k++){for(unsigned i=0;i<16;i++)data[i]=k*100+i;P(nvnCommandBufferUpdateUniformBuffer)(&command.buffer,reinterpret_cast<uint64_t>(destination.data()),sizeof(destination),k*sizeof(data),sizeof(data),data);memset(data,0xee,sizeof(data));}
        P(nvnCommandBufferSignalEvent)(&command.buffer,&event,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,7);
        P(nvnCommandBufferSignalEvent)(&command.buffer,&event,NVN_EVENT_SIGNAL_MODE_ADD,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,5);
        P(nvnCommandBufferReportCounter)(&command.buffer,NVN_COUNTER_TYPE_TIMESTAMP,reinterpret_cast<uint64_t>(&timestamp));auto handle=command.end();
        require(P(nvnEventGetValue)(&event)==0,"recording does not signal the event");submit(handle);
        for(unsigned k=0;k<20;k++)for(unsigned i=0;i<16;i++)require(destination[k*16+i]==k*100+i,"frontend replay uses recorded uniform bytes");
        require(P(nvnEventGetValue)(&event)==12,"ordered WRITE/ADD events complete after submission");
        require(timestamp.counter!=0&&timestamp.counter==timestamp.timestamp,"D2R timestamp report populates both fields");P(nvnEventFinalize)(&event);
    }
    puts("PASS Vulkan frontend multi-segment replay, uniform bytes, ordered events and timestamps");
    P(nvnMemoryPoolFinalize)(&pool);P(nvnQueueFinalize)(&queue);
}
#endif

int main(){try{
#ifndef OT2_FRONTEND_ONLY
    struct sigaction action{};action.sa_sigaction=dirty_test_fault;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV,&action,nullptr);sigaction(SIGBUS,&action,nullptr);
#endif
#ifndef OT2_FRONTEND_ONLY
    if(auto* path=getenv("OT2_PIPELINE_TEST_ROOT"))vkhost_configure(path);
    vkhost_init();
    if(auto* directory=getenv("OT2_PIXEL_ID_FIXTURE")){require(vkhost_test_pixel_id(directory),"PIXLD sample ID pixel oracle");return 0;}
    if(auto* shader=getenv("OT2_HISTOGRAM_SHADER")){require(vkhost_test_histogram(shader),"uniform grey luminance histogram");return 0;}
    if(getenv("OT2_PRESENT_FORMATS_ONLY")){require(vkhost_test_present_formats(),"presentation must preserve display-encoded bytes");return 0;}
    if(getenv("OT2_FILTER_FORMATS_ONLY")){require(vkhost_test_filter_formats(),"linear filtering format pixel oracle");return 0;}
    if(getenv("OT2_TEXEL_FORMATS_ONLY")){require(vkhost_test_texel_formats(),"integer texel input pixel oracle");return 0;}
    if(getenv("OT2_STORAGE_FORMATS_ONLY")){require(vkhost_test_storage_formats(),"integer texel read/write pixel oracle");return 0;}
    if(getenv("OT2_PRECISE_PREPARATION"))vkhost_test_precision_enabled(1);
    if(auto* fixture=getenv("OT2_LAYERED_FIXTURE")){require(vkhost_test_layered_render(fixture,getenv("OT2_LAYERED_CASE")?getenv("OT2_LAYERED_CASE"):"simple-3d"),"layered render pixels");return 0;}
    if(getenv("OT2_PREPARATION_ONLY")){require(vkhost_test_precise_preparation(),"unrelated writes retain prepared resources; related writes and replacement invalidate; GPU bytes agree");puts("PASS precise resource preparation");return 0;}
    vkhost_test_wait_pipelines();
    if(getenv("OT2_WARM_ONLY")){
        printf("PORTABLE_WARM created=%llu rejected=%llu\n",(unsigned long long)vkhost_test_pipeline_stat(1),(unsigned long long)vkhost_test_pipeline_stat(3));
        require(vkhost_test_pipeline_stat(1)>0&&!vkhost_test_pipeline_stat(3),"observed game pipeline corpus compiles on this device");return 0;
    }
    if(getenv("OT2_DESCRIPTOR_CAPACITY_ONLY")){
        require(vkhost_test_descriptor_capacity(),"descriptor working set must survive across GPU fences without pool churn");
        puts("PASS descriptor capacity: 2049 live sets, distinct GPU outputs and guards, zero warm pool creates");return 0;
    }
#endif
    NVNdeviceBuilder db{};P(nvnDeviceBuilderSetDefaults)(&db);require(P(nvnDeviceInitialize)(&device,&db),"device");
    capabilities_contract();
    Commands source,copied,called;
    std::array<uint32_t,320> destination{};std::array<uint32_t,16> input{};
    source.begin();
    for(unsigned k=0;k<20;k++){for(unsigned i=0;i<16;i++)input[i]=k*100+i;
        P(nvnCommandBufferUpdateUniformBuffer)(&source.buffer,reinterpret_cast<uint64_t>(destination.data()),sizeof(destination),k*sizeof(input),sizeof(input),input.data());input.fill(0xeeeeeeee);}
    auto original=source.end();require(source.callbacks>2,"small grants exercise callback and JUMP");
    copied.begin();P(nvnCommandBufferCopyCommands)(&copied.buffer,1,&original);auto copy=copied.end();
    called.begin();P(nvnCommandBufferCallCommands)(&called.buffer,1,&original);auto call=called.end();
    auto calls=records(call);require(calls.size()==1&&calls[0]->op==OP_CALL&&payload<uint64_t>(calls[0])==original,"CallCommands preserves reference");
    {
        const uint64_t handles[]={call,call};auto* pinned=nvn_commands_pin(2,handles);
        auto index=nvn_commands_index(pinned,2,handles);size_t updates=0;
        for(auto* p:index){const auto& h=*reinterpret_cast<const op_hdr*>(p);require(h.op!=OP_CALL&&h.op!=OP_JUMP&&h.op!=OP_END,"index flattens control records");updates+=h.op==OP_UPDATE_UB;}
        require(updates==40,"index follows JUMP and repeated CALL, preserving execution order");
        alignas(8) op_hdr unknown{OP_END,8};uint64_t outside=reinterpret_cast<uint64_t>(&unknown);
        require(nvn_commands_index(pinned,1,&outside).empty(),"index rejects memory outside pinned grants");
        nvn_commands_unpin(pinned);
    }
    memset(source.memory.data(),0xcc,source.memory.size());
    auto copied_records=records(copy);require(copied_records.size()==20,"CopyCommands follows all source segments");
    for(unsigned k=0;k<20;k++){auto h=copied_records[k];require(h->op==OP_UPDATE_UB,"copied opcode");auto values=reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(h)+24);
        for(unsigned i=0;i<16;i++)require(values[i]==k*100+i,"recorded/copy uniform data is owned");}
    puts("PASS NVN callbacks, segment links, inline data, Copy/Call ownership");

    NVNtextureBuilder tb{};P(nvnTextureBuilderSetDefaults)(&tb);P(nvnTextureBuilderSetSize2D)(&tb,8,4);P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    struct TextureGuard{NVNtexture texture;uint64_t guard=0x92ef9284aabbccdd;} guarded{};
    P(nvnTextureInitialize)(&guarded.texture,&tb);require(guarded.guard==0x92ef9284aabbccdd,"actual 128-byte texture ABI");
    const NVNtexture* targets[]={&guarded.texture};Commands snapshots;
    snapshots.begin();P(nvnCommandBufferSetRenderTargets)(&snapshots.buffer,1,targets,nullptr,nullptr,nullptr);
    auto before=snapshots.end();auto rt=&payload<p_set_rt>(records(before)[0]);auto serial=rt->color[0]->serial;
    P(nvnTextureFinalize)(&guarded.texture);P(nvnTextureBuilderSetSize2D)(&tb,16,16);P(nvnTextureInitialize)(&guarded.texture,&tb);
    require(rt->color[0]->d.width==8&&rt->color[0]->d.height==4&&rt->color[0]->serial==serial,"recorded texture version survives same-address rebuild");
    require(rt->color[0]!=reinterpret_cast<const tex_t*>(&guarded.texture),"recording owns texture metadata");
    require(guarded.guard==0x92ef9284aabbccdd,"snapshot does not extend guest object ABI");
    P(nvnTextureFinalize)(&guarded.texture);
    puts("PASS NVN object snapshot version and guest ABI");
    // Copy must acquire ownership of referenced snapshots, not just duplicate
    // the pointer bytes. Finalizing the source pool releases its own lease.
    {
        Commands retained;uint64_t retained_handle=0;const tex_t* version=nullptr;
        {
            Commands temporary;P(nvnTextureBuilderSetSize2D)(&tb,23,7);P(nvnTextureInitialize)(&guarded.texture,&tb);
            NVNcopyRegion region{0,0,0,1,1,1};float color[4]{};uint32_t ucolor[4]{};int icolor[4]{};uint64_t bytes[4]{};
            temporary.begin();
            P(nvnCommandBufferCopyBufferToTexture)(&temporary.buffer,reinterpret_cast<uint64_t>(bytes),&guarded.texture,nullptr,&region,0);
            P(nvnCommandBufferCopyTextureToBuffer)(&temporary.buffer,&guarded.texture,nullptr,&region,reinterpret_cast<uint64_t>(bytes),0);
            P(nvnCommandBufferCopyTextureToTexture)(&temporary.buffer,&guarded.texture,nullptr,&region,&guarded.texture,nullptr,&region,0);
            P(nvnCommandBufferClearTexture)(&temporary.buffer,&guarded.texture,nullptr,&region,color,15);
            P(nvnCommandBufferClearTextureui)(&temporary.buffer,&guarded.texture,nullptr,&region,ucolor,15);
            P(nvnCommandBufferClearTexturei)(&temporary.buffer,&guarded.texture,nullptr,&region,icolor,15);
            auto recorded=temporary.end();version=payload<p_copy_b2t>(records(recorded)[0]).tex;
            retained.begin();P(nvnCommandBufferCopyCommands)(&retained.buffer,1,&recorded);retained_handle=retained.end();
            P(nvnTextureFinalize)(&guarded.texture);memset(&guarded.texture,0xa5,sizeof(guarded.texture));
        }
        auto list=records(retained_handle);require(list.size()==6,"all texture commands copied");
        for(auto item:list){const tex_t* texture=nullptr;
            if(item->op==OP_COPY_B2T||item->op==OP_COPY_T2B)texture=payload<p_copy_b2t>(item).tex;
            if(item->op==OP_CLEAR_TEX)texture=payload<p_clear_tex>(item).tex;
            if(item->op==OP_COPY_T2T){auto& pair=payload<p_copy_t2t>(item);require(pair.src==pair.dst,"same-version copies share metadata");texture=pair.src;}
            require(texture==version&&texture->d.width==23&&texture->d.height==7,"copy owns every texture snapshot after source pool is gone");
        }
    }
    puts("PASS NVN texture transfers/clears copy snapshot ownership across pool retirement");
    {
        NVNtexture source{},destination{};
        P(nvnTextureBuilderSetSize2D)(&tb,16,8);
        P(nvnTextureInitialize)(&source,&tb);P(nvnTextureInitialize)(&destination,&tb);
        Commands command;command.begin();
        P(nvnCommandBufferDownsample)(&command.buffer,&source,&destination);
        auto list=records(command.end());require(list.size()==1&&list[0]->op==OP_COPY_T2T,"downsample records real transfer");
        const auto& copy=payload<p_copy_t2t>(list[0]);
        require(!copy.has_sv&&!copy.has_dv&&copy.sr.width==16&&copy.sr.height==8&&copy.sr.depth==1&&copy.dr.width==16&&copy.dr.height==8,"downsample covers default texture views");
        P(nvnTextureFinalize)(&source);P(nvnTextureFinalize)(&destination);
    }
    puts("PASS NVN whole-texture downsample records resolve regions");
#ifdef OT2_FRONTEND_ONLY
    {
        NVNprogram p{};NVNshaderData shaders[2]{};
        std::array<std::array<uint8_t,0x880>,2> controls{};std::array<uint64_t,2> code{0x12345678,0xabcdef};
        for(unsigned i=0;i<2;i++){uint32_t magic=0x98761234,size=8;memcpy(controls[i].data(),&magic,4);memcpy(controls[i].data()+0x704,&size,4);memcpy(controls[i].data()+0x714,&i,4);shaders[i]={reinterpret_cast<uint64_t>(&code[i]),controls[i].data()};}P(nvnProgramInitialize)(&p,&device);P(nvnProgramSetShaders)(&p,1,shaders);
        Commands command;command.begin();P(nvnCommandBufferBindProgram)(&command.buffer,&p,NVN_SHADER_STAGE_VERTEX_BIT);auto handle=command.end();
        auto* before=&payload<p_bind_program>(records(handle)[0]);auto serial=before->prog->serial;
        code[0]=0xeeeeeeee;controls[0][123]=0xff;P(nvnProgramSetShaders)(&p,2,shaders);P(nvnProgramFinalize)(&p);memset(&p,0x5a,sizeof(p));
        require(*reinterpret_cast<const uint64_t*>(before->prog->sd[0].data)==0x12345678&&static_cast<const uint8_t*>(before->prog->sd[0].control)[123]==0,"program owns shader code/control after original memory changes");
        require(before->prog->nstage==1&&before->prog->serial==serial,"shader replacement preserves the recorded program version");
    }
    puts("PASS NVN program version survives SetShaders and Finalize");
#endif
    {
        size_t s0,t0,p0,s,t,p;nvn_commands_stats(&s0,&t0,&p0);void* pending=nullptr;const tex_t* version=nullptr;
        {
            Commands command;P(nvnTextureBuilderSetSize2D)(&tb,31,9);P(nvnTextureInitialize)(&guarded.texture,&tb);
            command.begin();P(nvnCommandBufferSetRenderTargets)(&command.buffer,1,targets,nullptr,nullptr,nullptr);auto handle=command.end();
            version=payload<p_set_rt>(records(handle)[0]).color[0];pending=nvn_commands_pin(1,&handle);P(nvnTextureFinalize)(&guarded.texture);
        }
        nvn_commands_stats(&s,&t,&p);require(t==t0+1&&version->d.width==31,"queued submission pins metadata after source grants retire");
        nvn_commands_unpin(pending);nvn_commands_stats(&s,&t,&p);require(s==s0&&t==t0&&p==p0,"snapshot metadata is reclaimed after its final owner");
    }
    puts("PASS NVN queued-segment pin and exact final-owner reclamation");
#ifndef OT2_FRONTEND_ONLY
    gpu_contract();
    if(getenv("OT2_PIPELINE_TEST_ROOT")){
        vkhost_test_wait_pipelines();auto cold=vkhost_test_pipeline_stat(0),warm=vkhost_test_pipeline_stat(1),claimed=vkhost_test_pipeline_stat(2),rejected=vkhost_test_pipeline_stat(3);
        printf("PIPELINE_CONTRACT foreground=%llu warm=%llu claimed=%llu rejected=%llu\n",(unsigned long long)cold,(unsigned long long)warm,(unsigned long long)claimed,(unsigned long long)rejected);
        if(getenv("OT2_EXPECT_PARTIAL_WARM"))require(cold&&warm&&claimed&&rejected,"damaged cached module falls back to foreground creation without affecting other pipelines");
        else if(getenv("OT2_EXPECT_WARM"))require(!cold&&warm&&claimed,"restarted process uses prewarmed pipelines for every tested draw/compute without foreground creation");
        else require(cold&&!warm&&!claimed,"cold/disabled negative control creates foreground pipelines");
        if(getenv("OT2_EXPECT_BAD_RECIPE"))require(rejected,"malformed optional recipe rejected without affecting rendering");
        if(getenv("OT2_EXPECT_CLEAN_RECIPES"))require(!rejected,"consolidated cache drops malformed optional recipe and repairs damaged module");
    }
    require(vkhost_test_pipeline_singleflight(),"foreground/background pipeline creation deduplicates in-flight work and retries failed warm-up");
    puts("PASS eight concurrent pipeline consumers create once; failed optional warm-up retries");
#endif
    return 0;
}catch(const std::exception& error){fprintf(stderr,"FAIL NVN frontend: %s\n",error.what());return 1;}}
