#include "text_input.h"
#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
extern "C" void ot2_host_log(const char*,...);
extern "C" uint64_t ot2_call_guest_address(uint64_t,const uint64_t*);
namespace ot2 {
namespace {
constexpr uint64_t cancelled=0x29f;
std::mutex mutex;std::condition_variable changed;bool available=false;uint64_t next_id=0;
struct Pending {TextInputRequest request;bool done=false,accepted=false;std::u16string result;};
std::shared_ptr<Pending> pending;
template<class T>T read(const uint8_t* data,size_t offset){T out;memcpy(&out,data+offset,sizeof(out));return out;}
std::u16string read_text(const uint8_t* data,size_t count){std::u16string text;for(size_t i=0;i<count;i++){auto c=read<char16_t>(data,i*2);if(!c)break;text+=c;}return text;}
/* 自动化钩子：debug.ot2.autoname 非空时，键盘请求直接用该名字完成，
 * 等同用户在键盘里输入后按确定（走同一条 complete_text_input 路径）。
 * 仅用于无人值守复现，默认未设置时完全不生效。
 * Automation hook: when debug.ot2.autoname is non-empty, complete the keyboard
 * request with that name, exactly as a user typing it and pressing OK would
 * (same complete path). Inactive unless the property is set. */
bool auto_name(std::u16string& out){
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX]{};
    if(__system_property_get("debug.ot2.autoname",value)<=0)return false;
    for(const char* p=value;*p;p++)out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*p)));
    return !out.empty();
#else
    (void)out;return false;
