# NS-translate-Android

**面向 Nintendo Switch 游戏的 Android 转译层与打包工具。**

用你自己的 NSP、更新和 DLC，构建可在 Android 上离线运行的应用。原版 ARM64 游戏代码原生执行，
宿主提供所需系统服务，并把 NVN 图形调用转换为 Vulkan；游戏着色器在构建时从零售数据中提取，
离线翻译为 SPIR-V。

Android 13+ · ARM64 · Vulkan · MIT · **需要自备游戏文件和密钥**

本仓库提供源码、工具和适配配置，不包含、不分发游戏程序、资源、着色器或密钥。
请只使用自己合法拥有的游戏文件；生成的安装包含有游戏内容，请勿分发。

## 支持的游戏

| 游戏 | 引擎 | 版本 | Title ID | 状态 |
|---|---|---|---|---|
| OCTOPATH TRAVELER II（八方旅人 II） | Unreal Engine 4 | 1.1.1（更新 131072） | `0100a3501946e000` | 可玩 |
| TRIANGLE STRATEGY（三角战略） | Halley（着色器容器） | 1.1.1（更新 262144） | `0100cc80140f8000` | 可玩 |
| 宝可梦 明亮珍珠 | Unity 2019.4 | 1.3.0（更新 393216） | `010018e011d92000` | 实验性 |

每个游戏都需要 [profiles/](profiles/) 下的一份配置。配置里声明着色器适配器
（`resource_adapter`）、支持的主模块构建 ID，以及按标题的补丁（`patches`）。
构建 ID 不匹配时会直接拒绝，而不是假设“大概兼容”。

输入格式为 NSP（common ticket）。不支持 XCI、NSZ、个性化 ticket 和 DataPatch。

## 这个分支做了什么

运行时核心（装载器、ARM64 执行、NVN→Vulkan 翻译、系统服务）来自 Switch2Android，
下面这些是在其之上新增的工作。

**Unity 支持**
- 从 UnityFS 的 `Shader.compressedBlob` 中提取 Switch 平台的 NVN 着色器，
  生成 `{key}.control/.code` 供离线翻译：`tools/extract_unity_shaders.py`。
- Unity 着色器身份处理（`tools/unity_shader_identity.py`）与 `resource_adapter: "unity"` 配置链路。

**引擎适配层**（`app/.../EngineHooks.java`、`native/game_profile.*`）
- 引擎身份优先取配置里的 `resource_adapter`/`engine.kind`，缺失时按内容特征探测；
  调试可用 `debug.ot2.engine=unity|ue4|generic` 强制。
- 引擎级策略集中在一处：默认出图间隔、是否存在帧率补丁（UE4 的 `t.MaxFPS` CVar）、存档目录约定。
- 按标题的补丁改为声明式（`game-build.json` 的 `"patches": [...]`），不再按标题硬编码：
  `settime-host`、`fade-instant`。
- 启动自检：`SERVICE_COVERAGE sdk_exports=… hooked=… native=…`；
  `debug.ot2.servicecheck=1` 可列出宿主未接管的 SDK 符号。

**图形快速路径**
- 整图清除改用 `vkCmdClearColorImage` / `vkCmdClearDepthStencilImage`；
  `DEPTH_STENCIL` 目标上的“只清深度”改用 `vkCmdClearAttachments`，保留 stencil。
  珍珠实测：**GPU 26.2 → 17.4 ms/帧**（城镇场景）。
- 几何缓存预算 64 MiB → 128 MiB。
- 出图间隔覆盖（`debug.ot2.interval`，默认 `1` = 最多 60Hz）与画面填充模式
  （`debug.ot2.aspect` = `fit` / `crop` / `stretch`）。

**运行稳定性**
- 只有 Activity 处于 resumed 且有焦点时才呈现；修掉切后台、surface 被销毁后宿主线程崩溃的问题。

**机内设置面板**（不用重打包、不用 ADB）
- 同时按屏幕上的 **「−」和「+」**（两指同按，或 0.7 秒内先后按下）打开面板：
  画面填充、帧率上限、虚拟按键显示。选择即时生效并会保存。

## 使用条件

