// Rabbids Go Home (RGWE41): its AI lives in Ai2CppWiiFinal.rso, a 3.3 MB module of compiled C++ the
// game loads at run time (SCR2CPP::b_Init -> DLL_h_LoadModule). Static recompilation needs that code at a
// known address, so the module is always placed at the same reserved MEM1 range and linked there by the
// game's own RSOLinkListFixed; port_rabbids/tools/rso_prelink.py links the same image for the translator.
//
// Layout (GuestRegion::Facts):
//   module  rsoModuleBase  header + sections up to the end of .data (what the game keeps after linking)
//   bss     rsoBssBase
//   tables  rsoScratchBase (MEM2, above the game's arena): the whole file. The header's table offsets are
//           rebased onto it, so relocation/import/export tables stay reachable (exports are looked up
//           later through RSOFindExportSymbolAddr) without spending MEM1 on them.

#include <atomic>
#include <set>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "hle_stubs.h"
#include "memory.h"
#include "wiimote_virtual.h"
#include "runtime_log.h"

namespace {

// LynWiiRetail.elf symbols (RGWE41 only).
constexpr uint32_t kMemManager = 0x8058FE28u;          // MEM_g_Manager
constexpr uint32_t kMemDefaultFlag = 0x806C27F0u;      // MEM_DefaultFlag
constexpr uint32_t kAllocExt = 0x80087104u;            // MEM_Manager_Concrete_WII::p_AllocExt(size, flags)
constexpr uint32_t kFree = 0x800873A4u;                // MEM_Manager_Concrete_WII::Free(ptr)
constexpr uint32_t kOpenReadFile = 0x800836D0u;        // STD_h_IOOpenReadFile(name)
constexpr uint32_t kCloseFile = 0x8008388Cu;           // STD_IOCloseFile(fd)
constexpr uint32_t kReadFile = 0x800839DCu;            // STD_b_IOReadFile(fd, buf, size)
constexpr uint32_t kGetFileSize = 0x8008495Cu;         // STD_i64_IOGetFileSize(fd) -> r3:r4
constexpr uint32_t kRsoLinkListFixed = 0x80373ECCu;    // RSOLinkListFixed(module, bss, level)
constexpr uint32_t kRsoUnLinkList = 0x80373EE4u;       // RSOUnLinkList(module)

constexpr const char* kFixedModuleName = "Ai2CppWiiFinal.rso";

// RSO header fields (RSOObjectHeader).
constexpr uint32_t kHdrNumSections = 0x08, kHdrSectionInfo = 0x0C, kHdrName = 0x10, kHdrBssSize = 0x1C;
constexpr uint32_t kHdrProlog = 0x24, kHdrEpilog = 0x28;
constexpr uint32_t kHdrTableOffsets[] = {0x30, 0x38, 0x40, 0x48, 0x4C, 0x54}; // int/ext rel, exports, names, imports, names

uint32_t CallGuest(CpuContext* ctx, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
    ctx->gpr[3] = a0;
    ctx->gpr[4] = a1;
    ctx->gpr[5] = a2;
    ctx->gpr[6] = a3;
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3];
}

std::string ReadGuestString(uint32_t address, size_t limit = 256) {
    std::string out;
    for (size_t i = 0; i < limit; ++i) {
        const char c = static_cast<char>(Memory::Read8(address + static_cast<uint32_t>(i)));
        if (c == 0) break;
        out.push_back(c);
    }
    return out;
}

bool IsFixedModule(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    return base == kFixedModuleName;
}

