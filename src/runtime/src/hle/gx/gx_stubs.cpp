#include <atomic>
#include <cstdlib>
#include <vector>
#include <mutex>
#include "guest_abi/guest_region.h"
#include "guest_abi/dispatch.h"
#include "guest_interrupt_context.h"
#include "gx_internal.h"
#include "runtime_log.h"

extern "C" void __GXSetSUTexRegs();

// ============================================================================
// FIFO Write Helpers
// ============================================================================

static void FifoWrite(u32 val, uint32_t sizeBytes) {
    if (GxWorker::ShouldDefer()) {
        EnsureAuroraFrameForFifo();
        GxWorker::FifoWrite(val, sizeBytes);
        return;
    }
    HleFifoWrite(val, sizeBytes);
}

extern "C" void GX_HLE_FIFO_WriteFloat(float val) {
    u32 raw; std::memcpy(&raw, &val, 4);
    try { FifoWrite(raw, 4); } catch (...) { RT_LOGF(RT_TAG_GX, "FIFO write float failed\n"); }
}

extern "C" void GX_HLE_FIFO_Write32(uint32_t val) { FifoWrite(val, 4); }
extern "C" void GX_HLE_FIFO_Write16(uint16_t val) { FifoWrite(static_cast<u32>(val), 2); }
extern "C" void GX_HLE_FIFO_Write8(uint8_t val) { FifoWrite(static_cast<u32>(val), 1); }

extern "C" void GX__SetDrawSync_8016ed08(uint32_t token) {
    (void)token;
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) {
        if (Memory::Read32(gd + 0x5FCu)) GX__SetDirtyState_8016ee78();
        Memory::Write16(gd + 2, 0);
    } } catch (...) {}
}

// On hardware the PE raises a token interrupt when the GPU reaches this token and __GXTokenInterruptHandler
// calls the GXSetDrawSyncCallback callback (TokenCB). MKWii never registers one; PokéPark recycles its two
// frame slots from it (MainGameLoopIteration waits for a free slot), so for it the callback runs here.
static bool DeliverGxBreakPoint(CpuContext* cpu);
static std::mutex g_pokeparkTokenMutex;
static std::vector<uint32_t> g_pokeparkPendingTokens;

static void PokeParkInvokeDrawSyncCallback(uint32_t token) {
    if (::GuestRegion::IsMarioKart()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pokeparkTokenMutex);
    g_pokeparkPendingTokens.push_back(token & 0xFFFFu);
}

// Called from the runtime's interrupt-delivery point (ProcessAlarmQueue), like the PE token interrupt.
extern "C" bool PokePark_ProcessDrawSyncTokens(CpuContext* cpu) {
    std::vector<uint32_t> tokens;
    {
        std::lock_guard<std::mutex> lock(g_pokeparkTokenMutex);
        tokens.swap(g_pokeparkPendingTokens);
    }
    const bool breakPoint = DeliverGxBreakPoint(cpu);
    if (tokens.empty() || !cpu) {
        return breakPoint;
    }
    const uint32_t kTokenCallbackAddr = MKW_GADDR(803867E0); // TokenCB (sbss, GXSetDrawSyncCallback)
    for (const uint32_t token : tokens) {
        const uint32_t callback = Memory::Read32(kTokenCallbackAddr);
        if (callback == 0) {
            continue;
        }
        cpu->gpr[3] = token;
        static const bool trace = std::getenv("MKW_LOADLOG") != nullptr;
        if (trace) RT_LOG(RT_TAG_GX) << "[drawsync] token=0x" << std::hex << token << " cb=0x" << callback << std::dec << " enter" << std::endl;
        InvokeIndirectCpu(callback, cpu);
        if (trace) RT_LOG(RT_TAG_GX) << "[drawsync] token=0x" << std::hex << token << std::dec << " returned" << std::endl;
    }
    return true;
}

// GX breakpoints (Rabbids' K3D frame queue: GXEnableBreakPt after each queued frame, BPCallback recycles
// the buffer, and VIPostCallback counts retraces without it as a GPU hang). The HLE GPU consumes the FIFO
// as it is written, so an enabled breakpoint is reached at once: the CP interrupt is delivered at the
// next interrupt point, which first disables the breakpoint as __GXCPInterruptHandler does.
static std::atomic<uint32_t> g_gxBreakPtCallback{0};
static std::atomic<bool> g_gxBreakPtPending{false};

extern "C" uint32_t GX__SetBreakPtCallback_8016d054(uint32_t callback) {
    return g_gxBreakPtCallback.exchange(callback);
}
PPC_NATIVE_OVERRIDE(8016d054, GX__SetBreakPtCallback_8016d054, uint32_t, (uint32_t callback), (callback));

extern "C" void GX__EnableBreakPt_8016d098(uint32_t breakPt) {
    (void)breakPt;
    g_gxBreakPtPending = true;
}
PPC_NATIVE_OVERRIDE_VOID(8016d098, GX__EnableBreakPt_8016d098, (uint32_t breakPt), (breakPt));

extern "C" void GX__DisableBreakPt_8016d138() {
    g_gxBreakPtPending = false;
}
PPC_NATIVE_OVERRIDE_VOID(8016d138, GX__DisableBreakPt_8016d138, (), ());

// Resets the CP/PI FIFOs on hardware; the HLE GPU has nothing in flight.
extern "C" void GX__AbortFrame_8016e848() {
    g_gxBreakPtPending = false;
}
PPC_NATIVE_OVERRIDE_VOID(8016e848, GX__AbortFrame_8016e848, (), ());

