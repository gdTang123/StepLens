// StepLens M0 性能摸底 spike（方案 B 命令行原型）
// 用法：
//   spike bench  <file> [--no-mesh]             读取→转换→释放读取器→统计→三角化(粗/方案B/细)
//   spike cancel-read <file> <fraction>         读取阶段在已读字节达到 fraction 时由另一线程请求取消
//   spike cancel-transfer <file> <fraction>     转换阶段在进度达到 fraction 时由另一线程请求取消
//   spike cancel-parse <file> [delay_s]         流读到 EOF 后再等 delay_s（默认 2 秒）请求取消。
//                                               请求赶在 ReadStream
//                                               返回前：cancel_stage=parsing，cancel_requested=1。
//                                               读取先结束：cancel_stage=not_covered，cancel_requested=0。读取失败退出码非
//                                               0。
// 所有输出为 "key=value" 行，便于汇总。
// Windows 上用宽字符命令行打开文件，交给 OCCT 的文件名是 UTF-8（7.9.3 / 8.0.1 按 UTF-8 转成宽路径）。
#include "occtsupport/occt_compat.h"

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Gravity.hxx>
#include <Message_Messenger.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <OSD_Parallel.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <Standard_Version.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
#include <psapi.h>
#include <DbgHelp.h>
#include <shellapi.h>
// clang-format on
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shell32.lib")
#include <malloc.h>
#else
#include <malloc.h>
#include <sys/resource.h>
#endif
#include <Standard_Failure.hxx>

#include <typeinfo>

using Clock = std::chrono::steady_clock;
static Clock::time_point g_t0 = Clock::now();
static double now_s() {
    return std::chrono::duration<double>(Clock::now() - g_t0).count();
}

