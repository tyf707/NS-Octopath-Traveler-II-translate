#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace ot2 {
// OT2 1.1.1 only. These are runtime UE4 settings, not clock multipliers or
// executable patches. Source/verified CVar identities: docs/frame-rate-policy.md.
// The caller holds the guest mapping lock for the entire validate/write batch.
class FrameRatePatch {
    uintptr_t base=0;size_t size=0;
public:
    static constexpr std::array<uint8_t,32> supported_build={
        0x0d,0x96,0x49,0x01,0x13,0x12,0xf6,0x2e,0x93,0xf6,0xa5,0x67,0x3a,0x09,0xd7,0xe9};
    enum Result {unsupported,not_ready,invalid,unchanged,changed};
    void configure(uintptr_t address,size_t length,const std::array<uint8_t,32>& build){
        base=build==supported_build?address:0;size=length;
    }
    // 供引擎适配层报告：本构建是否有匹配的 CVar 表。
    bool supported() const {return base!=0;}
    // Memory exposes resolve(address, length, write): nullptr for unmapped or
    // protected memory. No pointer from the original game is trusted unchecked.
    template<class Memory> Result apply(Memory& memory,int fps){
        if(!base)return unsupported;
        if(fps!=30&&fps!=60)return invalid;
        constexpr std::array<size_t,5> offsets={0x075ad0e0,0x06f017e8,0x076fef68,0x076fe648,0x076d1398};
        std::array<uint32_t*,5> addresses{};
        std::array<std::array<uint32_t,2>,5> old{};
        for(size_t i=0;i<offsets.size();++i){
            const auto offset=offsets[i];
            if(size<8||offset>size-8||base>UINTPTR_MAX-offset)return invalid;
            auto* source=memory.resolve(base+offset,8,false);if(!source)return invalid;
            uintptr_t pointer;memcpy(&pointer,source,8);if(!pointer)return not_ready;
            const size_t extra=i==3?0x7a4:0;
            if(pointer>UINTPTR_MAX-extra)return invalid;pointer+=extra;
            if(pointer%4||pointer>UINTPTR_MAX-8)return invalid;
            auto* destination=memory.resolve(pointer,8,true);if(!destination)return invalid;
            addresses[i]=static_cast<uint32_t*>(destination);
            // Each UE CVar has separate game/render-thread copies; 32-bit
            // accesses match the guest's own aligned accesses without tearing.
            for(size_t j=0;j<2;++j)old[i][j]=__atomic_load_n(addresses[i]+j,__ATOMIC_RELAXED);
        }
        for(size_t i=0;i<5;++i)for(size_t j=0;j<2;++j){
            auto word=old[i][j];float value=std::bit_cast<float>(word);
            if(i==0&&word>5)return invalid;
            if(i==1&&word>1)return invalid;
            if(i==2&&(!std::isfinite(value)||value<0||value>240))return invalid;
            if(i==3&&((j==0&&word!=7)||(j==1&&(!std::isfinite(value)||value<20||value>240))))return invalid;
            if(i==4&&(!std::isfinite(value)||value<1||value>100))return invalid;
            for(size_t k=0;k<i;++k)if(reinterpret_cast<uintptr_t>(addresses[i])<reinterpret_cast<uintptr_t>(addresses[k])+8&&reinterpret_cast<uintptr_t>(addresses[k])<reinterpret_cast<uintptr_t>(addresses[i])+8)return invalid;
        }
        const auto rate=std::bit_cast<uint32_t>(float(fps));
        const auto budget=std::bit_cast<uint32_t>(fps==60?16.666666f:33.33f);
        const uint32_t sync=fps==60?1:2,vsync=fps==60?0:1;
        const std::array<std::array<uint32_t,2>,5> desired={{{sync,sync},{vsync,vsync},{rate,rate},{7,rate},{budget,budget}}};
        bool wrote=false;
        // No writes until every pointer and every field has passed validation.
        for(size_t i=0;i<5;++i)for(size_t j=0;j<2;++j)if(old[i][j]!=desired[i][j]){
            __atomic_store_n(addresses[i]+j,desired[i][j],__ATOMIC_RELAXED);wrote=true;
        }
        return wrote?changed:unchanged;
    }
};
}
