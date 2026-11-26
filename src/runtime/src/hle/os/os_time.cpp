#include "abi_bridge.h"
#include "fiber_manager.h"
#include "guest_abi/guest_region.h"
#include <chrono>
#include <atomic>
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "os_internal.h"

#include <cstdint>
#include <iostream>

namespace {
// Read the 64-bit time base the way the SDK does: retry until the upper word is
// unchanged across the lower-word read, so a rollover in between is never
// observed as a jump.
void ReadTimeBaseRegisters(uint32_t& outHi, uint32_t& outLo)
{
    while (true) {
        const uint32_t hi1 = PPC_Mftbu();
        const uint32_t lo = PPC_Mftb();
        const uint32_t hi2 = PPC_Mftbu();
        if (hi1 == hi2) {
            outHi = hi1;
            outLo = lo;
            return;
        }
    }
}
} // namespace

namespace OsHleInternal {
uint64_t ReadSystemTime()
{
    uint32_t hi1 = 0;
    uint32_t lo = 0;
    ReadTimeBaseRegisters(hi1, lo);

    const uint64_t timeBase = (static_cast<uint64_t>(hi1) << 32) | lo;
    const uint32_t baseHi = ::Memory::Read32(0x800030D8u);
    const uint32_t baseLo = ::Memory::Read32(0x800030DCu);
    const uint64_t base = (static_cast<uint64_t>(baseHi) << 32) | baseLo;
    return base + timeBase;
}
} // namespace OsHleInternal

// Busy-waits on interrupt-driven state ("while (queue full && OSGetTick() - t0 < timeout) {}", Rabbids' K3D
// frame queue) never see an interrupt in translated code. For such games the time reads double as interrupt
// points: due VI retraces and alarms (incl. GX breakpoint/token callbacks) are delivered, at most every 1 ms.
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void SelectThread_801a9c08(CpuContext* ctx);
static void ServiceInterruptsFromTimeRead(CpuContext* ctx)
{
    if (!::GuestRegion::IsRabbids()) {
        return;
    }
    static thread_local bool s_inService = false;
    static std::atomic<int64_t> s_nextPollNs{0};
    if (s_inService || !Fiber::GuestFiberManager::IsInitialized()) {
        return;
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t next = s_nextPollNs.load(std::memory_order_relaxed);
    if (now < next || !s_nextPollNs.compare_exchange_strong(next, now + 1000000)) {
        return;
    }
    s_inService = true;
    try {
        VI_HLE_ProcessRetracesDeferred(8);
        OS_HLE_ProcessAlarmsDeferred(8);
        // Interrupt exit: a woken higher-priority thread (K3D's CleanupThread drains the frame queue
        // after each GX breakpoint) runs now, as __OSReschedule does on hardware.
        if (ctx != nullptr && OS_HLE_InterruptsEnabled() && ::Memory::Read32(MKW_GADDR(8038691C)) != 0) {
            const uint32_t r3 = ctx->gpr[3];
            ctx->gpr[3] = 0;
            SelectThread_801a9c08(ctx);
            ctx->gpr[3] = r3;
        }
    } catch (...) {
        s_inService = false;
        throw;
    }
    s_inService = false;
}

// OSGetTick -> lower 32 bits of the time base. Synthetic identity (MKWii inlines mftb).
extern "C" void OSGetTick_8fff0040(CpuContext* ctx)
{
    uint32_t hi = 0;
    uint32_t lo = 0;
    ReadTimeBaseRegisters(hi, lo);
    ServiceInterruptsFromTimeRead(ctx);
    ctx->gpr[3] = lo;
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0040, OSGetTick_8fff0040, "OSGetTick_8fff0040");

// OSGetTime -> returns 64-bit time base (hi in r3, lo in r4).
extern "C" void OS__GetTime_HLE(CpuContext* ctx)
{
    if (!ctx) {
        return;
    }
    ServiceInterruptsFromTimeRead(ctx);

    uint32_t hi = 0;
    uint32_t lo = 0;
    ReadTimeBaseRegisters(hi, lo);
    ctx->gpr[3] = hi;
    ctx->gpr[4] = lo;
}

PPC_NATIVE_OVERRIDE_VOID(801AAD5C, OS__GetTime_HLE, (CpuContext* ctx), (ctx));


// ----------------------------------------------------------------------------
// OSGetSystemTime HLE - map system time structure to a 64-bit return
extern "C" uint32_t OS____GetSystemTime_801aad7c(uint32_t /*r3_hi*/, uint32_t /*r4_lo*/, uint32_t /*r5*/, int32_t /*r6*/, uint32_t /*r7*/, uint32_t /*r8*/) {
    // Save/disable interrupts (mirrors the original implementation).
    int saved_level = OS__DisableInterrupts_801a65ac();

    uint64_t now = 0;
    try {
        now = ReadSystemTime();
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS____GetSystemTime_801aad7c", e);
        now = (static_cast<uint64_t>(PPC_Mftbu()) << 32) | PPC_Mftb();
    }
    const uint32_t hi = static_cast<uint32_t>(now >> 32);
    const uint32_t lo = static_cast<uint32_t>(now & 0xFFFFFFFFu);
    OS__RestoreInterrupts_801a65d4(saved_level);

    if (CpuContext* ctx = CurrentCpuContext()) {
        ctx->gpr[3] = hi;
        ctx->gpr[4] = lo;
    }
    return hi;
}

PPC_NATIVE_OVERRIDE(801AAD7C, OS____GetSystemTime_801aad7c, uint32_t, (uint32_t r3_low, uint32_t r4_high, uint32_t r5_unused, int32_t r6_unused, uint32_t r7_unused, uint32_t r8_unused), (r3_low, r4_high, r5_unused, r6_unused, r7_unused, r8_unused));
