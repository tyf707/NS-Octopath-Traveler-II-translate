#include "game_profile.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
namespace ot2 {
namespace {
std::atomic<int> engine_kind_value{0};
}
Engine engine_kind() {return static_cast<Engine>(engine_kind_value.load(std::memory_order_relaxed));}
void set_engine_kind(int value) {engine_kind_value.store(value < 0 || value > 2 ? 0 : value,std::memory_order_relaxed);}
namespace {
std::mutex patch_mutex;
std::string patch_list;
bool patch_list_set=false;
}
void set_patch_list(const char* csv) {
    std::lock_guard lock(patch_mutex);
    patch_list_set=true;
    patch_list=csv?csv:"";
}
bool patch_list_configured() {std::lock_guard lock(patch_mutex);return patch_list_set;}
const char* patch_list_text() {std::lock_guard lock(patch_mutex);return patch_list_set?patch_list.c_str():"(builtin defaults)";}
bool patch_enabled(const char* id,bool default_on) {
    if(!id) return false;
    std::lock_guard lock(patch_mutex);
    if(!patch_list_set) return default_on;
    const size_t wanted=std::strlen(id);
    size_t at=0;
    while(at<=patch_list.size()) {
        size_t end=patch_list.find(',',at);
        if(end==std::string::npos) end=patch_list.size();
        if(end-at==wanted && patch_list.compare(at,wanted,id)==0) return true;
        if(end==patch_list.size()) break;
        at=end+1;
    }
    return false;
}
const char* engine_name() {
    switch (engine_kind()) {case Engine::unity:return "unity";case Engine::ue4:return "ue4";default:return "generic";}
}
}
