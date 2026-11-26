#include "guest_abi/guest_region.h"

#include <cstdio>
#include <cstdlib>

namespace GuestRegion {

const Facts kFacts[kCount] = {
    {"RMCP01", 'P', 0x524D4350u, 1u, 0x80399180u, "EUR", "RVL-001(EUR)", "LEH", "PAL", "EU", 0x800060A4u, 0x00240E18u, 0u, true, 0u, 0u, 0u, 0u},
    {"RMCE01", 'E', 0x524D4345u, 0u, 0x80394E00u, "USA", "RVL-001(USA)", "LU", "NTSC", "US", 0x800060A4u, 0x00240E18u, 0u, true, 0u, 0u, 0u, 0u},
    {"RMCJ01", 'J', 0x524D434Au, 0u, 0x80398B00u, "JPN", "RVL-001(JPN)", "LJ", "NTSC", "JP", 0x800060A4u, 0x00240E18u, 0u, true, 0u, 0u, 0u, 0u},
    // PokéPark: MEM1 arena lo = OSInit's align32(0x8049A040) (stack top from __init_registers).
    {"R8AE01", 'E', 0x52384145u, 0u, 0x8049A040u, "USA", "RVL-001(USA)", "LU", "NTSC", "US", 0x80004050u, 0x0038161Du, 0x803ED4A0u, true, 0u, 0u, 0u, 0u},
    // Rabbids Go Home: arena lo = __ArenaLo (ELF symbol); TMD asks for IOS55 (latest revision v5663).
    // MEM2 arena hi = 0x933E0000 as on a real console: its allocator breaks with a 118 MB MEM2 heap. The
    // module tables sit right above it, in what is IOS memory on hardware.
    {"RGWE41", 'E', 0x52475745u, 0u, 0x806D7EE0u, "USA", "RVL-001(USA)", "LU", "NTSC", "US", 0x80005F90u, 0x0037161Fu, 0x80606EA0u, false,
     0x81490000u, 0x817EC760u, 0x933E0000u, 0x00680000u},
};

const Facts& CurrentFacts() noexcept { return kFacts[static_cast<size_t>(Current())]; }

void FatalUnknownAddress(uint32_t pal) noexcept {
    std::fprintf(stderr, "Guest region: PAL address 0x%08X has no %s address in guest_versions.inc\n",
                 pal, CurrentFacts().gameId);
    std::fflush(stderr);
    std::abort();
}

} // namespace GuestRegion
