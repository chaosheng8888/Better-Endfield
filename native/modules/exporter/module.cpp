// BetterEndfield Scene Exporter — S3 minimal link test (v0.2.1: fix reference-type Type arg passing).
//
// S3 目标（只验证链路，不导网格）：游戏内按热键(F9)，在 Unity 主线程枚举
// “当前已加载场景”里指定类型的全部对象，把数量和名字写到本地 txt。
// 先打通 注入 -> 主线程泵 -> 静态枚举 -> 数组遍历取名字 -> 写文件 整条路。
//
// ★S3 最小验证选 UnityEngine.Camera（全场景只有 1~2 个、最轻），而不是 Renderer
//   （大世界上万个、FindObjectsOfType 全局重遍历，在 LateUpdate 渲染回调里重入易崩）。
//   Camera 链路跑通后，S4 再换成 Renderer/Mesh，并用分帧或更安全的 hook 点处理重枚举。
//
// ★所有 IL2CPP/托管调用都走 Safe* 封装（__try/__except SEH 兜底，对齐 model 模块）：
//   单次坏调用只记日志、不再把整个游戏踢崩，可反复按 F9 调试。
//
// 架构严格对齐 camera 模块：后台线程只捕获热键(置原子请求)，一切 Unity 对象
// 读写都在 hook 到的每帧主线程方法里执行。

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
    // 数组长度：Array.GetLength(int dimension) -> int。程序集名必须带 .dll。
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

// S3 最小验证目标：UnityEngine.Camera 的 System.Type 托管对象（S4 再换 Renderer）。
void* g_target_type = nullptr;
uint32_t g_target_type_root = 0; // GC 保活句柄

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

// ---------------------------------------------------------------------------
// SEH 安全封装：以下三个函数只使用裸指针/POD，不含需要析构的 C++ 对象，
// 因此可以用 __try/__except 包住 IL2CPP 调用，访问违例时返回失败而不是崩游戏。
// ---------------------------------------------------------------------------

