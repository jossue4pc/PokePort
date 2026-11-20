// gx_copy.cpp - Framebuffer Copy Operations
#include <string>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include "gx_internal.h"

#include "settings_overlay.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
// Copy destinations stay GPU-only until an explicit/lazy readback. Track the
// guest ranges that own those results so a later data-cache flush over a reused
// allocation can retire the stale GPU texture before it is considered by a
// subsequent GXLoadTexObj. There is at most one live range per destination;
// rewriting the destination replaces its previous extent.
std::mutex g_efbCopyDestinationsMutex;
std::map<uint32_t, uint32_t> g_efbCopyDestinations;
uint32_t g_largestEfbCopyDestination = 0;

// How many destinations touch each 64 KiB bucket of guest RAM. Nearly every DCStoreRange misses all
// destinations, and an empty bucket answers that without walking the map.
constexpr uint32_t kEfbCopyBucketShift = 16;
std::unordered_map<uint32_t, uint32_t> g_efbCopyBuckets;

void CountEfbCopyBucketsLocked(uint64_t start, uint32_t size, int32_t delta) {
    const uint64_t last = (start + size - 1u) >> kEfbCopyBucketShift;
    for (uint64_t bucket = start >> kEfbCopyBucketShift; bucket <= last; ++bucket) {
        g_efbCopyBuckets[static_cast<uint32_t>(bucket)] += static_cast<uint32_t>(delta);
    }
}

bool AnyEfbCopyBucketLocked(uint64_t start, uint32_t size) {
    const uint64_t first = start >> kEfbCopyBucketShift;
    const uint64_t last = (start + size - 1u) >> kEfbCopyBucketShift;
    if (last - first + 1u > g_efbCopyBuckets.size()) {
        return true; // wider than the index; the map walk answers
    }
    for (uint64_t bucket = first; bucket <= last; ++bucket) {
        const auto it = g_efbCopyBuckets.find(static_cast<uint32_t>(bucket));
        if (it != g_efbCopyBuckets.end() && it->second != 0) {
            return true;
        }
    }
    return false;
}

void RememberEfbCopyDestination(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
    const uint32_t canonical = CanonicalizeGxMainRamAddress(addr);
    const auto [it, inserted] = g_efbCopyDestinations.try_emplace(canonical, size);
    if (inserted) {
        CountEfbCopyBucketsLocked(canonical, size, +1);
    } else if (it->second != size) {
        CountEfbCopyBucketsLocked(canonical, it->second, -1);
        CountEfbCopyBucketsLocked(canonical, size, +1);
        it->second = size;
    }
    // A conservative monotonic maximum lets invalidation jump directly to
    // the only map interval that could overlap instead of walking every copy
    // destination on every DCStoreRange.
    g_largestEfbCopyDestination = std::max(g_largestEfbCopyDestination, size);
}

} // namespace

void InvalidateEfbCopyDestinationsForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }

    const uint64_t dirtyStart = CanonicalizeGxMainRamAddress(addr);
    const uint64_t dirtyEnd = dirtyStart + size;
    std::vector<uint32_t> retired;
    {
        std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
        if (!AnyEfbCopyBucketLocked(dirtyStart, size)) {
            return;
        }
        const uint64_t earliestCandidate =
            dirtyStart > g_largestEfbCopyDestination ? dirtyStart - g_largestEfbCopyDestination : 0;
        for (auto it = g_efbCopyDestinations.lower_bound(static_cast<uint32_t>(earliestCandidate));
             it != g_efbCopyDestinations.end() && static_cast<uint64_t>(it->first) < dirtyEnd;) {
            const uint64_t copyStart = it->first;
            const uint64_t copyEnd = copyStart + it->second;
            if (dirtyEnd <= copyStart || dirtyStart >= copyEnd) {
                ++it;
                continue;
            }
            retired.push_back(it->first);
            CountEfbCopyBucketsLocked(it->first, it->second, -1);
            it = g_efbCopyDestinations.erase(it);
        }
    }

    // Preserve FIFO ordering: the destroy command is emitted before any later
    // texture load that can consume the freshly flushed RAM bytes.
    for (const uint32_t copyAddr : retired) {
        GXDestroyCopyTex(GuestToHostPtr(copyAddr));
    }
}

