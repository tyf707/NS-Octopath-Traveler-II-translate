#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Limits are retention ceilings, not reservations. Vulkan does not expose the
// host allocation size of a descriptor pool or buffer view. Conservative unit
// charges derive ceilings; real MemAvailable is sampled again as they fill.
struct CacheBudget {
    static constexpr uint64_t MiB=1024*1024;
    size_t descriptors=8192,texel_views=8192;
    size_t geometry_bytes=128*MiB,geometry_entries=4096;
    size_t free_buffer_bytes=128*MiB,free_buffers=128;
    size_t resource_bytes=64*MiB,resource_entries=8192;
    uint64_t allowance=0,reserve=0;
    bool pressure=false;
    static CacheBudget reduced(){
        CacheBudget b;b.descriptors=1024;b.texel_views=2048;
        b.geometry_bytes=8*MiB;b.geometry_entries=256;
        b.free_buffer_bytes=16*MiB;b.free_buffers=16;b.resource_bytes=8*MiB;b.resource_entries=1024;b.pressure=true;return b;
    }
    static CacheBudget background(){
        CacheBudget b;b.geometry_bytes=16*MiB;b.geometry_entries=512;
        b.free_buffer_bytes=32*MiB;b.free_buffers=32;b.resource_bytes=16*MiB;b.resource_entries=2048;return b;
    }
};
class CacheBudgetPolicy {
    CacheBudget previous;
    uint64_t pressure_until=0;
    bool recovering=false;
    static size_t rounded(uint64_t n,size_t floor,size_t cap){
        return std::clamp<size_t>((n/1024)*1024,floor,cap);
    }
    static size_t stable(size_t next,size_t old){
        // A quarter of headroom avoids repeatedly rebuilding a working set
        // when system file-cache accounting or another app fluctuates.
        return next>old&&next-old<old/4 ? old : next<old&&old-next<old/4 ? old : next;
    }
public:
    void allocation_pressure(uint64_t seconds){recovering=true;pressure_until=seconds+30;}
    CacheBudget update(uint64_t total,uint64_t available,uint64_t seconds,bool large=true,bool resource_cache=true){
        using B=CacheBudget;
        if(!total||available>total){
            // Unknown/failed memory observations must not enable large caches.
            previous=recovering?B::reduced():B{};return previous;
        }
        const uint64_t reserve=std::max<uint64_t>(768*B::MiB,total/8);
        if(available<reserve){recovering=true;pressure_until=seconds+30;}
        if(recovering&&(seconds<pressure_until||available<reserve+512*B::MiB)){
            previous=B::reduced();previous.reserve=reserve;return previous;
        }
        recovering=false;
        B next;next.reserve=reserve;
        if(large){
            next.allowance=std::min({(available-reserve)/4,total/16,768*B::MiB});
            next.descriptors=stable(rounded(next.allowance*(resource_cache?9:12)/16/(16*1024),8192,65536),previous.descriptors);
            next.texel_views=stable(rounded(next.allowance*(resource_cache?3:4)/16/1024,8192,131072),previous.texel_views);
            next.resource_bytes=stable(std::clamp<uint64_t>(next.allowance/4,16*B::MiB,192*B::MiB),previous.resource_bytes);
            next.resource_entries=rounded(next.resource_bytes/4096,2048,32768);
        }
        previous=next;return next;
    }
};
