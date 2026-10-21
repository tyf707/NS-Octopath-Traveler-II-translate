#include "../host_services.h"
#include "../time_zone.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <atomic>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

extern "C" void ot2_host_log(const char*,...) {}
extern "C" [[noreturn]] void ot2_host_die(const char*,...) {throw std::runtime_error("Unexpected host failure");}
extern "C" uint64_t ot2_call_guest(const char*,const uint64_t*) {throw std::runtime_error("Unexpected guest call");}
extern "C" uint64_t ot2_call_guest_address(uint64_t,const uint64_t*) {throw std::runtime_error("Unexpected guest address call");}
extern "C" int ot2_signal_event(uint32_t){return 0;}
extern "C" void vkhost_init(){}
extern "C" uint64_t vkhost_frame_count(){return 0;}
extern "C" void* ot2_nvn_lookup(const char*){return nullptr;}
namespace ot2 {void set_audio_active(bool) {}}

static void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
template<class T> uint64_t addr(T* p){return reinterpret_cast<uint64_t>(p);}
static uint64_t call(const char* name,std::initializer_list<uint64_t> values){
    uint64_t r[8]{};std::copy(values.begin(),values.end(),r);
    for(const auto& s:ot2::host_services())if(!strcmp(s.name,name))return s.call(r);
    throw std::runtime_error(std::string("Missing service: ")+name);
}
constexpr const char* load="_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE";
constexpr const char* convert="_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE";
struct Calendar {int16_t year;uint8_t month,day,hour,minute,second,pad;};
struct Info {uint32_t weekday,yearday;char zone[8];uint32_t dst;int32_t offset;};
using Rule=std::array<uint8_t,0x4000>;
template<class T> struct Guard {uint64_t before=0x123456789abcdef0;T value{};uint64_t after=0xfedcba9876543210;bool intact()const{return before==0x123456789abcdef0&&after==0xfedcba9876543210;}};
static Rule rule(const char* name){Rule r{};std::array<char,36> location{};strncpy(location.data(),name,35);require(!call(load,{addr(r.data()),addr(location.data())}),"load named timezone");return r;}
static Info at(const Rule& r,int64_t seconds,int hour,int minute=0){Calendar c{};Info i{};require(!call(convert,{addr(&c),addr(&i),addr(&seconds),addr(r.data())}),"convert supplied rule");require(c.hour==hour&&c.minute==minute,"calendar must use the supplied rule, not the process timezone");return i;}
static std::vector<uint8_t> synthetic(const char* footer){
    std::vector<uint8_t> file;
    // Two RFC 9636 TZif headers and fixed UTC data blocks; the v2 footer
    // supplies the recurrence being tested independently of the OS database.
    for(int i=0;i<2;i++){std::array<uint8_t,44> h{};memcpy(h.data(),"TZif2",5);h[39]=1;h[43]=4;file.insert(file.end(),h.begin(),h.end());file.insert(file.end(),{0,0,0,0,0,0,'U','T','C',0});}
    file.push_back('\n');file.insert(file.end(),footer,footer+strlen(footer));file.push_back('\n');return file;
}
static Rule encoded(const std::vector<uint8_t>& bytes){Rule r{};require(!ot2::time_zone_encode(r.data(),bytes),"encode synthetic TZif");return r;}
static void parser_contract(){
    auto julian=encoded(synthetic("AAA0BBB,J60/0,J300/0"));
    require(at(julian,1709164800,0).dst==0&&at(julian,1709251200,1).dst==1,"Julian rule skips February 29");
    auto ordinal=encoded(synthetic("AAA0BBB,59/0,300/0"));require(at(ordinal,1709164800,1).dst==1,"zero-based ordinal rule counts February 29");
    auto signed_time=encoded(synthetic("AAA0BBB,M3.2.0/-2,M11.1.0/26"));
    require(at(signed_time,1710021599,21,59).dst==0&&at(signed_time,1710021600,23).dst==1,"negative transition time crosses the previous day");
    require(at(signed_time,1730681999,1,59).dst==1&&at(signed_time,1730682000,1).dst==0,"transition after 24:00 crosses the next day");
    auto always=encoded(synthetic("AAA0BBB,0/0,J365/25"));for(int64_t t:{1704067199LL,1704067200LL,1735689599LL,1735689600LL}){Info i{};require(!call(convert,{0,addr(&i),addr(&t),addr(always.data())})&&i.dst==1&&i.offset==3600,"all-year DST around year boundaries");}
    auto valid=synthetic("UTC0");auto v1=std::vector<uint8_t>(valid.begin(),valid.begin()+54);v1[4]=0;require(at(encoded(v1),0,0).offset==0,"version 1 fixed zone");
    for(auto bad:{"ABC25","AB0","AAA0BBB","AAA0BBB,M13.1.0,M11.1.0","AAA0BBB,J0,J300","AAA0BBB,366,300","AAA0BBB,M3.2.0/168,M11.1.0","AAA0BBB,M3.2.0/2:60,M11.1.0"}){auto data=synthetic(bad);Guard<Rule> r;r.value.fill(0xa5);auto before=r.value;require(ot2::time_zone_encode(r.value.data(),data)!=0&&r.value==before&&r.intact(),"invalid footer rejected without output writes");}
    size_t checked=0;
    auto probe=[&](std::span<const uint8_t> bytes){Guard<Rule> r;r.value.fill(0x5a);auto before=r.value;auto result=ot2::time_zone_encode(r.value.data(),bytes);require(r.intact(),"parser cannot exceed the rule ABI");if(result)require(r.value==before,"invalid TZif preserves prior rule");else {int64_t t=1704067200;Guard<Calendar> c;Guard<Info> i;require(!call(convert,{addr(&c.value),addr(&i.value),addr(&t),addr(r.value.data())})&&c.intact()&&i.intact(),"accepted mutated rule remains bounded and convertible");}checked++;};
    for(size_t n=0;n<valid.size();n++)probe(std::span<const uint8_t>(valid.data(),n));
    for(size_t n=0;n<valid.size();n++)for(unsigned bit=0;bit<8;bit++){auto mutation=valid;mutation[n]^=1u<<bit;probe(mutation);}
    std::cout<<"PASS TZif v1/v2, Julian/ordinal/signed/all-year recurrence and "<<checked<<" parser truncation/mutation cases\n";
}
static void core(){
    // Unity's FILETIME clock refuses to initialize unless this SDK context is
    // available, even when GetCurrentTime itself succeeds.
    struct Context { int64_t offset,steady_seconds; std::array<uint8_t,16> source; };
    static_assert(sizeof(Context)==0x20);
    constexpr const char* context="_ZN2nn4time23StandardUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE";
    Guard<Context> first,second;
    const auto before=time(nullptr);
    require(!call(context,{addr(&first.value)})&&first.intact(),"user clock context writes exactly 0x20 bytes");
    require(!call(context,{addr(&second.value)})&&second.intact(),"repeat clock context succeeds");
    require(!memcmp(&first.value,&second.value,sizeof(Context)),"unchanged clock returns a stable context");
    const auto anchor=first.value.offset+first.value.steady_seconds;
    require(anchor>=before-1&&anchor<=time(nullptr)+1,"clock context anchors real UTC seconds to monotonic time");
    require(std::any_of(first.value.source.begin(),first.value.source.end(),[](auto b){return b!=0;}),"host clock has a nonzero source identity");
    require(call(context,{0})==uint64_t(116+(901<<9)),"null clock context is rejected");
    std::cout<<"PASS real UTC user clock context, stable source/anchor and ABI guards\n";
    auto utc=rule("UTC"),ny=rule("America/New_York"),tokyo=rule("Asia/Tokyo");
    constexpr int64_t jan=1704067200;
    require(at(utc,jan,0).offset==0,"UTC offset");
    require(at(ny,jan,19).offset==-18000,"New York winter offset");
    require(at(tokyo,jan,9).offset==32400,"Tokyo offset");
    require(at(ny,1710053999,1,59).dst==0&&at(ny,1710054000,3).dst==1,"spring DST jump at the exact transition");
    require(at(ny,1730613599,1,59).dst==1&&at(ny,1730613600,1).dst==0,"fall repeated hour distinguishes its two offsets");
    auto lord=rule("Australia/Lord_Howe"),kathmandu=rule("Asia/Kathmandu");
    require(at(lord,jan,11).offset==39600&&at(lord,1719792000,10,30).offset==37800,"southern hemisphere half-hour DST");
    require(at(kathmandu,jan,5,45).offset==20700,"quarter-hour offset");
    Guard<Calendar> c;Guard<Info> info;
    int64_t t=-1;require(!call(convert,{addr(&c.value),addr(&info.value),addr(&t),addr(utc.data())}),"negative timestamp");
    require(c.value.year==1969&&c.value.month==12&&c.value.day==31&&c.value.second==59&&info.value.weekday==3&&info.value.yearday==364&&c.intact()&&info.intact(),"calendar ABI, pre-epoch fields and guards");
    t=1709164800;require(!call(convert,{addr(&c.value),addr(&info.value),addr(&t),addr(utc.data())}),"leap date");require(c.value.year==2024&&c.value.month==2&&c.value.day==29&&info.value.yearday==59,"Gregorian leap day and zero-based yearday");
    auto packed=call("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE",{addr(&t)});require(!memcmp(&packed,&c.value,8),"UTC result returns the 8-byte CalendarTime in x0");
    require(!call(convert,{0,0,addr(&t),addr(utc.data())}),"optional calendar outputs");
    std::cout<<"PASS calendar UTC/negative/leap, northern/southern DST boundaries, fractional offsets and ABI guards\n";

    auto copy=ny;ny.fill(0);auto saved=copy;
    std::atomic<bool> good{true};std::vector<std::thread> workers;
    for(int i=0;i<8;i++)workers.emplace_back([&,i]{try{for(int n=0;n<250;n++){if(i%2)at(copy,jan,19);else at(tokyo,jan,9);}}catch(...){good=false;}});
    for(auto& w:workers)w.join();require(good&&copy==saved,"copied immutable rules are independent across threads and original destruction");
    std::cout<<"PASS copied rule value semantics and 2000 concurrent independent conversions\n";

    Guard<Rule> r;r.value.fill(0xa5);auto untouched=r.value;std::array<char,36> name{};
    for(auto bad:{"NoSuch/Zone","../etc/passwd","/etc/localtime","Asia//Tokyo","."}){name.fill(0);strncpy(name.data(),bad,35);require(call(load,{addr(r.value.data()),addr(name.data())})!=0&&r.value==untouched&&r.intact(),"invalid location preserves destination");}
    name.fill('X');require(call(load,{addr(r.value.data()),addr(name.data())})!=0&&r.value==untouched,"unterminated location rejected within 36 bytes");
    name.fill(0);memcpy(name.data(),"UTC",4);require(!call(load,{addr(r.value.data()),addr(name.data())})&&r.intact(),"load writes exactly 0x4000 bytes");
    for(auto bad:{Rule{},r.value}){if(bad==r.value)bad[40]^=1;c.value={2024,1,2,3,4,5,0};auto before=c.value;t=0;require(call(convert,{addr(&c.value),0,addr(&t),addr(bad.data())})!=0&&!memcmp(&before,&c.value,8)&&c.intact(),"unknown or corrupt rule cannot succeed or modify output");}
    for(auto extreme:{std::numeric_limits<int64_t>::min(),std::numeric_limits<int64_t>::max()}){auto before=c.value;require(call(convert,{addr(&c.value),addr(&info.value),addr(&extreme),addr(utc.data())})==uint64_t(116+(201<<9))&&!memcmp(&before,&c.value,8)&&info.intact(),"overflow has a time-domain result and preserves output");}
    require(call(convert,{0,0,0,addr(utc.data())})==uint64_t(116+(901<<9)),"null PosixTime pointer");
    Guard<std::array<char,36>> location;require(!call("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE",{addr(location.value.data())})&&location.intact(),"device location ABI");
#ifdef __ANDROID__
    char property[PROP_VALUE_MAX]{};__system_property_get("persist.sys.timezone",property);require(!strcmp(location.value.data(),*property?property:"UTC"),"device location follows Android setting");
#endif
    require(!call(load,{addr(r.value.data()),addr(location.value.data())}),"reported device location loads a real rule");
    std::cout<<"PASS failure atomicity, corrupt/overflow/null inputs, bounded location, device zone="<<location.value.data()<<'\n';
    std::cout<<"PASS independent timezone rules through actual SDK adapters\n";
}
template<class T> static T read(std::ifstream& in){T value{};require(bool(in.read(reinterpret_cast<char*>(&value),sizeof(value))),"truncated corpus");return value;}
static void corpus(const char* path){
    std::ifstream in(path,std::ios::binary);require(bool(in),"open timezone corpus");auto magic=read<std::array<char,8>>(in);require(!memcmp(magic.data(),"OT2TZC01",8),"corpus format");auto count=read<uint32_t>(in);require(count<2000,"bounded zone count");size_t total=0;
    for(unsigned k=0;k<count;k++){
        auto name=read<std::array<char,36>>(in);auto n=read<uint32_t>(in);require(n<0x4000&&name.back()==0,"bounded corpus record");std::vector<uint8_t> data(n);require(bool(in.read(reinterpret_cast<char*>(data.data()),n)),"corpus TZif");Rule r{};
        if(ot2::time_zone_encode(r.data(),data))throw std::runtime_error(std::string("TZif rejected: ")+name.data());
#ifdef __ANDROID__
        auto loaded=rule(name.data());require(loaded==r,"Android bundled timezone lookup matches the exact source TZif");
#endif
        auto samples=read<uint32_t>(in);require(samples<10000,"bounded sample count");
        for(unsigned j=0;j<samples;j++){
            auto seconds=read<int64_t>(in);auto expected=read<Calendar>(in);auto expected_info=read<Info>(in);Calendar c{};Info i{};
            auto result=call(convert,{addr(&c),addr(&i),addr(&seconds),addr(r.data())});
            if(result||memcmp(&c,&expected,8)||memcmp(&i,&expected_info,24))throw std::runtime_error(std::string("corpus mismatch ")+name.data()+" epoch="+std::to_string(seconds)+" offset="+std::to_string(i.offset)+" expected="+std::to_string(expected_info.offset)+" zone="+i.zone+" expected_zone="+expected_info.zone);
            total++;
        }
    }
    require(in.peek()==EOF,"no unused corpus data");std::cout<<"PASS timezone corpus zones="<<count<<" conversions="<<total<<" (Python zoneinfo oracle, same database)\n";
}
int main(int argc,char** argv){try{
    core();parser_contract();if(argc==3&&!strcmp(argv[1],"--corpus"))corpus(argv[2]);else require(argc==1,"usage: [--corpus file]");
}catch(const std::exception& e){std::cerr<<"FAIL time: "<<e.what()<<'\n';return 1;}}