// ---------- 内存 ----------
#ifdef _WIN32
// Windows：工作集（≈ RSS）+ 提交内存（Private Bytes）。PeakWorkingSet 不能重置，
// 所以用 20 ms 采样线程记录"自上次 resetHwm 以来"的峰值。
static PROCESS_MEMORY_COUNTERS_EX memNow() {
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof(c);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c));
    return c;
}
static double rssMB() {
    return memNow().WorkingSetSize / 1048576.0;
}
static double commitMB() {
    return memNow().PrivateUsage / 1048576.0;
}
static std::atomic<double> g_hwm{0}, g_hwmCommit{0};
static void sampleOnce() {
    auto c = memNow();
    double w = c.WorkingSetSize / 1048576.0, p = c.PrivateUsage / 1048576.0;
    double o = g_hwm.load();
    while (w > o && !g_hwm.compare_exchange_weak(o, w)) {
    }
    o = g_hwmCommit.load();
    while (p > o && !g_hwmCommit.compare_exchange_weak(o, p)) {
    }
}
static void startSampler() {
    static std::thread t([] {
        for (;;) {
            sampleOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    t.detach();
}
static double hwmMB() {
    sampleOnce();
    return g_hwm.load();
}
static double hwmCommitMB() {
    sampleOnce();
    return g_hwmCommit.load();
}
static void resetHwm() {
    g_hwm = rssMB();
    g_hwmCommit = commitMB();
}
static double maxrssMB() {
    return memNow().PeakWorkingSetSize / 1048576.0;
}
static double maxCommitMB() {
    return memNow().PeakPagefileUsage / 1048576.0;
}
static void malloc_trim(int) {
    _heapmin();
} // Windows 对应：把空闲堆还给系统

// 崩溃时：写 minidump（spike-crash.dmp）并把带符号的调用栈打印到 stderr（需要 exe 和 OCCT DLL 旁边有 PDB）
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "crash_exception_code=0x%08lX\n", ep->ExceptionRecord->ExceptionCode);
    HANDLE f = CreateFileA("spike-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                          &mei, nullptr, nullptr);
        CloseHandle(f);
        std::fprintf(stderr, "crash_dump=spike-crash.dmp\n");
    }
    HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 fr{};
    fr.AddrPC.Offset = ctx.Rip;
    fr.AddrFrame.Offset = ctx.Rbp;
    fr.AddrStack.Offset = ctx.Rsp;
    fr.AddrPC.Mode = fr.AddrFrame.Mode = fr.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (int n = 0; n < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &fr, &ctx, nullptr,
                                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++n) {
        DWORD64 a = fr.AddrPC.Offset, d = 0;
        if (!a)
            break;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        char mod[MAX_PATH] = "?";
        DWORD64 base = SymGetModuleBase64(proc, a);
        if (base)
            GetModuleFileNameA(HMODULE(base), mod, MAX_PATH);
        IMAGEHLP_LINE64 ln{};
        ln.SizeOfStruct = sizeof(ln);
        DWORD disp = 0;
        bool hasSym = SymFromAddr(proc, a, &d, sym), hasLine = SymGetLineFromAddr64(proc, a, &disp, &ln);
        std::fprintf(stderr, "crash_frame_%02d=%s!%s+0x%llx %s:%lu\n", n,
                     std::strrchr(mod, '\\') ? std::strrchr(mod, '\\') + 1 : mod, hasSym ? sym->Name : "?",
                     (unsigned long long)d, hasLine ? ln.FileName : "", hasLine ? ln.LineNumber : 0);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; // 进程以异常码退出
}
#else
static long procStatusKB(const char* key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    size_t n = strlen(key);
    while (std::getline(f, line))
        if (line.compare(0, n, key) == 0)
            return std::stol(line.substr(n + 1));
    return -1;
}
static double rssMB() {
    return procStatusKB("VmRSS") / 1024.0;
}
static double hwmMB() {
    return procStatusKB("VmHWM") / 1024.0;
} // 峰值 RSS（可被重置）
static void resetHwm() {
    std::ofstream f("/proc/self/clear_refs");
    f << "5";
} // 重置 VmHWM
static double maxrssMB() {
    rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss / 1024.0;
}
#endif
static void kv(const char* k, double v) {
    std::printf("%s=%.6g\n", k, v);
    std::fflush(stdout);
}
static void kvs(const char* k, const std::string& v) {
    std::printf("%s=%s\n", k, v.c_str());
    std::fflush(stdout);
}

#ifdef _WIN32
static std::string utf8FromWide(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0')
        return {};
    // C++17 path::u8string() 是 UTF-8 的 std::string。不用 WideCharToMultiByte：
    // WIN32_LEAN_AND_MEAN 下 CI 的 Windows SDK 不从 Windows.h 带出这个 API。
    return std::filesystem::path(text).u8string();
}
#endif

static std::string utf8FromPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return utf8FromWide(path.c_str());
#else
    return path.string();
#endif
}

static FILE* openBinary(const std::filesystem::path& path) {
#ifdef _WIN32
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0)
        return nullptr;
    return file;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

struct WatcherJoin {
    std::thread& thread;
    std::atomic<bool>& done;
    ~WatcherJoin() {
        done.store(true, std::memory_order_release);
        if (thread.joinable())
            thread.join();
    }
};

// ---------- 取消标志 ----------
static std::atomic<bool> g_cancel{false};
static std::atomic<double> g_cancelReqTime{-1};
// 只有取消线程在 ReadStream 返回之前真正发出请求时才置位。
static std::atomic<bool> g_cancelIssued{false};

// ---------- 带进度/取消的 streambuf ----------
class ProgressStreamBuf : public std::streambuf {
public:
    explicit ProgressStreamBuf(const std::filesystem::path& path, size_t chunk = 1 << 20) : m_buf(chunk) {
        m_f = openBinary(path);
        if (m_f == nullptr)
            return;
        if (std::fseek(m_f, 0, SEEK_END) != 0) {
            std::fclose(m_f);
            m_f = nullptr;
            return;
        }
        const long sz = std::ftell(m_f);
        if (sz < 0) {
            std::fclose(m_f);
            m_f = nullptr;
            return;
        }
        m_total = static_cast<uint64_t>(sz);
        if (std::fseek(m_f, 0, SEEK_SET) != 0) {
            std::fclose(m_f);
            m_f = nullptr;
            m_total = 0;
        }
    }
    ~ProgressStreamBuf() override {
        if (m_f)
            std::fclose(m_f);
    }
    bool ok() const {
        return m_f != nullptr;
    }
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> calls{0};
    // steady_clock 的 time_since_epoch 计数；-1 表示流还没读到 EOF。给取消线程看。
    std::atomic<long long> eofAtNs{-1};
    double tFirst = -1, tEof = -1, tCancelEof = -1;
    uint64_t total() const {
        return m_total;
    }
    std::vector<std::pair<double, double>> samples; // (时间, 已读比例)，每 5% 记一次
protected:
    int_type underflow() override {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        calls++;
        if (tFirst < 0)
            tFirst = now_s();
        if (g_cancel.load()) {
            if (tCancelEof < 0)
                tCancelEof = now_s();
            return traits_type::eof();
        }
        size_t n = m_f == nullptr ? 0 : std::fread(m_buf.data(), 1, m_buf.size(), m_f);
        if (n == 0) {
            if (tEof < 0) {
                tEof = now_s();
                eofAtNs.store(static_cast<long long>(Clock::now().time_since_epoch().count()),
                              std::memory_order_release);
            }
            return traits_type::eof();
        }
        uint64_t b = bytes += n;
        double frac = m_total == 0 ? 1.0 : double(b) / double(m_total);
        if (samples.empty() || frac - samples.back().second >= 0.05 || b == m_total)
            samples.push_back({now_s(), frac});
        setg(m_buf.data(), m_buf.data(), m_buf.data() + n);
        return traits_type::to_int_type(*gptr());
    }

private:
    FILE* m_f = nullptr;
    std::vector<char> m_buf;
    uint64_t m_total = 0;
};

// ---------- OCCT 进度桥 ----------
class Bridge : public Message_ProgressIndicator {
public:
    std::atomic<double> pos{0};
    std::atomic<uint64_t> nShow{0}, nBreak{0};
    double lastBreak = -1, maxBreakGap = 0, maxBreakGapAt = 0;
    double tStart = 0;
    std::mutex mu;
    bool UserBreak() override {
        nBreak++;
        double t = now_s();
        {
            std::lock_guard<std::mutex> l(mu);
            double prev = lastBreak < 0 ? tStart : lastBreak;
            if (t - prev > maxBreakGap) {
                maxBreakGap = t - prev;
                maxBreakGapAt = GetPosition();
                maxBreakGapEnd = t;
            }
            lastBreak = t;
        }
        return g_cancel.load();
    }
    std::vector<std::pair<double, double>> samples; // (时间, 进度)，进度每涨 2% 或每过 2 秒记一次
    double maxBreakGapEnd = 0;
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {
        nShow++;
        double p = GetPosition();
        pos = p;
        double t = now_s();
        std::lock_guard<std::mutex> l(mu);
        if (samples.empty() || p - samples.back().second >= 0.02 || t - samples.back().first >= 2.0)
            samples.push_back({t, p});
    }
    void Reset() override {
        Message_ProgressIndicator::Reset();
    }
};

// ---------- 读取 ----------
struct Loaded {
    opencascade::handle<TDocStd_Document> doc;
    bool ok = false;
};
static bool isIges(const std::string& p) {
    std::string e = p.substr(p.find_last_of('.') + 1);
    for (auto& c : e)
        c = (char)tolower(c);
    return e == "igs" || e == "iges";
}
static opencascade::handle<TDocStd_Document> newDoc() {
    opencascade::handle<TDocStd_Document> doc;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
    return doc;
}
static void setupStepReader(STEPCAFControl_Reader& r) {
    r.SetColorMode(true);
    r.SetNameMode(true);
    r.SetLayerMode(false);
    r.SetPropsMode(false);
    r.SetGDTMode(false);
    r.SetMatMode(false);
    r.SetViewMode(false);
    r.SetSHUOMode(true);
}

// ---------- 统计 ----------
struct Proto {
    TopoDS_Shape shape;
    int faces = 0;
    int instances = 0;
};
struct Stats {
    long protoFaces = 0, protoSolids = 0, instFaces = 0, instSolids = 0;
    long nProtos = 0, nInstances = 0, nFreeShapes = 0;
};
// 收集原型（简单形状标签）与实例数（按装配展开）
static void walk(const opencascade::handle<XCAFDoc_ShapeTool>& st, const TDF_Label& lab,
                 std::unordered_map<int, int>& instCount, std::vector<TDF_Label>& protoLabels,
                 std::unordered_map<int, int>& labIndex, int mult) {
    TDF_Label ref = lab;
    if (XCAFDoc_ShapeTool::IsReference(lab))
        XCAFDoc_ShapeTool::GetReferredShape(lab, ref);
    if (XCAFDoc_ShapeTool::IsAssembly(ref)) {
        TDF_LabelSequence comps;
        XCAFDoc_ShapeTool::GetComponents(ref, comps, false);
        for (int i = 1; i <= comps.Length(); ++i)
            walk(st, comps(i), instCount, protoLabels, labIndex, mult);
        return;
    }
    int tag = ref.Tag();
    // 用标签条目路径作键（Tag 在 shapes 根下唯一）
    if (!labIndex.count(tag)) {
        labIndex[tag] = (int)protoLabels.size();
        protoLabels.push_back(ref);
    }
    instCount[tag] += mult;
}

static size_t sumTriangles(const TopoDS_Shape& s, long* facesNoTri) {
    size_t n = 0;
    long miss = 0;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopLoc_Location loc;
        auto tri = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
        if (tri.IsNull())
            miss++;
        else
            n += tri->NbTriangles();
    }
    if (facesNoTri)
        *facesNoTri += miss;
    return n;
}
static double diag(const TopoDS_Shape& s) {
    Bnd_Box b;
    BRepBndLib::Add(s, b, false);
    if (b.IsVoid())
        return 0;
    return std::sqrt(b.SquareExtent());
}

