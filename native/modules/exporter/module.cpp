// BetterEndfield Scene Exporter — v0.5.0 (S4-2 fix: BakeMesh readable probe across all Chen lod0 parts, diagnose sharedMesh.isReadable; hotkey Ctrl+Shift+E).
//
// S3 目标（只验证链路，不导网格）：游戏内按组合热键(Ctrl+Shift+E)，在 Unity 主线程枚举
// “当前已加载场景”里指定类型的全部对象，把数量和名字写到本地 txt。
// 先打通 注入 -> 主线程泵 -> 静态枚举 -> 数组遍历取名字 -> 写文件 整条路。
//
// ★S3 最小验证选 UnityEngine.Camera（全场景只有 1~2 个、最轻），而不是 Renderer
//   （大世界上万个、FindObjectsOfType 全局重遍历，在 LateUpdate 渲染回调里重入易崩）。
//   Camera 链路跑通后，S4 再换成 Renderer/Mesh，并用分帧或更安全的 hook 点处理重枚举。
//
// ★所有 IL2CPP/托管调用都走 Safe* 封装（__try/__except SEH 兜底，对齐 model 模块）：
//   单次坏调用只记日志、不再把整个游戏踢崩，可反复按 Ctrl+Shift+E 调试。
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
#include <utility>
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
    // S4-1 实例属性 getter：SkinnedMeshRenderer.get_sharedMesh() -> Mesh（无参）。
    {"skinned.get_shared_mesh",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer",
            "get_sharedMesh", nullptr, "UnityEngine.Mesh", 0}},
    // S4-1 实例属性 getter：Mesh.get_vertexCount() -> int（无参，值类型返回需 unbox）。
    {"mesh.get_vertex_count",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "get_vertexCount", nullptr, "System.Int32", 0}},
    // S4-2 实例属性 getter：Mesh.get_vertices() -> UnityEngine.Vector3[]（无参，值类型结构体数组）。
    {"mesh.get_vertices",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "get_vertices", nullptr, "UnityEngine.Vector3[]", 0}},
    // S4-2 修复：Mesh 构造函数（object_new 分配后必须调 .ctor 原生初始化才能用）。
    {"mesh.ctor",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            ".ctor", nullptr, "System.Void", 0}},
    // S4-2 诊断：Mesh.isReadable（运行时网格常为 false，导致 .vertices 返回空数组）。
    {"mesh.is_readable",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "get_isReadable", nullptr, "System.Boolean", 0}},
    // S4-2 修复：SkinnedMeshRenderer.BakeMesh(Mesh) 把当前蒙皮姿态烘焙进一个可读 Mesh。
    {"skinned.bake_mesh",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer",
            "BakeMesh", "UnityEngine.Mesh", "System.Void", 1}},
};

using PumpFn = void(__fastcall*)(void* instance, void* method);
PumpFn g_original_pump = nullptr;

// S3 验证目标：UnityEngine.Camera 的 System.Type 托管对象。
void* g_target_type = nullptr;
uint32_t g_target_type_root = 0; // GC 保活句柄
// S4-1 目标：UnityEngine.SkinnedMeshRenderer 的 System.Type 托管对象。
void* g_skinned_type = nullptr;
uint32_t g_skinned_type_root = 0;
// S4-2：UnityEngine.Mesh 的原生类信息（class_info，用于 object_new 新建烘焙网格；元数据不需 GC 保活）。
void* g_mesh_class_info = nullptr;

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

