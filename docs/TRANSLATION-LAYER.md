# 安卓通用转译层（Switch → Android）

本文说明当前仓库里转译层的分层、哪些已经通用、哪些必须按引擎/按游戏适配，
以及《八方旅人》（UE4）与《明亮珍珠》（Unity）如何共用同一套核心。

## 1. 结论先行

- **不能**一步把两个移植合并成“一个通用层”，因为每款游戏都有少量非通用部分
  （按标题的补丁、着色器缓存、按键映射、存档布局）。
- **但核心已经通用**：两份移植本来就是同一套 ARM64 执行器 + NVN→Vulkan 转译 + Horizon 系统服务，
  `GameProfile` 只认 `game-build.json`（title id / 模块清单 / 校验），与引擎无关。
- 本轮新增 **引擎适配层（EngineHooks）**，把“按引擎”的策略从核心里拆出来，
  让 Unity / UE4 走不同默认值，而核心代码不再出现游戏名判断。

## 2. 分层

```
┌──────────────────────────────────────────────────────────────┐
│ GameProfile（按游戏）                                          │
│   title id、ExeFS 模块清单与 sha256、game-build.json、        │
│   按标题的补丁（如 0x1C177C0 的 SystemTimeWork.SetTime）       │
├──────────────────────────────────────────────────────────────┤
│ EngineHooks（按引擎，本轮新增）                                │
│   defaultPresentInterval / supportsFrameRatePatch /           │
│   saveConvention；Unity 与 UE4 各有实现                        │
├──────────────────────────────────────────────────────────────┤
│ Core（与游戏无关，已通用）                                      │
│   loader.cpp / guest.cpp             ARM64 装载与执行           │
│   graphics/*                          NVN → Vulkan 转译        │
│   host_services.cpp / audio / input / movie / save_transaction │
│   presentation.inc                   显示与呈现                 │
└──────────────────────────────────────────────────────────────┘
```

## 3. 引擎适配器接口

`app/src/main/java/dev/ot2/port/EngineHooks.java`

| 方法 | 作用 | Unity | UE4 |
|---|---|---|---|
| `defaultPresentInterval()` | 默认出图间隔 | `1`（≤60Hz，加载按帧推进更快） | `1` |
| `supportsFrameRatePatch()` | 是否有引擎级帧率补丁 | `false` | `true`（`t.MaxFPS` CVar） |
| `saveConvention()` | 存档目录约定（诊断用） | `userdata/save` | `Saved/SaveGames` |

引擎识别顺序：

1. `game-build.json` 里的 `engine.kind`（`unity` / `ue4` / `generic`）；
2. 文件特征探测：
   - Unity：`Data/Managed`、`Data/globalgamemanagers`、`Data/StreamingAssets`
   - UE4：`Content/Paks`、`Engine`
3. 都没有 → `generic`（跟随游戏自身节奏，不做引擎假设）。

识别结果会：写入 `probe.json` 的 `engine` 字段、通过 JNI `setNativeEngineKind()`
通知 native（`native/game_profile.cpp`），并作为 `applySettings()` 的默认帧率来源。

## 4. native 侧

`native/game_profile.h` / `native/game_profile.cpp`

```cpp
enum class Engine : int { generic = 0, unity = 1, ue4 = 2 };
Engine engine_kind();          // 当前引擎
void set_engine_kind(int);     // 由 Java 启动前设置
const char* engine_name();     // "unity" / "ue4" / "generic"
```

已接入的位置（示例）：

- `guest.cpp` 的 `SETTIME_HOST` 补丁：原来硬编码 `010018e011d92000`，
  现在改为 **标题命中 或 引擎 == Unity** 时默认启用（仍可用 `debug.ot2.fixtime=0` 关闭）。
  这是“把按标题的判断提升成按引擎的判断”的第一个实际迁移点。

## 5. 八方旅人（UE4）如何接

UE4 的帧率补丁（`native/frame_rate_patch.h`，按构建 ID `0d964901…` 匹配 5 个 CVar 表）
现在由引擎适配层决定是否安装：

```cpp
if (engine == ue4) { configure(...); log("FRAMERATE_PATCH engine=ue4 build=supported|unmatched"); }
else              { log("FRAMERATE_PATCH skipped engine=unity|generic patch=none"); }
```

`EngineHooks.supportsFrameRatePatch()` / `frameRatePatchKind()`（`ue4-cvar` / `none`）
会写进 `probe.json`，接新 UE4 标题时一眼能看出补丁是否生效。


