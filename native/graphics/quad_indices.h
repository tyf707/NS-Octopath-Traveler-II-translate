#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

// Expand complete NVN quads in submission order. Incomplete trailing vertices
// do not form a primitive. Original vertex IDs and winding are retained.
inline std::vector<uint32_t> quad_indices(const void* source, size_t count, unsigned type, uint32_t first=0) {
    if(type>2)throw std::invalid_argument("Invalid quad index type");
    if(count/4>std::numeric_limits<uint32_t>::max()/6 ||
       (!source && count && uint64_t(first)+count-1>std::numeric_limits<uint32_t>::max()))
        throw std::overflow_error("Quad index range overflow");
    auto at=[&](size_t i){
        if(!source)return uint32_t(first+i);
        const auto* bytes=static_cast<const uint8_t*>(source);
        if(type==0)return uint32_t(bytes[i]);
        if(type==1){uint16_t value;memcpy(&value,bytes+i*2,2);return uint32_t(value);}
        uint32_t value;memcpy(&value,bytes+i*4,4);return value;
    };
    std::vector<uint32_t> result;result.reserve(count/4*6);
    for(size_t i=0;i+3<count;i+=4){
        const uint32_t a=at(i),b=at(i+1),c=at(i+2),d=at(i+3);
        result.insert(result.end(),{a,b,c,a,c,d});
    }
    return result;
}