// 只在 Unity 主线程调用（由 DetourPump 触发）。导出相机（S3 已跑通，作为“管道存活”哨兵）。
void ExportCameras() {
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

// ===========================================================================
// S4 第一层：枚举当前场景全部 SkinnedMeshRenderer（蒙皮网格，主要是在场角色/怪物，
// 数量可控，不会像静态 MeshFilter 那样上万），对每个：get_sharedMesh 拿到网格，
// 再 get_vertexCount 读顶点数，写“名字 + 顶点数”。验证 渲染器→共享网格→几何规模 这条数据链。
// 所有 IL2CPP 调用都走 Invoke/SafeUnbox/SafeCopyName（内部 SEH 兜底），本函数可正常用 C++ 容器。
// ===========================================================================
void ExportSkinnedMeshes() {
    MethodContract* find_all = Contract("object.find_all_of_type");
    MethodContract* get_name = Contract("object.get_name");
    MethodContract* get_length = Contract("array.get_length");
    MethodContract* get_value = Contract("array.get_value");
    MethodContract* get_mesh = Contract("skinned.get_shared_mesh");
    MethodContract* get_vcount = Contract("mesh.get_vertex_count");
    if (!g_skinned_type || !find_all->resolved || !get_name->resolved ||
        !get_length->resolved || !get_value->resolved ||
        !get_mesh->resolved || !get_vcount->resolved) {
        Log("Skinned export skipped: required contracts/type not fully resolved.");
        return;
    }

    Log("skinned step 1/4: FindObjectsOfType(SkinnedMeshRenderer) ...");
    void* find_params[1]{ g_skinned_type }; // System.Type 引用类型，直接放对象指针
    void* objects = Invoke(find_all, nullptr /* 静态 */, find_params);
    if (!objects) { Log("skinned step 1 failed or nothing."); return; }

    int32_t dimension = 0;
    void* len_params[1]{&dimension};
    int32_t count = 0;
    bool len_fault = false;
    void* len_boxed = Invoke(get_length, objects, len_params);
    if (!SafeUnbox(len_boxed, &count, sizeof(count), &len_fault)) {
        Log("skinned step 2 failed: Array.GetLength.");
        return;
    }
    char lm[96];
    std::snprintf(lm, sizeof(lm), "skinned step 2 ok: SkinnedMeshRenderer count = %d",
        static_cast<int>(count));
    Log(lm);
    if (count < 0 || count > 200000) {
        Log("skinned step 2 aborted: implausible count.");
        return;
    }

    // 单帧处理上限，防意外数量卡顿；总数仍如实写出。
    const int32_t kProcessCap = 2000;
    const int32_t process = count < kProcessCap ? count : kProcessCap;

    struct Row { std::string name; int32_t verts; bool has_mesh; };
    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(process));
    long long total_verts = 0;

    Log("skinned step 3/4: reading sharedMesh and vertexCount per renderer ...");
    for (int32_t i = 0; i < process; ++i) {
        int32_t index = i;
        void* index_params[1]{&index};
        void* renderer = Invoke(get_value, objects, index_params);
        Row row;
        row.verts = 0;
        row.has_mesh = false;
        if (!renderer) { row.name = "<null>"; rows.push_back(std::move(row)); continue; }

        char nb[256]{};
        bool nf = false;
        void* name_obj = Invoke(get_name, renderer, nullptr);
        if (!nf && SafeCopyName(name_obj, nb, sizeof(nb), &nf) > 0) row.name = nb;
        else row.name = "<unnamed>";

        void* mesh = Invoke(get_mesh, renderer, nullptr); // 实例、无参
        if (mesh) {
            row.has_mesh = true;
            int32_t vc = 0;
            bool vf = false;
            void* vc_boxed = Invoke(get_vcount, mesh, nullptr); // 实例、无参，返回 boxed int
            if (SafeUnbox(vc_boxed, &vc, sizeof(vc), &vf)) {
                row.verts = vc;
                total_verts += vc;
            }
        }
        rows.push_back(std::move(row));
    }

    Log("skinned step 4/4: writing text file ...");
    wchar_t local_app_data[MAX_PATH];
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return;
    const std::wstring be_root = std::wstring(local_app_data) + L"\\BetterEndfield";
    const std::wstring dir = be_root + L"\\scene-export";
    CreateDirectoryW(be_root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, _countof(path), L"%ls\\skinned_meshes_%llu.txt", dir.c_str(),
        static_cast<unsigned long long>(GetTickCount64()));
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w, ccs=UTF-8") != 0 || !file) {
        Log("skinned: cannot open output file.");
        return;
    }
    fwprintf(file,
        L"BetterEndfield SkinnedMeshRenderer count: %d (processed %d), total vertices: %lld\n",
        count, process, total_verts);
    for (int32_t i = 0; i < static_cast<int32_t>(rows.size()); ++i) {
        const std::wstring wn = Utf8ToWide(rows[static_cast<size_t>(i)].name);
        if (rows[i].has_mesh)
            fwprintf(file, L"[%d] %ls  verts=%d\n", i, wn.c_str(), rows[i].verts);
        else
            fwprintf(file, L"[%d] %ls  (no sharedMesh)\n", i, wn.c_str());
    }
    fclose(file);

    char done[160];
    std::snprintf(done, sizeof(done),
        "Skinned export FINISHED: %d renderers processed, total vertices=%lld.",
        static_cast<int>(rows.size()), total_verts);
    Log(done);
}