void DumpPools(const char* when) {
    const uint32_t h1 = kMemManager + 24u + 5316u;
    const uint32_t end = Memory::Read32(h1 + 12u);
    RT_LOG(RT_TAG_OS) << "[pools] " << when << std::hex << " n=0x" << Memory::Read32(h1 + 5280u) << " free=0x"
                      << Memory::Read32(h1 + 5284u) << " full=0x" << Memory::Read32(h1 + 5288u) << " cls3=0x"
                      << Memory::Read32(h1 + 5292u + 12u);
    for (uint32_t idx = 0; idx < 6; ++idx) {
        const uint32_t chunk = end - (idx + 1u) * 256u;
        std::cerr << " c" << idx << "=" << Memory::Read32(chunk + 240u) << "/" << Memory::Read32(chunk + 248u) << "/"
                  << Memory::Read32(chunk + 252u);
    }
    std::cerr << std::dec << std::endl;
}

void CallOriginal(CpuContext* ctx, uint32_t pal) {
    (*::GuestFunctions::NextTranslation(::GuestRegion::Translate(pal)))(ctx);
}

} // namespace

// DLL_h_LoadModule(const char* name) -> handle {module, bss}
extern "C" void RabbidsLoadModule_8fff0014(CpuContext* ctx) {
    const uint32_t nameAddr = ctx->gpr[3];
    const std::string name = ReadGuestString(nameAddr);
    const auto& facts = ::GuestRegion::CurrentFacts();
    if (!IsFixedModule(name) || facts.rsoModuleBase == 0) {
        RT_LOG(RT_TAG_OS) << "[rso] loading " << name << " the game's way (not pre-translated)" << std::endl;
        CallOriginal(ctx, 0x8FFF0014u);
        return;
    }
    const uint32_t savedLr = ctx->lr;
    const uint32_t X = facts.rsoModuleBase, B = facts.rsoBssBase, S = facts.rsoScratchBase;

    DumpPools("before load");
    const uint32_t handle = CallGuest(ctx, kAllocExt, kMemManager, 8u, Memory::Read32(kMemDefaultFlag));
    DumpPools("after handle");
    const uint32_t fd = CallGuest(ctx, kOpenReadFile, nameAddr);
    if (handle == 0 || fd == 0) {
        RT_LOG(RT_TAG_OS) << "[rso] " << name << ": open failed" << std::endl;
        ctx->gpr[3] = 0;
        ctx->lr = savedLr;
        return;
    }
    CallGuest(ctx, kGetFileSize, fd);
    const uint32_t size = (ctx->gpr[4] + 31u) & ~31u;
    if (size == 0 || size > facts.rsoScratchSize || CallGuest(ctx, kReadFile, fd, S, size) == 0) {
        RT_LOG(RT_TAG_OS) << "[rso] " << name << ": read failed (size 0x" << std::hex << size << std::dec << ")" << std::endl;
        CallGuest(ctx, kCloseFile, fd);
        ctx->gpr[3] = 0;
        ctx->lr = savedLr;
        return;
    }
    CallGuest(ctx, kCloseFile, fd);
    DumpPools("after read");

    // Module part: header and every loaded section, i.e. up to the end of the last non-bss section.
    const uint32_t numSections = Memory::Read32(S + kHdrNumSections);
    const uint32_t sectionInfo = Memory::Read32(S + kHdrSectionInfo);
    uint32_t moduleEnd = 0;
    for (uint32_t i = 0; i < numSections; ++i) {
        const uint32_t off = Memory::Read32(S + sectionInfo + i * 8u) & ~1u;
        const uint32_t len = Memory::Read32(S + sectionInfo + i * 8u + 4u);
        if (off != 0 && off + len > moduleEnd) moduleEnd = off + len;
    }
    const uint32_t bssSize = Memory::Read32(S + kHdrBssSize);
    if (X + moduleEnd > B || B + bssSize > facts.rsoBssBase + 0x2000u) {
        RT_LOG(RT_TAG_OS) << "[rso] " << name << " does not fit the reserved range" << std::endl;
        ctx->gpr[3] = 0;
        ctx->lr = savedLr;
        return;
    }
    for (uint32_t i = 0; i < moduleEnd; i += 4) Memory::Write32(X + i, Memory::Read32(S + i));
    // Tables (and the name, which follows .data) stay in the MEM2 copy: rebase their offsets onto it.
    for (const uint32_t field : kHdrTableOffsets) {
        Memory::Write32(X + field, S + Memory::Read32(S + field) - X);
    }
    Memory::Write32(X + kHdrName, S + Memory::Read32(S + kHdrName) - X);
    for (uint32_t i = 0; i < bssSize; i += 4) Memory::Write32(B + i, 0);

    Memory::Write32(handle + 0u, X);
    Memory::Write32(handle + 4u, bssSize ? B : 0u);
    const uint32_t linked = CallGuest(ctx, kRsoLinkListFixed, X, bssSize ? B : 0u, 2u);
    RT_LOG(RT_TAG_OS) << "[rso] " << name << " linked at 0x" << std::hex << X << "-0x" << (X + moduleEnd)
                      << " bss 0x" << B << " tables 0x" << S << " -> " << linked << std::dec << std::endl;
    for (uint32_t heap = 0; heap < 2; ++heap) {
        const uint32_t h = kMemManager + 24u + heap * 5316u; // MEM_Manager_Concrete_WII::heaps[heap]
        RT_LOG(RT_TAG_OS) << "[rso] heap" << heap << " base=0x" << std::hex << Memory::Read32(h) << " end=0x"
                          << Memory::Read32(h + 4u) << " +8=0x" << Memory::Read32(h + 8u) << " +12=0x"
                          << Memory::Read32(h + 12u) << std::dec << std::endl;
    }
    DumpPools("after link");
    const uint32_t prolog = Memory::Read32(X + kHdrProlog);
    if (prolog != 0) {
        ctx->gpr[3] = X;
        InvokeIndirectCpu(prolog, ctx);
    }
    DumpPools("after prolog");
    ctx->gpr[3] = handle;
    ctx->lr = savedLr;
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0014, RabbidsLoadModule_8fff0014, "RabbidsLoadModule_8fff0014");

