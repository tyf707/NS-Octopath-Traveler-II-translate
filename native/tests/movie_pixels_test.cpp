#include "../movie_pixels.h"
#include <array>
#include <cstdlib>
#include <cstdio>
void require(bool x){if(!x)std::abort();}
int main(){
    require(ot2::movie::decoder_output_supported(2,4,1)); // VP8 -> CPU NV12 (Unity title).
    require(ot2::movie::decoder_output_supported(1,3,2)); // Existing NVN output.
    require(ot2::movie::decoder_output_supported(4,1,1)); // Existing audio PCM.
    require(!ot2::movie::decoder_output_supported(2,4,2));
    require(!ot2::movie::decoder_output_supported(4,4,1));
    require(!ot2::movie::decoder_output_supported(9,4,1));
    std::array<uint8_t,24> planar{};
    planar[0]=10;planar[1]=11;planar[4]=20;planar[5]=21;
    planar[16]=30;planar[20]=40;
    ot2::movie::YuvLayout cpu{2,2,4,4,0,0,1,1,19,1,2};
    std::array<uint8_t,8> packed{};packed.fill(0xa5);
    require(ot2::movie::nv12(planar,cpu,std::span(packed).subspan(1,6)));
    require((packed==std::array<uint8_t,8>{0xa5,10,11,20,21,30,40,0xa5}));
    planar[17]=40;cpu.format=21;
    packed.fill(0xa5);require(ot2::movie::nv12(planar,cpu,std::span(packed).subspan(1,6)));
    require((packed==std::array<uint8_t,8>{0xa5,10,11,20,21,30,40,0xa5}));
    auto unchanged=packed;
    require(!ot2::movie::nv12(std::span(planar).first(23),cpu,packed)&&packed==unchanged);
    require(!ot2::movie::nv12(planar,cpu,std::span(packed).first(5))&&packed==unchanged);
    cpu.format=0x7fa30c04;require(!ot2::movie::nv12(planar,cpu,packed)&&packed==unchanged);
    cpu.format=21;cpu.left=1;require(!ot2::movie::nv12(planar,cpu,packed)&&packed==unchanged);
    cpu.left=0;cpu.right=2;require(!ot2::movie::nv12(planar,cpu,packed)&&packed==unchanged);
    // A padded 4x4 image cropped to its central 2x2. Chroma remains addressed
    // against the coded image, not the visible rectangle.
    std::array<uint8_t,48> src{};std::fill(src.begin(),src.end(),128);
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)src[y*8+x]=235;
    ot2::movie::YuvLayout p{4,4,8,4,1,1,2,2,21,1,2};std::array<uint8_t,16> dst{};
    require(ot2::movie::rgba(src,p,dst));for(auto b:dst)require(b==255);
    src[1*8+1]=16;require(ot2::movie::rgba(src,p,dst));require(dst[0]==0&&dst[1]==0&&dst[2]==0&&dst[3]==255);
    require(!ot2::movie::rgba(std::span(src).first(47),p,dst));
    p.format=0x7fa30c04;require(!ot2::movie::rgba(src,p,dst));
    p.format=19;p.right=4;require(!ot2::movie::rgba(src,p,dst));p.right=2;
    require(ot2::movie::rgba(src,p,dst));
    std::array<uint8_t,6> red{81,81,81,81,90,240};std::array<uint8_t,16> rgb{},bgr{};
    p={2,2,2,2,0,0,1,1,19,2,2};require(ot2::movie::rgba(red,p,rgb));require(ot2::movie::rgba(red,p,bgr,true));
    require(rgb[0]>250&&rgb[1]<3&&rgb[2]<3&&bgr[0]==rgb[2]&&bgr[2]==rgb[0]);
    // Interleaved chroma needs an even-sized pair even for an odd image width.
    std::array<uint8_t,11> odd{};std::array<uint8_t,24> odd_rgb{};
    p={3,2,3,2,0,0,2,1,21,2,2};require(!ot2::movie::rgba(odd,p,odd_rgb));
    std::puts("movie pixel layout, crop, bounds and color order: PASS");
}
