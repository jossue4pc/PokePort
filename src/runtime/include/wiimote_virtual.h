#pragma once

// Virtual Wii Remote (wiimote_virtual.cpp): which GameCube-pad control (itself bound to keys or
// gamepad buttons by the existing controller settings) drives each Wii Remote button.

#include <array>
#include <cstdint>
#include <string>

namespace Wiimote::Virtual {

// kC/kZ: Nunchuk buttons (only used when the Nunchuk is attached, see NunchukAttached()).
enum Button : int { kA, kB, kOne, kTwo, kPlus, kMinus, kHome, kShake, kC, kZ, kButtonCount };
enum Source : int { kNone, kPadA, kPadB, kPadX, kPadY, kPadZ, kPadStart, kPadL, kPadR, kPadCStick, kSourceCount };

struct Mapping {
    std::array<int, kButtonCount> source{};
    bool upright = false; // false: held sideways (d-pad on the left), PokePark's default
};

const char* ButtonLabel(int button);
const char* SourceLabel(int source);
Mapping DefaultMapping();
// Current mapping (loaded from Config.toml on first use).
Mapping Get();
// Applies immediately and persists to Config.toml.
void Set(const Mapping& mapping);
// True when this game uses the virtual remote (every game except Mario Kart Wii, unless overridden).
bool Enabled();
// True when the virtual remote carries a Nunchuk (Rabbids Go Home; MKW_NUNCHUK=0|1 overrides).
// The PC stick then drives the Nunchuk stick and only the d-pad drives the remote's d-pad.
bool NunchukAttached();
// Extension encryption (the game writes 0xAA to 0xA400F0 and a key to 0xA40040): the game's WPAD derives
// two 8-byte tables from the key and decodes each byte as (e ^ sb[j]) + ft[j], j = (offset + i) % 8. A
// game-specific provider reads those tables from guest memory so the remote can encode with the inverse.
using ExtCipherProvider = bool (*)(uint8_t ft[8], uint8_t sb[8]);
void SetExtCipherProvider(ExtCipherProvider provider);
ExtCipherProvider CurrentExtCipherProvider();

} // namespace Wiimote::Virtual
