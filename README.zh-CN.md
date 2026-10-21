# Switch2Android

**面向 Nintendo Switch 游戏的开源 Android 兼容层与打包工具。** 用你自己的 NSP、更新和 DLC，构建可在 Android 上离线运行的应用。原版 ARM64 游戏代码原生执行，宿主提供所需系统服务，将 NVN 图形调用转换为 Vulkan；游戏着色器在构建时离线翻译为 SPIR-V。

Android 13+ · ARM64 · Vulkan · MIT · **需要自备游戏文件和密钥**

本仓库提供源码、工具和适配配置，不包含、不分发游戏程序、资源、着色器或密钥。请只使用自己合法拥有的游戏文件；生成的安装包含有游戏内容，请勿分发。

## 支持的游戏

| 游戏 | 版本 | Title ID |
|---|---|---|
| OCTOPATH TRAVELER II（八方旅人 II） | 1.1.1（更新 131072） | `0100a3501946e000` |
| TRIANGLE STRATEGY（三角战略） | 1.1.1（更新 262144） | `0100cc80140f8000` |

兼容性取决于游戏及其版本。构建需要 [profiles/](profiles/) 中有对应的适配配置。

输入格式为带 common ticket 的 NSP。不支持 XCI、NSZ、personalized ticket 和 DataPatch 包。

## 使用条件

**Android 设备：** Android 13 及以上、ARM64 处理器、支持 Vulkan，建议至少 8 GB 内存。安装《八方旅人 II》需预留 14 GB 可用存储空间，《三角战略》需预留 20 GB。不需要 root。

**构建工具：**

- Python 3.11+、Git、Make 和 C/C++ 编译器。
- JDK 21 和 CMake。
- Android SDK platform 36、NDK 28.2.13676358、SDK CMake 3.22.1 和 platform-tools。
- .NET SDK 10、Rust/Cargo 和 SPIRV-Tools（`spirv-val`）。

以下命令以 macOS 和 Homebrew 为例。先安装 Android SDK 及其命令行工具，并将 `ANDROID_HOME` 设为你的 SDK 目录：

```sh
brew install python@3.13 openjdk@21 cmake rust dotnet spirv-tools
export JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager 'platforms;android-36' 'ndk;28.2.13676358' 'cmake;3.22.1' 'platform-tools'
sdkmanager --licenses
```

确认 `dotnet --list-sdks` 包含 10.x。首次构建需要联网获取开发依赖，生成的应用可离线运行。《八方旅人 II》构建时建议预留约 60 GB 磁盘空间。

## 构建应用

不想手动配置和打包？把 NSP 目录和密钥文件的路径告诉 agent，然后让它“阅读 [INSTALL.txt](INSTALL.txt)，按步骤打包 APK 并安装到我的 Android 设备”。指南包含依赖准备、打包、安装和启动检查。

以下命令均在仓库根目录执行。游戏文件、密钥、构建工作目录和生成的安装包应放在源码目录之外。

### 1. 准备游戏文件

将本体 NSP、更新和 DLC NSP 放在同一目录。工具通过内容元数据识别文件，文件名可以自行命名。

```text
~/switch2android-input/
  base.nsp
  update.nsp
  dlc.nsp
~/keys/prod.keys
```

两款支持的游戏都需要包含各自的 1.1.1 更新，DLC 可选。

### 2. 准备工具

```sh
S2A_WORK="$HOME/switch2android-work"
S2A_OUT="$HOME/switch2android-output"
./scripts/bootstrap-tools.sh "$S2A_WORK"
.venv/bin/python switch2android.py doctor
```

准备脚本会构建固定版本的 hactool 和 repak，并将 Python 依赖安装到 `.venv`。若 `doctor` 报告 `MISSING`，请先补齐对应依赖。

### 3. 构建

```sh
.venv/bin/python switch2android.py build "$HOME/switch2android-input" \
  --keys "$HOME/keys/prod.keys" \
  --work "$S2A_WORK" --output "$S2A_OUT"
```

工具会选择最新更新及匹配的 DLC，提取并校验游戏文件，翻译着色器，生成已签名的 Release APK。首次构建耗时较长，后续构建会复用工作目录中的缓存。

| 选项 | 用途 |
|---|---|
| `--title-id 0100a3501946e000` | 输入目录包含多个游戏时，指定要构建的游戏 |
| `--version 131072` | 指定更新版本 |
| `--no-dlc` | 不打包 DLC |
| `--embed never` | 将游戏资源单独放入 `.s2a` 文件 |

输出按游戏和版本分目录存放：

```text
switch2android-output/0100a3501946e000-v131072/
  game.apk
  game.s2a
  package.json
  INSTALL.txt
```

