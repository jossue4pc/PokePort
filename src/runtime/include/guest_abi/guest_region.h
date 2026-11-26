#pragma once

// Guest addresses are named by their PAL (RMCP01) address. Which address that identity has
// depends on the region the game was translated from, which runtime/game supplies at link
// time, so one compiled runtime serves every region.
//
//   MKW_GADDR(801A9E84)       address of that identity in the running region
//   MKW_GADDR_CT(801A9E84)    same, as a constant expression; translated code only
//
// guest_versions.inc maps PAL ranges to the other regions. A PAL address outside every range
// of a region does not compile.

#include <cstddef>
#include <cstdint>

namespace GuestRegion {

enum class Id : uint8_t {
    Pal = 0,
    NtscU = 1,
    NtscJ = 2,
    // PokéPark Wii (R8AE01): another game, not a MKWii region. Its column maps the PAL
    // identities the runtime names to PokéPark addresses. An identity absent from that DOL gets a
    // unique, unmapped sentinel 0xF0xxxxxx (never a guest address), so constant-keyed templates stay distinct.
    PokeParkU = 3,
    // Rabbids Go Home (RGWE41): same idea, column generated from LynWiiRetail.elf's symbols.
    RabbidsU = 4,
};
inline constexpr size_t kCount = 5;
// Columns >= kFirstOtherGame are other games, not MKWii regions.
inline constexpr size_t kFirstOtherGame = 3;

struct Row {
    uint32_t address[kCount];

    constexpr uint32_t In(Id region) const { return address[static_cast<size_t>(region)]; }
};

struct Range {
    Id region;
    uint32_t first;
    uint32_t last;
    int32_t delta;
};

inline constexpr Range kRanges[] = {
#define MKW_REGION_RANGE(region, first, last, delta) {Id::region, 0x##first##u, 0x##last##u, delta},
#include "guest_abi/guest_versions.inc"
#undef MKW_REGION_RANGE
};
inline constexpr size_t kRangeCount = sizeof(kRanges) / sizeof(kRanges[0]);

constexpr bool Precedes(Id region, uint32_t address, const Range& range) {
    return region < range.region || (region == range.region && address < range.first);
}

constexpr bool RangesAreOrdered() {
    for (size_t i = 0; i < kRangeCount; ++i) {
        const Range& range = kRanges[i];
        if (range.region == Id::Pal || range.first > range.last) {
            return false;
        }
        if (i != 0 && !Precedes(kRanges[i - 1].region, kRanges[i - 1].last, range)) {
            return false;
        }
    }
    return true;
}
static_assert(RangesAreOrdered(), "guest_versions.inc ranges must be sorted by region and PAL address without overlaps");

struct Mapping {
    bool found;
    uint32_t address;
};

constexpr Mapping Map(uint32_t pal, Id region) {
    if (region == Id::Pal) {
        return {true, pal};
    }
    size_t lo = 0;
    size_t hi = kRangeCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (Precedes(region, pal, kRanges[mid])) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (lo == 0) {
        return {false, 0};
    }
    const Range& range = kRanges[lo - 1];
    if (range.region != region || pal > range.last) {
        return {false, 0};
    }
    return {true, static_cast<uint32_t>(static_cast<int64_t>(pal) + range.delta)};
}

constexpr Row RowOf(uint32_t pal) {
    const Mapping ntscU = Map(pal, Id::NtscU);
    const Mapping ntscJ = Map(pal, Id::NtscJ);
    if (!ntscU.found || !ntscJ.found) {
        throw "PAL address outside guest_abi/guest_versions.inc";
    }
    Row row{{pal, ntscU.address, ntscJ.address}};
    for (size_t game = kFirstOtherGame; game < kCount; ++game) {
        const Mapping other = Map(pal, static_cast<Id>(game));
        row.address[game] = other.found ? other.address : (0xF0000000u | (pal & 0x00FFFFFFu));
    }
    return row;
}

constexpr bool SameInEveryColumn(const Row& row, uint32_t pal) {
    for (size_t i = 1; i < kCount; ++i) {
        if (row.address[i] != pal) {
            return false;
        }
    }
    return true;
}

constexpr bool InAnyColumn(const Row& row, uint32_t target) {
    for (size_t i = 0; i < kCount; ++i) {
        if (row.address[i] == target) {
            return true;
        }
    }
    return false;
}

} // namespace GuestRegion

