// NAND/ISFS HLE: the synchronous RVL NAND* library.
//
// Shared state and helpers live in nand_internal.h.

#include "guest_abi/guest_region.h"
#include "nand_internal.h"

#include <atomic>
#include <cerrno>

namespace {
[[maybe_unused]] bool NandTraceOn() { static const bool on = std::getenv("MKW_NANDLOG") != nullptr; return on; }
[[maybe_unused]] const char* NandTracePath(uint32_t p) { static thread_local std::string s; s.clear(); try { for (uint32_t i = 0; i < 64; ++i) { const char c = static_cast<char>(Memory::Read8(p + i)); if (!c) break; s.push_back(c); } } catch (...) {} return s.c_str(); }
[[maybe_unused]] const char* NandTraceFd(uint32_t fi) { static thread_local std::string s; s.clear(); try { s = "fd=" + std::to_string(static_cast<int32_t>(Memory::Read32(fi))); } catch (...) {} return s.c_str(); }
}

#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>

#endif

// ============================================================================
// Local helpers
// ============================================================================

// Unchecked NUL-terminated write into guest memory. The RVL NAND library hands
// out fixed-size caller buffers and never tells us how big they are, so this
// deliberately matches the library's own unbounded copy.
static void WriteGuestNandPath(uint32_t address, const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        Memory::Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(value[i]));
    }
    Memory::Write8(address + static_cast<uint32_t>(value.size()), 0);
}

// NANDGetHomeDir and NANDGetCurrentDir both answer with the title's data
// directory; the SDK keeps no separate working directory for us to track.
static int32_t WriteNandDataDir(uint32_t outPathPtr) {
    if (!outPathPtr) {
        return NAND_RESULT_INVALID;
    }
    WriteGuestNandPath(outPathPtr, CurrentNandDataDir());
    return NAND_RESULT_OK;
}

// fileInfoPtr -> fd -> live handle. Logs and returns null when the guest hands
// us a descriptor that was never opened (or was already closed).
static FileHandle* ResolveNandFileHandle(const char* who, uint32_t fileInfoPtr) {
    const int32_t fd = static_cast<int32_t>(Memory::Read32(fileInfoPtr));
    FileHandle* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError(who, "invalid fd=%d", fd);
        return nullptr;
    }
    return handle;
}

// ============================================================================
// The synchronous RVL NAND* library
// ============================================================================

static bool RenameNoReplace(const std::filesystem::path& from,
                            const std::filesystem::path& to,
                            std::error_code& error) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error.clear();
        return true;
    }
    error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#elif defined(__linux__)
    const int result = syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE);
    if (result == 0) {
        error.clear();
        return true;
    }
    error = std::error_code(errno, std::generic_category());
    return false;
#else
    (void)from;
    (void)to;
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
#endif
}

extern "C" int32_t NANDInit_HLE(void) {
    // IOS empties /tmp at every boot. Without it a save or copy interrupted in an earlier session
    // (stale *.dan, *.nandsafe.tmp) leaks into this one. Only the emulated NAND's /tmp is touched.
    static bool s_tmpCleared = false;
    if (!s_tmpCleared && !::GuestRegion::IsMarioKart()) {
        s_tmpCleared = true;
        const std::filesystem::path tmp = TranslateNandPath("/tmp");
        std::error_code ec;
        if (!tmp.empty() && tmp.filename() == "tmp" && std::filesystem::is_directory(tmp, ec)) {
            size_t removed = 0;
            for (const auto& entry : std::filesystem::directory_iterator(tmp, ec)) {
                std::error_code rec;
                removed += static_cast<size_t>(std::filesystem::remove_all(entry.path(), rec));
            }
            std::fprintf(stderr, "[nand] /tmp emptied at boot (%zu entries)\n", removed);
        }
    }
    // Initialize ISFS
    ISFS_OpenLib_Initialize(&GetPersistentCpuContext());

    // NANDHomeDir is at 0x80346D20 (from ESP_GetDataDir output)
    WriteGuestNandPath(MKW_GADDR(80346D20), CurrentNandDataDir());

    // Mark NAND as initialized (0x80386848 = 2)
    Memory::Write32(MKW_GADDR(80386848), 2);

    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019E18C, NANDInit_HLE, int32_t, (void), ());