static bool DeliverGxBreakPoint(CpuContext* cpu) {
    // At most one breakpoint per VI retrace: the GPU finishing one frame per field is what paces K3D's
    // frame queue on hardware (an instant breakpoint lets the game outrun the display and overflow it).
    static uint32_t s_lastRetrace = 0;
    if (!cpu || !g_gxBreakPtPending.load()) {
        return false;
    }
    const uint32_t retrace = Memory::Read32(MKW_GADDR(80386BE4)); // retraceCount
    if (retrace == s_lastRetrace) {
        return false;
    }
    s_lastRetrace = retrace;
    g_gxBreakPtPending = false;
    if (const uint32_t callback = g_gxBreakPtCallback.load(); callback != 0) {
        InvokeIndirectCpu(callback, cpu);
    }
    return true;
}

static std::atomic<uint32_t> g_lastDrawSyncToken{0};

extern "C" void GX__SetDrawSync_8016e9fc(uint32_t token) {
    GX__SetDrawSync_8016ed08(token);
    g_lastDrawSyncToken = token & 0xFFFFu;
    PokeParkInvokeDrawSyncCallback(token);
}

// GXReadDrawSync reads the PE token register; the HLE GPU has reached every token as soon as it is set.
// Synthetic identity (no MKWii counterpart): Rabbids' WGL_WaitDrawSync spins on it.
extern "C" uint32_t GXReadDrawSync_8fff003c() {
    return g_lastDrawSyncToken.load();
}
PPC_NATIVE_OVERRIDE(8FFF003C, GXReadDrawSync_8fff003c, uint32_t, (), ());
PPC_NATIVE_OVERRIDE_VOID(8016e9fc, GX__SetDrawSync_8016e9fc, (uint32_t token), (token));

extern "C" void GX__FinishInterruptHandler_8016ed94() {
    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(Memory::Read16(gd + 0x0Au) | 0x0008u));
        Memory::Write8(kGxDrawDoneFlagAddr, 1);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016ed94, GX__FinishInterruptHandler_8016ed94, (), ());

extern "C" void GX__DrawDone_8016eab0() {
    GX_SYNC_INLINE();
    try { Memory::Write8(kGxDrawDoneFlagAddr, 0); } catch (...) {}
    GXDrawDone(); GX__FinishInterruptHandler_8016ed94();
}
PPC_NATIVE_OVERRIDE_VOID(8016eab0, GX__DrawDone_8016eab0, (), ());

extern "C" void GX__PixModeSync_8016eb70() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
    GxWorker::Run([] { GXPixModeSync(); });
}
PPC_NATIVE_OVERRIDE_VOID(8016eb70, GX__PixModeSync_8016eb70, (), ());

// ============================================================================
// Hardware Revision / Thread Query - No-ops
// ============================================================================

extern "C" void __GX__InitRevisionBits_8016b720() {}
PPC_NATIVE_OVERRIDE_VOID(8016b720, __GX__InitRevisionBits_8016b720, (), ());

// ============================================================================
// Texture State Management - Aurora handles internally
// ============================================================================

extern "C" void __GX__SetSUTexRegs_801712f0() {
    GxWorker::Run([] { __GXSetSUTexRegs(); });
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801712f0, __GX__SetSUTexRegs_801712f0, (), ());

extern "C" void __GX__SetTmemConfig_80171458(uint32_t mode) {
    // TMEM layout configuration - Aurora manages internally
    (void)mode;
}
PPC_NATIVE_OVERRIDE_VOID(80171458, __GX__SetTmemConfig_80171458, (uint32_t mode), (mode));

extern "C" void __GX__FlushTextureState_80171c28() {
    // BP texture state flush - Aurora handles via API
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(80171c28, __GX__FlushTextureState_80171c28, (), ());

// ============================================================================
// Copy Configuration - No-ops for features Aurora doesn't use
// ============================================================================

extern "C" void GX__SetDispCopyFrame2Field_8016f5f8(uint32_t f) {
    GxWorker::Run([f] { GXSetDispCopyFrame2Field(f); });
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFCFFFu) | ((f & 3u) << 12));
            Memory::Write32(gd + 0x24Cu, Memory::Read32(gd + 0x24Cu) & 0xFFFFCFFFu);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016f5f8, GX__SetDispCopyFrame2Field_8016f5f8, (uint32_t f), (f));

extern "C" void GX__SetCopyClamp_8016f618(uint32_t c) {
    GxWorker::Run([c] { GXSetCopyClamp(static_cast<GXFBClamp>(c)); });
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            const uint32_t clamp = c & 3u;
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFFFFCu) | clamp);
            Memory::Write32(gd + 0x24Cu, (Memory::Read32(gd + 0x24Cu) & 0xFFFFFFFCu) | clamp);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016f618, GX__SetCopyClamp_8016f618, (uint32_t c), (c));

extern "C" void GX__ClearBoundingBox_8016fecc() {
    GxWorker::Run([] { GXClearBoundingBox(); });
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 2, 0);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016fecc, GX__ClearBoundingBox_8016fecc, (), ());

// ============================================================================
// FIFO/State Management - No-ops
// ============================================================================

extern "C" void GX__SetDirtyState_8016ee78() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write32(gd + 0x5FCu, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016ee78, GX__SetDirtyState_8016ee78, (), ());

extern "C" void GX__ResetWriteGatherPipe_8016e6b0() {
    // WPAR reset - not needed on host
}
PPC_NATIVE_OVERRIDE_VOID(8016e6b0, GX__ResetWriteGatherPipe_8016e6b0, (), ());