#endif
}
// Encode whole Unicode code points only, leaving room for NUL. Invalid UTF-16
// is replaced, never read past the caller's capacity or split surrogate pairs.
std::string utf8(const std::u16string& text,size_t capacity){std::string out;
    for(size_t i=0;i<text.size();i++){uint32_t c=text[i];if(c>=0xd800&&c<=0xdbff&&i+1<text.size()&&text[i+1]>=0xdc00&&text[i+1]<=0xdfff){c=0x10000+((c-0xd800)<<10)+(text[++i]-0xdc00);}else if(c>=0xd800&&c<=0xdfff)c=0xfffd;
        char bytes[4];size_t n=0;if(c<0x80){bytes[n++]=c;}else if(c<0x800){bytes[n++]=0xc0|(c>>6);bytes[n++]=0x80|(c&63);}else if(c<0x10000){bytes[n++]=0xe0|(c>>12);bytes[n++]=0x80|((c>>6)&63);bytes[n++]=0x80|(c&63);}else{bytes[n++]=0xf0|(c>>18);bytes[n++]=0x80|((c>>12)&63);bytes[n++]=0x80|((c>>6)&63);bytes[n++]=0x80|(c&63);}
        if(out.size()+n>=capacity)break;out.append(bytes,n);
    }return out;
}
}
std::optional<TextInputRequest> pending_text_input(){std::lock_guard lock(mutex);if(!pending||pending->done)return {};return pending->request;}
bool complete_text_input(uint64_t id,bool accepted,const std::u16string& text){
    std::lock_guard lock(mutex);if(!pending||pending->done||pending->request.id!=id)return false;
    if(accepted&&(text.size()<size_t(pending->request.minimum)||text.size()>size_t(pending->request.maximum)))return false;
    pending->accepted=accepted;pending->result=text;pending->done=true;changed.notify_all();return true;
}
void set_text_input_available(bool value){std::lock_guard lock(mutex);available=value;if(!value&&pending){pending->done=true;pending->accepted=false;}changed.notify_all();}
uint64_t show_keyboard(uint64_t* registers){
    const auto* arg=reinterpret_cast<const uint8_t*>(registers[1]);auto* output=reinterpret_cast<uint8_t*>(registers[0]);if(!arg||!output)return cancelled;
    ot2_host_log("KEYBOARD enter utf8=%u check=%u callback=%llx capacity=%llu",arg[0x3bd],arg[0x3d0],(unsigned long long)read<uintptr_t>(arg,0x3d8),(unsigned long long)read<uint64_t>(output,8));
    // Verified against OT2 SDK ShowKeyboard 0x39df50 and SetInitialTextUtf8
    // 0x39ecd0; these offsets match the D2R protocol, not its character rules.
    auto request=std::make_shared<Pending>();auto& r=request->request;
    r.title=read_text(arg+0x24,65);r.subtitle=read_text(arg+0xa6,129);r.guide=read_text(arg+0x1a8,257);
    r.maximum=std::clamp(read<int32_t>(arg,0x3ac),0,1002);if(!r.maximum)r.maximum=1002;
    r.minimum=std::clamp(read<int32_t>(arg,0x3b0),0,r.maximum);
    auto work=read<uintptr_t>(arg,0x4d0);auto work_size=read<uint64_t>(arg,0x4d8);auto offset=read<int32_t>(arg,0x3c0),length=read<int32_t>(arg,0x3c4);
    if(work&&offset>=0&&length>0&&length<=500&&uint64_t(offset)<=work_size&&uint64_t(length)*2<=work_size-offset)r.initial=read_text(reinterpret_cast<const uint8_t*>(work)+offset,length);
retry:
    {std::unique_lock lock(mutex);if(!available||pending)return cancelled;r.id=++next_id;request->done=false;request->accepted=false;pending=request;changed.notify_all();
     std::u16string name;if(auto_name(name)){request->accepted=true;request->result=name;request->done=true;changed.notify_all();}
     changed.wait(lock,[&]{return request->done;});pending.reset();}
    ot2_host_log("KEYBOARD response id=%llu accepted=%u units=%zu",(unsigned long long)r.id,request->accepted,request->result.size());
    if(!request->accepted)return cancelled;
    auto* buffer=read<uint8_t*>(output,0);auto capacity=read<uint64_t>(output,8);if(!buffer||!capacity)return 0;
    // SDK GetRequiredStringBufferSize returns 0x7d4. Truncate by whole code
    // points as D2R does, also honoring a smaller caller-provided capacity.
    capacity=std::min<uint64_t>(capacity,0x7d4);
    std::vector<uint8_t> candidate(capacity);size_t bytes_used=0;
    if(arg[0x3bd]){auto bytes=utf8(request->result,capacity);bytes_used=bytes.size();memcpy(candidate.data(),bytes.data(),bytes_used);}
    else if(capacity>=2){auto count=std::min(request->result.size(),size_t(capacity/2-1));if(count&&request->result[count-1]>=0xd800&&request->result[count-1]<=0xdbff)--count;bytes_used=count*2;memcpy(candidate.data(),request->result.data(),bytes_used);}
    if(arg[0x3d0] && read<uintptr_t>(arg,0x3d8)){
        // SDK GetInteractiveOutStorageCallback passes the check work buffer,
        // an output message length, and a String whose size is candidate bytes
        // (not the caller's capacity). Never hold the UI mutex across guest code.
        struct String {uint8_t* data;uint64_t size;} text{candidate.data(),bytes_used};
        uint64_t message_size=0,args[8]={read<uintptr_t>(arg,0x4e0),reinterpret_cast<uint64_t>(&message_size),reinterpret_cast<uint64_t>(&text)};
        auto check=ot2_call_guest_address(read<uintptr_t>(arg,0x3d8),args);
        ot2_host_log("KEYBOARD check result=%llu message_bytes=%llu",(unsigned long long)check,(unsigned long long)message_size);
        if(check){
            // Nonzero is not success. Reopen for editing rather than silently
            // accepting Bad/Prompt/Silent; optional prompt override is not exposed.
            r.initial=request->result;r.guide=u"名称未通过游戏校验，请修改后重试。";
            goto retry;
        }
    }
    memcpy(buffer,candidate.data(),capacity);
    ot2_host_log("KEYBOARD return success");return 0;
}
}
