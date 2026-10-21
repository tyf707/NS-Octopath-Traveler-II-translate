package dev.ot2.port;

import java.util.Locale;

/** 游戏引擎类型。核心转译层与引擎无关，引擎差异由 EngineHooks 适配。 */
enum EngineKind {
    UNITY, UE4, GENERIC;

    static EngineKind parse(String value) {
        if (value == null) return null;
        switch (value.toLowerCase(Locale.ROOT)) {
            case "unity": case "unity3d": return UNITY;
            case "ue4": case "unreal": case "unrealengine": return UE4;
            case "generic": case "unknown": return GENERIC;
            default: return null;
        }
    }

    /** 打包期 resource_adapter（unreal/unity/halley/generic）→ 引擎类型。 */
    static EngineKind parseAdapter(String adapter) {
        if (adapter == null) return null;
        switch (adapter.toLowerCase(Locale.ROOT)) {
            // 只有能确定引擎的适配器才作为引擎身份；generic/halley 表示“无引擎信息”，
            // 交给文件特征探测，避免把 Unity 游戏误判成 generic。
            case "unreal": return UE4;
            case "unity": return UNITY;
            default: return null;
        }
    }

    /** 传给 native（0=generic 1=unity 2=ue4）。 */
    int nativeId() {
        switch (this) { case UNITY: return 1; case UE4: return 2; default: return 0; }
    }

    String label() { return name().toLowerCase(Locale.ROOT); }
}