struct MeshLevel {
    const char* name;
    double c, cg, angleDeg;
    bool ctrlSurf, allowQD;
};

static void meshLevel(std::vector<Proto>& protos, const std::vector<double>& pdiag, double gdiag,
                      const MeshLevel& L) {
    for (auto& p : protos)
        BRepTools::Clean(p.shape);
    malloc_trim(0);
    resetHwm();
    double rss0 = rssMB();
    const int bigFaces = 2000;
    std::vector<int> small, big;
    for (int i = 0; i < (int)protos.size(); ++i)
        (protos[i].faces > bigFaces ? big : small).push_back(i);
    auto params = [&](int i, bool par) {
        IMeshTools_Parameters p;
        double d = L.c * pdiag[i];
        double dmax = L.cg * gdiag;
        if (d > dmax)
            d = dmax;
        if (d < 0.005)
            d = 0.005;
        p.Deflection = d;
        p.Angle = L.angleDeg * M_PI / 180.0;
        p.Relative = false;
        p.InParallel = par;
        p.MinSize = d * 0.1;
        p.ControlSurfaceDeflection = L.ctrlSurf;
        p.AllowQualityDecrease = L.allowQD;
        p.CleanModel = true;
        return p;
    };
    double t0 = now_s();
    OSD_Parallel::For(0, (int)small.size(), [&](int k) {
        int i = small[k];
        BRepMesh_IncrementalMesh m(protos[i].shape, params(i, false));
    });
    double t1 = now_s();
    for (int i : big) {
        BRepMesh_IncrementalMesh m(protos[i].shape, params(i, true));
    }
    double t2 = now_s();
    size_t triProto = 0, triInst = 0;
    long miss = 0;
    for (auto& p : protos) {
        size_t n = sumTriangles(p.shape, &miss);
        triProto += n;
        triInst += n * p.instances;
    }
    std::string pre = std::string("mesh_") + L.name + "_";
    kv((pre + "c").c_str(), L.c);
    kv((pre + "cg").c_str(), L.cg);
    kv((pre + "angle_deg").c_str(), L.angleDeg);
    kv((pre + "time_small_s").c_str(), t1 - t0);
    kv((pre + "time_big_s").c_str(), t2 - t1);
    kv((pre + "time_s").c_str(), t2 - t0);
    kv((pre + "big_protos").c_str(), big.size());
    kv((pre + "tri_proto").c_str(), triProto);
    kv((pre + "tri_instances").c_str(), triInst);
    kv((pre + "faces_without_tri").c_str(), miss);
    kv((pre + "rss_before_mb").c_str(), rss0);
    kv((pre + "rss_after_mb").c_str(), rssMB());
    kv((pre + "peak_rss_mb").c_str(), hwmMB());
#ifdef _WIN32
    kv((pre + "peak_commit_mb").c_str(), hwmCommitMB());
#endif
}