默认将不超过 1 GiB 的资源包内嵌到 APK。较大的游戏（包括当前支持的两款游戏）生成 APK 和独立的 `.s2a` 资源文件。内嵌模式不会单独生成 `.s2a` 文件。

已有输出目录不会被覆盖。重新构建时请指定新的 `--output` 目录，`--work` 可保持不变以复用缓存。

## 安装与游玩

将生成的文件复制到 Android 设备，安装并打开 `game.apk`。若提示选择资源文件，请选择同一次构建生成的 `game.s2a`。首次启动会校验并导入资源。

也可以开启 USB 调试，连接设备后通过 ADB 安装：

```sh
adb devices
.venv/bin/python switch2android.py install \
  "$S2A_OUT/0100a3501946e000-v131072" --serial YOUR_DEVICE_SERIAL
```

这条命令会安装 APK，按需传输资源并打开应用。手工导入成功后，可删除“下载”目录中的 `.s2a` 副本以释放空间。

**更新应用：** 使用相同签名密钥构建并覆盖安装。不要先卸载，Android 会在卸载时删除应用存档。每个游戏使用独立的应用 ID 和存档。

本地构建使用 `~/.android/debug.keystore` 签名，应用本身仍是经过优化、关闭调试标记的 Release 版本。请备份这份密钥，换电脑时继续使用。未变更且已导入的资源会直接复用。

## 工具与开发

查看 NSP 元数据，或校验生成的资源包：

```sh
.venv/bin/python switch2android.py inspect "$HOME/switch2android-input" \
  --keys "$HOME/keys/prod.keys" --hactool "$S2A_WORK/tools/hactool/hactool"
.venv/bin/python switch2android.py verify \
  "$S2A_OUT/0100a3501946e000-v131072/game.s2a"
```

打包工具位于 `s2a/`，Android 应用位于 `app/`，原生运行时和 Vulkan 后端位于 `native/`，离线着色器工具位于 `tools/`。

### 着色器适配器

为 Horizon 编译的着色器会在构建时从零售数据中提取，并预先翻译为 SPIR-V。使用哪个适配器由配置的 `resource_adapter` 决定：

| 引擎 | NVN 二进制的来源 | 适配器 |
|---|---|---|
| 虚幻引擎 | `RomFS/**/*.pak` 加 `GlobalShaderCache-SF_GLSL_SWITCH.bin` | `tools/extract_shaders.py` |
| Unity | UnityFS 包内的 `Shader.compressedBlob` | `tools/extract_unity_shaders.py` |

Unity 把各平台编译好的程序存放在压缩的子程序块里。在 Switch 上这些程序就是 NVN 着色器二进制，封装格式为 `[u32 control_size][u32 code_size][control][code]`，即长度在前、控制块紧随其后；而虚幻的封装是代码在前。提取器会遍历包、解压对应平台的数据、切出每一个格式正确的二进制，并写出 `{key}.control/.code/.asm` 供离线翻译器使用。与虚幻数据一致，key 是控制块的 FNV-1a 哈希，因此能和运行时绘制时算出的 key 对上。Unity 的控制块理论上也带汇编文本，但零售版本会把它剥掉；`.asm` 为空意味着无法推断纹理与图像槽位表，翻译器会以空绑定运行。

`resource_adapter` 为 `generic` 的配置不使用任何适配器，这是着色器无法用上述方式恢复的引擎的正确设置。

运行以下检查不需要游戏文件：

```sh
source .venv/bin/activate
./scripts/check.sh
./gradlew :app:assembleRelease :app:lintRelease
dotnet run --project tools/shader_tests/ShaderRegressionTests.csproj -c Release
```

反馈问题时请附上游戏版本、设备型号、Android 版本和复现步骤，不要上传游戏文件或密钥。

## 致谢

本项目建立在以下开源工作之上，感谢各位作者和维护者：

- [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)：原生宿主架构、运行时服务、NVN 前端与离线着色器工具。
- [D2R Android](https://github.com/dodola/d2r-mobile)：Android / Vulkan 兼容与输入设计。
- [nxnative](https://github.com/seedds/nxnative)：游戏适配配置与本地打包流程。
- Ryujinx / Ryubing：独立着色器翻译库和 SPIR-V 生成器。
- deko3d、nvn.h、bcdec、LZ4、nlohmann JSON、SPIR-V Headers：图形接口、贴图处理与基础库。
- hactool、repak、SPIRV-Tools、.NET、Gradle：提取、校验与构建工具。

## 许可证

[MIT](LICENSE)。第三方许可证及来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，也随应用一同提供。
