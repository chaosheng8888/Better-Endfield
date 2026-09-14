// BetterEndfield Scene Exporter — v0.9.3 (full raw-material capture for offline glTF assembly:
// read-only static bind-pose vertex streams 0 (pos/norm/tan), 1 (uv) and 2 (skin BlendWeight/BlendIndices)
// via Mesh.GetVertexBuffer -> Graphics.CopyBuffer into our CopySource|CopyDestination staging ->
// AsyncGPUReadback; triangle indices via CPU GetIndices AND the read-only MeshData.CopyIndicesIntoPtr
// channel (the latter targets isReadable=false parts); PLUS bindposes / classic 4-bone weights /
// ordered bone names; every part is dumped to
// scene-export/chen_raw_<tick>/; no game buffer target is ever mutated; see TickGpuVertexReadback):
// *** NEVER call set_vertexBufferTarget on game meshes/renderers *** — v0.7.4~v0.7.8 did, and it
// tore down+rebuilt GPU-resident vertex buffers with no CPU copy, so hair/body/clothes vanished
// on screen right after Ctrl+E (only CPU-backed face/eyebrow/cloth_03 survived). Confirmed by user:
// character is intact before the hotkey, dismembers only after. v0.7.9 only READS the current-frame
// SMR.GetVertexBuffer() buffer the engine already built for drawing, then AsyncGPUReadback.
// Path B (Mesh.GetVertexBuffer(stream)) and both set_vertexBufferTarget calls are removed.
// machine driven every frame by DetourPump -> TickGpuVertexReadback. Hotkey Ctrl+E).
//
// S3 目标（只验证链路，不导网格）：游戏内按组合热键(Ctrl+E)，在 Unity 主线程枚举
// “当前已加载场景”里指定类型的全部对象，把数量和名字写到本地 txt。
// 先打通 注入 -> 主线程泵 -> 静态枚举 -> 数组遍历取名字 -> 写文件 整条路。
//
// ★S3 最小验证选 UnityEngine.Camera（全场景只有 1~2 个、最轻），而不是 Renderer
//   （大世界上万个、FindObjectsOfType 全局重遍历，在 LateUpdate 渲染回调里重入易崩）。
//   Camera 链路跑通后，S4 再换成 Renderer/Mesh，并用分帧或更安全的 hook 点处理重枚举。
//
// ★所有 IL2CPP/托管调用都走 Safe* 封装（__try/__except SEH 兜底，对齐 model 模块）：
//   单次坏调用只记日志、不再把整个游戏踢崩，可反复按 Ctrl+E 调试。
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
#include <cmath>
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
    // S4-2 诊断：Renderer.isVisible（该部件当前是否被任意相机渲染/在画面内），定义在 Renderer 基类。
    {"renderer.is_visible",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
            "get_isVisible", nullptr, "System.Boolean", 0}},
    // v0.7.0 MeshData 只读直读通道（绕开 isReadable=false）。嵌套类用“外层.内层”点号写法。
    // ★多参数类型串必须用竖线 | 分隔（host 的 SplitParameters 只认 |；写逗号会被整段当成一个参数而匹配失败）。
    // Mesh.AcquireReadOnlyMeshData(Mesh) 公共静态入口：内部真正向原生锁定只读顶点、返回已填充的 MeshDataArray。
    // 私有 MeshDataArray..ctor(Mesh,bool) 实测只做空初始化(调完 len=0/ptrs=null)，弃用。
    // 返回类型故意留 nullptr 不校验（嵌套值类型名可能带 +/.），靠方法名+1个Mesh参数唯一匹配。
    {"mesh.acquire_ro",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "AcquireReadOnlyMeshData", "UnityEngine.Mesh", nullptr, 1}},
    // MeshDataArray.Dispose()：读完必须释放对原生顶点缓冲的锁定。
    {"mda.dispose",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh.MeshDataArray",
            "Dispose", nullptr, "System.Void", 0}},
    // MeshData.GetVertexCount(IntPtr self) -> int（Injected 静态风格，self=内部指针显式传）。
    {"md.vcount",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh.MeshData",
            "GetVertexCount", "System.IntPtr", "System.Int32", 1}},
    // MeshData.CopyAttributeIntoPtr(self, Position=0, Float32=0, dim=3, dst)：
    // 直接把全部顶点的 Position（每点12字节）连续拷进我方原生缓冲区，不经托管数组/泛型。
    {"md.copy_pos",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh.MeshData",
            "CopyAttributeIntoPtr",
            "System.IntPtr|UnityEngine.Rendering.VertexAttribute|UnityEngine.Rendering.VertexAttributeFormat|System.Int32|System.IntPtr",
            "System.Void", 5}},
    // ===== v0.9.3 read-only MeshData index channel (the official path for isReadable=false meshes).
    // Injected-style: the FIRST arg is the MeshData native IntPtr (m_Ptrs[0] out of a MeshDataArray).
    // NONE of these enter the mandatory-ready gate; a miss only leaves indices empty, never fatal.
    // MeshData.GetSubMeshCount(IntPtr self) -> int.
    {"md.submesh_count",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh.MeshData",
            "GetSubMeshCount", "System.IntPtr", "System.Int32", 1}},
    // MeshData.GetIndexCount(IntPtr self, int submesh) -> int.
    {"md.index_count",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh.MeshData",
            "GetIndexCount", "System.IntPtr|System.Int32", "System.Int32", 2}},
    // MeshData.CopyIndicesIntoPtr(self, sub, applyBaseVertex, dstStride, dst) -> void (int32 indices).
    {"md.copy_indices",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh.MeshData",
            "CopyIndicesIntoPtr",
            "System.IntPtr|System.Int32|System.Boolean|System.Int32|System.IntPtr",
            "System.Void", 5}},
    // ===== v0.7.8 GPU skinned-vertex readback chain (RVAs verified against IL2CPP dump) =====
    // Parameter-type strings are intentionally left null so the host matches by method name +
    // parameter count only (avoids exact-name pitfalls for nested enums / generic Action).
    // SMR.set_vertexBufferTarget(GraphicsBuffer.Target value type): request a CopySource-capable
    // vertex buffer binding. We pass Vertex(1)|CopySource(4)=5 at call time.
    {"skinned.set_vbt",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer",
            "set_vertexBufferTarget", nullptr, "System.Void", 1}},
    // SMR.GetVertexBuffer() -> GraphicsBuffer: current-frame already-skinned GPU vertex buffer.
    {"skinned.get_vb",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer",
            "GetVertexBuffer", nullptr, "UnityEngine.GraphicsBuffer", 0}},
    // GraphicsBuffer.get_count / get_stride (instance, no args -> int).
    {"gb.get_count",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
            "get_count", nullptr, "System.Int32", 0}},
    {"gb.get_stride",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
            "get_stride", nullptr, "System.Int32", 0}},
    // Mesh.GetVertexAttributeCountImpl() -> int (vertex channel count; layout metadata stays readable).
    {"mesh.attr_count",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "GetVertexAttributeCountImpl", nullptr, "System.Int32", 0}},
    // Mesh.GetVertexAttribute(int) -> VertexAttributeDescriptor (boxed value type, unbox 16 bytes).
    {"mesh.get_attr",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "GetVertexAttribute", nullptr, nullptr, 1}},
    // AsyncGPUReadback.Request(ComputeBuffer, Action) static -> AsyncGPUReadbackRequest (boxed).
    // Name "Request" + 2 params uniquely selects the ComputeBuffer overload (Texture overloads have 4).
    {"gpuread.request",
        {"UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "AsyncGPUReadback",
            "Request", nullptr, nullptr, 2}},
    // AsyncGPUReadbackRequest is a sealed STRUCT (SIZE 0x20, m_Ptr@0x10). We use the STATIC _Injected
    // bindings and pass the UNBOXED struct pointer as the first by-ref argument. Passing the boxed
    // object crashes: the 0x10 object header overlaps the struct's m_Ptr slot, so the native side
    // dereferences the monitor word (null) -> SEH. v0.7.8 fix.
    {"gpr.wait",
        {"UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "AsyncGPUReadbackRequest",
            "WaitForCompletion_Injected", nullptr, "System.Void", 1}},
    {"gpr.has_error",
        {"UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "AsyncGPUReadbackRequest",
            "HasError_Injected", nullptr, "System.Boolean", 1}},
    {"gpr.is_done",
        {"UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "AsyncGPUReadbackRequest",
            "IsDone_Injected", nullptr, "System.Boolean", 1}},
    {"gpr.get_data_raw",
        {"UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "AsyncGPUReadbackRequest",
            "GetDataRaw_Injected", nullptr, "System.IntPtr", 2}},
    // SystemInfo.SupportsAsyncGPUReadback() static -> bool (device capability probe, optional).
    {"sysinfo.supports_gpr",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SystemInfo",
            "SupportsAsyncGPUReadback", nullptr, "System.Boolean", 0}},
    // ===== v0.7.8 diagnostics: Mesh per-stream source buffers, visibility, device identity =====
    // Mesh.set_vertexBufferTarget(int): make the SOURCE (bind-pose) mesh GPU buffer CopySource-capable.
    {"mesh.set_vbt",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "set_vertexBufferTarget", nullptr, "System.Void", 1}},
    // Mesh.GetVertexBuffer(int stream) -> GraphicsBuffer: one stream of the source mesh vertex layout.
    {"mesh.get_vb_stream",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "GetVertexBuffer", nullptr, "UnityEngine.GraphicsBuffer", 1}},
    // GraphicsBuffer.target flags (int).
    {"gb.get_target",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
            "get_target", nullptr, "System.Int32", 0}},
    {"gb.ctor",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
            ".ctor", nullptr, "System.Void", 3}},
    {"gb.release",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
            "Release", nullptr, "System.Void", 0}},
    {"graphics.copy_buffer",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Graphics",
            "CopyBuffer", "UnityEngine.GraphicsBuffer|UnityEngine.GraphicsBuffer", "System.Void", 2}},
    // ===== v0.9.3 full-raw-material capture (geometry + skinning). None of these are in the
    // mandatory-ready gate: if a CPU accessor is empty for a GPU-resident mesh, that item is simply
    // left empty and logged, never blocking the proven stream0/1 vertex readback main chain.
    // Mesh.subMeshCount getter -> int (number of separate triangle index lists).
    {"mesh.submesh_count",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "get_subMeshCount", nullptr, "System.Int32", 0}},
    // Mesh.GetIndices(int submesh, bool applyBaseVertex=false) -> int[] . The 2-arg overload is
    // uniquely selected by the Int32|Boolean signature; return type left null (name+arity unique).
    {"mesh.get_indices",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "GetIndices", "System.Int32|System.Boolean", nullptr, 2}},
    // Mesh.bindposes getter -> Matrix4x4[] (inverse-bind, column-major, 64B/bone). 0-arg unique.
    {"mesh.bindposes",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "get_bindposes", nullptr, nullptr, 0}},
    // Mesh.GetBoneWeightsImpl() -> BoneWeight[] classic 4-weight (past header 32B/vertex:
    // 4 float weights then 4 int bone indices). 0-arg unique, return type left null.
    {"mesh.bone_weights",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
            "GetBoneWeightsImpl", nullptr, nullptr, 0}},
    // SkinnedMeshRenderer.bones getter -> Transform[] (skeleton nodes; names read in array order).
    {"skinned.get_bones",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer",
            "get_bones", nullptr, nullptr, 0}},
    {"sysinfo.devtype",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SystemInfo",
            "GetGraphicsDeviceType", nullptr, nullptr, 0}},
    {"sysinfo.devname",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SystemInfo",
            "GetGraphicsDeviceName", nullptr, "System.String", 0}},
    {"sysinfo.supports_prop",
        {"UnityEngine.CoreModule.dll", "UnityEngine", "SystemInfo",
            "get_supportsAsyncGPUReadback", nullptr, "System.Boolean", 0}},
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
// v0.7.0：UnityEngine.Mesh.MeshDataArray 嵌套类信息（object_new 装箱后调 .ctor 拿只读网格数据）。
void* g_mda_class_info = nullptr;
void* g_gb_class_info = nullptr; // v0.9.0 UnityEngine.GraphicsBuffer class_info for object_new (staging buffer)