// DLL_UnloadModule(handle): the fixed module's memory is not the heap's, so only the handle is freed.
extern "C" void RabbidsUnloadModule_8fff0018(CpuContext* ctx) {
    const uint32_t handle = ctx->gpr[3];
    const auto& facts = ::GuestRegion::CurrentFacts();
    if (facts.rsoModuleBase == 0 || handle == 0 || Memory::Read32(handle) != facts.rsoModuleBase) {
        CallOriginal(ctx, 0x8FFF0018u);
        return;
    }
    const uint32_t savedLr = ctx->lr;
    const uint32_t X = facts.rsoModuleBase;
    const uint32_t epilog = Memory::Read32(X + kHdrEpilog);
    if (epilog != 0) {
        ctx->gpr[3] = X;
        InvokeIndirectCpu(epilog, ctx);
    }
    CallGuest(ctx, kRsoUnLinkList, X);
    CallGuest(ctx, kFree, kMemManager, handle);
    RT_LOG(RT_TAG_OS) << "[rso] fixed module unloaded" << std::endl;
    ctx->lr = savedLr;
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0018, RabbidsUnloadModule_8fff0018, "RabbidsUnloadModule_8fff0018");

// ---------------------------------------------------------------------------------------------
// Bring-up tracing (MKW_MEMTRACE=1): MEM_Manager_Concrete_WII entry points, logging any pointer that
// falls outside both heaps (MEM1 heap0 / MEM2 heap1), with the caller's return address.
// ---------------------------------------------------------------------------------------------
namespace {

bool MemTrace() {
    static const bool on = std::getenv("MKW_MEMTRACE") != nullptr;
    return on;
}

bool InGameHeaps(uint32_t p) {
    for (uint32_t heap = 0; heap < 2; ++heap) {
        const uint32_t h = kMemManager + 24u + heap * 5316u;
        if (p >= Memory::Read32(h) && p < Memory::Read32(h + 12u)) return true;
    }
    return false;
}

// Pool chunk descriptors (last 16 bytes of each 256-byte chunk below heap1's end): report changes
// that happen outside the allocator, i.e. between two traced calls.
struct PoolWatch {
    uint32_t words[8][4]{};
    bool valid = false;
};
PoolWatch g_poolWatch;
std::string g_lastCall;

void SnapshotPools(PoolWatch& w) {
    const uint32_t h1 = kMemManager + 24u + 5316u;
    const uint32_t end = Memory::Read32(h1 + 12u);
    for (uint32_t idx = 0; idx < 8; ++idx)
        for (uint32_t k = 0; k < 4; ++k) w.words[idx][k] = Memory::Read32(end - (idx + 1u) * 256u + 240u + k * 4u);
    w.valid = true;
}

void CheckPools(const char* phase, const char* name, uint32_t lr) {
    PoolWatch now;
    SnapshotPools(now);
    if (g_poolWatch.valid) {
        for (uint32_t idx = 0; idx < 8; ++idx)
            for (uint32_t k = 0; k < 4; ++k)
                if (now.words[idx][k] != g_poolWatch.words[idx][k]) {
                    RT_LOG(RT_TAG_OS) << "[poolwatch] chunk" << idx << "+" << (240 + k * 4) << " 0x" << std::hex
                                      << g_poolWatch.words[idx][k] << " -> 0x" << now.words[idx][k] << " between ("
                                      << g_lastCall << ") and " << phase << " " << name << " lr=0x" << lr << std::dec
                                      << std::endl;
                }
    }
    g_poolWatch = now;
}

// Walks heap1's pool lists; returns an empty string when they are consistent.
std::string CheckPoolLists() {
    const uint32_t h1 = kMemManager + 24u + 5316u;
    const uint32_t end = Memory::Read32(h1 + 12u);
    const uint32_t n = Memory::Read32(h1 + 5280u);
    auto chunk = [&](uint32_t idx) { return end - (idx + 1u) * 256u; };
    char buf[160];
    for (uint32_t list = 0; list < 7; ++list) { // 0..5 classes, 6 = full
        uint32_t idx = Memory::Read32(h1 + (list < 6 ? 5292u + list * 4u : 5288u));
        uint32_t prev = 0x1FFFFFFFu;
        for (int steps = 0; idx != 0x1FFFFFFFu; ++steps) {
            if (idx >= n || steps > 64) {
                std::snprintf(buf, sizeof(buf), "list %u reaches index 0x%x (n=%u)", list, idx, n);
                return buf;
            }
            const uint32_t c = chunk(idx);
            const uint32_t w248 = Memory::Read32(c + 248u), w252 = Memory::Read32(c + 252u);
            if ((w248 >> 3) != prev) {
                std::snprintf(buf, sizeof(buf), "list %u chunk %u prev=0x%x expected 0x%x", list, idx, w248 >> 3, prev);
                return buf;
            }
            const bool full = Memory::Read32(c + 240u) == 0 && Memory::Read32(c + 244u) == 0;
            if (list < 6 && ((w252 & 7u) != list || full)) {
                std::snprintf(buf, sizeof(buf), "class list %u has chunk %u (class %u, full=%d)", list, idx, w252 & 7u, full);
                return buf;
            }
            if (list == 6 && !full) {
                std::snprintf(buf, sizeof(buf), "full list has non-full chunk %u", idx);
                return buf;
            }
            prev = idx;
            idx = w252 >> 3;
        }
    }
    return {};
}

void TraceMem(CpuContext* ctx, uint32_t pal, const char* name, bool checkArg) {
    if (!MemTrace()) {
        CallOriginal(ctx, pal);
        return;
    }
    CheckPools("before", name, ctx->lr);
    {
        // The free block just below heap1's tmp area: its header must never read 0.
        static uint32_t s_link = 0, s_hdr = 0;
        const uint32_t h1 = kMemManager + 24u + 5316u;
        const uint32_t t4 = Memory::Read32(h1 + 4u);
        const uint32_t link = (Memory::Read32(t4 - 4u) & 0x40000000u) ? Memory::Read32(t4 - 8u) : 0u;
        const uint32_t hdr = link ? Memory::Read32(link) : 0u;
        if ((link != s_link && link != 0 && (hdr & 0x01FFFFFFu) == 0) || (link == s_link && link != 0 && hdr != s_hdr && (hdr & 0x01FFFFFFu) == 0)) {
            RT_LOG(RT_TAG_OS) << "[hdrwatch] free block 0x" << std::hex << s_link << "->0x" << link << " header 0x" << s_hdr << " -> 0x" << hdr
                              << " between (" << g_lastCall << ") and " << name << " lr=0x" << ctx->lr << std::dec << std::endl;
        }
        s_link = link;
        s_hdr = hdr;
    }
    static std::atomic<int> s_depth{0};
    static std::atomic<uint32_t> s_outerThread{0};
    static const char* s_outerName = "";
    const uint32_t thread = Memory::Read32(0x800000E4u); // __OSCurrentThread
    if (s_depth.load() > 0) {
        static int s_reentries = 0;
        if (s_reentries++ < 10) {
            RT_LOG(RT_TAG_OS) << "[memtrace] REENTRY " << name << " lr=0x" << std::hex << ctx->lr << " thread=0x" << thread
                              << " while " << s_outerName << " thread=0x" << s_outerThread.load() << std::dec << std::endl;
        }
    } else {
        s_outerName = name;
        s_outerThread = thread;
    }
    struct DepthGuard { std::atomic<int>& d; DepthGuard(std::atomic<int>& x) : d(x) { ++d; } ~DepthGuard() { --d; } } guard(s_depth);
    const uint32_t lr = ctx->lr, a4 = ctx->gpr[4], a5 = ctx->gpr[5];
    {
        const uint32_t h1 = kMemManager + 24u + 5316u;
        const std::string nm(name);
        const uint32_t size = nm == "p_Realloc" ? a5 : a4;
        const bool alloc = nm == "p_AllocExt" || (nm == "p_Realloc" && a4 == 0);
        const uint32_t cls = ((size + 3u) & ~3u) >= 4u ? ((((size + 3u) & ~3u) - 4u) >> 2) : 0u;
        static int s_newChunkDumps = 0;
        if (alloc && size != 0 && size <= 0x18 && Memory::Read32(h1 + 5292u + cls * 4u) == 0x1FFFFFFFu &&
            Memory::Read32(h1 + 5280u) >= 4u && s_newChunkDumps++ < 6) {
            const uint32_t t4 = Memory::Read32(h1 + 4u), t8 = Memory::Read32(h1 + 8u);
            const uint32_t foot = Memory::Read32(t4 - 4u), link = Memory::Read32(t4 - 8u);
            RT_LOG(RT_TAG_OS) << "[poolnew] " << name << " size 0x" << std::hex << size << " cls" << cls << " lr=0x" << lr
                              << " tmp4=0x" << t4 << " tmp8=0x" << t8 << " foot=0x" << foot << " [t4-8]=0x" << link
                              << " hdr(link)=0x" << (link >= 0x90000000u && link < 0x94000000u ? Memory::Read32(link) : 0u)
                              << std::dec << std::endl;
        }
    }
    const bool badArg = checkArg && a4 != 0 && !InGameHeaps(a4);
    CallOriginal(ctx, pal);
    const uint32_t r = ctx->gpr[3];
    {
        const uint32_t h1e = Memory::Read32(kMemManager + 24u + 5316u + 12u);
        static int s_descDumps = 0;
        if (r < h1e && r >= h1e - 8u * 256u && ((h1e - r) % 256u) == 16u && s_descDumps++ < 3) {
            RT_LOG(RT_TAG_OS) << "[poolwatch] " << name << "(0x" << std::hex << a4 << ", 0x" << a5
                              << ") returned a chunk descriptor 0x" << r << " lr=0x" << lr << "; state before:" << std::dec << std::endl;
            const uint32_t h1 = kMemManager + 24u + 5316u;
            std::cerr << std::hex << "  heads:";
            for (uint32_t c = 0; c < 8; ++c) std::cerr << " cls" << c << "=" << Memory::Read32(h1 + 5292u + c * 4u);
            std::cerr << " free=" << Memory::Read32(h1 + 5284u) << " full=" << Memory::Read32(h1 + 5288u) << std::endl;
            for (uint32_t idx = 0; idx < 8; ++idx)
                std::cerr << "  chunk" << idx << " bm=" << g_poolWatch.words[idx][0] << " bm2=" << g_poolWatch.words[idx][1]
                          << " prev=" << g_poolWatch.words[idx][2] << " next=" << g_poolWatch.words[idx][3] << std::endl;
            std::cerr << std::dec;
        }
    }
    {
        static bool s_reported = false;
        if (!s_reported) {
            const std::string bad = CheckPoolLists();
            if (!bad.empty()) {
                s_reported = true;
                RT_LOG(RT_TAG_OS) << "[poolcheck] INCONSISTENT after " << name << "(0x" << std::hex << a4 << ", 0x" << a5
                                  << ") -> 0x" << r << " lr=0x" << lr << std::dec << ": " << bad << std::endl;
                RT_LOG(RT_TAG_OS) << "[poolcheck] previous call: " << g_lastCall << std::endl;
                const uint32_t h1 = kMemManager + 24u + 5316u;
                std::cerr << std::hex << "  before: n=" << "?";
                for (uint32_t idx = 0; idx < 6; ++idx)
                    std::cerr << " c" << idx << "=" << g_poolWatch.words[idx][0] << "/" << g_poolWatch.words[idx][1] << "/"
                              << g_poolWatch.words[idx][2] << "/" << g_poolWatch.words[idx][3];
                std::cerr << std::endl << "  after: heads";
                for (uint32_t c = 0; c < 6; ++c) std::cerr << " " << Memory::Read32(h1 + 5292u + c * 4u);
                std::cerr << " full=" << Memory::Read32(h1 + 5288u) << " free=" << Memory::Read32(h1 + 5284u) << std::endl;
                PoolWatch now;
                SnapshotPools(now);
                for (uint32_t idx = 0; idx < 6; ++idx)
                    std::cerr << "  c" << idx << "=" << now.words[idx][0] << "/" << now.words[idx][1] << "/"
                              << now.words[idx][2] << "/" << now.words[idx][3] << std::endl;
                std::cerr << std::dec;
            }
        }
    }
    SnapshotPools(g_poolWatch); // the allocator itself may change them
    {
        // Live pool slots of heap1 (its top 64 KiB): catch frees of slots that are not allocated.
        static std::set<uint32_t> s_live;
        const uint32_t h1e = Memory::Read32(kMemManager + 24u + 5316u + 12u);
        auto inPools = [&](uint32_t p) { return p != 0 && p < h1e && p >= h1e - 0x10000u; };
        const std::string n(name);
        const bool frees = n == "Free" || n == "FreeTmp" || n.rfind("p_Realloc", 0) == 0;
        if (frees && inPools(a4)) {
            if (s_live.erase(a4) == 0) {
                static int s_bad = 0;
                if (s_bad++ < 12)
                    RT_LOG(RT_TAG_OS) << "[memtrace] FREE-UNKNOWN " << name << "(0x" << std::hex << a4 << ") lr=0x" << lr
                                      << std::dec << std::endl;
            }
        }
        if (n.rfind("Free", 0) != 0 && inPools(r)) {
            if (!s_live.insert(r).second) {
                static int s_dup = 0;
                if (s_dup++ < 12)
                    RT_LOG(RT_TAG_OS) << "[memtrace] DOUBLE-ALLOC " << name << " -> 0x" << std::hex << r << " lr=0x" << lr
                                      << std::dec << std::endl;
            }
        }
    }
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s(0x%x) -> 0x%x lr=0x%x", name, a4, r, lr);
        g_lastCall = buf;
    }
    bool badRet = r != 0 && !InGameHeaps(r);
    const uint32_t h1 = kMemManager + 24u + 5316u;
    static uint32_t s_lastDefaultHeap = 1;
    const uint32_t defaultHeap = Memory::Read32(0x806C0DE8u); // MEM_DefaultHeap
    if (defaultHeap != s_lastDefaultHeap) {
        RT_LOG(RT_TAG_OS) << "[memtrace] MEM_DefaultHeap 0x" << std::hex << s_lastDefaultHeap << " -> 0x" << defaultHeap
                          << " (seen in " << name << ", lr=0x" << lr << ")" << std::dec << std::endl;
        s_lastDefaultHeap = defaultHeap;
    }
    if (std::string(name).rfind("Free", 0) == 0) badRet = false; // void
    const bool nearTop = (a4 >= 0x97600000u && a4 < 0x98000000u) || (r >= 0x97600000u && r < 0x98000000u);
    if (nearTop && !badArg && !badRet) {
        RT_LOG(RT_TAG_OS) << "[memtrace] top " << name << "(0x" << std::hex << a4 << ", 0x" << a5 << ") -> 0x" << r
                          << " lr=0x" << lr << " tmp=0x" << Memory::Read32(h1 + 4u) << std::dec << std::endl;
    }
    if (badArg || badRet) {
        RT_LOG(RT_TAG_OS) << "[memtrace] " << name << "(0x" << std::hex << a4 << ", 0x" << a5 << ") -> 0x" << r
                          << " lr=0x" << lr << " heap1 tmp=0x" << Memory::Read32(h1 + 4u) << "/0x"
                          << Memory::Read32(h1 + 8u) << " end=0x" << Memory::Read32(h1 + 12u) << std::dec
                          << (badArg ? " BAD-ARG" : "") << (badRet ? " BAD-RESULT" : "") << std::endl;
        static int s_dumps = 0;
        if (badRet && s_dumps++ < 2) {
            const uint32_t end = Memory::Read32(h1 + 12u);
            RT_LOG(RT_TAG_OS) << "[memtrace] heap1 5280=0x" << std::hex << Memory::Read32(h1 + 5280u) << " 5284=0x"
                              << Memory::Read32(h1 + 5284u) << " 5288=0x" << Memory::Read32(h1 + 5288u);
            for (uint32_t c = 0; c < 7; ++c) std::cerr << " cls" << c << "=0x" << Memory::Read32(h1 + 5292u + c * 4u);
            std::cerr << std::dec << std::endl;
            for (uint32_t idx = 0; idx < 8; ++idx) {
                const uint32_t chunk = end - (idx + 1u) * 256u;
                RT_LOG(RT_TAG_OS) << "[memtrace]   chunk" << idx << " @0x" << std::hex << chunk << " bm=0x"
                                  << Memory::Read32(chunk + 240u) << " bm2=0x" << Memory::Read32(chunk + 244u)
                                  << " 248=0x" << Memory::Read32(chunk + 248u) << " 252=0x" << Memory::Read32(chunk + 252u)
                                  << std::dec << std::endl;
            }
        }
    }
}

} // namespace