// ---------- 主流程 ----------
// 解析盲区取消的收尾：只有请求落在 ReadStream 返回之前才算 cancel_stage=parsing。
// 读取失败必须非 0 退出，不能被“取消测试”当成成功。
static int finishCancelParse(double tr0, double tr1, double delay, bool readOk, ProgressStreamBuf* sb) {
    const double req = g_cancelReqTime.load(std::memory_order_acquire);
    const bool issued = g_cancelIssued.load(std::memory_order_acquire);
    const bool duringParse = issued && req >= 0.0 && req <= tr1;
    kv("cancel_requested", duringParse ? 1.0 : 0.0);
    kv("cancel_delay_s", delay);
    if (sb != nullptr && sb->tEof >= 0)
        kv("read_end_after_eof_s", tr1 - sb->tEof);
    if (issued && req >= 0.0) {
        // 相对读完时刻：负数表示请求发生在 ReadStream 返回之前。
        kv("cancel_req_minus_read_end_s", req - tr1);
        kv("cancel_req_at_s", req - tr0);
        if (sb != nullptr && sb->tEof >= 0)
            kv("cancel_after_eof_s", req - sb->tEof);
    }
    if (duringParse) {
        kvs("cancel_stage", "parsing");
        kv("cancel_latency_readstream_return_ms", (tr1 - req) * 1000.0);
    } else {
        kvs("cancel_stage", "not_covered");
    }
    if (!readOk) {
        kvs("error", "read failed");
        return 2;
    }
    return 0;
}