// 热键总入口（只在主线程）：先导相机（哨兵，证明基础管道活着），再导蒙皮网格（S4 第一层），
// 最后做陈千语顶点坐标探针（S4 第二层最小验证）。所有 IL2CPP 调用内部已有 SEH 兜底。
// ===========================================================================
// S4-2：UnityEngine.Vector3 = 3 个紧排 float（x/y/z，unbox 后即字段起点，共 12 字节）。
// ===========================================================================
struct BE_Vec3 { float x, y, z; };

// 读托管一维数组长度，失败返回 -1（dimension 固定 0）。
int32_t ArrayLengthOf(MethodContract* get_length, void* arr) {
    if (!arr) return -1;
    int32_t dimension = 0;
    void* p[1]{&dimension};
    int32_t len = 0;
    bool fault = false;
    if (!SafeUnbox(Invoke(get_length, arr, p), &len, sizeof(len), &fault)) return -1;
    return len;
}

// 顶点坐标探针（v0.5.0）：收集所有 S_actor_chen_*_lod0 部件，逐个对照
// sharedMesh（直接读；运行时网格常 isReadable=false → .vertices 返回空）与 BakeMesh 烘焙出的可读网格，
// 写“每部件对照表”，并对第一个烘焙出非空顶点的部件完整读 xyz、算 AABB、抽样坐标验证正确性。
void ExportVertexProbe() {
    MethodContract* find_all = Contract("object.find_all_of_type");
    MethodContract* get_name = Contract("object.get_name");
    MethodContract* get_length = Contract("array.get_length");
    MethodContract* get_value = Contract("array.get_value");
    MethodContract* get_mesh = Contract("skinned.get_shared_mesh");
    MethodContract* get_vcount = Contract("mesh.get_vertex_count");
    MethodContract* get_verts = Contract("mesh.get_vertices");
    MethodContract* m_ctor = Contract("mesh.ctor");
    MethodContract* m_readable = Contract("mesh.is_readable");
    MethodContract* bake = Contract("skinned.bake_mesh");
    if (!g_skinned_type || !g_mesh_class_info ||
        !find_all->resolved || !get_name->resolved || !get_length->resolved || !get_value->resolved ||
        !get_mesh->resolved || !get_vcount->resolved || !get_verts->resolved ||
        !m_ctor->resolved || !m_readable->resolved || !bake->resolved) {
        Log("vertex-probe skipped: contracts/type not ready.");
        return;
    }
    static const char* kPrefix = "S_actor_chen_";
    static const char* kLodTag = "_lod0";
    const size_t kPrefixLen = std::strlen(kPrefix);
    const int32_t kPartCap = 20;
    const int32_t kVertCap = 80000;

    const DWORD t0 = GetTickCount();
    Log("vertex-probe: collecting all Chen lod0 parts ...");
    void* find_params[1]{ g_skinned_type };
    void* objs = Invoke(find_all, nullptr, find_params);
    if (!objs) { Log("vertex-probe: enumerate failed."); return; }
    const int32_t total = ArrayLengthOf(get_length, objs);
    if (total < 0) { Log("vertex-probe: length failed."); return; }

    struct Part { void* renderer; std::string name; };
    std::vector<Part> parts;
    for (int32_t i = 0; i < total && static_cast<int32_t>(parts.size()) < kPartCap; ++i) {
        int32_t idx = i;
        void* ip[1]{&idx};
        void* r = Invoke(get_value, objs, ip);
        if (!r) continue;
        char nb[256]{};
        bool nf = false;
        void* no = Invoke(get_name, r, nullptr);
        if (!nf && SafeCopyName(no, nb, sizeof(nb), &nf) > 0) {
            if (std::strncmp(nb, kPrefix, kPrefixLen) == 0 && std::strstr(nb, kLodTag))
                parts.push_back(Part{ r, nb });
        }
    }
    if (parts.empty()) { Log("vertex-probe: no S_actor_chen_*_lod0 part loaded."); return; }
    {
        char m[128];
        std::snprintf(m, sizeof(m), "vertex-probe: %d Chen lod0 parts collected.",
            static_cast<int>(parts.size()));
        Log(m);
    }

    // 新建一个可复用的烘焙目标 Mesh，并调原生构造函数。
    void* baked = g_host->object_new(g_host->context, g_mesh_class_info);
    if (!baked) { Log("vertex-probe: object_new(Mesh) failed."); return; }
    Invoke(m_ctor, baked, nullptr);
    const uint32_t baked_handle = g_host->gchandle_new(g_host->context, baked, 0);

    struct Row { std::string name; int32_t svc, slen, blen; bool readable; };
    std::vector<Row> rows;
    bool sampled = false;
    std::string sample_name;
    std::vector<BE_Vec3> spts;
    BE_Vec3 smn{0, 0, 0}, smx{0, 0, 0};
    int sfail = 0, sbvc = 0;

    for (size_t pi = 0; pi < parts.size(); ++pi) {
        void* r = parts[pi].renderer;
        void* shared = Invoke(get_mesh, r, nullptr);
        int32_t svc = -1, slen = -1;
        bool readable = false;
        if (shared) {
            bool vf = false; int32_t vc = 0;
            if (SafeUnbox(Invoke(get_vcount, shared, nullptr), &vc, sizeof(vc), &vf)) svc = vc;
            bool rf = false, rd = false;
            if (SafeUnbox(Invoke(m_readable, shared, nullptr), &rd, sizeof(rd), &rf)) readable = rd;
            slen = ArrayLengthOf(get_length, Invoke(get_verts, shared, nullptr));
        }
        // 把当前蒙皮姿态烘焙进 baked（每次覆盖）。
        void* bake_params[1]{ baked };
        Invoke(bake, r, bake_params);
        int32_t blen = ArrayLengthOf(get_length, Invoke(get_verts, baked, nullptr));
        int32_t bvc = -1;
        { bool f = false; int32_t v = 0;
          if (SafeUnbox(Invoke(get_vcount, baked, nullptr), &v, sizeof(v), &f)) bvc = v; }
        rows.push_back(Row{ parts[pi].name, svc, slen, blen, readable });

        if (!sampled && blen > 0) {
            sampled = true;
            sample_name = parts[pi].name;
            sbvc = bvc;
            void* barr = Invoke(get_verts, baked, nullptr);
            const int32_t n = blen < kVertCap ? blen : kVertCap;
            spts.reserve(n);
            for (int32_t i = 0; i < n; ++i) {
                int32_t idx = i;
                void* vp[1]{&idx};
                BE_Vec3 p{0, 0, 0};
                bool pf = false;
                void* box = Invoke(get_value, barr, vp);
                if (!box || !SafeUnbox(box, &p, sizeof(BE_Vec3), &pf)) { ++sfail; continue; }
                if (spts.empty()) { smn = smx = p; }
                else {
                    if (p.x < smn.x) smn.x = p.x; if (p.y < smn.y) smn.y = p.y; if (p.z < smn.z) smn.z = p.z;
                    if (p.x > smx.x) smx.x = p.x; if (p.y > smx.y) smx.y = p.y; if (p.z > smx.z) smx.z = p.z;
                }
                spts.push_back(p);
            }
        }
    }
    if (baked_handle) g_host->gchandle_free(g_host->context, baked_handle);

    int shared_nonempty = 0, baked_nonempty = 0;
    for (auto& rw : rows) { if (rw.slen > 0) ++shared_nonempty; if (rw.blen > 0) ++baked_nonempty; }
    {
        char fm[300];
        std::snprintf(fm, sizeof(fm),
            "vertex-probe: parts=%d sharedNonEmpty=%d bakedNonEmpty=%d sample='%s' verts=%d fail=%d %lu ms",
            static_cast<int>(rows.size()), shared_nonempty, baked_nonempty,
            sample_name.c_str(), static_cast<int>(spts.size()), sfail,
            static_cast<unsigned long>(GetTickCount() - t0));
        Log(fm);
    }

    wchar_t local_app_data[MAX_PATH];
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return;
    const std::wstring be_root = std::wstring(local_app_data) + L"\\BetterEndfield";
    const std::wstring dir = be_root + L"\\scene-export";
    CreateDirectoryW(be_root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, _countof(path), L"%ls\\chen_vertex_probe_%llu.txt", dir.c_str(),
        static_cast<unsigned long long>(GetTickCount64()));
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w, ccs=UTF-8") != 0 || !file) {
        Log("vertex-probe: open output file fail.");
        return;
    }
    fwprintf(file, L"Chen vertex probe v0.5.0 (sharedMesh vs BakeMesh)\nparts: %d\n",
        static_cast<int>(rows.size()));
    fwprintf(file, L"%-44ls %8s %8s %10s %s\n", L"part", L"shr_vc", L"shr_len", L"bake_len", L"readable");
    for (auto& rw : rows) {
        const std::wstring wn = Utf8ToWide(rw.name);
        fwprintf(file, L"%-44ls %8d %8d %10d %s\n", wn.c_str(), rw.svc, rw.slen, rw.blen,
            rw.readable ? L"true" : L"false");
    }
    fwprintf(file, L"\nsampled baked part: %ls\n", Utf8ToWide(sample_name).c_str());
    fwprintf(file, L"baked vertexCount: %d, read verts: %d, failed: %d\n",
        sbvc, static_cast<int>(spts.size()), sfail);
    fwprintf(file, L"AABB min: %.5f %.5f %.5f\nAABB max: %.5f %.5f %.5f\nsize: %.5f %.5f %.5f\n",
        smn.x, smn.y, smn.z, smx.x, smx.y, smx.z, smx.x - smn.x, smx.y - smn.y, smx.z - smn.z);
    fwprintf(file, L"--- first 5 vertices ---\n");
    for (int i = 0; i < 5 && i < static_cast<int>(spts.size()); ++i)
        fwprintf(file, L"[%d] %.5f %.5f %.5f\n", i, spts[i].x, spts[i].y, spts[i].z);
    fwprintf(file, L"--- last 5 vertices ---\n");
    int ls = static_cast<int>(spts.size()) - 5;
    if (ls < 5) ls = 5;
    for (int i = ls; i < static_cast<int>(spts.size()); ++i)
        fwprintf(file, L"[%d] %.5f %.5f %.5f\n", i, spts[i].x, spts[i].y, spts[i].z);
    fclose(file);
    Log("vertex-probe FINISHED: chen_vertex_probe_*.txt written.");
}

