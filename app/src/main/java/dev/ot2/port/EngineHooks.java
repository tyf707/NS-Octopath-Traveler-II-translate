package dev.ot2.port;

import org.json.JSONObject;
import java.io.File;

/**
 * 引擎适配器：把“引擎相关”的策略从核心转译层里分离出来。
 *
 * 分层：
 *   core        —— ARM64 装载执行、NVN→Vulkan、Horizon 系统服务、输入/音频/存档（与游戏无关）
 *   EngineHooks —— 帧率策略、加载节奏、存档目录约定等（按引擎）
 *   GameProfile —— 标题 id、模块清单、按标题的补丁（按游戏）
 */
interface EngineHooks {
    EngineKind kind();

    /** 默认出图间隔：1=最多 60Hz，2=跟随游戏（通常 30Hz），-1=跟随游戏请求。 */
    int defaultPresentInterval();

    /** 引擎是否有帧率补丁可用（UE4 走 t.MaxFPS CVar，Unity 只能改出图间隔）。 */
    boolean supportsFrameRatePatch();

    /** 存档目录约定（展示/诊断用）。 */
    String saveConvention();

    /** 帧率补丁实现方式：ue4 走 CVar（t.MaxFPS），unity/none 只能改出图间隔。 */
    String frameRatePatchKind();

    // ---- 注册表 ----
    EngineHooks UNITY = new Hooks(EngineKind.UNITY, 1, false, "userdata/save", "none");
    EngineHooks UE4 = new Hooks(EngineKind.UE4, 1, true, "Saved/SaveGames", "ue4-cvar");
    EngineHooks GENERIC = new Hooks(EngineKind.GENERIC, 1, false, "unknown", "none");

    static EngineHooks current() { return Holder.installed; }

    static void install(EngineHooks hooks) { if (hooks != null) Holder.installed = hooks; }

    static EngineHooks of(EngineKind kind) {
        if (kind == EngineKind.UNITY) return UNITY;
        if (kind == EngineKind.UE4) return UE4;
        return GENERIC;
    }

    /**
     * 优先读 game-build.json 的 engine.kind；没有就按文件特征探测。
     * Unity: Data/Managed、Data/globalgamemanagers、Data/StreamingAssets
     * UE4:   Content/Paks、Engine
     */
    static EngineKind detect(File gameRoot, JSONObject manifest) {
        // 调试覆盖：adb shell setprop debug.ot2.engine ue4（排查引擎适配用）。
        EngineKind forced = EngineKind.parse(systemProperty("debug.ot2.engine"));
        if (forced != null) return forced;
        // 1) 显式 engine.kind
        JSONObject engine = manifest == null ? null : manifest.optJSONObject("engine");
        if (engine != null) {
            EngineKind declared = EngineKind.parse(engine.optString("kind", null));
            if (declared != null) return declared;
        }
        // 2) 复用打包期的 resource_adapter（shader 适配器选择），保持“引擎身份”单一来源
        if (manifest != null) {
            EngineKind fromAdapter = EngineKind.parseAdapter(manifest.optString("resource_adapter", null));
            if (fromAdapter != null) return fromAdapter;
        }
        if (gameRoot != null) {
            // 内容根既可能是 game/，也可能是 game/RomFS/（本仓库的解包布局）。
            File[] bases = { gameRoot, new File(gameRoot, "RomFS") };
            for (File base : bases) {
                if (new File(base, "Data/Managed").isDirectory()
                        || new File(base, "Data/globalgamemanagers").isFile()
                        || new File(base, "Data/StreamingAssets").isDirectory()) return EngineKind.UNITY;
            }
            for (File base : bases) {
                if (new File(base, "Content/Paks").isDirectory()
                        || new File(base, "Engine").isDirectory()) return EngineKind.UE4;
            }
        }
        return EngineKind.GENERIC;
    }

    static String systemProperty(String key) {
        try {
            Class<?> type = Class.forName("android.os.SystemProperties");
            return (String) type.getMethod("get", String.class).invoke(null, key);
        } catch (Throwable unavailable) {
            return null;
        }
    }

    final class Holder { static volatile EngineHooks installed = GENERIC; }

    final class Hooks implements EngineHooks {
        private final EngineKind kind;
        private final int interval;
        private final boolean frameRatePatch;
        private final String save;
        private final String patchKind;
        Hooks(EngineKind kind, int interval, boolean frameRatePatch, String save, String patchKind) {
            this.kind = kind; this.interval = interval; this.frameRatePatch = frameRatePatch; this.save = save; this.patchKind = patchKind;
        }
        @Override public EngineKind kind() { return kind; }
        @Override public int defaultPresentInterval() { return interval; }
        @Override public boolean supportsFrameRatePatch() { return frameRatePatch; }
        @Override public String saveConvention() { return save; }
        @Override public String frameRatePatchKind() { return patchKind; }
    }
}