std::atomic_bool g_export_request{false};
std::atomic_int g_hotkey{VK_F9};
std::atomic_bool g_input_stop{false};
std::thread g_input_thread;
std::atomic_bool g_contracts_ready{false};

// v0.7.8 cross-frame GPU readback state (see TickGpuVertexReadback).
std::atomic_int g_gpu_phase{0}; // 0=idle, 1=waiting skin frames / reading back
int g_gpu_wait = 0;
DWORD g_gpu_start_tick = 0;

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

// ===========================================================================
// v0.7.8 GPU skinned-vertex readback — the only channel that works for
// isReadable=false GPU-resident skinned meshes. Cross-frame state machine:
//   Start (Ctrl+E frame): enumerate Chen lod0 parts, set vertexBufferTarget,
//     pin renderer/mesh with GCHandle across frames;
//   Tick (each following frame): wait >=3 frames for the SMR to re-skin into
//     the CopySource buffer, then per part GetVertexBuffer -> AsyncGPUReadback
//     -> WaitForCompletion -> GetDataRaw, parse Position; finalize when all
//     parts are collected (or after a 12-frame budget).
// ===========================================================================

struct BE_VAD { int32_t attribute, format, dimension, stream; }; // VertexAttributeDescriptor (16 bytes past header)

struct GpuPartResult {
    std::string name;
    int32_t expect_vc = -1, count = -1, stride = -1;
    int32_t pos_off = -1, pos_fmt = -1, pos_dim = 0, n_finite = 0;
    bool got_buffer = false, req_error = false, ok = false;
    std::string layout;
    BE_Vec3 mn{0,0,0}, mx{0,0,0};
    // v0.7.8 diagnostics
    int32_t vis = -1;                                  // Renderer.isVisible
    int32_t n_stream = 0;                              // distinct streams in the vertex layout
    bool a_got = false; int32_t a_count = -1, a_stride = -1, a_target = -1; // path A: SMR.GetVertexBuffer()
    std::string raw; // v0.8.0 per-part first-96-bytes hex, to decode the real interleaved stride offline
    int32_t b_got[3] = {0,0,0};                        // path B: Mesh.GetVertexBuffer(stream)
    int32_t b_count[3] = {-1,-1,-1}, b_stride[3] = {-1,-1,-1}, b_target[3] = {-1,-1,-1};
    bool breq_tried = false, breq_boxed = false, breq_haserr = false; void* breq_mptr = nullptr;
    // v0.9.0 PATH M (correct route): source Mesh.GetVertexBuffer(0) = bind-pose STATIC GPU buffer
    // (persistent since load, unlike SMR current-frame). Relay it through our OWN staging GraphicsBuffer
    // via Graphics.CopyBuffer, then AsyncGPUReadback the staging buffer. We never mutate game targets.
    int32_t m_readable = -1;                            // Mesh.isReadable (expected false)
    bool m_src_got = false; int32_t m_count=-1, m_stride=-1, m_target=-1; // static source buffer desc
    bool m_stage_built = false, m_copy_ok = false, m_req_haserr = false;  // staging built / copy ok / readback err
    int32_t m_finite = 0;                               // finite bind-pose positions decoded
    BE_Vec3 mmn{0,0,0}, mmx{0,0,0};                     // bind-pose Position AABB
    std::string mraw;                                   // first bytes of the relayed static buffer
    // v0.9.3 full-raw-material capture diagnostics
    int32_t s_count[3] = {-1,-1,-1}, s_stride[3] = {-1,-1,-1};
    int32_t submesh_count = -1, idx_count = 0, bind_bones = 0, bw_verts = 0, bones_n = 0;
    bool idx_got = false, bind_got = false, bw_got = false, bones_got = false;
    int32_t idx_src = 0; // 0=none, 1=CPU GetIndices, 2=read-only MeshData.CopyIndicesIntoPtr
};