extern "C" int32_t NANDGetCurrentDir_HLE(uint32_t outPathPtr) {
    return WriteNandDataDir(outPathPtr);
}
PPC_NATIVE_OVERRIDE(8019E390, NANDGetCurrentDir_HLE, int32_t, (uint32_t outPathPtr), (outPathPtr));

extern "C" int32_t NANDCheck_HLE(uint32_t blockSize, uint32_t blockCount, uint32_t outResults) {
    const int32_t result = NandCheckContract::WriteHealthyResult(
        outResults,
        [](uint32_t address, size_t length) { return Memory::Contains(address, length); },
        [](uint32_t address, uint32_t value) {
            try {
                Memory::Write32(address, value);
                return true;
            } catch (const Memory::AccessViolation&) {
                return false;
            }
        });
    if (result != NAND_RESULT_OK) {
        LogNandError("NANDCheck",
                     "FAILED: invalid four-byte result buffer 0x%08X (blockSize=%u blockCount=%u)",
                     outResults, blockSize, blockCount);
    }
    return result;
}
PPC_NATIVE_OVERRIDE(8019EAD0, NANDCheck_HLE, int32_t, (uint32_t blockSize, uint32_t blockCount, uint32_t outResults), (blockSize, blockCount, outResults));

static int32_t NANDOpen_HLE_Impl(uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path || !fileInfoPtr) {
        LogNandError("NANDOpen", "invalid params: path=%p fileInfo=0x%08X", path, fileInfoPtr);
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);
    if (hostPath.empty()) {
        LogNandError("NANDOpen", "FAILED to translate path '%s'", path);
        return NAND_RESULT_INVALID;
    }

    if (const auto result = NandCheckSystemSaveRead("NANDOpen", hostPath, mode))
        return *result;

    // Existing-file write opens go through a shadow copy seeded from the original, so a
    // crash between NANDWrite and NANDClose cannot leave a torn file (the game patches
    // sub-ranges, e.g. ghost saves at a non-zero offset). New files still create in place.
    if ((mode == 2 || mode == 3) && ::GuestRegion::IsMarioKart() && PathExists(hostPath) && !IsDirectory(hostPath)) {
        // (Console NANDOpen writes in place; the shadow is a MKWii crash-safety extra. PokePark reads a
        // save back through a second handle before closing the writer, so a shadow made it look corrupted.)
        if (IsHostPathOpen(hostPath)) {
            // Another live handle already refers to this file. A shadow would hide the
            // writes from that handle, so stay in place for this open.
            LogNandWarning("NANDOpen", "WARNING: '%s' already has a live handle, writing in place",
                           HostPathText(hostPath).c_str());
        } else {
            const std::filesystem::path tempPath = SafeTempPathFor(hostPath);
            if (DiscardStaleSafeTemp(tempPath)) {
                std::error_code ec;
                std::filesystem::copy_file(hostPath, tempPath,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    LogNandWarning("NANDOpen", "WARNING: could not seed shadow '%s' (%s), writing in place",
                                   HostPathText(tempPath).c_str(), ec.message().c_str());
                    NandRemove(tempPath);
                } else {
                    FILE* shadow = NandFopen(tempPath, "r+b");
                    if (!shadow) {
                        LogNandWarning("NANDOpen", "WARNING: could not open shadow '%s', writing in place",
                                       HostPathText(tempPath).c_str());
                        NandRemove(tempPath);
                    } else {
                        const int32_t shadowFd = AllocateFd(tempPath, shadow, static_cast<int32_t>(mode));
                        {
                            std::lock_guard<std::mutex> lock(g_fdMutex);
                            auto it = g_fileHandles.find(shadowFd);
                            if (it != g_fileHandles.end()) {
                                it->second.safeCommitPath = hostPath;
                            }
                        }
                        Memory::Write32(fileInfoPtr, static_cast<uint32_t>(shadowFd));
                        Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_OPEN);
                        return NAND_RESULT_OK;
                    }
                }
            }
        }
    }

    const char* fopenMode = "rb";
    if (mode == 1) fopenMode = "rb";
    else if (mode == 2) fopenMode = "r+b";
    else if (mode == 3) fopenMode = "r+b";
    
    FILE* file = NandFopen(hostPath, fopenMode);
    if (!file && mode >= 2) {
        // Try creating for write modes
        file = NandFopen(hostPath, "w+b");
    }
    
    // Create parent directories and retry
    if (!file && CreateParentDirectories(hostPath)) {
        file = NandFopen(hostPath, mode >= 2 ? "w+b" : "rb");
    }

    if (!file) {
        if (IsFaceLibResourcePath(path) && SeedFaceLibResource(hostPath)) {
            file = NandFopen(hostPath, fopenMode);
        }
        if (!file) {
            LogNandError("NANDOpen", "FAILED to open");
            return NAND_RESULT_NOEXISTS;
        }
    }

    int32_t fd = AllocateFd(hostPath, file, mode);
    Memory::Write32(fileInfoPtr, static_cast<uint32_t>(fd));
    Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_OPEN);
    return NAND_RESULT_OK;
}
extern "C" int32_t NANDOpen_HLE(uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode) {
    const int32_t r = NANDOpen_HLE_Impl(pathPtr, fileInfoPtr, mode);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDOpen %s pathPtr=0x%08X fileInfoPtr=0x%08X mode=0x%08X -> %d\n", NandTracePath(pathPtr), static_cast<unsigned>(pathPtr), static_cast<unsigned>(fileInfoPtr), static_cast<unsigned>(mode), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019C800, NANDOpen_HLE, int32_t, (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode), (pathPtr, fileInfoPtr, mode));

static int32_t NANDClose_HLE_Impl(uint32_t fileInfoPtr) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }
    
    uint8_t openFlag = Memory::Read8(fileInfoPtr + 0x8a);
    if (openFlag == NAND_OPEN_FLAG_SAFE_OPEN || openFlag == NAND_OPEN_FLAG_SAFE_OPEN_ASYNC) {
        // Console behaviour (nandClose @ 0x8019CA80): only openFlag==1 is accepted, a safe
        // handle falls through to -8. It is neither closed nor committed here.
        LogNandWarning("NANDClose", "WARNING: refusing safe-open handle (flag=%u), NANDSafeClose is required", openFlag);
        return NAND_RESULT_INVALID;
    }
    if (openFlag != NAND_OPEN_FLAG_OPEN) {
        LogNandWarning("NANDClose", "file not open (flag=%u)", openFlag);
        return NAND_RESULT_INVALID;
    }

    int32_t fd = static_cast<int32_t>(Memory::Read32(fileInfoPtr));

    // Flush, fsync and (for shadowed write handles) atomically publish. A failure here is
    // reported to the guest instead of being swallowed, so NandUtil_close surfaces it as a
    // save error rather than a silent success on top of a half-written file.
    const int32_t result = CommitAndCloseFd("NANDClose", fd, /*missingFdIsError=*/false);
    if (result != NAND_RESULT_OK) {
        return result;
    }

    Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_CLOSED); // Mark as closed
    return NAND_RESULT_OK;
}
extern "C" int32_t NANDClose_HLE(uint32_t fileInfoPtr) {
    const int32_t r = NANDClose_HLE_Impl(fileInfoPtr);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDClose %s fileInfoPtr=0x%08X -> %d\n", NandTraceFd(fileInfoPtr), static_cast<unsigned>(fileInfoPtr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019CA80, NANDClose_HLE, int32_t, (uint32_t fileInfoPtr), (fileInfoPtr));

static int32_t NANDRead_HLE_Impl(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDRead", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    if (length == 0) {
        return 0;
    }
    // The host pointer is only valid for the range it was resolved with.
    if (!Memory::Contains(bufferPtr, length)) {
        LogNandError("NANDRead", "invalid buffer 0x%08X length=%u", bufferPtr, length);
        return NAND_RESULT_INVALID;
    }
    uint8_t* buffer = Memory::GetPointer(bufferPtr, length);

    size_t bytesRead = std::fread(buffer, 1, length, handle->file);
    return static_cast<int32_t>(bytesRead);
}
extern "C" int32_t NANDRead_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    const int32_t r = NANDRead_HLE_Impl(fileInfoPtr, bufferPtr, length);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDRead %s fileInfoPtr=0x%08X bufferPtr=0x%08X length=0x%08X -> %d\n", NandTraceFd(fileInfoPtr), static_cast<unsigned>(fileInfoPtr), static_cast<unsigned>(bufferPtr), static_cast<unsigned>(length), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019B7A4, NANDRead_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length), (fileInfoPtr, bufferPtr, length));

