#include "host_services.h"
#include "input.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include "thread_services.h"
#ifdef __ANDROID__
#include <jni.h>
#endif
#include <atomic>
#include <cstring>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <mutex>
namespace {
std::mutex pad_mutex;uint64_t buttons{},presses{};int32_t lx{},ly{},rx{},ry{};std::atomic<int64_t> sampling{};std::atomic<bool> input_trace{false};
struct PadState {int64_t sample;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,padding;};
static_assert(sizeof(PadState)==40);
using R=uint64_t*;
uint64_t pad(R r){auto& out=*reinterpret_cast<PadState*>(r[0]);out={};out.sample=++sampling;
#ifdef __ANDROID__
    /* debug.ot2.trace=1 打开输入日志（不需要改 Java）。 */
    /* debug.ot2.trace=1 enables input logging without touching Java. */
    static std::atomic<int> trace_checked{0};
    if(!input_trace.load() && !trace_checked.exchange(1)){char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.ot2.trace",v)>0)input_trace.store(true);}
#endif
    if(*reinterpret_cast<const uint32_t*>(r[1])==0x20){std::lock_guard lock(pad_mutex);out.attributes=3;out.buttons=buttons|presses;if(input_trace && out.buttons)ot2_host_log("HID_PAD sample=%lld buttons=%llx",(long long)out.sample,(unsigned long long)out.buttons);presses=0;out.lx=lx;out.ly=ly;out.rx=rx;out.ry=ry;}return 0;}
uint64_t absent_pad(R r){auto& out=*reinterpret_cast<PadState*>(r[0]);out={};out.sample=++sampling;return 0;}
uint64_t pad_states(R r){
    if(!r[0] || static_cast<int32_t>(r[1])<=0)return 0;
    uint64_t args[2]={r[0],r[2]};
    pad(args);
    return 1;
}
// Fixed capacity: at most 16 guest fingers and four pending gestures per
// finger. Moves coalesce; a quick down/up remains Start then End across reads.
struct TouchPoint {int64_t delta;uint32_t attributes,id;int32_t x,y,dx,dy,angle,reserved;};
struct TouchState {int64_t sample;int32_t count,reserved;TouchPoint points[16];};
static_assert(sizeof(TouchPoint)==0x28 && sizeof(TouchState)==0x290);
struct Gesture {int host_id{},x{},y{};bool held{},reported{};};
struct Finger {std::array<Gesture,4> queue{};int count{};};
std::array<Finger,16> fingers;
std::mutex touch_mutex;
std::atomic<uint64_t> display_size{(uint64_t(1280)<<32)|720};
int64_t touch_sample{},last_touch_time{};
uint64_t read_touch(R r) {
    auto& out=*reinterpret_cast<TouchState*>(r[0]);out={};
    std::lock_guard lock(touch_mutex);
    auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto delta=last_touch_time?std::max<int64_t>(0,now-last_touch_time):0;last_touch_time=now;
    out.sample=++touch_sample;
    for(unsigned id=0;id<fingers.size();++id){
        auto& finger=fingers[id];if(!finger.count)continue;
        auto& g=finger.queue[0];auto& point=out.points[out.count++];
        point={delta,g.reported?(g.held?0u:2u):1u,id,g.x,g.y,30,30,0,0};g.reported=true;
        if(point.attributes==2){for(int i=1;i<finger.count;++i)finger.queue[i-1]=finger.queue[i];--finger.count;}
    }
    if(input_trace&&out.count)ot2_host_log("HID_TOUCH sample=%lld count=%d first=%u,%u,%d,%d",(long long)out.sample,out.count,out.points[0].id,out.points[0].attributes,out.points[0].x,out.points[0].y);
    return 0;
}
uint64_t read_touches(R r){if(int32_t(r[1])<=0)return 0;read_touch(r);return 1;}
uint64_t nothing(R){return 0;}
uint64_t vibration_absent(R r){*reinterpret_cast<bool*>(r[0])=false;return 0;}
uint64_t vibration_batch(R r){
    // Permission is disabled for this host (the same capability as D2R).
    // The original SDK can still enqueue a two-device batch during a scene
    // change, independently of the public handle enumeration result. Apply
    // the disabled-output policy at this final sink as well as SendVibrationValue.
    // Completing a suppressed request must not create a Horizon HID session.
    if(int32_t(r[2])<0)ot2_host_die("Invalid vibration batch count");
    static std::atomic<bool> logged{false};
    if(r[2] && !logged.exchange(true))ot2_host_log("HID_VIBRATION_SUPPRESSED count=%d permitted=0",int32_t(r[2]));
    return 0;
}
}
namespace ot2 {
void touch_display_size(uint32_t w,uint32_t h){if(w&&h)display_size.store((uint64_t(w)<<32)|h,std::memory_order_relaxed);}
void touch_event(TouchAction action,int id,float x,float y,int w,int h){
    if(id<0||w<=0||h<=0||!std::isfinite(x)||!std::isfinite(y))return;
    const auto size=display_size.load(std::memory_order_relaxed);
    double sw=size>>32,sh=uint32_t(size),scale=std::min(w/sw,h/sh);
    const int dw=std::max(1,int(sw*scale)),dh=std::max(1,int(sh*scale)),dx=(w-dw)/2,dy=(h-dh)/2;
    if(action==TouchAction::down&&(x<dx||y<dy||x>=dx+dw||y>=dy+dh))return;
    int px=std::min(1279,int(std::clamp((x-dx)/dw,0.0f,1.0f)*1280)),py=std::min(719,int(std::clamp((y-dy)/dh,0.0f,1.0f)*720));
    std::lock_guard lock(touch_mutex);
    Gesture* active=nullptr;
    for(auto& f:fingers)for(int i=0;i<f.count;++i)if(f.queue[i].held&&f.queue[i].host_id==id)active=&f.queue[i];
    if(action==TouchAction::down){
        if(active)return;
        Finger* target=nullptr;
        for(auto& f:fingers)if(f.count&&f.queue[f.count-1].host_id==id){if(f.count==4)return;target=&f;break;}
        if(!target)for(auto& f:fingers)if(!f.count){target=&f;break;}
        if(!target)for(auto& f:fingers)if(f.count<4&&!f.queue[f.count-1].held){target=&f;break;}
        if(target)target->queue[target->count++]={id,px,py,true,false};
    }else if(active){active->x=px;active->y=py;if(action==TouchAction::up)active->held=false;}
}
void touch_cancel(){
    std::lock_guard lock(touch_mutex);
    for(auto& f:fingers){
        if(f.count&&f.queue[0].reported){f.count=1;f.queue[0].held=false;}
        else f.count=0;
    }
}
}
#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativeTouch(JNIEnv*,jclass,jint action,jint id,jfloat x,jfloat y,jint w,jint h){if(action>=0&&action<=2)ot2::touch_event(ot2::TouchAction(action),id,x,y,w,h);}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_cancelNativeTouch(JNIEnv*,jclass){ot2::touch_cancel();}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativeInputTrace(JNIEnv*,jclass,jboolean enabled){input_trace=enabled;}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_clearNativePad(JNIEnv*,jclass){std::lock_guard lock(pad_mutex);buttons=presses=0;lx=ly=rx=ry=0;ot2::touch_cancel();}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativePad(JNIEnv*,jclass,jlong value,jint x,jint y,jint z,jint w) {
    std::lock_guard lock(pad_mutex);lx=x;ly=y;rx=z;ry=w;
    uint64_t next=uint64_t(value)|(x<-16383?1ULL<<16:0)|(y>16383?1ULL<<17:0)|(x>16383?1ULL<<18:0)|(y<-16383?1ULL<<19:0)
        |(z<-16383?1ULL<<20:0)|(w>16383?1ULL<<21:0)|(z>16383?1ULL<<22:0)|(w<-16383?1ULL<<23:0);
    presses|=next&~buttons;buttons=next;
}
#endif
namespace ot2 {
const std::vector<HostService>& input_services(){static const std::vector<HostService> table={
    {"_ZN2nn3hid15GetNpadStyleSetERKj",[](R r)->uint64_t{return *reinterpret_cast<const uint32_t*>(r[0])==0x20?2:0;}},
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_21NpadHandheldLarkStateERKj",pad},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",pad_states},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_21NpadHandheldLarkStateEiRKj",pad_states},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",absent_pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",absent_pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",absent_pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",absent_pad},
    {"_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",nothing},
    {"_ZN2nn3hid20IsVibrationPermittedEv",nothing},
    {"_ZN2nn3hid24IsVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE",nothing},
    {"_ZN2nn3hid6detail20IsVibrationPermittedEPb",vibration_absent},
    {"_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE",vibration_absent},
    {"_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi",vibration_batch},
    {"_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE",nothing},
    {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE",nothing},
    {"_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE",[](R r)->uint64_t{memset(reinterpret_cast<void*>(r[0]),0,8);return 0;}},
    {"_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",nothing},
    {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE",nothing},
    {"_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE",nothing},
    {"_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",nothing},
    {"_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",nothing},
    {"_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",read_touch},
    {"_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi",read_touches},
    {"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE",[](R r)->uint64_t{memset(reinterpret_cast<void*>(r[0]),0,40);*reinterpret_cast<int64_t*>(r[0])=++sampling;return 0;}},
    // This SDK copies three 16-byte pairs (detail function at sdk+0x217d40).
    {"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",[](R r)->uint64_t{memset(reinterpret_cast<void*>(r[0]),0,48);*reinterpret_cast<int64_t*>(r[0])=++sampling;return 0;}},
    {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",[](R r)->uint64_t{auto* out=reinterpret_cast<uint32_t*>(r[0]);if(out){out[0]=1;out[1]=0x20;}return 0;}},
};return table;}
}
