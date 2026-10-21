#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <cstring>

namespace ot2::movie {
// ByteBuffer output from MediaCodec: OMX planar (19) or semiplanar (21).
// Reject opaque/vendor layouts instead of interpreting tiled memory as pixels.
struct YuvLayout {int width{},height{},stride{},slice{},left{},top{},right{},bottom{},format{},standard{1},range{2};};
inline bool decoder_output_supported(int type,int output,int mode){
    if(type>=1&&type<=3)return (mode==2&&output==3)||(mode==1&&(output==2||output==4));
    return (type==4||type==5)&&mode==1&&output==1;
}
// SDK DecoderOutputFormat 4 copies Y followed by interleaved UV (subsdk0
// DecoderImpl::ConvertDecoderOutputFormat, branch +0x6c). Normalize Android's
// padded planar/semiplanar buffers to that packed NV12 layout, without RGB loss.
inline bool nv12(std::span<const uint8_t> source,const YuvLayout& p,std::span<uint8_t> out){
    if(p.width<=0||p.height<=0||p.stride<p.width||p.slice<p.height||p.stride>16384||p.slice>16384||
       p.left<0||p.top<0||p.right<p.left||p.bottom<p.top||p.right>=p.width||p.bottom>=p.height||
       (p.format!=19&&p.format!=21)||(p.format==21&&p.stride<2*((p.width+1)/2)))return false;
    const size_t w=p.right-p.left+1,h=p.bottom-p.top+1;
    if((w|h|size_t(p.left)|size_t(p.top))&1)return false; // No implicit chroma-phase resampling.
    const size_t ys=size_t(p.stride)*p.slice,cs=size_t((p.stride+1)/2)*((p.slice+1)/2);
    const size_t needed=p.format==19?ys+2*cs:ys+size_t(p.stride)*((p.slice+1)/2);
    if(source.size()<needed||out.size()<w*h+w*(h/2))return false;
    for(size_t y=0;y<h;++y)memcpy(out.data()+y*w,source.data()+(y+p.top)*p.stride+p.left,w);
    for(size_t y=0;y<h/2;++y)for(size_t x=0;x<w/2;++x){
        const size_t cx=x+p.left/2,cy=y+p.top/2,d=w*h+y*w+2*x;
        if(p.format==19){const auto s=cy*((p.stride+1)/2)+cx;out[d]=source[ys+s];out[d+1]=source[ys+cs+s];}
        else{const auto s=ys+cy*p.stride+2*cx;out[d]=source[s];out[d+1]=source[s+1];}
    }
    return true;
}
inline bool rgba(std::span<const uint8_t> source,const YuvLayout& p,std::span<uint8_t> out,bool bgra=false){
    if(p.width<=0||p.height<=0||p.stride<p.width||p.slice<p.height||p.stride>16384||p.slice>16384||
       p.left<0||p.top<0||p.right<p.left||p.bottom<p.top||p.right>=p.width||p.bottom>=p.height||
       (p.format!=19&&p.format!=21)||
       (p.format==21&&p.stride<2*((p.width+1)/2)))return false;
    const size_t ys=size_t(p.stride)*p.slice,cs=size_t((p.stride+1)/2)*((p.slice+1)/2);
    const size_t needed=p.format==19?ys+2*cs:ys+size_t(p.stride)*((p.slice+1)/2);
    const size_t w=p.right-p.left+1,h=p.bottom-p.top+1;
    if(source.size()<needed||out.size()<w*h*4)return false;
    auto clamp=[](int v){return uint8_t(std::clamp((v+128)>>8,0,255));};
    for(size_t y=0;y<h;++y)for(size_t x=0;x<w;++x){
        const size_t px=x+p.left,py=y+p.top;
        const int Y=source[py*p.stride+px];int U,V;
        if(p.format==19){size_t uv=(py/2)*((p.stride+1)/2)+px/2;U=source[ys+uv]-128;V=source[ys+cs+uv]-128;}
        else{size_t uv=ys+(py/2)*p.stride+(px/2)*2;U=source[uv]-128;V=source[uv+1]-128;}
        const bool full=p.range==1,bt709=p.standard==1;
        const int luma=full?256*Y:298*(Y-16);
        const int R=luma+(full?(bt709?403:359):(bt709?459:409))*V;
        const int G=luma-(full?(bt709?48:88):(bt709?55:100))*U-(full?(bt709?120:183):(bt709?136:208))*V;
        const int B=luma+(full?(bt709?475:454):(bt709?541:516))*U;
        auto* d=out.data()+(y*w+x)*4;d[bgra?2:0]=clamp(R);d[1]=clamp(G);d[bgra?0:2]=clamp(B);d[3]=255;
    }
    return true;
}
}