static int run(const std::string& mode, const std::filesystem::path& path, double frac, bool doMesh) {
    g_cancel.store(false, std::memory_order_relaxed);
    g_cancelReqTime.store(-1.0, std::memory_order_relaxed);
    g_cancelIssued.store(false, std::memory_order_relaxed);
    const std::string pathUtf8 = utf8FromPath(path);
    kvs("file", pathUtf8);
    kvs("mode", mode);
    kvs("occt_version", OCC_VERSION_COMPLETE);
    kv("threads_hw", std::thread::hardware_concurrency());
    kv("rss_start_mb", rssMB());
    bool iges = isIges(pathUtf8);
    if (mode == "cancel-parse" && iges) {
        kv("cancel_requested", 0);
        kvs("cancel_stage", "not_covered");
        kvs("error", "cancel-parse requires STEP");
        kv("read_ok", 0);
        return 2;
    }
    auto doc = newDoc();
    std::unique_ptr<STEPCAFControl_Reader> sr;
    std::unique_ptr<IGESCAFControl_Reader> ir;

    // ---- 阶段1：读取 ----
    std::unique_ptr<ProgressStreamBuf> sb;
    std::thread watcher;
    std::atomic<bool> readDone{false};
    WatcherJoin joinWatcher{watcher, readDone};
    resetHwm();
    double tr0 = now_s();
    bool readOk = false;
    if (!iges) {
        sr = std::make_unique<STEPCAFControl_Reader>();
        setupStepReader(*sr);
        sb = std::make_unique<ProgressStreamBuf>(path);
        if (!sb->ok()) {
            kvs("error", "open failed");
            kv("read_ok", 0);
            if (mode == "cancel-parse")
                return finishCancelParse(tr0, now_s(), frac, false, nullptr);
            return 2;
        }
        if (mode == "cancel-read") {
            watcher = std::thread([&] {
                while (sb->bytes.load() < frac * sb->total())
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                g_cancelReqTime = now_s();
                g_cancel = true;
            });
        } else if (mode == "cancel-parse") {
            // 流已经读到 EOF、ReadStream 还在建实体的那段没有 underflow 检查点。
            // 等到 EOF 之后再延迟 frac 秒请求取消，用来量这段盲区要多久才返回。
            watcher = std::thread([&] {
                long long stamp = -1;
                while ((stamp = sb->eofAtNs.load(std::memory_order_acquire)) < 0) {
                    if (readDone.load(std::memory_order_acquire))
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                const auto eofTp = Clock::time_point(Clock::duration(static_cast<Clock::rep>(stamp)));
                const double eofAt = std::chrono::duration<double>(eofTp - g_t0).count();
                while (now_s() - eofAt < frac) {
                    if (readDone.load(std::memory_order_acquire))
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (readDone.load(std::memory_order_acquire))
                    return;
                // 先记下请求时刻，再发布“已发出”。读线程用 req<=读完时刻判断是否落在解析期间。
                g_cancelReqTime.store(now_s(), std::memory_order_relaxed);
                g_cancelIssued.store(true, std::memory_order_release);
                g_cancel.store(true, std::memory_order_release);
            });
        }
        std::istream in(sb.get());
        IFSelect_ReturnStatus st = sr->ReadStream(pathUtf8.c_str(), in);
        readOk = (st == IFSelect_RetDone);
        kv("read_status", (double)st);
    } else {
        ir = std::make_unique<IGESCAFControl_Reader>();
        {
            bool attrs = getenv("SPIKE_IGES_NO_ATTRS") == nullptr; // 对照：关掉颜色/名称读取
            ir->SetColorMode(attrs);
            ir->SetNameMode(attrs);
            ir->SetLayerMode(false);
        }
        IFSelect_ReturnStatus st = ir->ReadFile(pathUtf8.c_str());
        readOk = (st == IFSelect_RetDone);
        kv("read_status", (double)st);
    }
    double tr1 = now_s();
    readDone.store(true, std::memory_order_release);
    if (watcher.joinable())
        watcher.join();
    kv("read_time_s", tr1 - tr0);
    kv("read_ok", readOk);
    if (sb) {
        kv("stream_bytes", (double)sb->bytes.load());
        kv("stream_total", (double)sb->total());
        kv("stream_underflow_calls", (double)sb->calls.load());
        if (sb->tEof >= 0)
            kv("stream_eof_at_s", sb->tEof - tr0); // 流读完的时刻
        std::string s;
        for (auto& p : sb->samples) {
            char b[64];
            snprintf(b, 64, "%.2f@%.1f%% ", p.first - tr0, p.second * 100);
            s += b;
        }
        kvs("stream_progress_samples", s);
    }
    kv("read_rss_mb", rssMB());
    kv("read_peak_rss_mb", hwmMB());
#ifdef _WIN32
    kv("read_peak_commit_mb", hwmCommitMB());
#endif
    if (mode == "cancel-read") {
        double req = g_cancelReqTime.load();
        kv("cancel_req_at_s", req - tr0);
        if (sb->tCancelEof >= 0)
            kv("cancel_eof_returned_after_ms", (sb->tCancelEof - req) * 1000);
        kv("cancel_latency_readstream_return_ms", (tr1 - req) * 1000);
        double tx0 = now_s();
        sr.reset();
        doc.Nullify();
        malloc_trim(0);
        double tx1 = now_s();
        kv("cancel_release_ms", (tx1 - tx0) * 1000);
        kv("cancel_total_to_released_ms", (tx1 - req) * 1000);
        kv("rss_after_release_mb", rssMB());
        return 0;
    }
    if (mode == "cancel-parse") {
        double tx0 = now_s();
        sr.reset();
        doc.Nullify();
        malloc_trim(0);
        double tx1 = now_s();
        kv("cancel_release_ms", (tx1 - tx0) * 1000);
        kv("rss_after_release_mb", rssMB());
        return finishCancelParse(tr0, tr1, frac, readOk, sb.get());
    }
    if (!readOk) {
        kvs("error", "read failed");
        return 2;
    }

    // ---- 阶段2：转换 ----
    opencascade::handle<Bridge> br = new Bridge();
    std::thread w2;
    if (mode == "cancel-transfer") {
        w2 = std::thread([&] {
            // frac <= 1：按进度位置触发；frac > 1：按开始转换后的秒数触发
            if (frac <= 1) {
                while (br->pos.load() < frac)
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            } else {
                while (now_s() - br->tStart < frac)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            g_cancelReqTime = now_s();
            g_cancel = true;
        });
    }
    resetHwm();
    br->tStart = now_s();
    double tt0 = br->tStart;
    bool trOk = false;
    try {
        if (getenv("SPIKE_NO_PROGRESS"))
            trOk = sr ? sr->Transfer(doc) : ir->Transfer(doc); // 对照：不挂进度
        else
            trOk = sr ? sr->Transfer(doc, br->Start()) : ir->Transfer(doc, br->Start());
    } catch (const std::exception& e) {
        kvs("transfer_exception", e.what());
    }
    double tt1 = now_s();
    if (w2.joinable()) {
        if (!g_cancel) {
            g_cancel = true;
        }
        w2.join();
    }
    kv("transfer_time_s", tt1 - tt0);
    kv("transfer_ok", trOk);
    kv("transfer_rss_mb", rssMB());
    kv("transfer_peak_rss_mb", hwmMB());
#ifdef _WIN32
    kv("transfer_peak_commit_mb", hwmCommitMB());
#endif
    kv("transfer_show_calls", (double)br->nShow.load());
    kv("transfer_userbreak_calls", (double)br->nBreak.load());
    kv("transfer_max_userbreak_gap_s", br->maxBreakGap);
    kv("transfer_max_gap_at_pos", br->maxBreakGapAt);
    kv("transfer_final_pos", br->pos.load());
    kv("transfer_max_gap_end_at_s", br->maxBreakGapEnd - tt0);
    {
        std::string s;
        for (auto& p : br->samples) {
            char b[64];
            snprintf(b, 64, "%.1f@%.0f%% ", p.first - tt0, p.second * 100);
            s += b;
        }
        kvs("transfer_progress_samples", s);
    }
    if (mode == "cancel-transfer") {
        double req = g_cancelReqTime.load();
        kv("cancel_req_at_s", req - tt0);
        kv("cancel_latency_transfer_return_ms", (tt1 - req) * 1000);
        double tx0 = now_s();
        sr.reset();
        ir.reset();
        doc->Main().ForgetAllAttributes(true);
        doc.Nullify();
        malloc_trim(0);
        double tx1 = now_s();
        kv("cancel_release_ms", (tx1 - tx0) * 1000);
        kv("cancel_total_to_released_ms", (tx1 - req) * 1000);
        kv("rss_after_release_mb", rssMB());
        return 0;
    }

    // ---- 阶段3：释放读取器 ----
    double tf0 = now_s();
    sr.reset();
    ir.reset();
    double tf1 = now_s();
    double rssBeforeTrim = rssMB();
    malloc_trim(0);
    kv("release_reader_s", tf1 - tf0);
    kv("release_rss_before_trim_mb", rssBeforeTrim);
    kv("release_rss_after_trim_mb", rssMB());

    // ---- 阶段4：建模型（原型去重 + 计数） ----
    double tm0 = now_s();
    auto st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    TDF_LabelSequence frees;
    st->GetFreeShapes(frees);
    std::unordered_map<int, int> instCount;
    std::vector<TDF_Label> protoLabels;
    std::unordered_map<int, int> labIndex;
    for (int i = 1; i <= frees.Length(); ++i)
        walk(st, frees(i), instCount, protoLabels, labIndex, 1);
    std::vector<Proto> protos;
    Stats S;
    S.nFreeShapes = frees.Length();
    // 用 TShape 指针去重（同一个 TShape 被多个标签引用的情况）
    std::unordered_set<const void*> seenFace, seenSolid;
    BRep_Builder bb;
    TopoDS_Compound all;
    bb.MakeCompound(all);
    for (auto& lab : protoLabels) {
        Proto p;
        p.shape = XCAFDoc_ShapeTool::GetShape(lab);
        p.instances = instCount[lab.Tag()];
        if (p.shape.IsNull())
            continue;
        long f = 0, so = 0;
        for (TopExp_Explorer ex(p.shape, TopAbs_FACE); ex.More(); ex.Next()) {
            f++;
            seenFace.insert(ex.Current().TShape().get());
        }
        for (TopExp_Explorer ex(p.shape, TopAbs_SOLID); ex.More(); ex.Next()) {
            so++;
            seenSolid.insert(ex.Current().TShape().get());
        }
        p.faces = (int)f;
        S.protoFaces += f;
        S.protoSolids += so;
        S.instFaces += f * p.instances;
        S.instSolids += so * p.instances;
        S.nInstances += p.instances;
        bb.Add(all, p.shape);
        protos.push_back(p);
    }
    double tm1 = now_s();
    kv("model_build_s", tm1 - tm0);
    kv("free_shapes", S.nFreeShapes);
    kv("prototypes", protos.size());
    kv("instances", S.nInstances);
    kv("faces_proto", S.protoFaces);
    kv("faces_unique_tshape", seenFace.size());
    kv("faces_instances", S.instFaces);
    kv("solids_proto", S.protoSolids);
    kv("solids_unique_tshape", seenSolid.size());
    kv("solids_instances", S.instSolids);
    kv("model_rss_mb", rssMB());

    if (doMesh) {
        double tb0 = now_s();
        std::vector<double> pd(protos.size());
        for (size_t i = 0; i < protos.size(); ++i)
            pd[i] = diag(protos[i].shape);
        double gd = diag(all); // 注意：原型合并的包围盒（未施加实例变换），作为全局尺度近似
        kv("bbox_time_s", now_s() - tb0);
        kv("global_diag", gd);
        MeshLevel coarse{"coarse", 0.004, 0.0005, 30, false, true};
        MeshLevel planb{"planB", 0.002, 0.0003, 25, false, true};
        MeshLevel fine{"fine", 0.001, 0.0002, 20, true, false};
        meshLevel(protos, pd, gd, coarse);
        meshLevel(protos, pd, gd, fine);
        meshLevel(protos, pd, gd, planb);
    }
    kv("total_time_s", now_s());
    kv("peak_rss_process_mb", maxrssMB());
#ifdef _WIN32
    kv("peak_commit_process_mb", maxCommitMB()); // Windows 提交内存峰值（与 Linux RSS 对比用）
#endif
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    int argcW = 0;
    wchar_t** argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    if (argvW == nullptr || argcW < 3) {
        std::fprintf(stderr, "usage: spike bench|cancel-read|cancel-transfer|cancel-parse <file> "
                             "[fraction|delay_s|--no-mesh]\n");
        if (argvW != nullptr)
            LocalFree(argvW);
        return 1;
    }
    const std::string mode = utf8FromWide(argvW[1]);
    const std::filesystem::path path(argvW[2]);
    std::string third;
    if (argcW > 3)
        third = utf8FromWide(argvW[3]);
    LocalFree(argvW);
    (void)argc;
    (void)argv;
#else
    if (argc < 3) {
        std::fprintf(stderr, "usage: spike bench|cancel-read|cancel-transfer|cancel-parse <file> "
                             "[fraction|delay_s|--no-mesh]\n");
        return 1;
    }
    const std::string mode = argv[1];
    const std::filesystem::path path(argv[2]);
    const std::string third = argc > 3 ? std::string(argv[3]) : std::string();
#endif
    // 关掉 OCCT 默认消息输出，避免日志刷屏影响计时（只保留 Fail 级别）
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashFilter);
    startSampler();
#endif
    kvs("occt_version_at_start", OCC_VERSION_COMPLETE);
    for (auto& p : Message::DefaultMessenger()->Printers())
        p->SetTraceLevel(Message_Fail);
    double frac = mode == "cancel-parse" ? 2.0 : 0.5;
    bool doMesh = true;
    if (!third.empty()) {
        if (third == "--no-mesh")
            doMesh = false;
        else
            frac = std::atof(third.c_str());
    }
    try {
        return run(mode, path, frac, doMesh);
    } catch (const Standard_Failure& e) {
        std::printf("fatal_occt_exception=%s: %s\n", typeid(e).name(), steplens::occt::failureMessage(e));
        return 3;
    } // 7.9.3 中不继承 std::exception
    catch (const std::exception& e) {
        std::printf("fatal_exception=%s\n", e.what());
        return 3;
    }
}