static int32_t NANDWrite_HLE_Impl(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDWrite", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    if (length == 0) {
        return 0;
    }
    if (!Memory::Contains(bufferPtr, length)) {
        LogNandError("NANDWrite", "invalid buffer 0x%08X length=%u", bufferPtr, length);
        return NAND_RESULT_INVALID;
    }
    const uint8_t* buffer = Memory::GetPointer(bufferPtr, length);

    size_t bytesWritten = std::fwrite(buffer, 1, length, handle->file);
    std::fflush(handle->file);
    return static_cast<int32_t>(bytesWritten);
}
extern "C" int32_t NANDWrite_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    if (NandTraceOn()) { if (const CpuContext* c = TryGetCpuContext()) { std::string chain; char b[16]; uint32_t sp = c->gpr[1]; std::snprintf(b, sizeof(b), "%08X", c->lr); chain += b; try { for (int i = 0; i < 8 && sp; ++i) { sp = Memory::Read32(sp); if (!sp) break; std::snprintf(b, sizeof(b), ",%08X", Memory::Read32(sp + 4)); chain += b; } } catch (...) {} std::fprintf(stderr, "[nandlog] write-chain len=0x%X %s\n", length, chain.c_str()); } }
    const int32_t r = NANDWrite_HLE_Impl(fileInfoPtr, bufferPtr, length);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDWrite %s fileInfoPtr=0x%08X bufferPtr=0x%08X length=0x%08X -> %d\n", NandTraceFd(fileInfoPtr), static_cast<unsigned>(fileInfoPtr), static_cast<unsigned>(bufferPtr), static_cast<unsigned>(length), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019B884, NANDWrite_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length), (fileInfoPtr, bufferPtr, length));

