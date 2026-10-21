#pragma once
// 引擎类型：核心转译层（ARM64 执行 / NVN→Vulkan / 系统服务）与游戏无关，
// 引擎相关策略（帧率、加载节奏、存档目录等）通过这里的“引擎适配器”注入。
// Java 侧在启动前根据 game-build.json 的 engine.kind 或文件特征探测后调用 set_engine_kind()。
namespace ot2 {
enum class Engine : int { generic = 0, unity = 1, ue4 = 2 };
Engine engine_kind();
void set_engine_kind(int value);
const char* engine_name();

// 按标题的补丁开关：由 game-build.json 的 "patches" 数组声明（逗号分隔传入）。
// 未声明时退回内置默认值，保证已有包行为不变。
void set_patch_list(const char* csv);
bool patch_enabled(const char* id, bool default_on);
bool patch_list_configured();
const char* patch_list_text();
}