extern "C" void RabbidsAllocExt_8fff001c(CpuContext* ctx) { TraceMem(ctx, 0x8FFF001Cu, "p_AllocExt", false); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF001C, RabbidsAllocExt_8fff001c, "RabbidsAllocExt_8fff001c");
extern "C" void RabbidsAllocAlignExt_8fff0020(CpuContext* ctx) { TraceMem(ctx, 0x8FFF0020u, "p_AllocAlignExt", false); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0020, RabbidsAllocAlignExt_8fff0020, "RabbidsAllocAlignExt_8fff0020");
extern "C" void RabbidsAllocTmp_8fff0024(CpuContext* ctx) { TraceMem(ctx, 0x8FFF0024u, "p_AllocTmp", false); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0024, RabbidsAllocTmp_8fff0024, "RabbidsAllocTmp_8fff0024");
extern "C" void RabbidsRealloc_8fff0028(CpuContext* ctx) { TraceMem(ctx, 0x8FFF0028u, "p_Realloc", true); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0028, RabbidsRealloc_8fff0028, "RabbidsRealloc_8fff0028");
extern "C" void RabbidsFreeTmp_8fff002c(CpuContext* ctx) { TraceMem(ctx, 0x8FFF002Cu, "FreeTmp", true); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF002C, RabbidsFreeTmp_8fff002c, "RabbidsFreeTmp_8fff002c");
extern "C" void RabbidsReallocAlign_8fff0030(CpuContext* ctx) { TraceMem(ctx, 0x8FFF0030u, "p_ReallocAlign", true); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0030, RabbidsReallocAlign_8fff0030, "RabbidsReallocAlign_8fff0030");
extern "C" void RabbidsFree_8fff0034(CpuContext* ctx) { TraceMem(ctx, 0x8FFF0034u, "Free", true); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0034, RabbidsFree_8fff0034, "RabbidsFree_8fff0034");

