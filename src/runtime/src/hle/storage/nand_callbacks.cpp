#include "guest_abi/guest_region.h"
#include <cstdlib>
#include <cstdio>
#include "nand_internal.h"
#include "guest_callback_state.h"

// Guest callback dispatch: host NAND work completes synchronously, so an "async" call just
// queues its guest completion callback here and an HLE pump dispatches it later.

struct PendingNandCallback {
    uint32_t callbackPtr = 0;
    int32_t result = 0;
    uint32_t commandBlock = 0;
};

static std::mutex g_pendingNandCallbackMutex;
static std::deque<PendingNandCallback> g_pendingNandCallbacks;

static void DispatchNandCallback(CpuContext* cpu, uint32_t callbackPtr, int32_t result, uint32_t commandBlock) {
    if (callbackPtr == 0) {
        return;
    }

    // The callback signature is: void callback(int result, void* commandBlock)
    // We need to invoke it through the translated function system
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        LogNandError("InvokeNandCallback", "callback 0x%08X not found in registry", callbackPtr);
        return;
    }

    if (cpu) {
        // IOS/NAND completions are asynchronous side calls. Run them on a
        // scratch guest CPU context so callback argument setup and any
        // transient LR/CTR/nonvolatile register changes do not corrupt the
        // interrupted thread that happened to pump the completion queue.
        CpuContext callbackCpu = *cpu;
        callbackCpu.gpr[3] = static_cast<uint32_t>(result);
        callbackCpu.gpr[4] = commandBlock;
        CpuContextScope callbackScope(&callbackCpu);
        InvokeIndirectCpu(callbackPtr, &callbackCpu);
    } else {
        auto& callbackCpu = GetPersistentCpuContext();
        callbackCpu.gpr[3] = static_cast<uint32_t>(result);
        callbackCpu.gpr[4] = commandBlock;
        InvokeIndirectCpu(callbackPtr, &callbackCpu);
    }
}

// RGWE41: RVL_Nand::ResultCallback / ResultChannelCallback, either queued directly (HLE NAND*Async) or at
// the end of an ISFS request: _isfsFuncCb(ret, ctx) -> ctx->cb (+256) with ctx->arg (+260) = the
// NANDCommandBlock, whose callback (+4) is RVL_Nand's.
static bool IsRabbidsRvlNandCompletion(uint32_t callbackPtr, uint32_t arg) {
    auto isRvl = [](uint32_t cb) { return cb == 0x80425F48u || cb == 0x80425F58u; };
    if (isRvl(callbackPtr)) return true;
    if (callbackPtr != 0x803D7C80u) return false; // _isfsFuncCb
    try {
        const uint32_t block = Memory::Read32(arg + 260u);
        return block >= 0x80000000u && block < 0x81800000u && isRvl(Memory::Read32(block + 4u));
    } catch (...) {
        return false;
    }
}

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t commandBlock) {
    if (callbackPtr == 0) {
        return;
    }

    static const bool s_log = std::getenv("MKW_NANDLOG") != nullptr;
    if (s_log) {
        std::fprintf(stderr, "[nandcb] queue cb=0x%08X result=%d arg=0x%08X%c", callbackPtr, result, commandBlock, 10);
    }
    // Rabbids' RVL_Nand spins on a flag its result callbacks set (while (!ms_bNandCallFinished) {}), a loop
    // that reaches no interrupt point. Those callbacks only store the result and the flag, so they run now.
    if (::GuestRegion::IsRabbids() && IsRabbidsRvlNandCompletion(callbackPtr, commandBlock)) {
        if (CpuContext* cpu = TryGetCpuContext()) {
            DispatchNandCallback(cpu, callbackPtr, result, commandBlock);
            return;
        }
    }
    std::lock_guard<std::mutex> lock(g_pendingNandCallbackMutex);
    g_pendingNandCallbacks.push_back({callbackPtr, result, commandBlock});
}

// Guest NAND callbacks can re-enter this drain: RFL's hidden-Mii loader chains one async
// load per Mii as its own completion callback, and a database fattened by online play
// nested hundreds of frames deep and crashed at boot. Hardware is iterative via the IPC
// interrupt, so a nested drain refuses here and the outermost loop dispatches the queue flat.
bool NandProcessPendingCallbacks(CpuContext* cpu, int maxToProcess) {
    if (maxToProcess <= 0) {
        return false;
    }
    if (GuestCallbacks::nandDraining) {
        return false;
    }
    struct DrainScope {
        DrainScope() { GuestCallbacks::nandDraining = true; }
        ~DrainScope() { GuestCallbacks::nandDraining = false; }
    } drainScope;

    // Host-side NAND work finishes immediately. Drain a generous chunk of the guest
    // callback queue in one pump so multi-step chains (open -> read -> close -> user cb)
    // do not get stranded across many retrace ticks and trip the RFL/Mii error path.
    const size_t maxCallbacksThisPump = std::max<size_t>(static_cast<size_t>(maxToProcess), 64);
    size_t processed = 0;
    while (processed < maxCallbacksThisPump) {
        PendingNandCallback cb{};
        {
            std::lock_guard<std::mutex> lock(g_pendingNandCallbackMutex);
            if (g_pendingNandCallbacks.empty()) {
                break;
            }
            cb = g_pendingNandCallbacks.front();
            g_pendingNandCallbacks.pop_front();
        }
        static const bool s_log = std::getenv("MKW_NANDLOG") != nullptr;
        if (s_log) std::fprintf(stderr, "[nandcb] dispatch cb=0x%08X result=%d%c", cb.callbackPtr, cb.result, 10);
        DispatchNandCallback(cpu, cb.callbackPtr, cb.result, cb.commandBlock);
        ++processed;
    }
    return processed != 0;
}