// v0.7.8 device identity, filled once at start.
int32_t g_dev_type = -1; bool g_sup_method = false, g_sup_prop = false;
std::string g_dev_name;
std::string g_raw_dump; // v0.7.8 raw per-vertex floats of one part, to decode physical stream layout

struct GpuPart {
    uint32_t renderer_root = 0; void* renderer = nullptr;
    uint32_t mesh_root = 0;     void* mesh = nullptr;
    std::string name;
    int32_t expect_vc = 0;
    bool collected = false;
    int miss = 0; // consecutive frames its lod0 GPU buffer was absent (SEH/empty)
    bool requested = false; // phase-1 dispatch done exactly once
    // v0.9.3 per-stream GPU readback. stream0 = Position/Normal/Tangent, stream1 = TexCoord0/2,
    // stream2 = skinning BlendWeight + BlendIndices (UNorm16/UInt8 on most parts; Float32/UInt32 on
    // face/eyebrow/cloth_03). All three ride the same proven CopyBuffer relay; CPU GetBoneWeightsImpl
    // is kept only as a readable-part cross-check.
    struct StreamReq {
        bool dispatched = false, done = false, err = false;
        void* src = nullptr;    uint32_t src_root = 0;
        void* stage = nullptr;  uint32_t stage_root = 0;
        void* req = nullptr;    uint32_t req_root = 0;
        int32_t count = -1, stride = -1;
        std::vector<uint8_t> bytes;
    };
    StreamReq sr[3];
    int nstreams = 1;
    // v0.9.3 CPU-side captures (filled synchronously in phase 1; indices additionally have a MeshData path).
    std::vector<int32_t> indices;        // all submeshes' indices concatenated
    std::vector<int32_t> sub_idxcount;   // index count per submesh (matches the concatenation)
    std::vector<uint8_t> bindposes;      // Matrix4x4[] raw, 64B/bone, column-major
    std::vector<uint8_t> boneweights;    // BoneWeight[] raw, 32B/vertex (4 float w + 4 int idx)
    std::string bones_names;             // SMR.bones Transform names in order, one per line
    GpuPartResult pr;
};

std::vector<GpuPart> g_gpu_parts;

