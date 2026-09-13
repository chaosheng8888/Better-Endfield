// BetterEndfield Scene Exporter — S2 minimal skeleton.
//
// 第一阶段目标（只验证链路，不导网格）：游戏内按热键(默认 F9)，在 Unity 主线程
// 枚举“当前已加载场景”里所有激活的 Renderer(渲染器)，把数量和名字写到本地 txt。
// 先打通 注入 -> 主线程泵 -> 枚举对象 -> 写文件 整条路，之后再逐步加
// 网格/蒙皮/材质贴图，最后序列化成 glTF。
//
// 架构严格对齐 camera 模块：后台线程只负责捕获热键(置一个原子请求)，
// 一切 Unity 对象读写都在 hook 到的每帧主线程方法里执行。

#include <BetterEndfield/ModuleApi.h>
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace BetterEndfield::Exporter {
namespace {

constexpr char kModuleId[] = "betterendfield.exporter";

struct MethodContract {
    const char* key;
    BE_MethodDescriptorV1 descriptor;
    void* pointer = nullptr;
    const void* method_info = nullptr;
    bool resolved = false;
};

const BE_HostApiV1* g_host = nullptr;

MethodContract g_contracts[]{
    // 每帧都在 Unity 主线程被调用的终末地相机方法，当作我们的“主线程执行泵”。
    {"pump",
        {"Gameplay.Beyond.dll", "Beyond.Gameplay.View", "CameraMono",
            "_ProcessDitherByPitch", nullptr, "System.Void", 0}},
    // 静态方法：Object.FindObjectsOfType(Type) -> Object[]，一次枚举某类全部对象。
    {"object.find_all_of_type",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Object",
            "FindObjectsOfType", "System.Type", "UnityEngine.Object[]", 1}},
    // 实例属性 getter：Object.get_name() -> String。
    {"object.get_name",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Object",
            "get_name", nullptr, "System.String", 0}},
    // 数组长度：Array.GetLength(int dimension) -> int。
    {"array.get_length",
        {"mscorlib.dll", "System", "Array",
            "GetLength", "System.Int32", "System.Int32", 1}},
    // 数组取元素：Array.GetValue(int index) -> object。
    {"array.get_value",
        {"mscorlib.dll", "System", "Array",
            "GetValue", "System.Int32", "System.Object", 1}},
};

using PumpFn = void(__fastcall*)(void* instance, void* method);
PumpFn g_original_pump = nullptr;

void* g_renderer_type = nullptr;   // UnityEngine.Renderer 的 System.Type 托管对象
uint32_t g_renderer_type_root = 0; // GC 保活句柄

std::atomic_bool g_export_request{false};
std::atomic_int g_hotkey{VK_F9};
std::atomic_bool g_input_stop{false};
std::thread g_input_thread;
std::atomic_bool g_contracts_ready{false};

void Log(const std::string& message) {
    if (g_host && g_host->log) {
        g_host->log(g_host->context, kModuleId, message.c_str());
    }
}

MethodContract* Contract(const char* key) {
    for (auto& c : g_contracts) {
        if (std::strcmp(c.key, key) == 0) return &c;
    }
    return nullptr;
}

void* Invoke(const MethodContract* method, void* instance, void** parameters) {
    if (!method || !method->method_info || !g_host || !g_host->runtime_invoke) {
        return nullptr;
    }
    void* exception = nullptr;
    void* result = g_host->runtime_invoke(
        g_host->context, method->method_info, instance, parameters, &exception);
    if (exception) {
        Log(std::string("Invoke raised a managed exception in ") + method->key);
        return nullptr;
    }
    return result;
}

template <typename T>
bool UnboxValue(void* boxed, T& out) {
    if (!boxed || !g_host || !g_host->object_unbox) return false;
    void* raw = g_host->object_unbox(g_host->context, boxed);
    if (!raw) return false;
    std::memcpy(&out, raw, sizeof(T));
    return true;
}

