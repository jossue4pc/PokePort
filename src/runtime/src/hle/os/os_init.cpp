// Boot/init hooks, OSFatal, and assorted hardware-init stubs.

#include "platform/host_platform.h"
#include <iterator>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"
#include "system_bridge.h"
#include "guest_function_imports.h"

extern "C" void OSInitAlarm_RecompModLateInit_801a961c(CpuContext* ctx) {
    MKW_GUEST_FUNC(801A961C)(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x801A961C, OSInitAlarm_RecompModLateInit_801a961c, "OSInitAlarm_RecompModLateInit_801a961c");

extern "C" void StaticRProlog_RecompModInit_8055531c(CpuContext* ctx) {
    RecompMod::RunMemoryInitializers();
    MKW_GUEST_FUNC(8055531C)(ctx);
    RecompMod::RunPostRelInitializers();
}

REGISTER_NATIVE_FUNCTION_AS(0x8055531C, StaticRProlog_RecompModInit_8055531c, "StaticRProlog_RecompModInit_8055531c");

namespace {
std::string ReadGuestCStringLimited(uint32_t address, size_t limit = 4096) {
    if (address == 0) {
        return "<null>";
    }

    std::string text;
    text.reserve(128);
    for (size_t i = 0; i < limit; ++i) {
        const uint8_t ch = Memory::Read8(address + static_cast<uint32_t>(i));
        if (ch == 0) {
            return text;
        }
        text.push_back(static_cast<char>(ch));
    }
    text += "<unterminated>";
    return text;
}
}

extern "C" void OSFatal_HLE_801a4ec4(CpuContext* ctx) {
    const uint32_t fg = ctx ? ctx->gpr[3] : 0;
    const uint32_t bg = ctx ? ctx->gpr[4] : 0;
    const uint32_t messagePtr = ctx ? ctx->gpr[5] : 0;
    RT_LOG(RT_TAG_OS) << "OS::Fatal called fg=0x" << std::hex << fg
              << " bg=0x" << bg
              << " message=0x" << messagePtr
              << std::dec << " '" << ReadGuestCStringLimited(messagePtr) << "'" << std::endl;
    if (ctx) {
        SystemBridge::DumpCpuState(ctx);
    }
    const std::string guestMessage = ReadGuestCStringLimited(messagePtr);
    const std::string details =
        guestMessage.empty() ? std::string("OS::Fatal was called without a message.") : guestMessage;
    // MarkFatalErrorReported below suppresses the atexit reporter, so this path
    // has to write its own artifacts or the run folder gets nothing.
    RuntimeCrash::WriteCrashArtifacts("osfatal", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a fatal error", details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

REGISTER_NATIVE_FUNCTION_AS(0x801A4EC4, OSFatal_HLE_801a4ec4, "OSFatal_HLE_801a4ec4");

extern "C" void GKI_delay_HLE_801301b4()
{
    // The guest's GKI_delay is empty: BTA_Init spins on it while the Bluetooth
    // module answers through IPC interrupts and the 2 ms BTU alarm. Service both
    // here and let the wall clock advance.
    OS_HLE_ProcessAlarms(32);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

PPC_NATIVE_OVERRIDE_VOID(801301B4, GKI_delay_HLE_801301b4, (), ());

// Serial Interface (SI) - GameCube controller ports; stubbed since we don't emulate the MMIO.

// SIInit (0x801b2de0): skips MMIO setup at 0xCD006434 and controller detection.
extern "C" void SIInit_801b2de0()
{
    RT_LOG(RT_TAG_OS) << "SIInit_801b2de0 called: skipping MMIO register setup and controller detection" << std::endl;
}

// SISetSamplingRate (0x801b3acc): ignored, we don't emulate SI polling timing.
extern "C" void HLE_SISetSamplingRate_801b3acc(uint32_t msec)
{
    RT_LOG(RT_TAG_OS) << "HLE_SISetSamplingRate_801b3acc called: msec=" << msec << ": Stubbed success." << std::endl;
}

// Video Interface (VI) - TV output.

// VIGetTvFormat (0x801bacd8): CRITICAL, must report the game's own TV format or it
// misbehaves/panics.
extern "C" uint32_t HLE_VIGetTvFormat_801bacd8()
{
    // VI_NTSC = 0, VI_PAL = 1, VI_MPAL = 2
    RT_LOG(RT_TAG_OS) << "HLE_VIGetTvFormat_801bacd8 called: returning " << ::GuestRegion::CurrentFacts().viTvFormat
                      << std::endl;
    return ::GuestRegion::CurrentFacts().viTvFormat;
}

REGISTER_NATIVE_FUNCTION(0x801B2DE0, SIInit_801b2de0);
PPC_NATIVE_OVERRIDE_VOID(801B3ACC, HLE_SISetSamplingRate_801b3acc, (uint32_t msec), (msec));

// OS____InitMemoryProtection (0x801A7DFC): real version touches MMU/MMIO we don't emulate;
// no-op and return success so boot doesn't stall.
extern "C" uint32_t OS____InitMemoryProtection_801a7dfc(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8)
{
    RT_LOG(RT_TAG_OS) << "OS____InitMemoryProtection_801a7dfc called (stubbed): r3=0x" << std::hex << r3 << std::dec << std::endl;

    return 0;
}

PPC_NATIVE_OVERRIDE(801A7DFC, OS____InitMemoryProtection_801a7dfc, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8), (r3, r4, r5, r6, r7, r8));

// OSGetConsoleType (0x8019f33c): standard Wii = 0x12, NDEV (expanded MEM2) = 0x10000012;
// MKWii uses the NDEV result to enable its extra-memory heap path.
extern "C" uint32_t OS__GetConsoleType_8019f33c(uint32_t /*r4*/, uint32_t /*r5*/, uint32_t /*r6*/,
                                                uint32_t /*r7*/, uint32_t /*r8*/, uint32_t /*r31*/)
{
    constexpr uint32_t kRetailMem2Size = 64u * 1024u * 1024u;
    const uint32_t physicalMem2Size = Memory::Read32(0x80003118u);
    const uint32_t consoleType =
        physicalMem2Size == kRetailMem2Size ? 0x00000012u : 0x10000012u;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        RT_LOG(RT_TAG_OS) << "OSGetConsoleType: MEM2="
                  << (physicalMem2Size / (1024u * 1024u))
                  << " MB, returning 0x" << std::hex << consoleType << std::dec << std::endl;
    }
    return consoleType;
}

// Register the function
PPC_NATIVE_OVERRIDE(8019F33C, OS__GetConsoleType_8019f33c, uint32_t, (uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r31), (r4, r5, r6, r7, r8, r31));

// OSGetResetCode (0x801a8a50): real version reads MMIO 0xCC003024; we always report
// Cold Boot (0).
extern "C" uint32_t OSGetResetCode_801a8a50()
{
    // Log occasionally just to track boot flow
    static bool logged = false;
    if (!logged) {
        RT_LOG(RT_TAG_OS) << "OSGetResetCode_801a8a50 called: returning 0 (Cold Boot)" << std::endl;
        logged = true;
    }
    return 0; 
}

// Register the function
PPC_NATIVE_OVERRIDE(801A8A50, OSGetResetCode_801a8a50, uint32_t, (), ());

// __OSInitSTM (0x801AB848): real version opens /dev/stm/* handles. We stub it by writing
// fake handles and the success flag into the SDA (r13) block so OSResetSystem's checks pass.
extern "C" uint32_t __OSInitSTM_HLE_801ab848(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) return 0;

    RT_LOG(RT_TAG_OS) << "__OSInitSTM_HLE_801ab848 called: initializing STM state" << std::endl;

    // STM_Initialized, /dev/stm/immediate and /dev/stm/eventhook.
    const uint32_t kStmInitializedAddr = MKW_GADDR(80386934);
    const uint32_t kStmImmediateHandleAddr = MKW_GADDR(80386938);
    const uint32_t kStmEventHookHandleAddr = MKW_GADDR(8038693C);

    try {
        // Mark STM as initialized
        ::Memory::Write32(kStmInitializedAddr, 1);

        // Fake non-zero handles so callers' zero-checks pass.
        ::Memory::Write32(kStmImmediateHandleAddr, 0x00535401); // "ST\x01"
        ::Memory::Write32(kStmEventHookHandleAddr, 0x00535402); // "ST\x02"

        // Default Power/Reset callback pointers are left unset; safe since we never fire
        // the STM hardware interrupt that would invoke them.
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "__OSInitSTM: Failed to write STM state to SDA @ 0x"
                  << std::hex << e.address() << std::dec << " (" << e.reason() << ")" << std::endl;
        return 0; // Return failure
    }

    // Return 1 (success)
    return 1;
}

// Register the function
PPC_NATIVE_OVERRIDE(801AB848, __OSInitSTM_HLE_801ab848, uint32_t, (CpuContext* ctx), (ctx));


extern "C" void OS____PSInit_801a04a0()
{
    RT_LOG(RT_TAG_OS) << "OS____PSInit_801a04a0 called (stubbed)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(801A04A0, OS____PSInit_801a04a0, (), ());



extern "C" void __init_hardware_80006348()
{
    RT_LOG(RT_TAG_OS) << "__init_hardware_80006348 called (stubbed)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(80006348,__init_hardware_80006348, (), ());


// PPC SPR (Special Purpose Register) access stubs; these registers don't exist on x86 so
// each one just logs and no-ops.

// Each PPC_NATIVE_OVERRIDE_VOID line stays written out per stub (not looped) because the
// translator text-scans them to decide which addresses to skip translating.
#define PPC_SPR_STUB_BODY(name, message) \
    extern "C" void name() { RT_LOG(RT_TAG_OS) << message << std::endl; }

PPC_SPR_STUB_BODY(PPCMfhid0_8012e574, "PPCMfhid0 called (stubbed) - Move From HID0")
PPC_NATIVE_OVERRIDE_VOID(8012e574, PPCMfhid0_8012e574, (), ());

PPC_SPR_STUB_BODY(PPCMthid0_8012e57c, "PPCMthid0 called (stubbed) - Move To HID0")
PPC_NATIVE_OVERRIDE_VOID(8012e57c, PPCMthid0_8012e57c, (), ());

extern "C" void PPCMtdec_8012e594()
{
    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMtdec called (stubbed) - Move To Decrementer" << std::endl;
    }
    // We don't simulate decrementer exceptions, so pump due alarms here instead. Capped at
    // 32, not 1: a single alarm let unrelated periodic alarms backlog and delay
    // AsyncDisplay's pacing alarm by multiple retraces.
    OS_HLE_ProcessAlarms(32);
}
PPC_NATIVE_OVERRIDE_VOID(8012E594, PPCMtdec_8012e594, (), ());

extern "C" void PPCSync_8012e59c()
{
    // PowerPC `sync`. Translated guest code runs on one host thread at a time
    // and the runtime's own cross-thread state uses C++ atomics, so there is no
    // guest-visible reordering for this barrier to prevent.
}
PPC_NATIVE_OVERRIDE_VOID(8012e59c, PPCSync_8012e59c, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr0_8012e5b8, "PPCMtmmcr0 called (stubbed) - Move To MMCR0")
PPC_NATIVE_OVERRIDE_VOID(8012e5b8, PPCMtmmcr0_8012e5b8, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr1_8012e5c0, "PPCMtmmcr1 called (stubbed) - Move To MMCR1")
PPC_NATIVE_OVERRIDE_VOID(8012e5c0, PPCMtmmcr1_8012e5c0, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc1_8012e5c8, "PPCMtpmc1 called (stubbed) - Move To PMC1")
PPC_NATIVE_OVERRIDE_VOID(8012e5c8, PPCMtpmc1_8012e5c8, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc2_8012e5d0, "PPCMtpmc2 called (stubbed) - Move To PMC2")
PPC_NATIVE_OVERRIDE_VOID(8012e5d0, PPCMtpmc2_8012e5d0, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc3_8012e5d8, "PPCMtpmc3 called (stubbed) - Move To PMC3")
PPC_NATIVE_OVERRIDE_VOID(8012e5d8, PPCMtpmc3_8012e5d8, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc4_8012e5e0, "PPCMtpmc4 called (stubbed) - Move To PMC4")
PPC_NATIVE_OVERRIDE_VOID(8012e5e0, PPCMtpmc4_8012e5e0, (), ());

extern "C" uint32_t PPCMfhid2_8012e630_impl()
{
    if (CpuContext* cpu = TryGetCpuContext()) {
        if (cpu->hid2 == 0) {
            cpu->hid2 = 0x10000000u;
        }
        return cpu->hid2;
    }
    return 0x10000000u;
}
// Register the function using a return-value stub
extern "C" void PPCMfhid2_HLE_8012e630(CpuContext* ctx)
{
    ctx->gpr[3] = PPCMfhid2_8012e630_impl();
}
REGISTER_TRANSLATED_FUNCTION(0x8012e630, PPCMfhid2_HLE_8012e630);

extern "C" void PPCMthid2_8012e638(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    cpu->hid2 = cpu->gpr[3];

    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMthid2 set HID2=0x" << std::hex << cpu->hid2 << std::dec << std::endl;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8012e638, PPCMthid2_8012e638, (CpuContext* ctx), (ctx));

PPC_SPR_STUB_BODY(PPCMfwpar_8012e640,
                  "PPCMfwpar called (stubbed) - Move From Write Pipe Address Register")
PPC_NATIVE_OVERRIDE_VOID(8012e640, PPCMfwpar_8012e640, (), ());

PPC_SPR_STUB_BODY(PPCMtwpar_8012e64c,
                  "PPCMtwpar called (stubbed) - Move To Write Pipe Address Register")
PPC_NATIVE_OVERRIDE_VOID(8012e64c, PPCMtwpar_8012e64c, (), ());

PPC_SPR_STUB_BODY(PPCDisableSpeculation_8012e654, "PPCDisableSpeculation called (stubbed)")
PPC_NATIVE_OVERRIDE_VOID(8012e654, PPCDisableSpeculation_8012e654, (), ());

PPC_SPR_STUB_BODY(PPCMthid4_8012e684, "PPCMthid4 called (stubbed) - Move To HID4")
PPC_NATIVE_OVERRIDE_VOID(8012e684, PPCMthid4_8012e684, (), ());

// ----------------------------------------------------------------------------
// PokéPark-only natives (synthetic PAL identities 0x8FFFxxxx; see guest_region.h).
// The OS ROM font: OSGetFontEncode/OSInitFont read the VI DTV register (0xCC00206E) and the font ROM
// over EXI. USA console => ANSI; the ROM is virtual (fonts only, see OSReadROM_8fff0010).
// ----------------------------------------------------------------------------
extern "C" uint32_t OSGetFontEncode_8fff0000()
{
    return 0; // OS_FONT_ENCODE_ANSI
}
PPC_NATIVE_OVERRIDE(8FFF0000, OSGetFontEncode_8fff0000, uint32_t, (), ());

// OSInitFont: the translated SDK original runs (ReadFont -> __OSReadROM below -> Yay0 Decode -> ExpandFontSheet);
// this wrapper only logs the outcome.
extern "C" void OSInitFont_8fff0004(CpuContext* ctx) {
    const uint32_t fontData = ctx->gpr[3];
    (*[] {
        static const ::GuestFunctions::Entry entry =
            ::GuestFunctions::NextTranslation(::GuestRegion::Translate(0x8FFF0004u));
        return entry;
    }())(ctx);
    RT_LOG(RT_TAG_OS) << "OSInitFont(0x" << std::hex << fontData << std::dec << ") -> " << ctx->gpr[3] << std::endl;
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0004, OSInitFont_8fff0004, "OSInitFont_8fff0004");

// __OSReadROM(buf, length, offset): the IPL ROM over EXI. Only the system fonts are served, from Dolphin's free
// replacements (Data/Sys/GC/font_western.bin and font_japanese.bin, Yay0 like the real ROM) at their ROM offsets.
// Looked up in MKW_FONT_DIR, then <exe>/fonts, then the working directory's fonts/.
namespace {
struct VirtualIplRom {
    std::vector<uint8_t> ansi;
    std::vector<uint8_t> sjis;
    bool loaded = false;
};
std::vector<uint8_t> ReadFontFile(const char* name) {
    std::vector<std::filesystem::path> dirs;
    if (const char* env = std::getenv("MKW_FONT_DIR"); env != nullptr && *env != 0) dirs.emplace_back(env);
    if (const auto exeDir = RuntimePlatform::ExecutableDirectory()) dirs.emplace_back(*exeDir / "fonts");
    dirs.emplace_back(std::filesystem::path("fonts"));
    for (const auto& dir : dirs) {
        std::ifstream in(dir / name, std::ios::binary);
        if (in) {
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    }
    return {};
}
VirtualIplRom& IplRom() {
    static VirtualIplRom rom;
    if (!rom.loaded) {
        rom.loaded = true;
        rom.ansi = ReadFontFile("font_western.bin");
        rom.sjis = ReadFontFile("font_japanese.bin");
        RT_LOG(RT_TAG_OS) << "[font] IPL ROM fonts: western " << rom.ansi.size() << " bytes, japanese " << rom.sjis.size()
                          << " bytes" << (rom.ansi.empty() ? " (missing: put Dolphin's font_western.bin in fonts/)" : "")
                          << std::endl;
    }
    return rom;
}
} // namespace

extern "C" uint32_t OSReadROM_8fff0010(uint32_t buf, int32_t length, int32_t offset) {
    constexpr int32_t kAnsiOffset = 0x1AFF00;
    constexpr int32_t kSjisOffset = 0x1FCF00;
    VirtualIplRom& rom = IplRom();
    if (length <= 0) return 1u;
    try {
        uint8_t* dst = static_cast<uint8_t*>(::Memory::GetPointer(buf, static_cast<size_t>(length)));
        std::memset(dst, 0, static_cast<size_t>(length));
        const auto copyFrom = [&](const std::vector<uint8_t>& image, int32_t base) {
            const int64_t start = static_cast<int64_t>(offset) - base;
            for (int32_t i = 0; i < length; ++i) {
                const int64_t at = start + i;
                if (at >= 0 && at < static_cast<int64_t>(image.size())) dst[i] = image[static_cast<size_t>(at)];
            }
        };
        if (offset >= kSjisOffset) {
            copyFrom(rom.sjis, kSjisOffset);
        } else if (offset >= kAnsiOffset) {
            copyFrom(rom.ansi, kAnsiOffset);
        }
    } catch (...) {
        return 0u;
    }
    return 1u;
}
PPC_NATIVE_OVERRIDE(8FFF0010, OSReadROM_8fff0010, uint32_t, (uint32_t buf, int32_t length, int32_t offset), (buf, length, offset));

// ----------------------------------------------------------------------------
// PokéPark bring-up tracing wrappers (MKW_LOADLOG=1): call the translated original and log the result.
// ----------------------------------------------------------------------------
static bool PokeParkLoadTrace() {
    static const bool trace = std::getenv("MKW_LOADLOG") != nullptr;
    return trace;
}

// fn_800150E0: double-buffered .dac stream reader (returns 1 when the archive is fully decompressed).
extern "C" void PokeParkDacStreamRead_8fff0008(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    (*[] {
        static const ::GuestFunctions::Entry entry =
            ::GuestFunctions::NextTranslation(::GuestRegion::Translate(0x8FFF0008u));
        return entry;
    }())(ctx);
    if (PokeParkLoadTrace()) {
        RT_LOG(RT_TAG_OS) << "[load] DacStreamRead obj=0x" << std::hex << self << " state=" << int(::Memory::Read8(self + 48u))
                          << " buf=" << int(::Memory::Read8(self + 49u)) << " consumed=0x" << ::Memory::Read32(self + 36u)
                          << " -> " << ctx->gpr[3] << std::dec << std::endl;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF0008, PokeParkDacStreamRead_8fff0008, "PokeParkDacStreamRead_8fff0008");

extern "C" void DecompressDacChunk_8fff000c(CpuContext* ctx) {
    const uint32_t a0 = ctx->gpr[3], a1 = ctx->gpr[4], a2 = ctx->gpr[5];
    (*[] {
        static const ::GuestFunctions::Entry entry =
            ::GuestFunctions::NextTranslation(::GuestRegion::Translate(0x8FFF000Cu));
        return entry;
    }())(ctx);
    if (PokeParkLoadTrace()) {
        RT_LOG(RT_TAG_OS) << "[load] DecompressDacChunk(0x" << std::hex << a0 << ", 0x" << a1 << ", 0x" << a2
                          << ") -> " << ctx->gpr[3] << std::dec << std::endl;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8FFF000C, DecompressDacChunk_8fff000c, "DecompressDacChunk_8fff000c");
