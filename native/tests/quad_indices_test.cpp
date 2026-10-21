#include "../graphics/quad_indices.h"
#include <cassert>
#include <cstdio>

int main(){
    assert((quad_indices(nullptr,8,2,5)==std::vector<uint32_t>{5,6,7,5,7,8,9,10,11,9,11,12}));
    const uint8_t u8[]={4,2,9,7,99};
    const uint16_t u16[]={400,200,900,700};
    const uint32_t u32[]={400000,200000,900000,700000};
    assert((quad_indices(u8,5,0)==std::vector<uint32_t>{4,2,9,4,9,7}));
    assert((quad_indices(u16,4,1)==std::vector<uint32_t>{400,200,900,400,900,700}));
    assert((quad_indices(u32,4,2)==std::vector<uint32_t>{400000,200000,900000,400000,900000,700000}));
    assert(quad_indices(nullptr,3,2).empty());
    bool invalid=false;try{quad_indices(nullptr,4,3);}catch(const std::invalid_argument&){invalid=true;}assert(invalid);
    bool overflow=false;try{quad_indices(nullptr,4,2,UINT32_MAX);}catch(const std::overflow_error&){overflow=true;}assert(overflow);
    std::puts("PASS quad indices: sequential IDs, u8/u16/u32, winding, trailing vertices, range checks");
}