std::string ManagedToUtf8(void* managed_string) {
    if (!managed_string || !g_host || !g_host->copy_managed_string) return {};
    char buffer[1024];
    const int copied = g_host->copy_managed_string(
        g_host->context, managed_string, buffer, sizeof(buffer));
    return copied > 0 ? std::string(buffer, static_cast<size_t>(copied)) : std::string{};
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        wide.data(), needed);
    return wide;
}

// 只在 Unity 主线程调用（由 DetourPump 触发）。
void RunExportOnMainThread() {
    MethodContract* find_all = Contract("object.find_all_of_type");
    MethodContract* get_name = Contract("object.get_name");
    MethodContract* get_length = Contract("array.get_length");
    MethodContract* get_value = Contract("array.get_value");
    if (!g_renderer_type || !find_all->resolved || !get_name->resolved ||
        !get_length->resolved || !get_value->resolved) {
        Log("Export skipped: required Unity contracts are not fully resolved.");
        return;
    }

    // Renderer 是 MeshRenderer / SkinnedMeshRenderer 的共同基类，
    // 一次枚举即可拿到当前场景全部可见网格渲染器。
    void* type_arg = g_renderer_type;
    void* find_params[1]{&type_arg};
    void* renderers = Invoke(find_all, nullptr /* 静态方法无 this */, find_params);
    if (!renderers) {
        Log("FindObjectsOfType(Renderer) returned nothing.");
        return;
    }

    int32_t dimension = 0;
    void* length_params[1]{&dimension};
    int32_t count = 0;
    if (!UnboxValue(Invoke(get_length, renderers, length_params), count)) {
        Log("Array.GetLength(0) failed.");
        return;
    }

    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        int32_t index = i;
        void* index_params[1]{&index};
        void* renderer = Invoke(get_value, renderers, index_params);
        if (!renderer) {
            names.emplace_back("<null>");
            continue;
        }
        void* name_object = Invoke(get_name, renderer, nullptr);
        names.push_back(name_object ? ManagedToUtf8(name_object) : "<unnamed>");
    }

    wchar_t local_app_data[MAX_PATH];
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) {
        Log("LOCALAPPDATA environment variable is unavailable.");
        return;
    }
    const std::wstring be_root = std::wstring(local_app_data) + L"\\BetterEndfield";
    const std::wstring dir = be_root + L"\\scene-export";
    CreateDirectoryW(be_root.c_str(), nullptr); // 已存在会失败，忽略即可
    CreateDirectoryW(dir.c_str(), nullptr);

    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, _countof(path), L"%ls\\renderers_%llu.txt", dir.c_str(),
        static_cast<unsigned long long>(GetTickCount64()));
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w, ccs=UTF-8") != 0 || !file) {
        Log("Could not open the scene-export text file for writing.");
        return;
    }
    fwprintf(file, L"BetterEndfield scene export — active Renderer count: %d\n", count);
    for (int32_t i = 0; i < static_cast<int32_t>(names.size()); ++i) {
        const std::wstring wide_name = Utf8ToWide(names[static_cast<size_t>(i)]);
        fwprintf(file, L"[%d] %ls\n", i, wide_name.c_str());
    }
    fclose(file);

    char message[128];
    std::snprintf(message, sizeof(message),
        "Scene export finished: %d renderers written to scene-export.", count);
    Log(message);
}