**Android 设备：** Android 13 或更高、ARM64、支持 Vulkan，建议 8GB 内存。
安装所需空间：八方旅人 II 约 14GB、三角战略约 20GB、宝可梦明亮珍珠约 7GB
（游戏内容 + 翻译后的着色器缓存）。

**构建工具：**

- Python 3.11+、Git、Make 和 C/C++ 编译器。
- JDK 21 与 CMake。
- Android SDK platform 36、NDK 28.2.13676358、SDK CMake 3.22.1 与 platform-tools。
- .NET SDK 10、Rust/Cargo 与 SPIRV-Tools（`spirv-val`）。

下面命令以 macOS + Homebrew 为例。先装 Android SDK 与命令行工具，再把 `ANDROID_HOME` 指向 SDK：

```sh
brew install python@3.13 openjdk@21 cmake rust dotnet spirv-tools
export JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager 'platforms;android-36' 'ndk;28.2.13676358' 'cmake;3.22.1' 'platform-tools'
sdkmanager --licenses
```

确认 `dotnet --list-sdks` 包含 10.x。首次构建需要联网拉取依赖，构建产物离线运行。
八方旅人 II 建议预留约 60GB 构建空间（明亮珍珠约 12GB）。

## 构建

以下命令在仓库根目录执行。游戏文件、密钥、工作目录和产物都放在源码目录之外。

### 1. 准备游戏文件

把基础 NSP 以及更新 / DLC NSP 放进同一个目录。文件按内容元数据识别，文件名不影响结果。

```text
~/ns-input/
  base.nsp
  update.nsp
  dlc.nsp
~/keys/prod.keys
```

按目标游戏配置里声明的版本准备更新文件。DLC 可选。

### 2. 准备工具

```sh
S2A_WORK="$HOME/ns-work"
S2A_OUT="$HOME/ns-output"
./scripts/bootstrap-tools.sh "$S2A_WORK"
.venv/bin/python switch2android.py doctor
```

（`switch2android.py` 是打包入口，名字沿用基础项目。）脚本会构建固定版本的 hactool 与 repak，
并把 Python 依赖装进 `.venv`。`doctor` 报出的 `MISSING` 项要先解决。

### 3. 构建

```sh
.venv/bin/python switch2android.py build "$HOME/ns-input" \
  --keys "$HOME/keys/prod.keys" \
  --work "$S2A_WORK" --output "$S2A_OUT"
```

工具会选择最新更新与匹配的 DLC，解包并校验游戏文件，翻译着色器，产出签名的 Release APK。
首次构建较慢，之后复用工作目录。

| 选项 | 用途 |
|---|---|
| `--title-id 010018e011d92000` | 输入目录里有多个游戏时指定其一 |
| `--version 393216` | 指定某个更新版本 |
| `--no-dlc` | 不包含 DLC |
| `--shader-cache DIR` | 复用已验证的离线着色器集合 |
| `--embed never` | 游戏资源放在独立的 `.s2a` 文件里 |

产物输出到按游戏命名的目录：

```text
ns-output/010018e011d92000-v393216/
  game.apk
  game.s2a
  package.json
  INSTALL.txt
```

小于 1 GiB 的资源包默认嵌进 APK；更大的游戏使用「APK + `.s2a`」。
已存在的输出目录不会被覆盖；重建时换一个 `--output`，并保持同一个 `--work` 以复用缓存。

## 安装与游玩

把产物拷到设备，安装 `game.apk` 并打开；如果提示选择文件，选同一次构建的 `game.s2a`。
资源会在首次启动时校验并导入。

也可以开启 USB 调试后用 ADB：

```sh
adb devices
.venv/bin/python switch2android.py install \
  "$S2A_OUT/010018e011d92000-v393216" --serial 你的设备序列号
```

命令会安装 APK、按需传输资源并打开应用。

**空间：** 只保留一套已导入的内容。导入更新的资源包时，旧的资源目录和临时 `.s2a` 会被自动删除，
不需要手动清理。

**升级：** 用同一签名密钥覆盖安装即可，不要先卸载——卸载会清掉应用私有目录里的存档。
每个游戏有独立的应用 ID 与存档。

本地构建使用 `~/.android/debug.keystore` 签名；应用本身是优化过的、不可调试的 Release 版本。
请备份该密钥，换电脑时继续使用。

