#include "memory.h"
#include "abi_bridge.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#include "hle/audio/ax_dsp.h"
#include "gx_worker.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <cwchar>
#include <chrono>
#include <thread>

namespace {

static std::string ReadGuestStringSafe(uint32_t addr)
{
    if (addr == 0) {
        return "";
    }
    std::string out;
    out.reserve(64);
    try {
        for (size_t i = 0; i < 512; ++i) {
            uint8_t b = Memory::Read8(addr + static_cast<uint32_t>(i));
            if (b == 0) break;
            out.push_back(static_cast<char>(b));
        }
    } catch (const Memory::AccessViolation&) {
        out = "<invalid_guest_ptr>";
    }
    return out;
}

} // namespace

// 0x8012E5A4 -> PPCHalt
extern "C" void PPCHalt_8012E5A4()
{
    // A halt is never a clean shutdown: the guest reaches it after OSPanic or an
    // unrecoverable OS error. Report it like every other fatal path so the run
    // folder gets the same artifact set, and exit non-zero.
    RT_LOGF(RT_TAG_OS, "PPCHalt called, exiting process\n");
    std::fflush(stderr);
    RuntimeCrash::WriteCrashArtifacts("halt", "The guest executed PPCHalt.");
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system halted the console",
                          "The game executed PPCHalt, which the console only reaches after an "
                          "unrecoverable error.");
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

PPC_NATIVE_OVERRIDE_VOID(8012E5A4, PPCHalt_8012E5A4, (), ());

// 0x801A2660 -> OS::Panic
extern "C" void OS__Panic_801A2660_Cpu(CpuContext* ctx)
{
    const uint32_t file_ptr = ctx ? ctx->gpr[3] : 0;
    const int line = ctx ? static_cast<int>(ctx->gpr[4]) : 0;
    const uint32_t fmt_ptr = ctx ? ctx->gpr[5] : 0;

    const std::string file = ReadGuestStringSafe(file_ptr);
    const std::string fmt = ReadGuestStringSafe(fmt_ptr);

    std::fprintf(stderr, "\n\n");
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "OS::Panic called!\n");
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "Location: %s:%d\n", file.empty() ? "<null>" : file.c_str(), line);
    if (!fmt.empty()) {
        RT_LOGF(RT_TAG_OS, "Message: %s\n", fmt.c_str());
    }
    std::fprintf(stderr, "========================================\n");
    std::fflush(stderr);
    
    RT_LOGF(RT_TAG_OS, "Dumping CPU state...\n");
    std::fflush(stderr);
    
    
    RT_LOGF(RT_TAG_OS, "CPU registers:\n");
    if (ctx) {
        std::fprintf(stderr, "  PC: 0x%08X  LR: 0x%08X  CTR: 0x%08X\n", ctx->pc, ctx->lr, ctx->ctr);
        std::fprintf(stderr, "  r1: 0x%08X  r2: 0x%08X  r13: 0x%08X\n", ctx->gpr[1], ctx->gpr[2], ctx->gpr[13]);
        std::fprintf(stderr, "  r3: 0x%08X  r4: 0x%08X  r5: 0x%08X\n", ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
    } else {
        std::fprintf(stderr, "  (CPU context is NULL)\n");
    }
    
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "Flushing output and exiting...\n");
    std::fflush(stderr);
    std::fflush(stdout);

    std::string details = "The game reported an OS panic";
    if (!file.empty()) {
        details += " at ";
        details += file;
        details += ":";
        details += std::to_string(line);
    }
    if (!fmt.empty()) {
        details += "\n\n";
        details += fmt;
    }
    RuntimeCrash::WriteCrashArtifacts("panic", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a panic", details);
    MarkFatalErrorReported();

    // Give extra time for output to flush with PowerShell redirection
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    std::exit(EXIT_FAILURE);
}

PPC_NATIVE_OVERRIDE_VOID(801A2660, OS__Panic_801A2660_Cpu, (CpuContext* ctx), (ctx));