int32_t UnboxInt(void* boxed, int32_t fallback = -1) {
    if (!boxed) return fallback;
    int32_t v = fallback; bool f = false;
    return (SafeUnbox(boxed, &v, sizeof(v), &f) && !f) ? v : fallback;
}
void* UnboxPtr(void* boxed) {
    if (!boxed) return nullptr;
    void* p = nullptr; bool f = false;
    return (SafeUnbox(boxed, &p, sizeof(p), &f) && !f) ? p : nullptr;
}
// Return the INNER data pointer of a boxed value type (no copy). This is the `this`/by-ref pointer
// that static _Injected struct bindings expect. SEH-guarded, POD-only body.
void* UnboxThis(void* boxed) {
    if (!boxed || !g_host || !g_host->object_unbox) return nullptr;
    __try { return g_host->object_unbox(g_host->context, boxed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// SEH-wrapped memcpy (only POD here, so __try is legal).
bool SafeMemcpy(void* dst, const void* src, size_t n) {
    if (!dst || !src || n == 0) return false;
    __try { std::memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int VertexFormatBytes(int fmt) {
    switch (fmt) {
        case 0: return 4;                                   // Float32
        case 1: return 2;                                   // Float16
        case 2: case 3: case 4: case 5: return 1;          // UNorm8/SNorm8/UInt8/SInt8
        case 6: case 7: case 8: case 9: case 10: return 2; // 16-bit
        case 11: case 12: return 4;                        // UInt32/SInt32
        default: return 4;
    }
}

// Stage 1 (the Ctrl+E frame): enumerate Chen lod0 parts, request the readback
// binding target, and pin objects for the next frames.
void StartGpuVertexReadback() {
    if (g_gpu_phase.load(std::memory_order_acquire) != 0) {
        Log("gpu-probe: a pass is already running, ignore this trigger."); return;
    }
    MethodContract* find_all = Contract("object.find_all_of_type");
    MethodContract* get_name = Contract("object.get_name");
    MethodContract* get_length = Contract("array.get_length");
    MethodContract* get_value = Contract("array.get_value");
    MethodContract* get_mesh = Contract("skinned.get_shared_mesh");
    MethodContract* get_vcount = Contract("mesh.get_vertex_count");
    MethodContract* get_vb0 = Contract("skinned.get_vb");
    // (v0.9.0) skinned.set_vbt intentionally NOT used: setting target on isReadable=false parts empties them.
    MethodContract* sup = Contract("sysinfo.supports_gpr");
    if (!g_skinned_type || !find_all->resolved || !get_name->resolved ||
        !get_length->resolved || !get_value->resolved || !get_mesh->resolved ||
        !get_vcount->resolved || !get_vb0->resolved) {
        Log("gpu-probe start skipped: base contracts not ready."); return;
    }
    if (sup->resolved) {
        bool ef = false;
        SafeUnbox(Invoke(sup, nullptr, nullptr), &g_sup_method, sizeof(g_sup_method), &ef);
        MethodContract* spp = Contract("sysinfo.supports_prop");
        if (spp && spp->resolved) { bool v=false; SafeUnbox(Invoke(spp,nullptr,nullptr),&v,sizeof(v),&ef); g_sup_prop=v; }
        MethodContract* dt = Contract("sysinfo.devtype");
        if (dt && dt->resolved) g_dev_type = UnboxInt(Invoke(dt, nullptr, nullptr), -1);
        MethodContract* dn = Contract("sysinfo.devname");
        if (dn && dn->resolved) { char nb[160]{}; bool nf=false; if (SafeCopyName(Invoke(dn,nullptr,nullptr),nb,sizeof(nb),&nf)>0) g_dev_name=nb; }
        char db[340]; std::snprintf(db, sizeof(db),
            "gpu-probe DEVICE type=%d (6=D3D11 12=D3D12 21=Vulkan) name='%s' supportsAsync method=%d prop=%d",
            g_dev_type, g_dev_name.c_str(), (int)g_sup_method, (int)g_sup_prop); Log(db);
    }

    void* fp[1]{ g_skinned_type };
    void* objs = Invoke(find_all, nullptr, fp);
    if (!objs) { Log("gpu-probe: enumerate SMR failed."); return; }
    const int32_t total = ArrayLengthOf(get_length, objs);
    if (total < 0) { Log("gpu-probe: array length failed."); return; }

    static const char* kPrefix = "S_actor_chen_";
    static const char* kLod = "_lod0";
    const size_t kpl = std::strlen(kPrefix);
    // v0.7.9: no kTarget / set_vertexBufferTarget — read-only, never mutate game GPU buffers.
    g_gpu_parts.clear(); g_raw_dump.clear();
    for (int32_t i = 0; i < total && static_cast<int32_t>(g_gpu_parts.size()) < 20; ++i) {
        int32_t idx = i; void* ip[1]{ &idx };
        void* r = Invoke(get_value, objs, ip);
        if (!r) continue;
        char nb[256]{}; bool nf = false;
        void* no = Invoke(get_name, r, nullptr);
        if (SafeCopyName(no, nb, sizeof(nb), &nf) <= 0) continue;
        if (std::strncmp(nb, kPrefix, kpl) != 0 || !std::strstr(nb, kLod)) continue;
        void* mesh = Invoke(get_mesh, r, nullptr);
        if (!mesh) { char w[256]; std::snprintf(w, sizeof(w), "gpu-probe: %s has no sharedMesh, skip.", nb); Log(w); continue; }
        const int32_t vc = UnboxInt(Invoke(get_vcount, mesh, nullptr));
        // v0.9.0: never mutate a buffer target (with isReadable=false that rebuilds an EMPTY buffer).
        // Just record Mesh.isReadable below for the diagnosis; the static GPU buffer is read via CopyBuffer relay.
        int32_t readable = -1; { MethodContract* mread = Contract("mesh.is_readable");
            if (mread && mread->resolved) { bool rv=false,ef=false; if (SafeUnbox(Invoke(mread,mesh,nullptr),&rv,sizeof(rv),&ef)&&!ef) readable=rv?1:0; } }
        // (v0.9.0) nothing is mutated, so Tick has no restore step.
        int32_t visv = -1; MethodContract* gvis = Contract("renderer.is_visible");
        if (gvis && gvis->resolved) { bool vv=false,ef=false; if (SafeUnbox(Invoke(gvis,r,nullptr),&vv,sizeof(vv),&ef)&&!ef) visv=vv?1:0; }
        const uint32_t rr = g_host->gchandle_new(g_host->context, r, 0);
        const uint32_t mr = g_host->gchandle_new(g_host->context, mesh, 0);
        GpuPart gp; gp.renderer_root = rr; gp.renderer = r;
        gp.mesh_root = mr; gp.mesh = mesh; gp.name = nb; gp.expect_vc = vc; gp.pr.vis = visv; gp.pr.m_readable = readable;
        g_gpu_parts.push_back(std::move(gp));
    }
    if (g_gpu_parts.empty()) { Log("gpu-probe: no S_actor_chen_*_lod0 part is loaded."); return; }
    g_gpu_wait = 0; g_gpu_start_tick = GetTickCount();
    g_gpu_phase.store(1, std::memory_order_release);
    char m[220]; std::snprintf(m, sizeof(m),
        "gpu-probe START v0.9.3: %d Chen lod0 parts; READ-ONLY GPU relay of vertex streams 0/1/2 (s2=weights/joints) + indices(CPU then MeshData) + bindposes/boneWeights/bones; materials dumped to chen_raw_<tick>; SMR current-frame is control only.",
        (int)g_gpu_parts.size()); Log(m);
}

// IL2CPP single-dim zero-based array memory layout: [object header 0x10][bounds* 0x8][length 0x8]
// then tightly-packed elements at +0x20. POD-only body so __try is legal. Caller passes a validated
// element count and a resized destination.
bool SafeReadManagedArray(void* arr, int32_t count, size_t elem_size, void* dst) {
    if (!arr || !dst || count <= 0 || elem_size == 0) return false;
    __try {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(arr) + 0x20;
        std::memcpy(dst, base, (size_t)count * elem_size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// v0.9.3 POD-only helper (a __try is illegal inside CollectOnePart because it holds C++ objects that
// need unwinding). Given the INTERIOR pointer of a boxed MeshDataArray struct, read m_Ptrs@0x10 and
// m_Length@0x18 and return the native pointer of the first MeshData (m_Ptrs[0]); nullptr on any fault.
void* MeshDataFirstPtr(void* arrInner, int32_t* outLen) {
    if (!arrInner) return nullptr;
    __try {
        uint8_t* base = reinterpret_cast<uint8_t*>(arrInner);
        void** pPtrs = *reinterpret_cast<void***>(base + 0x10);
        int32_t len = *reinterpret_cast<int32_t*>(base + 0x18);
        if (outLen) *outLen = len;
        if (!pPtrs || len < 1) return nullptr;
        return pPtrs[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// v0.9.3 collector. GPU-reads the static bind-pose vertex streams 0 (Position/Normal/Tangent),
// 1 (TexCoord) and 2 (BlendWeight/BlendIndices) via the proven Graphics.CopyBuffer ->
// AsyncGPUReadback relay (no game target mutated); triangle indices come from CPU GetIndices and,
// when that is empty, the read-only MeshData.CopyIndicesIntoPtr channel; also bindposes, classic
// 4-bone weights, and ordered bone names. Returns 0 = not ready, poll later; 1 = this part is done.
int CollectOnePart(GpuPart& gp) {
    MethodContract* get_vb = Contract("skinned.get_vb");
    MethodContract* gb_count = Contract("gb.get_count");
    MethodContract* gb_stride = Contract("gb.get_stride");
    MethodContract* attr_count = Contract("mesh.attr_count");
    MethodContract* get_attr = Contract("mesh.get_attr");
    MethodContract* request = Contract("gpuread.request");
    MethodContract* isdone = Contract("gpr.is_done");
    MethodContract* haserr = Contract("gpr.has_error");
    MethodContract* getraw = Contract("gpr.get_data_raw");
    MethodContract* get_length = Contract("array.get_length");
    MethodContract* get_value = Contract("array.get_value");
    MethodContract* get_name = Contract("object.get_name");
    GpuPartResult& R = gp.pr;
    R.name = gp.name; R.expect_vc = gp.expect_vc;

    auto GbDesc = [&](void* gb, int32_t& c, int32_t& st) {
        c = UnboxInt(Invoke(gb_count, gb, nullptr));
        st = UnboxInt(Invoke(gb_stride, gb, nullptr));
    };

    // ---------- phase 1 (once): layout + CPU captures + dispatch per-stream GPU requests ----------
    if (!gp.requested) {
        // reset CPU captures (phase 1 may run more than once if a static buffer shows up late)
        gp.indices.clear(); gp.sub_idxcount.clear(); gp.bindposes.clear();
        gp.boneweights.clear(); gp.bones_names.clear();
        R.idx_count = 0; R.bind_bones = 0; R.bw_verts = 0; R.bones_n = 0;
        R.idx_got = R.bind_got = R.bw_got = R.bones_got = false; R.idx_src = 0;

        const int32_t nac = UnboxInt(Invoke(attr_count, gp.mesh, nullptr));
        int maxStream = -1; char lay[320]{};
        if (nac > 0 && nac < 32) {
            for (int32_t ai = 0; ai < nac; ++ai) {
                int32_t aix = ai; void* ap[1]{ &aix };
                BE_VAD d{ -1,-1,-1,-1 }; bool df = false;
                if (SafeUnbox(Invoke(get_attr, gp.mesh, ap), &d, sizeof(d), &df) && !df) {
                    char one[48]; std::snprintf(one, sizeof(one), "a%d(f%d,d%d,s%d) ",
                        d.attribute, d.format, d.dimension, d.stream);
                    strncat_s(lay, one, _TRUNCATE);
                    if (d.stream > maxStream) maxStream = d.stream;
                }
            }
        }
        R.layout = lay;
        int ns = maxStream + 1; if (ns < 1) ns = 1; if (ns > 3) ns = 3; // v0.9.3 GPU-read streams 0/1/2 (s2=weights/joints)
        gp.nstreams = ns;

        // PATH A control diagnostic: SMR current-frame buffer descriptor only, bytes never read.
        void* agb = Invoke(get_vb, gp.renderer, nullptr);
        if (agb) { R.a_got = true; GbDesc(agb, R.a_count, R.a_stride); }

        // ----- v0.9.3 captures (each independently optional; a miss is never fatal) -----
        // (a1) triangle indices path 1 = CPU GetIndices, concatenated per submesh (readable parts).
        MethodContract* c_sub = Contract("mesh.submesh_count");
        MethodContract* c_idx = Contract("mesh.get_indices");
        if (c_sub && c_sub->resolved) {
            R.submesh_count = UnboxInt(Invoke(c_sub, gp.mesh, nullptr));
            if (R.submesh_count > 0 && R.submesh_count < 32 && c_idx && c_idx->resolved) {
                bool any = false;
                for (int32_t s = 0; s < R.submesh_count; ++s) {
                    int32_t sub = s; bool applyBase = false;
                    void* ip[2]{ &sub, &applyBase };
                    void* iarr = Invoke(c_idx, gp.mesh, ip);
                    const int32_t ilen = ArrayLengthOf(get_length, iarr);
                    if (ilen <= 0 || ilen > 5000000) { gp.sub_idxcount.push_back(0); continue; }
                    size_t oldn = gp.indices.size();
                    gp.indices.resize(oldn + ilen);
                    if (SafeReadManagedArray(iarr, ilen, 4, gp.indices.data() + oldn)) {
                        gp.sub_idxcount.push_back(ilen); R.idx_count += ilen; any = true;
                    } else { gp.indices.resize(oldn); gp.sub_idxcount.push_back(0); }
                }
                R.idx_got = any; if (any) R.idx_src = 1;
            }
        }
        // (a2) triangle indices path 2 = official read-only MeshData channel, tried only when the CPU
        // path came back empty (the isReadable=false GPU-resident case). AcquireReadOnlyMeshData returns
        // a boxed MeshDataArray struct; MeshDataFirstPtr pulls m_Ptrs[0] (the native MeshData self). The
        // MeshData bindings are static Injected style (instance=nullptr, self passed BY VALUE as arg 0).
        // Always Dispose to release the native lock, even on failure.
        if (R.idx_count == 0) {
            MethodContract* acq = Contract("mesh.acquire_ro");
            MethodContract* mdn = Contract("md.submesh_count");
            MethodContract* mdi = Contract("md.index_count");
            MethodContract* mdc = Contract("md.copy_indices");
            MethodContract* mdd = Contract("mda.dispose");
            if (acq && acq->resolved && mdn && mdn->resolved && mdi && mdi->resolved && mdc && mdc->resolved) {
                void* boxedArr = Invoke(acq, gp.mesh, nullptr); // boxed MeshDataArray
                void* arrInner = UnboxThis(boxedArr);
                int32_t mdLen = 0;
                void* mdSelf = MeshDataFirstPtr(arrInner, &mdLen);
                if (mdSelf) {
                    void* sa[1]{ &mdSelf };
                    int32_t nsub = UnboxInt(Invoke(mdn, nullptr, sa));
                    if (nsub > 0 && nsub < 32) {
                        bool any = false;
                        for (int32_t s = 0; s < nsub; ++s) {
                            int32_t sub = s;
                            void* ci[2]{ &mdSelf, &sub };
                            int32_t ic = UnboxInt(Invoke(mdi, nullptr, ci));
                            if (ic <= 0 || ic > 5000000) { gp.sub_idxcount.push_back(0); continue; }
                            size_t oldn = gp.indices.size();
                            gp.indices.resize(oldn + ic);
                            int32_t stride4 = 4; bool applyBase = false;
                            void* dst = gp.indices.data() + oldn;
                            void* cc[5]{ &mdSelf, &sub, &applyBase, &stride4, &dst };
                            Invoke(mdc, nullptr, cc);
                            gp.sub_idxcount.push_back(ic); R.idx_count += ic; any = true;
                        }
                        if (any) { R.idx_got = true; R.idx_src = 2; if (R.submesh_count <= 0) R.submesh_count = nsub; }
                    }
                }
                if (mdd && mdd->resolved && arrInner) Invoke(mdd, arrInner, nullptr); // struct instance method: this=interior
            }
        }
        // (b) bindposes: Matrix4x4[] (64B/bone, Unity column-major == glTF column-major)
        MethodContract* c_bind = Contract("mesh.bindposes");
        if (c_bind && c_bind->resolved) {
            void* barr = Invoke(c_bind, gp.mesh, nullptr);
            const int32_t blen = ArrayLengthOf(get_length, barr);
            if (blen > 0 && blen < 65536) {
                gp.bindposes.resize((size_t)blen * 64);
                if (SafeReadManagedArray(barr, blen, 64, gp.bindposes.data())) { R.bind_bones = blen; R.bind_got = true; }
                else gp.bindposes.clear();
            }
        }
        // (c) classic 4-bone weights: BoneWeight[] (32B/vertex = 4 float weights + 4 int indices)
        MethodContract* c_bw = Contract("mesh.bone_weights");
        if (c_bw && c_bw->resolved) {
            void* warr = Invoke(c_bw, gp.mesh, nullptr);
            const int32_t wlen = ArrayLengthOf(get_length, warr);
            if (wlen > 0 && wlen < 5000000) {
                gp.boneweights.resize((size_t)wlen * 32);
                if (SafeReadManagedArray(warr, wlen, 32, gp.boneweights.data())) { R.bw_verts = wlen; R.bw_got = true; }
                else gp.boneweights.clear();
            }
        }
        // (d) SMR.bones: ordered Transform names (skeleton node order matches bindposes/weight indices)
        MethodContract* c_bones = Contract("skinned.get_bones");
        if (c_bones && c_bones->resolved && get_value && get_value->resolved && get_name && get_name->resolved) {
            void* tarr = Invoke(c_bones, gp.renderer, nullptr);
            const int32_t tlen = ArrayLengthOf(get_length, tarr);
            if (tlen > 0 && tlen < 65536) {
                R.bones_n = tlen; R.bones_got = true;
                for (int32_t bi = 0; bi < tlen; ++bi) {
                    int32_t bix = bi; void* bp[1]{ &bix };
                    void* tr = Invoke(get_value, tarr, bp);
                    char bn[160]{}; bool bf = false;
                    SafeCopyName(Invoke(get_name, tr, nullptr), bn, sizeof(bn), &bf);
                    char line[192]; std::snprintf(line, sizeof(line), "%d %s\n", bi, bn);
                    gp.bones_names += line;
                }
            }
        }

        // ----- GPU per-stream relay. Pass 1: acquire every source + descriptor first (no allocation
        // yet), so a late/missing source retries next frame without leaking staging buffers. -----
        MethodContract* mget = Contract("mesh.get_vb_stream");
        MethodContract* gbctor = Contract("gb.ctor");
        MethodContract* copyb = Contract("graphics.copy_buffer");
        if (!mget || !mget->resolved || !gbctor || !gbctor->resolved || !g_gb_class_info ||
            !copyb || !copyb->resolved || !request || !request->resolved) {
            R.req_error = true; Log("gpu-probe: path M contracts/class not ready."); return 1;
        }
        void* srcs[3]{ nullptr, nullptr, nullptr };
        int32_t sc[3]{ -1,-1,-1 }, sst[3]{ -1,-1,-1 };
        for (int s = 0; s < ns; ++s) {
            int32_t si = s; void* spx[1]{ &si };
            void* src = Invoke(mget, gp.mesh, spx);
            int32_t c = -1, st = -1;
            if (src) GbDesc(src, c, st);
            if (!src || c <= 0 || st <= 0 || st > 1024) { ++gp.miss; return 0; } // retry whole phase next frame
            srcs[s] = src; sc[s] = c; sst[s] = st;
        }
        // Pass 2: all sources present -> build a staging buffer per stream, CopyBuffer on GPU, request.
        for (int s = 0; s < ns; ++s) {
            void* stage = g_host->object_new(g_host->context, g_gb_class_info);
            if (!stage) { R.req_error = true; return 1; }
            { int32_t usage = 12, cnt = sc[s], sstr = sst[s]; void* cp[3]{ &usage, &cnt, &sstr };
              Invoke(gbctor, stage, cp); }
            { void* cpar[2]{ srcs[s], stage }; Invoke(copyb, nullptr, cpar); }
            void* rq[2]{ stage, nullptr }; void* boxed = Invoke(request, nullptr, rq);
            if (!boxed) { MethodContract* rel = Contract("gb.release");
                if (rel && rel->resolved) Invoke(rel, stage, nullptr);
                gp.sr[s].err = true; continue; }
            gp.sr[s].src = srcs[s]; gp.sr[s].stage = stage; gp.sr[s].req = boxed;
            gp.sr[s].count = sc[s]; gp.sr[s].stride = sst[s]; gp.sr[s].dispatched = true;
            gp.sr[s].src_root = g_host->gchandle_new(g_host->context, srcs[s], 0);
            gp.sr[s].stage_root = g_host->gchandle_new(g_host->context, stage, 0);
            gp.sr[s].req_root = g_host->gchandle_new(g_host->context, boxed, 0);
            R.s_count[s] = sc[s]; R.s_stride[s] = sst[s];
        }
        gp.requested = true;
        char sb[360]; std::snprintf(sb, sizeof(sb),
            "gpu-probe[%s] v0.9.3 dispatched %d vertex stream(s) c0=%d st0=%d c2=%d st2=%d; idx=%d(src%d %d sub) bind=%d bw=%d bones=%d readable=%d Actrl=%d",
            gp.name.c_str(), ns, R.s_count[0], R.s_stride[0], R.s_count[2], R.s_stride[2],
            R.idx_count, R.idx_src, R.submesh_count,
            R.bind_bones, R.bw_verts, R.bones_n, R.m_readable, (int)R.a_got); Log(sb);
        return 0; // let the GPU readbacks finish across the next frames
    }

    // ---------- phase 2: poll every stream, then copy its full raw bytes ----------
    bool all_done = true;
    for (int s = 0; s < gp.nstreams; ++s) {
        auto& z = gp.sr[s];
        if (!z.dispatched) { all_done = false; continue; }
        if (z.done || z.err) continue;
        void* req = UnboxThis(z.req);
        if (!req) { z.err = true; continue; }
        void* sp[1]{ req };
        bool done = false, ef = false;
        if (!SafeUnbox(Invoke(isdone, nullptr, sp), &done, sizeof(done), &ef) || ef) { all_done = false; continue; }
        if (!done) { all_done = false; continue; }
        bool herr = false;
        if (SafeUnbox(Invoke(haserr, nullptr, sp), &herr, sizeof(herr), &ef) && !ef && herr) { z.err = true; continue; }
        int32_t layer = 0; void* dp[2]{ req, &layer };
        void* data = UnboxPtr(Invoke(getraw, nullptr, dp));
        if (!data) { z.err = true; continue; }
        size_t whole = (size_t)z.count * (size_t)z.stride;
        z.bytes.resize(whole);
        if (!SafeMemcpy(z.bytes.data(), data, whole)) { z.err = true; continue; }
        z.done = true;
        MethodContract* relnow = Contract("gb.release"); // staging bytes captured -> release our buffer
        if (relnow && relnow->resolved && z.stage) { Invoke(relnow, z.stage, nullptr); z.stage = nullptr; }
    }
    if (!all_done) return 0;

    // Stream0 Position finite/AABB diagnostic (same sanity check as v0.9.1) + first-96-byte hex.
    {
        auto& z0 = gp.sr[0];
        const int32_t stride = z0.stride;
        int32_t take = z0.count;
        if (gp.expect_vc > 0 && take > gp.expect_vc) take = gp.expect_vc;
        bool first = true;
        if (stride > 0 && !z0.bytes.empty()) {
            for (int32_t v = 0; v < take && (size_t)v * stride + 12 <= z0.bytes.size(); ++v) {
                const float* fp = reinterpret_cast<const float*>(z0.bytes.data() + (size_t)v * stride);
                BE_Vec3 p{ fp[0], fp[1], fp[2] };
                if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
                    std::abs(p.x) < 1.0e6f && std::abs(p.y) < 1.0e6f && std::abs(p.z) < 1.0e6f) {
                    ++R.m_finite;
                    if (first) { R.mmn = R.mmx = p; first = false; }
                    else {
                        if (p.x < R.mmn.x) R.mmn.x = p.x; if (p.y < R.mmn.y) R.mmn.y = p.y; if (p.z < R.mmn.z) R.mmn.z = p.z;
                        if (p.x > R.mmx.x) R.mmx.x = p.x; if (p.y > R.mmx.y) R.mmx.y = p.y; if (p.z > R.mmx.z) R.mmx.z = p.z;
                    }
                }
            }
        }
        R.ok = (R.m_finite > 0);
        int hb = (int)z0.bytes.size(); if (hb > 96) hb = 96;
        char rh09[120]; std::snprintf(rh09, sizeof(rh09), "RAWM96 %s mc=%d mst=%d bytes=%zu",
            gp.name.c_str(), z0.count, z0.stride, z0.bytes.size());
        R.mraw = rh09;
        for (int off = 0; off < hb; off += 16) {
            char row[120]; int p = std::snprintf(row, sizeof(row), "  %04x:", off);
            for (int j = 0; j < 16 && off + j < hb; ++j) p += std::snprintf(row + p, sizeof(row) - p, " %02x", z0.bytes[off + j]);
            p += std::snprintf(row + p, sizeof(row) - p, "\n");
            if (p < 0 || p > (int)sizeof(row) - 1) p = (int)sizeof(row) - 1;
            R.mraw += row;
        }
    }
    char ob[520]; std::snprintf(ob, sizeof(ob),
        "gpu-probe[%s] v0.9.3 OK finite=%d/%d s0=%zuB s1=%zuB s2=%zuB idx=%d(src%d) bind=%d bw=%d bones=%d bindAABB %.3f %.3f %.3f ~ %.3f %.3f %.3f",
        gp.name.c_str(), R.m_finite, gp.expect_vc, gp.sr[0].bytes.size(),
        gp.nstreams > 1 ? gp.sr[1].bytes.size() : (size_t)0,
        gp.nstreams > 2 ? gp.sr[2].bytes.size() : (size_t)0,
        R.idx_count, R.idx_src, R.bind_bones, R.bw_verts, R.bones_n,
        R.mmn.x, R.mmn.y, R.mmn.z, R.mmx.x, R.mmx.y, R.mmx.z); Log(ob);
    return 1;
}

// v0.9.3: write a concise overview txt AND a folder of full raw materials per part
// (stream0/1/2 vertex bytes [s2=weights/joints], triangle indices, bindposes, bone weights, ordered bone names)
// for offline Python glTF assembly with zero further game visits.
void WriteGpuProbeFile(DWORD elapsed_ms) {
    wchar_t local[MAX_PATH];
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return;
    const std::wstring root = std::wstring(local) + L"\\BetterEndfield";
    const std::wstring dir = root + L"\\scene-export";
    CreateDirectoryW(root.c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
    const unsigned long long tick64 = GetTickCount64();

    wchar_t rdir[MAX_PATH * 2];
    swprintf_s(rdir, _countof(rdir), L"%ls\\chen_raw_%llu", dir.c_str(), tick64);
    CreateDirectoryW(rdir, nullptr);

    auto DumpBin = [&](const wchar_t* fname, const std::vector<uint8_t>& bytes) -> bool {
        if (bytes.empty() || !fname) return false;
        wchar_t fp[MAX_PATH * 3]; swprintf_s(fp, _countof(fp), L"%ls\\%ls", rdir, fname);
        FILE* f = nullptr; if (_wfopen_s(&f, fp, L"wb") != 0 || !f) return false;
        size_t w = fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); return w == bytes.size();
    };

    // ---- per-part manifest + raw files ----
    wchar_t mf[MAX_PATH * 3]; swprintf_s(mf, _countof(mf), L"%ls\\manifest.txt", rdir);
    FILE* man = nullptr;
    if (_wfopen_s(&man, mf, L"w, ccs=UTF-8") == 0 && man) {
        fwprintf(man, L"Chen raw material capture v0.9.3  %lu ms\n", (unsigned long)elapsed_ms);
        fwprintf(man, L"DEVICE type=%d name='%ls'\n\n", g_dev_type, Utf8ToWide(g_dev_name).c_str());
        for (auto& gp : g_gpu_parts) {
            const GpuPartResult& R = gp.pr;
            const std::wstring wn = Utf8ToWide(gp.name);
            for (int s = 0; s < gp.nstreams; ++s) {
                wchar_t fn[192]; swprintf_s(fn, _countof(fn), L"%hs_s%d.bin", gp.name.c_str(), s);
                DumpBin(fn, gp.sr[s].bytes);
            }
            if (!gp.indices.empty()) {
                std::vector<uint8_t> raw(gp.indices.size() * 4);
                std::memcpy(raw.data(), gp.indices.data(), raw.size());
                wchar_t fn[192]; swprintf_s(fn, _countof(fn), L"%hs_idx.bin", gp.name.c_str());
                DumpBin(fn, raw);
            }
            if (!gp.bindposes.empty()) { wchar_t fn[192]; swprintf_s(fn, _countof(fn), L"%hs_bind.bin", gp.name.c_str()); DumpBin(fn, gp.bindposes); }
            if (!gp.boneweights.empty()) { wchar_t fn[192]; swprintf_s(fn, _countof(fn), L"%hs_bw.bin", gp.name.c_str()); DumpBin(fn, gp.boneweights); }
            if (!gp.bones_names.empty()) {
                wchar_t fn[192]; swprintf_s(fn, _countof(fn), L"%hs_bones.txt", gp.name.c_str());
                wchar_t bp[MAX_PATH * 3]; swprintf_s(bp, _countof(bp), L"%ls\\%ls", rdir, fn);
                FILE* bf = nullptr;
                if (_wfopen_s(&bf, bp, L"w, ccs=UTF-8") == 0 && bf) {
                    fwprintf(bf, L"%ls", Utf8ToWide(gp.bones_names).c_str()); fclose(bf);
                }
            }
            fwprintf(man, L"[%ls] vertexCount=%d readable=%d nstreams=%d\n", wn.c_str(), gp.expect_vc, R.m_readable, gp.nstreams);
            for (int s = 0; s < gp.nstreams; ++s)
                fwprintf(man, L"  stream%d count=%d stride=%d bytes=%zu dispatched=%d done=%d err=%d\n",
                    s, R.s_count[s], R.s_stride[s], gp.sr[s].bytes.size(),
                    (int)gp.sr[s].dispatched, (int)gp.sr[s].done, (int)gp.sr[s].err);
            fwprintf(man, L"  layout: %ls\n", Utf8ToWide(R.layout).c_str());
            fwprintf(man, L"  submesh=%d indices=%d src=%d got=%d ; bindposes=%d got=%d ; bwVerts=%d got=%d ; bones=%d got=%d\n",
                R.submesh_count, R.idx_count, R.idx_src, (int)R.idx_got, R.bind_bones, (int)R.bind_got,
                R.bw_verts, (int)R.bw_got, R.bones_n, (int)R.bones_got);
            fwprintf(man, L"  sub_idxcount:");
            for (size_t k = 0; k < gp.sub_idxcount.size(); ++k) fwprintf(man, L" %d", gp.sub_idxcount[k]);
            fwprintf(man, L"\n");
            fwprintf(man, L"  bindPose Position AABB %.5f %.5f %.5f ~ %.5f %.5f %.5f finite=%d\n",
                R.mmn.x, R.mmn.y, R.mmn.z, R.mmx.x, R.mmx.y, R.mmx.z, R.m_finite);
            if (!R.mraw.empty()) fwprintf(man, L"  %ls\n", Utf8ToWide(R.mraw).c_str());
            fwprintf(man, L"\n");
        }
        fclose(man);
    }

    // ---- concise overview for quick success/failure reading ----
    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, _countof(path), L"%ls\\chen_gpu_vertex_%llu.txt", dir.c_str(), tick64);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w, ccs=UTF-8") != 0 || !file) { Log("gpu-probe: open overview fail."); return; }
    int okcnt = 0; for (auto& gp : g_gpu_parts) if (gp.pr.ok) ++okcnt;
    fwprintf(file, L"Chen v0.9.3 raw capture overview  %lu ms (full materials in chen_raw_%llu)\n", (unsigned long)elapsed_ms, tick64);
    fwprintf(file, L"DEVICE type=%d name='%ls'\n", g_dev_type, Utf8ToWide(g_dev_name).c_str());
    fwprintf(file, L"parts: %d, stream0 readback ok: %d\n\n", (int)g_gpu_parts.size(), okcnt);
    fwprintf(file, L"%-28ls %6s %5s %7s %6s %9s %9s %8s %6s %6s %6s %5s\n",
        L"part", L"expect", L"read", L"s0cnt", L"s0st", L"s0bytes", L"s2bytes", L"idx/src", L"bind", L"bw", L"bones", L"ok");
    for (auto& gp : g_gpu_parts) {
        const GpuPartResult& R = gp.pr;
        const std::wstring wn = Utf8ToWide(gp.name);
        fwprintf(file, L"%-28ls %6d %5d %7d %6d %9zu %9zu %4d/%-2d %6d %6d %6d %5s\n", wn.c_str(),
            gp.expect_vc, R.m_readable, R.s_count[0], R.s_stride[0], gp.sr[0].bytes.size(),
            gp.nstreams > 2 ? gp.sr[2].bytes.size() : (size_t)0,
            R.idx_count, R.idx_src, R.bind_bones, R.bw_verts, R.bones_n, R.ok ? L"TRUE" : L"false");
    }
    fclose(file);
    char m[260]; std::snprintf(m, sizeof(m),
        "gpu-probe v0.9.3 written: %d/%d parts; raw folder chen_raw_%llu.", okcnt, (int)g_gpu_parts.size(), tick64);
    Log(m);
}

// Driven every frame by DetourPump. Wait >=3 frames for re-skinning, collect
// each part, and finalize once all are ready or after a 12-frame budget.
void TickGpuVertexReadback() {
    if (g_gpu_phase.load(std::memory_order_acquire) != 1) return;
    ++g_gpu_wait;
    if (g_gpu_wait < 3) return; // v0.9.0: static bind-pose buffer already exists; only let AsyncGPUReadback cross a few frames (no rebuild wait)
    for (auto& gp : g_gpu_parts) {
        if (gp.collected) continue;
        // v0.7.8: throttle parts whose lod0 buffer is absent, to avoid thousands of SEH probes/frame
        if (!gp.requested && gp.miss > 0 && (g_gpu_wait % 6) != 0) continue;
        if (CollectOnePart(gp) == 1) gp.collected = true; // 0 = GPU buffer not ready, retry later
    }
    bool all = true;
    for (auto& gp : g_gpu_parts) if (!gp.collected) { all = false; break; }
    if (!all) {
        // ~2.5s budget: hidden parts must first be re-uploaded, then the GPU readback completes.
        if (g_gpu_wait < 150) return;
        Log("gpu-probe: wait budget exhausted, finalize with whatever is ready.");
    }
    WriteGpuProbeFile(GetTickCount() - g_gpu_start_tick);
    // Release any staging buffer that never finished (finished ones already released in phase 2).
    { MethodContract* rel09 = Contract("gb.release");
      for (auto& gp : g_gpu_parts) for (int s = 0; s < gp.nstreams; ++s)
          if (rel09 && rel09->resolved && gp.sr[s].stage) Invoke(rel09, gp.sr[s].stage, nullptr); }
    for (auto& gp : g_gpu_parts) {
        if (gp.renderer_root) g_host->gchandle_free(g_host->context, gp.renderer_root);
        if (gp.mesh_root) g_host->gchandle_free(g_host->context, gp.mesh_root);
        for (int s = 0; s < 3; ++s) {
            if (gp.sr[s].src_root)   g_host->gchandle_free(g_host->context, gp.sr[s].src_root);
            if (gp.sr[s].stage_root) g_host->gchandle_free(g_host->context, gp.sr[s].stage_root);
            if (gp.sr[s].req_root)   g_host->gchandle_free(g_host->context, gp.sr[s].req_root);
        }
    }
    g_gpu_parts.clear();
    g_gpu_phase.store(0, std::memory_order_release);
    Log("gpu-probe FINISHED, back to idle.");
}


void RunExportOnMainThread() {
    ExportCameras();        // sentinel: proves inject -> pump -> enumerate -> file pipeline is alive
    ExportSkinnedMeshes();  // whole-scene SMR census for cross-check
    StartGpuVertexReadback(); // stage 1 of the v0.7.8 cross-frame GPU readback
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
        // 组合热键 Ctrl+E（E=Export 导出）。不用 Shift：Shift 是游戏冲刺键，三键同按会让角色迈步，
        // 也不利于以后多角色横列站定抓拍；Ctrl+E 游戏未绑定。边沿触发保证按住只导出一次。
        const bool down = GameWindowHasFocus() && IsKeyDown(VK_CONTROL) && IsKeyDown('E');
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
    TickGpuVertexReadback(); // advance the cross-frame GPU readback state machine every frame
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
                       Contract("skinned.bake_mesh")->resolved &&
                       Contract("renderer.is_visible")->resolved &&
                       Contract("skinned.get_vb")->resolved &&
                       Contract("mesh.get_vb_stream")->resolved &&
                       Contract("gb.ctor")->resolved &&
                       Contract("gb.release")->resolved &&
                       Contract("graphics.copy_buffer")->resolved &&
                       Contract("gb.get_count")->resolved &&
                       Contract("gb.get_stride")->resolved &&
                       Contract("mesh.attr_count")->resolved &&
                       Contract("mesh.get_attr")->resolved &&
                       Contract("gpuread.request")->resolved &&
                       Contract("gpr.is_done")->resolved &&
                       Contract("gpr.has_error")->resolved &&
                       Contract("gpr.get_data_raw")->resolved;
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

    // v0.7.0：解析嵌套类 UnityEngine.Mesh.MeshDataArray（点号外层.内层，供 object_new 装箱）。
    BE_ResolvedClassV1 mda_class{};
    if (host->resolve_class(host->context, "UnityEngine.CoreModule.dll",
            "UnityEngine", "Mesh.MeshDataArray", &mda_class) == BE_Result_Ok && mda_class.class_info) {
        g_mda_class_info = const_cast<void*>(mda_class.class_info);
        Log("Resolved support class: UnityEngine.Mesh.MeshDataArray");
    } else {
        Log("Could not resolve UnityEngine.Mesh.MeshDataArray nested class (MeshData direct channel disabled).");
    }

    // v0.9.0: resolve UnityEngine.GraphicsBuffer (object_new a staging buffer for GPU CopyBuffer relay).
    BE_ResolvedClassV1 gb_class{};
    if (host->resolve_class(host->context, "UnityEngine.CoreModule.dll",
            "UnityEngine", "GraphicsBuffer", &gb_class) == BE_Result_Ok && gb_class.class_info) {
        g_gb_class_info = const_cast<void*>(gb_class.class_info);
        Log("Resolved support class: UnityEngine.GraphicsBuffer");
    } else {
        Log("Could not resolve UnityEngine.GraphicsBuffer class (staging relay disabled).");
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
    Log("Scene Exporter v0.9.3 ready (READ-ONLY full raw capture: vertex streams 0/1/2 via CopyBuffer staging -> AsyncGPUReadback [s2=weights/joints], indices via CPU then read-only MeshData, bindposes/weights/bones -> chen_raw folder; no target mutation). Focus game, stand Chen at MID range full body, press Ctrl+E.");
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
    if (g_host && g_host->gchandle_free) {
        for (auto& gp : g_gpu_parts) {
            if (gp.renderer_root) g_host->gchandle_free(g_host->context, gp.renderer_root);
            if (gp.mesh_root) g_host->gchandle_free(g_host->context, gp.mesh_root);
        }
    }
    g_gpu_parts.clear();
    g_gpu_phase.store(0);
    g_target_type_root = 0;
    g_target_type = nullptr;
    g_skinned_type_root = 0;
    g_skinned_type = nullptr;
    g_host = nullptr;
}

const BE_ModuleApiV1 kApi{
    {kModuleId, "Scene Exporter", "0.9.3", BETTER_ENDFIELD_MODULE_ABI_V1},
    &Initialize, &ConfigurationChanged, &Shutdown};

} // namespace
} // namespace BetterEndfield::Exporter

BE_EXPORT const BE_ModuleApiV1* BE_CALL BetterEndfield_GetModuleApiV1() {
    return &BetterEndfield::Exporter::kApi;
}