// ============================================================================
// Display Copy Source/Destination
// ============================================================================

extern "C" void GX__SetDispCopySrc_8016f438(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GX_DEFER_CALL(GX__SetDispCopySrc_8016f438, l, t, w, h);
    GXSetDispCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
}
PPC_NATIVE_OVERRIDE_VOID(8016f438, GX__SetDispCopySrc_8016f438, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetDispCopyDst_8016f4b8(uint32_t w, uint32_t h) {
    GxWorker::Run([w, h] { GXSetDispCopyDst((u16)w, (u16)h); });
}
PPC_NATIVE_OVERRIDE_VOID(8016f4b8, GX__SetDispCopyDst_8016f4b8, (uint32_t w, uint32_t h), (w, h));

// ============================================================================
// Texture Copy Source/Destination
// ============================================================================

extern "C" void GX__SetTexCopySrc_8016f478(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GxWorker::Run([l, t, w, h] { GXSetTexCopySrc((u16)l, (u16)t, (u16)w, (u16)h); });
    g_texCopyState.srcLeft=(u16)l; g_texCopyState.srcTop=(u16)t;
    g_texCopyState.srcWidth=(u16)w; g_texCopyState.srcHeight=(u16)h;
}
PPC_NATIVE_OVERRIDE_VOID(8016f478, GX__SetTexCopySrc_8016f478, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetTexCopyDst_8016f4dc(uint32_t w, uint32_t h, uint32_t f, uint32_t m) {
    GxWorker::Run([w, h, f, m] { GXSetTexCopyDst((u16)w, (u16)h, (GXTexFmt)f, (GXBool)m); });
    g_texCopyState.dstWidth=(u16)w; g_texCopyState.dstHeight=(u16)h;
    g_texCopyState.dstFormat=f; g_texCopyState.dstMipmap=m;
}
PPC_NATIVE_OVERRIDE_VOID(8016f4dc, GX__SetTexCopyDst_8016f4dc, (uint32_t w, uint32_t h, uint32_t f, uint32_t m), (w, h, f, m));

extern "C" void GX__SetCopyFilter_8016fa40(uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa) {
    uint8_t sp[12][2]={}, vfb[7]={};
    if(spa) std::memcpy(sp, GuestToHostPtr(spa, 24), 24);
    if(vfa) std::memcpy(vfb, GuestToHostPtr(vfa, 7), 7);
    std::array<uint8_t, 24> spCopy{};
    std::array<uint8_t, 7> vfCopy{};
    std::memcpy(spCopy.data(), sp, 24);
    std::memcpy(vfCopy.data(), vfb, 7);
    GxWorker::Run([aa, vf, spCopy, vfCopy]() mutable {
        GXSetCopyFilter((GXBool)aa, reinterpret_cast<u8(*)[2]>(spCopy.data()), (GXBool)vf, vfCopy.data());
    });
}
PPC_NATIVE_OVERRIDE_VOID(8016fa40, GX__SetCopyFilter_8016fa40, (uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa), (aa, spa, vf, vfa));

extern "C" void GX__SetDispCopyGamma_8016fc24(uint32_t g) {
    GxWorker::Run([g] { GXSetDispCopyGamma((GXGamma)g); });
}
PPC_NATIVE_OVERRIDE_VOID(8016fc24, GX__SetDispCopyGamma_8016fc24, (uint32_t g), (g));

// ============================================================================
// Copy Execution
// ============================================================================

std::atomic<uint64_t> g_bringupCopyDisps{0}; // bring-up heartbeat (vi.cpp)

// ----------------------------------------------------------------------------
// MKW_SHOT=<frame>,<path.ppm>: bring-up thumbnail. At GXCopyDisp number <frame> (before the copy clears the EFB)
// request a 160x132 grid of EFB color peeks (every 4th pixel of 640x528); when the readback completes on a later
// frame, write it as a binary PPM. Lets a headless session see what the game draws.
// ----------------------------------------------------------------------------
namespace {
constexpr int kShotW = 152; // 608x456 EFB (PokéPark) sampled every 4th pixel
constexpr int kShotH = 114;
constexpr int kShotStep = 4;
constexpr int kShotRowsPerBand = 4096 / kShotW; // aurora readback: <= 4096 items per capture
constexpr int kShotBands = (kShotH + kShotRowsPerBand - 1) / kShotRowsPerBand;
struct BringupShot {
    uint64_t frame = 0;
    uint64_t every = 0; // MKW_SHOT_EVERY: one more thumbnail every N copies, named <base>_<frame>.ppm
    std::string base;
    std::string path;
    AuroraReadbackStream streams[kShotBands] = {};
    bool requested = false;
    std::atomic<bool> done{false};
    std::vector<u32> pixels = std::vector<u32>(kShotW * kShotH, 0);
    bool bandDone[kShotBands] = {};
};
BringupShot& Shot() {
    static BringupShot* shot = [] {
        auto* s = new BringupShot();
        if (const char* env = std::getenv("MKW_SHOT"); env && *env) {
            char* end = nullptr;
            s->frame = std::strtoull(env, &end, 10);
            if (end && *end == ',') s->path = end + 1;
            if (const char* ev = std::getenv("MKW_SHOT_EVERY"); ev && *ev && !s->path.empty()) {
                s->every = std::strtoull(ev, nullptr, 10);
                s->base = s->path;
                if (s->base.size() > 4 && s->base.substr(s->base.size() - 4) == ".ppm") s->base.resize(s->base.size() - 4);
                s->path = s->base + "_" + std::to_string(s->frame) + ".ppm";
            }
        }
        return s;
    }();
    return *shot;
}
void ShotRequest(BringupShot* shot) {
    for (int band = 0; band < kShotBands; ++band) {
        if (shot->streams[band] == 0) shot->streams[band] = AuroraReadbackCreateStream();
        std::vector<AuroraReadbackPeekRequest> points;
        const int y0 = band * kShotRowsPerBand, y1 = std::min(kShotH, y0 + kShotRowsPerBand);
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < kShotW; ++x) {
                points.push_back({static_cast<u64>(y * kShotW + x), AURORA_READBACK_PEEK_ARGB,
                                  static_cast<u16>(x * kShotStep), static_cast<u16>(y * kShotStep)});
            }
        }
        const GXBool ok = AuroraReadbackCapturePeeks(shot->streams[band], points.data(), static_cast<u32>(points.size()));
        RT_LOG(RT_TAG_GX) << "[shot] band " << band << " requested " << points.size() << " ok=" << int(ok) << std::endl;
    }
}
void ShotCollect(BringupShot* shot) {
    if (shot->done.load()) return;
    bool all = true;
    for (int band = 0; band < kShotBands; ++band) {
        if (shot->bandDone[band]) continue;
        const int y0 = band * kShotRowsPerBand, y1 = std::min(kShotH, y0 + kShotRowsPerBand);
        std::vector<AuroraReadbackDest> dests;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < kShotW; ++x)
                dests.push_back({static_cast<u64>(y * kShotW + x), &shot->pixels[y * kShotW + x], sizeof(u32), GX_FALSE});
        if (AuroraReadbackGet(shot->streams[band], dests.data(), static_cast<u32>(dests.size()))) {
            size_t found = 0;
            for (const auto& d : dests) found += d.found ? 1 : 0;
            shot->bandDone[band] = found != 0;
        }
        all = all && shot->bandDone[band];
    }
    if (!all) return;
    if (FILE* f = std::fopen(shot->path.c_str(), "wb")) {
        std::fprintf(f, "P6 %d %d 255\n", kShotW, kShotH);
        for (const u32 argb : shot->pixels) {
            const unsigned char rgb[3] = {static_cast<unsigned char>(argb >> 16), static_cast<unsigned char>(argb >> 8),
                                          static_cast<unsigned char>(argb)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
        RT_LOG(RT_TAG_GX) << "[shot] wrote " << shot->path << std::endl;
    }
    shot->done.store(true);
}
void ShotOnCopyDisp(uint64_t copyDispNumber) {
    BringupShot& shot = Shot();
    if (shot.path.empty()) return;
    BringupShot* p = &shot;
    if (shot.done.load()) {
        if (shot.every == 0 || copyDispNumber < shot.frame + shot.every) return;
        shot.frame += shot.every;
        shot.requested = false;
        const std::string next = shot.base + "_" + std::to_string(shot.frame) + ".ppm";
        auto reset = [p, next] {
            for (bool& b : p->bandDone) b = false;
            p->path = next;
            p->done.store(false);
        };
        if (GxWorker::ShouldDefer()) GxWorker::Submit(reset); else reset();
        return;
    }
    if (!shot.requested && copyDispNumber >= shot.frame) {
        shot.requested = true;
        if (GxWorker::ShouldDefer()) GxWorker::Submit([p] { ShotRequest(p); }); else ShotRequest(p);
    } else if (shot.requested) {
        if (GxWorker::ShouldDefer()) GxWorker::Submit([p] { ShotCollect(p); }); else ShotCollect(p);
    }
}
} // namespace

extern "C" void GX__CopyDisp_8016fc38(uint32_t da, uint32_t c) {
    const uint64_t copyDispNumber = g_bringupCopyDisps.fetch_add(1, std::memory_order_relaxed) + 1;
    EnsureAuroraFrameActive();
    ShotOnCopyDisp(copyDispNumber);
    if (GxWorker::ShouldDefer()) {
        // The copy is ordered with draws in Aurora's FIFO; frame end drains it.
        GxWorker::Submit([da, c] {
            GXCopyDisp(GuestToHostPtr(da), (GXBool)c);
        });
    } else {
        GXCopyDisp(GuestToHostPtr(da), (GXBool)c);
        // ImGui draw lists remain owned by the render worker until encoding completes.
        aurora_wait_for_frame_worker();
    }
    ++g_gxFrameCount;
    VI_HLE_SetXfbReady(da);
    settings_overlay::Draw();
    // Seal, pace to the VI retrace boundary (Aurora renders the sealed frame
    // during the wait), and pre-warm the next frame.
    VI_HLE_PresentFrame(/*presentedXfb=*/true, /*paceToRetrace=*/true);
}

PPC_NATIVE_OVERRIDE_VOID(8016fc38, GX__CopyDisp_8016fc38, (uint32_t da, uint32_t c), (da, c));


extern "C" void GX__CopyTex_8016fd74(uint32_t da, uint32_t c) {
    // Begin the frame before going inline so a deferred begin reaches the worker first.
    EnsureAuroraFrameActive();
    GX_SYNC_INLINE();
    // Flush dirty state without inserting a completion token before the ordered copy.
    GXFlush();
    const uint16_t rawSrcLeft = g_texCopyState.srcLeft;
    const uint16_t rawSrcTop = g_texCopyState.srcTop;
    const uint16_t rawSrcWidth = g_texCopyState.srcWidth;
    const uint16_t rawSrcHeight = g_texCopyState.srcHeight;

    // Keep the source in guest EFB coordinates. Aurora maps it to the scaled
    // EFB exactly once, matching Dolphin's ConvertEFBRectangle path.
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
    // EFB copies stay GPU-only except probe-sized ones (e.g. the 4x4 lens-flare depth probe),
    // which Aurora reads back asynchronously via efb_ram::schedule and land in guest RAM a frame
    // later. RISK: copies above the probe threshold, or on the offscreen list, are not
    // auto-downloaded, so guest reads see stale RAM; call aurora_flush_efb_copies_to_ram if a
    // copy needs reading back.
    GXCopyTex(GuestToHostPtr(da), (GXBool)c);
    RememberEfbCopyDestination(
        da, GXGetTexBufferSize(g_texCopyState.dstWidth, g_texCopyState.dstHeight,
                               g_texCopyState.dstFormat, GX_FALSE, 0));
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
}
PPC_NATIVE_OVERRIDE_VOID(8016fd74, GX__CopyTex_8016fd74, (uint32_t da, uint32_t c), (da, c));