// 0x801A8A80 -> OSResetSystem
extern "C" uint32_t OSResetSystem()
{
    RT_LOGF(RT_TAG_OS, "OSResetSystem: simulating console reset\n");
    std::fflush(stderr);
    // The AX mix worker holds resolved host pointers into the guest regions;
    // it must be stopped before those mappings are torn down.
    GxWorker::Stop();
    AxDspHle::ShutdownMixWorker();
    Memory::Reset();
    SetRuntimeExitCode(0);
    std::exit(EXIT_SUCCESS);
    return 0; // unreachable, but keeps the signature consistent with callers
}

PPC_NATIVE_OVERRIDE(801A8A80, OSResetSystem, uint32_t, (), ());

// ----------------------------------------------------------------------------
// Leaving the game the other ways (HOME menu "Wii Menu" / "Reset", the power button, the disc-error
// screen). On a console these reboot into the system menu or relaunch the title through
// ESP_LaunchTitle -> IOS_IoctlvReboot, i.e. raw IPC registers, which the HLE IOS does not have:
// IPCWriteReg aborts. Synthetic identities (other games map them in guest_versions.inc).
// ----------------------------------------------------------------------------
namespace {
[[noreturn]] void ExitCleanly(const char* why)
{
    RT_LOGF(RT_TAG_OS, "%s: closing the game\n", why);
    std::fflush(stderr);
    GxWorker::Stop();
    AxDspHle::ShutdownMixWorker();
    SetRuntimeExitCode(0);
    std::exit(EXIT_SUCCESS);
}
} // namespace

// OSShutdownSystem (power button)
extern "C" void OSShutdownSystem_8fff0050(CpuContext* ctx) { (void)ctx; ExitCleanly("OSShutdownSystem"); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0050, OSShutdownSystem_8fff0050, "OSShutdownSystem_8fff0050");

// __OSReturnToMenu (OSReturnToMenu / OSReturnToDataManager / OSReturnToSetting...: HOME menu "Wii Menu")
extern "C" void OSReturnToMenu_8fff0054(CpuContext* ctx) { (void)ctx; ExitCleanly("OSReturnToMenu"); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0054, OSReturnToMenu_8fff0054, "OSReturnToMenu_8fff0054");

// __OSReturnToMenuForError (after the disc/NAND error screens)
extern "C" void OSReturnToMenuForError_8fff0058(CpuContext* ctx) { (void)ctx; ExitCleanly("OSReturnToMenuForError"); }
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0058, OSReturnToMenuForError_8fff0058, "OSReturnToMenuForError_8fff0058");

// OSRestart (HOME menu "Reset"): start a fresh copy of this executable with the same command line.
extern "C" void OSRestart_8fff005c(CpuContext* ctx)
{
    (void)ctx;
#if defined(_WIN32)
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring commandLine = GetCommandLineW();
    if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        RT_LOGF(RT_TAG_OS, "OSRestart: relaunched the game\n");
    } else {
        RT_LOGF(RT_TAG_OS, "OSRestart: relaunch failed (%lu)\n", static_cast<unsigned long>(GetLastError()));
    }
#endif
    ExitCleanly("OSRestart");
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF005C, OSRestart_8fff005c, "OSRestart_8fff005c");

// 0x801AE58C -> exit(int status)
extern "C" uint32_t Exit_801AE58C(int status)
{
    RT_LOGF(RT_TAG_OS, "exit(status=%d) called via 0x801AE58C\n", status);
    std::fflush(stderr);
    if (status != 0) {
        const std::string details =
            "The game called exit(" + std::to_string(status) + ").";
        RuntimeCrash::WriteCrashArtifacts("exit", details);
        ShowRuntimeFatalPopup("the game exited with an error status", details);
        MarkFatalErrorReported();
    }
    GxWorker::Stop();
    SetRuntimeExitCode(status);
    std::exit(status);
    return static_cast<uint32_t>(status);
}

PPC_NATIVE_OVERRIDE(801AE58C, Exit_801AE58C, uint32_t, (int status), (status));
