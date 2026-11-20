#pragma once

// Virtual Wii Remote (wiimote_virtual.cpp): which GameCube-pad control (itself bound to keys or
// gamepad buttons by the existing controller settings) drives each Wii Remote button.

#include <array>
#include <string>

namespace Wiimote::Virtual {

enum Button : int { kA, kB, kOne, kTwo, kPlus, kMinus, kHome, kShake, kButtonCount };
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

} // namespace Wiimote::Virtual