// 安全调用 il2cpp_runtime_invoke。managed_exc 输出托管异常，fault 表示原生访问违例。
void* SafeRuntimeInvoke(const void* method_info, void* instance, void** parameters,
                        void** managed_exc, bool* fault) {
    *fault = false;
    if (managed_exc) *managed_exc = nullptr;
    if (!g_host || !g_host->runtime_invoke || !method_info) {
        *fault = true;
        return nullptr;
    }
    __try {
        return g_host->runtime_invoke(g_host->context, method_info, instance,
                                      parameters, managed_exc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return nullptr;
    }
}

// 安全拆箱值类型对象到 out（拷贝 bytes 字节）。
bool SafeUnbox(void* boxed, void* out, size_t bytes, bool* fault) {
    *fault = false;
    if (!boxed || !g_host || !g_host->object_unbox || !out) {
        *fault = true;
        return false;
    }
    __try {
        void* raw = g_host->object_unbox(g_host->context, boxed);
        if (!raw) return false;
        std::memcpy(out, raw, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return false;
    }
}

// 安全把托管字符串拷成 UTF-8，返回字节数。
int SafeCopyName(void* managed_string, char* buffer, size_t capacity, bool* fault) {
    *fault = false;
    if (!managed_string || !g_host || !g_host->copy_managed_string ||
        !buffer || capacity == 0) {
        *fault = true;
        return 0;
    }
    __try {
        return g_host->copy_managed_string(g_host->context, managed_string,
                                           buffer, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return 0;
    }
}

// 统一的托管方法调用入口：SEH 兜底 + 托管异常判断，任何失败都返回 nullptr 且不崩。
void* Invoke(const MethodContract* method, void* instance, void** parameters) {
    if (!method || !method->method_info) return nullptr;
    void* managed_exc = nullptr;
    bool fault = false;
    void* result = SafeRuntimeInvoke(method->method_info, instance, parameters,
                                     &managed_exc, &fault);
    if (fault) {
        Log(std::string("[SEH] native fault caught while invoking ") + method->key);
        return nullptr;
    }
    if (managed_exc) {
        Log(std::string("Invoke raised a managed exception in ") + method->key);
        return nullptr;
    }
    return result;
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
    if (!g_target_type || !find_all->resolved || !get_name->resolved ||
        !get_length->resolved || !get_value->resolved) {
        Log("Export skipped: required contracts/type are not fully resolved.");
        return;
    }

    // 第 1 步：静态枚举当前场景全部 Camera（S3 最小、最轻）。
    Log("export step 1/4: FindObjectsOfType(Camera) ...");
    // IL2CPP 调用约定：引用类型参数(如 System.Type 这种类对象)在参数槽里【直接放托管对象指针】；
    // 只有 int/bool/struct 等值类型才传“指向值的指针(&)”。作者 model 模块 GetComponentsInChildren
    // 就是 parameters[]{type_object, &bool} 这样写且已跑通。之前多包一层 &type_arg 会让运行时
    // 把栈地址当成对象解引用而 native fault。
    void* find_params[1]{ g_target_type };
    void* objects = Invoke(find_all, nullptr /* 静态方法无 this */, find_params);
    if (!objects) {
        Log("step 1 failed or returned nothing (FindObjectsOfType).");
        return;
    }
    Log("step 1 ok: got the managed array.");

    // 第 2 步：取数组长度。
    int32_t dimension = 0;
    void* length_params[1]{&dimension};
    int32_t count = 0;
    bool unbox_fault = false;
    void* length_boxed = Invoke(get_length, objects, length_params);
    if (!SafeUnbox(length_boxed, &count, sizeof(count), &unbox_fault)) {
        Log(unbox_fault ? "[SEH] native fault reading Array.GetLength."
                       : "step 2 failed: Array.GetLength(0).");
        return;
    }
    char len_msg[96];
    std::snprintf(len_msg, sizeof(len_msg), "step 2 ok: array length = %d",
        static_cast<int>(count));
    Log(len_msg);
    if (count < 0 || count > 100000) {
        Log("step 2 aborted: implausible count, refusing to iterate.");
        return;
    }

    // 第 3 步：逐个取元素 + 取名字（每个都独立兜底，坏一个不影响其余）。
    Log("step 3/4: iterating elements and reading names ...");
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        int32_t index = i;
        void* index_params[1]{&index};
        void* element = Invoke(get_value, objects, index_params);
        if (!element) {
            names.emplace_back("<null-or-fault>");
            continue;
        }
        char name_buf[256]{};
        bool name_fault = false;
        void* name_object = Invoke(get_name, element, nullptr);
        const int copied = SafeCopyName(name_object, name_buf, sizeof(name_buf),
                                        &name_fault);
        if (name_fault) {
            names.emplace_back("<name-fault>");
        } else if (copied > 0) {
            names.emplace_back(name_buf);
        } else {
            names.emplace_back("<unnamed>");
        }
    }

    // 第 4 步：写文件到 %LOCALAPPDATA%\BetterEndfield\scene-export。
    Log("step 4/4: writing text file ...");
    wchar_t local_app_data[MAX_PATH];
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) {
        Log("LOCALAPPDATA environment variable is unavailable.");
        return;
    }
    const std::wstring be_root = std::wstring(local_app_data) + L"\\BetterEndfield";
    const std::wstring dir = be_root + L"\\scene-export";
    CreateDirectoryW(be_root.c_str(), nullptr); // 已存在会失败，忽略
    CreateDirectoryW(dir.c_str(), nullptr);

    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, _countof(path), L"%ls\\cameras_%llu.txt", dir.c_str(),
        static_cast<unsigned long long>(GetTickCount64()));
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w, ccs=UTF-8") != 0 || !file) {
        Log("Could not open the scene-export text file for writing.");
        return;
    }
    fwprintf(file, L"BetterEndfield scene export — active Camera count: %d\n", count);
    for (int32_t i = 0; i < static_cast<int32_t>(names.size()); ++i) {
        const std::wstring wide_name = Utf8ToWide(names[static_cast<size_t>(i)]);
        fwprintf(file, L"[%d] %ls\n", i, wide_name.c_str());
    }
    fclose(file);

    char done[128];
    std::snprintf(done, sizeof(done),
        "Scene export FINISHED: %d cameras written to scene-export.", count);
    Log(done);
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

    ResolveContracts(); // 缺契约不致命，模块保持加载、空转，日志列出缺哪个。

    // S3 最小验证目标：UnityEngine.Camera（轻量）。
    BE_ResolvedClassV1 target_class{};
    if (host->resolve_class(host->context, "UnityEngine.CoreModule.dll",
            "UnityEngine", "Camera", &target_class) == BE_Result_Ok &&
        target_class.type_object) {
        g_target_type = target_class.type_object;
        g_target_type_root = host->gchandle_new(host->context, g_target_type, 0);
        Log("Resolved target class: UnityEngine.Camera");
    } else {
        Log("Could not resolve UnityEngine.Camera class.");
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
    Log("Scene Exporter v0.2.1 ready (S3 minimal=Camera, ref-type arg fixed). Focus the game and press F9.");
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
        if (g_target_type_root && g_host->gchandle_free) {
            g_host->gchandle_free(g_host->context, g_target_type_root);
        }
    }
    g_target_type_root = 0;
    g_target_type = nullptr;
    g_host = nullptr;
}

const BE_ModuleApiV1 kApi{
    {kModuleId, "Scene Exporter", "0.2.1", BETTER_ENDFIELD_MODULE_ABI_V1},
    &Initialize, &ConfigurationChanged, &Shutdown};

} // namespace
} // namespace BetterEndfield::Exporter

BE_EXPORT const BE_ModuleApiV1* BE_CALL BetterEndfield_GetModuleApiV1() {
    return &BetterEndfield::Exporter::kApi;
}