1. 打包时在 `game-build.json` 增加：
   ```json
   { "engine": { "kind": "ue4", "version": "4.27" } }
   ```
   （不写也能靠 `Content/Paks` 特征自动识别）
2. UE4 的帧率补丁继续用 `frame_rate_patch.h`（按 build id 精确匹配），
   由 `supportsFrameRatePatch()` 标记为可用；
3. 其余部分（装载、图形、服务、输入）与《明亮珍珠》完全共用，不需要改核心。

## 5b. 按标题的补丁：声明式（本轮新增）

补丁不再按标题硬编码，而是由 profile 声明、写进 `game-build.json`：

```json
{ "patches": ["settime-host"] }
```

native 侧（`native/game_profile.*`）：

```cpp
void set_patch_list(const char* csv);        // JNI setNativePatchList() 传入
bool patch_enabled(const char* id,bool default_on);
```

- 声明了 `patches` 数组 → **只启用列表里的补丁**（空数组 = 全部关闭）；
- 没声明 → 回退到内置默认（保持旧包行为），并打印
  `PATCHES configured=0 list=(builtin defaults)`；
- `debug.ot2.fixtime=0` 之类的调试属性仍然优先。

当前已迁移的补丁：

| id | 作用 | 默认（未声明时） |
|---|---|---|
| `settime-host` | `SystemTimeWork.SetTime` 用宿主时钟（珍珠命名流程） | Unity 引擎或珍珠标题时开启 |
| `fade-instant` | `Fader` 淡入淡出瞬时完成（调试用） | 关闭 |

## 5c. 服务覆盖自检（本轮新增）

启动时把游戏导出的 `_ZN2nn*` SDK 符号与宿主服务表对照，输出一行：

```
SERVICE_COVERAGE sdk_exports=15601 hooked=246 native=15355 table_entries=304
```

- `hooked`：已由宿主接管的服务数；
- `native`：跑游戏自带 SDK 代码的符号数（多数是纯计算，正常）；
- 新游戏接入时用 `adb shell setprop debug.ot2.servicecheck 1` 可额外打印
  `SERVICE_UNHOOKED` 清单（前 64 个），用于判断哪些 SDK 能力需要补宿主实现。

## 6. 仍未通用的部分（后续工作）

- **着色器缓存**：`shaders/` 下按标题的 `.code/.control/.spv/facts.json`，格式通用但内容各自生成；
- **按标题补丁**：`frame_rate_patch.h`、`SETTIME_HOST` 这类补丁仍带标题/build 判断，
  建议逐步下沉为 `patches/<title>/<name>` 的声明式描述；
- **按键映射**：目前是通用 Switch 手柄布局，个别游戏若需要特殊映射要单独配置；
- **存档格式**：`save_transaction.cpp` 只做原子写，游戏私有的存档结构仍由游戏自己管理；
- **服务覆盖矩阵**：不同游戏会用不同 Horizon 服务，缺哪个补哪个，尚无自动发现机制。

### 调试覆盖

```sh
adb shell setprop debug.ot2.engine ue4    # 强制按 UE4 适配（验证分支用）
adb shell setprop debug.ot2.engine unity  # 强制 Unity
adb shell setprop debug.ot2.engine ''     # 恢复自动探测
```

## 7. 验证状态（2026-10-21 真机回归通过）

| 检查点 | 结果 |
|---|---|
| `probe.json` | `"engine": "unity"`、`"engine_frame_rate_patch": false` |
| boot.log | `ENGINE adapter=unity` |
| 补丁 | `SETTIME_HOST installed` |
| 帧率 | `PRESENT_RATE ≈ 60.0fps` |
| 稳定性 | 无 `STOP`，画面正常 |
| UE4 分支 | `debug.ot2.engine=ue4` 强制后：`ENGINE adapter=ue4`、`FRAMERATE_PATCH engine=ue4 build=unmatched`（BDSP 构建 ID 不匹配，符合预期） |
| Unity 分支 | 默认：`engine=unity`、`FRAMERATE_PATCH skipped engine=unity patch=none`、`SETTIME_HOST installed` |

实现细节补充：

- 内容根既可能是 `game/`，也可能是 `game/RomFS/`（本仓库解包布局），探测两处都查；
- `generic` 的默认出图间隔也设为 `1`：识别失败时保持既有的 60Hz 上限，不因探测失败反而掉回 30；
- 引擎日志打在 `guest.cpp` 的 boot 阶段（`setNativeEngineKind` 早于 log 文件打开，日志会丢）。