void RunExportOnMainThread() {
    ExportCameras();
    ExportSkinnedMeshes();
    ExportVertexProbe();
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
        // 组合热键 Ctrl+Shift+E（E=Export 导出）。游戏单键 F 区(F9抽卡/F10简报等)被占用，
        // 三键组合游戏不会绑定，既不撞车也不会误触；边沿触发逻辑在下面保证按住只导出一次。
        const bool down = GameWindowHasFocus() && IsKeyDown(VK_CONTROL) &&
                          IsKeyDown(VK_SHIFT) && IsKeyDown('E');
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
                       Contract("array.get_value")->resolved &&
                       Contract("skinned.get_shared_mesh")->resolved &&
                       Contract("mesh.get_vertex_count")->resolved &&
                       Contract("mesh.get_vertices")->resolved &&
                       Contract("mesh.ctor")->resolved &&
                       Contract("mesh.is_readable")->resolved &&
                       Contract("skinned.bake_mesh")->resolved;
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

    // S4-1 目标类型：UnityEngine.SkinnedMeshRenderer（蒙皮网格渲染器）。
    BE_ResolvedClassV1 skinned_class{};
    if (host->resolve_class(host->context, "UnityEngine.CoreModule.dll",
            "UnityEngine", "SkinnedMeshRenderer", &skinned_class) == BE_Result_Ok &&
        skinned_class.type_object) {
        g_skinned_type = skinned_class.type_object;
        g_skinned_type_root = host->gchandle_new(host->context, g_skinned_type, 0);
        Log("Resolved target class: UnityEngine.SkinnedMeshRenderer");
    } else {
        Log("Could not resolve UnityEngine.SkinnedMeshRenderer class.");
    }

    // S4-2：解析 UnityEngine.Mesh（只取 class_info 供 object_new，烘焙出可读网格）。
    BE_ResolvedClassV1 mesh_class{};
    if (host->resolve_class(host->context, "UnityEngine.CoreModule.dll",
            "UnityEngine", "Mesh", &mesh_class) == BE_Result_Ok && mesh_class.class_info) {
        g_mesh_class_info = const_cast<void*>(mesh_class.class_info);
        Log("Resolved support class: UnityEngine.Mesh");
    } else {
        Log("Could not resolve UnityEngine.Mesh class.");
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
    Log("Scene Exporter v0.5.0 ready (Cameras + SkinnedMesh + BakeMesh vertex probe). Focus the game and press Ctrl+Shift+E.");
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
        if (g_skinned_type_root && g_host->gchandle_free) {
            g_host->gchandle_free(g_host->context, g_skinned_type_root);
        }
    }
    g_target_type_root = 0;
    g_target_type = nullptr;
    g_skinned_type_root = 0;
    g_skinned_type = nullptr;
    g_host = nullptr;
}

const BE_ModuleApiV1 kApi{
    {kModuleId, "Scene Exporter", "0.5.0", BETTER_ENDFIELD_MODULE_ABI_V1},
    &Initialize, &ConfigurationChanged, &Shutdown};

} // namespace
} // namespace BetterEndfield::Exporter

BE_EXPORT const BE_ModuleApiV1* BE_CALL BetterEndfield_GetModuleApiV1() {
    return &BetterEndfield::Exporter::kApi;
}