// ---------------------------------------------------------------------------------------------
// RVL_SplashScreenWaitCallback(): the loader spins on it (ViD::b_LoadGeneric: while (cb()) {}) until the
// health/strap screen thread finishes. On hardware the VI interrupt wakes that higher-priority thread
// (VIPostCallback -> OSWakeupThread) and it preempts the spinning one; translated code has no preemption
// point, so deliver the due retrace here (waking it, which switches threads) before each poll.
// ---------------------------------------------------------------------------------------------
extern "C" void SelectThread_801a9c08(CpuContext* ctx); // os_scheduler.cpp

extern "C" void RabbidsSplashWait_8fff0038(CpuContext* ctx) {
    VI_HLE_WaitForNextRetracePoll();
    VI_HLE_PollRetrace(ctx);
    // Wakeups inside retrace callbacks do not reschedule; do what __OSReschedule does on interrupt exit.
    if (Memory::Read32(0x806C2ECCu) != 0) { // RunQueueHint
        const uint32_t r3 = ctx->gpr[3];
        ctx->gpr[3] = 0;
        SelectThread_801a9c08(ctx);
        ctx->gpr[3] = r3;
    }
    CallOriginal(ctx, 0x8FFF0038u);
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0038, RabbidsSplashWait_8fff0038, "RabbidsSplashWait_8fff0038");

// ---------------------------------------------------------------------------------------------
// Nunchuk encryption tables: WPADiCreateKey stores them in the channel's control block
// (__rvl_p_wpadcb[0] + 2340 = ft, + 2348 = sb), WPADiDecode applies (e ^ sb) + ft.
// ---------------------------------------------------------------------------------------------
namespace {
bool RabbidsExtCipher(uint8_t ft[8], uint8_t sb[8]) {
    if (!::GuestRegion::IsRabbids()) return false;
    try {
        const uint32_t cb = Memory::Read32(0x805B2550u); // __rvl_p_wpadcb[0]
        if (cb < 0x80000000u || cb >= 0x81800000u) return false;
        for (uint32_t i = 0; i < 8; ++i) {
            ft[i] = Memory::Read8(cb + 2340u + i);
            sb[i] = Memory::Read8(cb + 2348u + i);
        }
        return true;
    } catch (...) {
        return false;
    }
}
const bool g_registerExtCipher = [] {
    Wiimote::Virtual::SetExtCipherProvider(&RabbidsExtCipher);
    return true;
}();
} // namespace