// Defined by runtime/game for the translated region.
extern "C" const uint8_t kMkwGuestRegion;

namespace GuestRegion {

inline Id Current() noexcept { return static_cast<Id>(kMkwGuestRegion); }

// False when the translated game is not Mario Kart Wii: MKWii-only modules stand down.
inline bool IsMarioKart() noexcept { return static_cast<size_t>(Current()) < kFirstOtherGame; }
inline bool IsPokePark() noexcept { return Current() == Id::PokeParkU; }
inline bool IsRabbids() noexcept { return Current() == Id::RabbidsU; }

template <uint32_t Pal>
inline uint32_t Address() noexcept {
    constexpr Row row = RowOf(Pal);
    if constexpr (SameInEveryColumn(row, Pal)) {
        return Pal;
    } else {
        return row.In(Current());
    }
}

// Folds to false at compile time for a constant target that no region uses for Pal.
template <uint32_t Pal>
inline bool Is(uint32_t target) noexcept {
    constexpr Row row = RowOf(Pal);
    return InAnyColumn(row, target) && target == row.In(Current());
}

[[noreturn]] void FatalUnknownAddress(uint32_t pal) noexcept;

// For hand-written registrations, which name the PAL address.
inline uint32_t Translate(uint32_t pal) noexcept {
    const Mapping mapping = Map(pal, Current());
    if (!mapping.found) {
        // PokéPark: a native whose SDK function is not in this DOL is simply not registered
        // (TranslatedFunctionRegistry::Register ignores address 0).
        // 0x8FFFxxxx are synthetic identities for PokéPark-only natives (no MKWii counterpart).
        if (!IsMarioKart() || pal >= 0x8FFF0000u) {
            return 0;
        }
        FatalUnknownAddress(pal);
    }
    return mapping.address;
}

struct Facts {
    const char* gameId;
    char letter;
    uint32_t gameCode;
    uint32_t viTvFormat;
    uint32_t mem1ArenaLo;
    // setting.txt defaults for a new NAND
    const char* area;
    const char* model;
    const char* productCode;
    const char* video;
    const char* gameRegion;
    // Translated DOL entry point (__start) the runtime boots.
    uint32_t entryAddress;
    // IOS the title runs on: (IOS number << 16) | revision (IOS36 v3608, IOS56 v5661, IOS55 v5663).
    uint32_t iosVersion;
    // DVD low-level "dummy" command block the HLE marks idle at boot (0 = none).
    uint32_t dvdDummyCommandBlock;
    // Run the DOL's .ctors before booting (MKWii/PokéPark). False when they need the game's own
    // heaps (Rabbids' allocate through MEM_g_Manager): its __start -> __init_cpp runs them itself.
    bool preRunDolCtors;
    // Fixed home of a run-time module the port pre-translates (Rabbids' Ai2CppWiiFinal.rso, see
    // hle/game/rabbids_rso.cpp). MEM1 arena hi is capped at rsoModuleBase and MEM2 arena hi at
    // rsoScratchBase, so the game's heaps never overlap them. 0 = none.
    uint32_t rsoModuleBase;
    uint32_t rsoBssBase;
    uint32_t rsoScratchBase;
    uint32_t rsoScratchSize;
};

// One row per GuestRegion::Id, in guest_region.cpp (kept out of this widely included header so a
// per-game value change does not rebuild every translated shard).
extern const Facts kFacts[kCount];

const Facts& CurrentFacts() noexcept;

} // namespace GuestRegion

#define MKW_GADDR(pal) (::GuestRegion::Address<0x##pal##u>())

#if defined(MKW_TRANSLATED_GUEST_REGION)
#define MKW_GADDR_CT(pal) \
    (::GuestRegion::RowOf(0x##pal##u).In(static_cast<::GuestRegion::Id>(MKW_TRANSLATED_GUEST_REGION)))
#endif