static int32_t NANDSeek_HLE_Impl(uint32_t fileInfoPtr, int32_t offset, int32_t whence) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDSeek", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    if (std::fseek(handle->file, offset, NandSeekOrigin(whence)) != 0) {
        return NAND_RESULT_UNKNOWN;
    }

    return static_cast<int32_t>(std::ftell(handle->file));
}
extern "C" int32_t NANDSeek_HLE(uint32_t fileInfoPtr, int32_t offset, int32_t whence) {
    const int32_t r = NANDSeek_HLE_Impl(fileInfoPtr, offset, whence);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDSeek %s fileInfoPtr=0x%08X offset=0x%08X whence=0x%08X -> %d\n", NandTraceFd(fileInfoPtr), static_cast<unsigned>(fileInfoPtr), static_cast<unsigned>(offset), static_cast<unsigned>(whence), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019B964, NANDSeek_HLE, int32_t, (uint32_t fileInfoPtr, int32_t offset, int32_t whence), (fileInfoPtr, offset, whence));

static int32_t NANDGetLength_HLE_Impl(uint32_t fileInfoPtr, uint32_t outLengthPtr) {
    if (!fileInfoPtr || !outLengthPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDGetLength", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    const NandFileExtent extent = NandProbeFileExtent(handle->file);
    Memory::Write32(outLengthPtr, static_cast<uint32_t>(extent.size));
    return NAND_RESULT_OK;
}
extern "C" int32_t NANDGetLength_HLE(uint32_t fileInfoPtr, uint32_t outLengthPtr) {
    const int32_t r = NANDGetLength_HLE_Impl(fileInfoPtr, outLengthPtr);
    if (NandTraceOn()) { try { std::fprintf(stderr, "[nandlog] length=%u\n", Memory::Read32(outLengthPtr)); } catch (...) {} }
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDGetLength %s fileInfoPtr=0x%08X outLengthPtr=0x%08X -> %d\n", NandTraceFd(fileInfoPtr), static_cast<unsigned>(fileInfoPtr), static_cast<unsigned>(outLengthPtr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019BF4C, NANDGetLength_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t outLengthPtr), (fileInfoPtr, outLengthPtr));

static int32_t NANDCreate_HLE_Impl(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path hostPath = TranslateNandPath(path);
    CreateParentDirectories(hostPath);

    // Check if file already exists
    if (PathExists(hostPath)) {
        return NAND_RESULT_EXISTS;
    }
    
    // Create empty file
    FILE* f = NandFopen(hostPath, "wb");
    if (!f) {
        return NAND_RESULT_UNKNOWN;
    }
    std::fclose(f);
    
    return NAND_RESULT_OK;
}
extern "C" int32_t NANDCreate_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const int32_t r = NANDCreate_HLE_Impl(pathPtr, perm, attr);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDCreate %s pathPtr=0x%08X perm=0x%08X attr=0x%08X -> %d\n", NandTracePath(pathPtr), static_cast<unsigned>(pathPtr), static_cast<unsigned>(perm), static_cast<unsigned>(attr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019B43C, NANDCreate_HLE, int32_t, (uint32_t pathPtr, uint32_t perm, uint32_t attr), (pathPtr, perm, attr));

static int32_t NANDDelete_HLE_Impl(uint32_t pathPtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    if (NandRemove(hostPath)) {
        return NAND_RESULT_OK;
    }
    
    return NAND_RESULT_UNKNOWN;
}
extern "C" int32_t NANDDelete_HLE(uint32_t pathPtr) {
    const int32_t r = NANDDelete_HLE_Impl(pathPtr);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDDelete %s pathPtr=0x%08X -> %d\n", NandTracePath(pathPtr), static_cast<unsigned>(pathPtr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019B59C, NANDDelete_HLE, int32_t, (uint32_t pathPtr), (pathPtr));

static int32_t NANDCreateDir_HLE_Impl(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);
    if (hostPath.empty()) {
        return NAND_RESULT_INVALID;
    }

    if (PathExists(hostPath)) {
        if (IsDirectory(hostPath)) {
            return NAND_RESULT_OK; // Already exists
        }
        return NAND_RESULT_EXISTS;
    }
    
    if (CreateDirectoryPath(hostPath)) {
        return NAND_RESULT_OK;
    }
    
    return NAND_RESULT_UNKNOWN;
}
extern "C" int32_t NANDCreateDir_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const int32_t r = NANDCreateDir_HLE_Impl(pathPtr, perm, attr);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDCreateDir %s pathPtr=0x%08X perm=0x%08X attr=0x%08X -> %d\n", NandTracePath(pathPtr), static_cast<unsigned>(pathPtr), static_cast<unsigned>(perm), static_cast<unsigned>(attr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019BBE0, NANDCreateDir_HLE, int32_t, (uint32_t pathPtr, uint32_t perm, uint32_t attr), (pathPtr, perm, attr));

static int32_t NANDMove_HLE_Impl(uint32_t srcPathPtr, uint32_t dstPathPtr) {
    // A cross-mount move is implemented as several host operations. Keep two
    // guest moves from interleaving those operations and corrupting recovery.
    static std::mutex moveMutex;
    std::lock_guard<std::mutex> lock(moveMutex);

    const char* srcPath = srcPathPtr ? (const char*)Memory::GetPointer(srcPathPtr) : nullptr;
    const char* dstPath = dstPathPtr ? (const char*)Memory::GetPointer(dstPathPtr) : nullptr;
    
    if (!srcPath || !dstPath) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path srcHost = TranslateNandPath(srcPath);
    const std::filesystem::path dstDirectoryHost = TranslateNandPath(dstPath);
    const std::filesystem::path srcName = srcHost.filename();
    if (srcName.empty()) {
        return NAND_RESULT_INVALID;
    }

    // RVL SDK nandMove always appends the source entry's relative name to the
    // second argument. The latter is a directory, not a complete destination
    // filename (for example /tmp/banner.bin -> <title home>/banner.bin).
    const std::filesystem::path dstHost = dstDirectoryHost / srcName;

    if (!PathExists(srcHost)) {
        return NAND_RESULT_NOEXISTS;
    }
    if (!IsDirectory(dstDirectoryHost)) {
        return NAND_RESULT_NOEXISTS;
    }
    if (PathExists(dstHost)) {
        return NAND_RESULT_EXISTS;
    }

    std::error_code ec;
    std::filesystem::rename(srcHost, dstHost, ec);
    if (!ec) {
        return NAND_RESULT_OK;
    }

    // Flatpak can expose the managed NAND and an external Riivolution save
    // directory as separate mounts. Linux cannot rename across mounts, but
    // nandMove must still work for files such as banner.bin. Preserve the
    // operation's semantics with a copy followed by source removal.
    if (ec == std::errc::cross_device_link) {
        static std::atomic<uint64_t> moveSequence{0};
#ifdef _WIN32
        const auto processId = GetCurrentProcessId();
#else
        const auto processId = getpid();
#endif
        std::filesystem::path scratchHost;
        std::error_code scratchEc;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto name = ".nandmove-" + std::to_string(processId) + "-" +
                              std::to_string(moveSequence.fetch_add(1)) + "-" +
                              std::to_string(attempt);
            const auto candidate = dstDirectoryHost / name;
            scratchEc.clear();
            if (std::filesystem::create_directory(candidate, scratchEc)) {
                scratchHost = candidate;
                break;
            }
            if (scratchEc && scratchEc != std::errc::file_exists) {
                LogNandError("NANDMove", "failed to claim temporary directory '%s': %s",
                             HostPathText(candidate).c_str(), scratchEc.message().c_str());
                return NAND_RESULT_UNKNOWN;
            }
        }
        if (scratchHost.empty()) {
            LogNandError("NANDMove", "could not claim a unique temporary directory");
            return NAND_RESULT_UNKNOWN;
        }

        const bool sourceIsDirectory = IsDirectory(srcHost);
        const std::filesystem::path tempHost = scratchHost / srcName;
        const auto cleanupScratch = [&]() {
            std::error_code cleanupEc;
            std::filesystem::remove_all(scratchHost, cleanupEc);
            if (cleanupEc) {
                LogNandError("NANDMove", "failed to clean up temporary directory '%s': %s",
                             HostPathText(scratchHost).c_str(), cleanupEc.message().c_str());
            }
        };

        std::error_code copyEc;
        if (sourceIsDirectory) {
            std::filesystem::copy(srcHost, tempHost,
                                  std::filesystem::copy_options::recursive, copyEc);
        } else {
            std::filesystem::copy_file(srcHost, tempHost, copyEc);
        }

        if (copyEc) {
            LogNandError("NANDMove", "cross-mount copy failed: %s", copyEc.message().c_str());
            cleanupScratch();
            return NAND_RESULT_UNKNOWN;
        }

        std::error_code publishEc;
        if (sourceIsDirectory) {
            RenameNoReplace(tempHost, dstHost, publishEc);
        } else {
            // link(2) and CreateHardLink do not replace an existing destination,
            // unlike rename(2) on POSIX. Both paths are already on the target
            // filesystem, so the link is a no-replace publication operation.
            std::filesystem::create_hard_link(tempHost, dstHost, publishEc);
        }
        if (publishEc) {
            LogNandError("NANDMove", "failed to publish cross-mount copy: %s",
                         publishEc.message().c_str());
            cleanupScratch();
            return NAND_RESULT_UNKNOWN;
        }
        cleanupScratch();

        std::error_code removeEc;
        std::filesystem::remove_all(srcHost, removeEc);
        if (!removeEc) {
            LogNandWarning("NANDMove", "used copy/remove fallback across mounts");
            return NAND_RESULT_OK;
        }

        // Keep the source as the authoritative copy when cleanup fails. The
        // destination was published atomically on its own mount; regular files
        // are rolled back below, while directories keep the complete copy when
        // their source removal was only partial. Cross-mount moves cannot
        // provide crash-atomicity, so this is best effort.
        LogNandError("NANDMove", "copy succeeded but source removal failed: %s",
                     removeEc.message().c_str());
        if (sourceIsDirectory) {
            // remove_all may have removed only part of a directory tree. Keep
            // the complete published copy rather than rolling it back to a
            // partially deleted source.
            LogNandWarning("NANDMove", "preserving published directory copy after partial source removal");
        } else {
            std::error_code rollbackEc;
            std::filesystem::remove_all(dstHost, rollbackEc);
            if (rollbackEc) {
                LogNandError("NANDMove", "failed to roll back destination '%s': %s",
                             HostPathText(dstHost).c_str(), rollbackEc.message().c_str());
            }
        }
        return NAND_RESULT_UNKNOWN;
    }

    LogNandError("NANDMove", "FAILED error=%d message='%s'", ec.value(), ec.message().c_str());
    return NAND_RESULT_UNKNOWN;
}
extern "C" int32_t NANDMove_HLE(uint32_t srcPathPtr, uint32_t dstPathPtr) {
    const int32_t r = NANDMove_HLE_Impl(srcPathPtr, dstPathPtr);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDMove %s srcPathPtr=0x%08X dstPathPtr=0x%08X -> %d\n", NandTracePath(srcPathPtr), static_cast<unsigned>(srcPathPtr), static_cast<unsigned>(dstPathPtr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019BEE8, NANDMove_HLE, int32_t, (uint32_t srcPathPtr, uint32_t dstPathPtr), (srcPathPtr, dstPathPtr));

static int32_t NANDGetStatus_HLE_Impl(uint32_t pathPtr, uint32_t outStatusPtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path || !outStatusPtr) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    // NANDStatus structure - fill with fake values
    // This is typically used to check permissions and file type
    Memory::Write32(outStatusPtr, 0);      // Magic/type
    Memory::Write32(outStatusPtr + 4, 0);  // Permissions  
    
    return NAND_RESULT_OK;
}
extern "C" int32_t NANDGetStatus_HLE(uint32_t pathPtr, uint32_t outStatusPtr) {
    const int32_t r = NANDGetStatus_HLE_Impl(pathPtr, outStatusPtr);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDGetStatus %s pathPtr=0x%08X outStatusPtr=0x%08X -> %d\n", NandTracePath(pathPtr), static_cast<unsigned>(pathPtr), static_cast<unsigned>(outStatusPtr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019C380, NANDGetStatus_HLE, int32_t, (uint32_t pathPtr, uint32_t outStatusPtr), (pathPtr, outStatusPtr));

static int32_t NANDGetType_HLE_Impl(uint32_t pathPtr, uint32_t outTypePtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path || !outTypePtr) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path hostPath = TranslateNandPath(path);
    
    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    // 1 = file, 2 = directory
    uint8_t type = IsDirectory(hostPath) ? 2 : 1;
    Memory::Write8(outTypePtr, type);
    return NAND_RESULT_OK;
}
extern "C" int32_t NANDGetType_HLE(uint32_t pathPtr, uint32_t outTypePtr) {
    const int32_t r = NANDGetType_HLE_Impl(pathPtr, outTypePtr);
    if (NandTraceOn()) std::fprintf(stderr, "[nandlog] NANDGetType %s pathPtr=0x%08X outTypePtr=0x%08X -> %d\n", NandTracePath(pathPtr), static_cast<unsigned>(pathPtr), static_cast<unsigned>(outTypePtr), r);
    return r;
}

PPC_NATIVE_OVERRIDE(8019E770, NANDGetType_HLE, int32_t, (uint32_t pathPtr, uint32_t outTypePtr), (pathPtr, outTypePtr));

// ============================================================================
// contentFastOpenNAND/contentReadNAND/contentCloseNAND: NAND-installed content (WADs) never
// exists here, everything comes from the DVD image, so refusing is the correct SDK answer.
// ============================================================================

extern "C" int32_t contentFastOpenNAND_HLE(uint32_t contentId, uint32_t outHandlePtr) {
    return ISFS_ENOENT;
}
PPC_NATIVE_OVERRIDE(8015BC80, contentFastOpenNAND_HLE, int32_t, (uint32_t contentId, uint32_t outHandlePtr), (contentId, outHandlePtr));

extern "C" int32_t contentReadNAND_HLE(uint32_t handlePtr, uint32_t buffer, uint32_t length, uint32_t outReadPtr) {
    return ISFS_EINVAL;
}
PPC_NATIVE_OVERRIDE(8015BCF8, contentReadNAND_HLE, int32_t, (uint32_t handlePtr, uint32_t buffer, uint32_t length, uint32_t outReadPtr), (handlePtr, buffer, length, outReadPtr));