bool IsKeyDown(int virtual_key) {
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

bool GameWindowHasFocus() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

// 后台线程：只捕获热键边沿，绝不直接碰 Unity 对象。
void InputThreadMain() {
    bool was_down = false;
    while (!g_input_stop.load(std::memory_order_acquire)) {
        const int hotkey = g_hotkey.load(std::memory_order_relaxed);
        const bool down = GameWindowHasFocus() && IsKeyDown(hotkey);
        if (down && !was_down) {
            g_export_request.store(true, std::memory_order_release);
        }
        was_down = down;
        Sleep(8);
    }
}

// 每帧主线程回调：先让游戏原本逻辑跑完，再处理导出请求。
void __fastcall DetourPump(void* instance, void* method) {
    if (g_original_pump) g_original_pump(instance, method);
    if (g_contracts_ready.load(std::memory_order_acquire) &&
        g_export_request.exchange(false, std::memory_order_acq_rel)) {
        RunExportOnMainThread();
    }
}

bool ResolveContracts() {
    for (auto& contract : g_contracts) {
        BE_ResolvedMethodV1 resolved{};
        if (g_host->resolve_method(g_host->context, &contract.descriptor, &resolved)
                == BE_Result_Ok && resolved.method_info) {
            contract.method_info = resolved.method_info;
            contract.pointer = resolved.method_pointer;
            contract.resolved = true;
        } else {
            Log(std::string("Method contract not found: ") + contract.key);
        }
    }
    const bool ready = Contract("pump")->resolved &&
                       Contract("object.find_all_of_type")->resolved &&
                       Contract("object.get_name")->resolved &&
                       Contract("array.get_length")->resolved &&
                       Contract("array.get_value")->resolved;
    g_contracts_ready.store(ready, std::memory_order_release);
    return ready;
}

BE_Result BE_CALL Initialize(const BE_HostApiV1* host) {
    if (!host || host->abi_version != BETTER_ENDFIELD_MODULE_ABI_V1 ||
        !host->resolve_method || !host->resolve_class || !host->runtime_invoke ||
        !host->object_unbox || !host->copy_managed_string || !host->create_hook ||
        !host->gchandle_new || !host->gchandle_free || !host->log) {
        return BE_Result_InvalidArgument;
    }
    g_host = host;

    ResolveContracts(); // 缺契约不致命，模块保持加载、空转，日志会列出缺哪个。

    BE_ResolvedClassV1 renderer_class{};
    if (host->resolve_class(host->context, "UnityEngine.CoreModule.dll",
            "UnityEngine", "Renderer", &renderer_class) == BE_Result_Ok &&
        renderer_class.type_object) {
        g_renderer_type = renderer_class.type_object;
        g_renderer_type_root = host->gchandle_new(host->context, g_renderer_type, 0);
    } else {
        Log("Could not resolve UnityEngine.Renderer class.");
    }

    MethodContract* pump = Contract("pump");
    if (!pump || !pump->resolved) {
        Log("Main-thread pump contract missing; exporter cannot run.");
        return BE_Result_ContractMismatch;
    }
    if (host->create_hook(host->context, kModuleId, pump->pointer,
            reinterpret_cast<void*>(&DetourPump),
            reinterpret_cast<void**>(&g_original_pump)) != BE_Result_Ok) {
        Log("Failed to install the main-thread pump hook.");
        return BE_Result_Failed;
    }

    g_input_stop.store(false, std::memory_order_release);
    g_input_thread = std::thread(InputThreadMain);
    Log("Scene Exporter ready. Focus the game and press F9 to list renderers.");
    return BE_Result_Ok;
}

BE_Result BE_CALL ConfigurationChanged(const char*) {
    return BE_Result_Ok;
}

void BE_CALL Shutdown() {
    g_input_stop.store(true, std::memory_order_release);
    if (g_input_thread.joinable()) g_input_thread.join();
    if (g_host) {
        if (g_host->release_module_hooks) {
            g_host->release_module_hooks(g_host->context, kModuleId);
        }
        if (g_renderer_type_root && g_host->gchandle_free) {
            g_host->gchandle_free(g_host->context, g_renderer_type_root);
        }
    }
    g_renderer_type_root = 0;
    g_renderer_type = nullptr;
    g_host = nullptr;
}

const BE_ModuleApiV1 kApi{
    {kModuleId, "Scene Exporter", "0.1.0", BETTER_ENDFIELD_MODULE_ABI_V1},
    &Initialize, &ConfigurationChanged, &Shutdown};

} // namespace
} // namespace BetterEndfield::Exporter

BE_EXPORT const BE_ModuleApiV1* BE_CALL BetterEndfield_GetModuleApiV1() {
    return &BetterEndfield::Exporter::kApi;
}