## 工具与开发

查看 NSP 元数据或校验资源包：

```sh
.venv/bin/python switch2android.py inspect "$HOME/ns-input" \
  --keys "$HOME/keys/prod.keys" --hactool "$S2A_WORK/tools/hactool/hactool"
.venv/bin/python switch2android.py verify "$S2A_OUT/010018e011d92000-v393216/game.s2a"
```

打包工具在 `s2a/`，Android 应用在 `app/`，原生运行时与 Vulkan 后端在 `native/`，
离线着色器工具在 `tools/`。

### 着色器适配器

用哪个适配器由配置里的 `resource_adapter` 决定：

| 引擎 | NVN 二进制来源 | 适配器 |
|---|---|---|
| Unreal Engine | `RomFS/**/*.pak` 与 `GlobalShaderCache-SF_GLSL_SWITCH.bin` | `tools/extract_shaders.py` |
| Unity | UnityFS 包内的 `Shader.compressedBlob` | `tools/extract_unity_shaders.py` |
| Halley | `shaders.dat`（`HALLEYPKG`） | 由离线工具处理 |

Unity 把各平台编译好的程序放在压缩的子程序块里。Switch 上的程序是 NVN 着色器二进制，
框架为 `[u32 control_size][u32 code_size][control][code]`；提取器会遍历包、解压 Switch 平台的数据、
切出每个格式正确的二进制并写出 `{key}.control/.code/.asm`。key 是控制块的 FNV-1a 哈希，
与运行时在绘制时算出的值一致。零售版会剥离汇编文本，因此 `.asm` 为空时纹理与图像槽位表无法推断，
翻译器会以空绑定运行。

只改元数据时优先用 `--shader-cache DIR`：直接复用已验证的着色器集合，不必重新遍历数 GB 的 RomFS。

### 运行时开关

```sh
adb shell setprop debug.ot2.aspect stretch     # fit | crop | stretch（默认）
adb shell setprop debug.ot2.interval 1         # 最多 60Hz（默认）；-1 = 跟随游戏（30Hz）
adb shell setprop debug.ot2.engine unity       # 强制引擎适配：unity | ue4 | generic
adb shell setprop debug.ot2.servicecheck 1     # 列出宿主未接管的 SDK 符号
adb shell setprop debug.ot2.fixtime 0          # 关闭 settime-host 补丁
```

机内设置面板里的选择按应用保存，优先级高于内置默认值。

### 无游戏文件时的检查

```sh
source .venv/bin/activate
./scripts/check.sh
./gradlew :app:assembleRelease :app:lintRelease
dotnet run --project tools/shader_tests/ShaderRegressionTests.csproj -c Release
```

反馈问题时请附上游戏版本、设备型号、Android 版本和复现步骤。不要附带游戏文件或密钥。

## 已知限制（宝可梦 明亮珍珠）

- 屋内场景最重：27–35fps（GPU 约 23ms/帧）。轻场景 58–60fps，城镇 40–47fps。
- 过场是按帧推进的。热缓存过场约 5.8 秒；首次进某区域或读档需要 15–27 秒。
- 16:9 画面默认拉伸铺满。`fit` 恢复黑边；`crop` 铺满但会裁掉上下边缘。

## 致谢

基于 n06ody 的 [Switch2Android](https://github.com/n06ody/switch2android) 核心
（ARM64 执行、NVN→Vulkan 翻译、Horizon 系统服务、打包流程），其本身又建立在以下开源工作之上：

- [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)：原生宿主架构、运行时服务、NVN 前端与离线着色器工具。
- [D2R Android](https://github.com/dodola/d2r-mobile)：Android/Vulkan 兼容与输入设计。
- [nxnative](https://github.com/seedds/nxnative)：游戏配置与本地打包流程。
- Ryujinx / Ryubing：独立着色器翻译器与 SPIR-V 生成。
- deko3d、nvn.h、bcdec、LZ4、nlohmann JSON 与 SPIR-V Headers：图形接口、纹理处理与基础库。
- hactool、repak、SPIRV-Tools、.NET 与 Gradle：解包、校验与构建工具。

## 许可证

[MIT](LICENSE)。第三方许可证与署名收集在 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，
并随应用一起分发。
