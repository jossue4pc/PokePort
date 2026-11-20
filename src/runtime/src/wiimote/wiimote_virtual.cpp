// Virtual Wii Remote for games that only take Wii Remotes (PokePark Wii).
// It speaks the remote's HID protocol to the emulated Bluetooth controller
// (status, EEPROM/register reads and writes, every data reporting mode) and is
// driven by the PC's controller or keyboard through aurora's PAD layer, held
// sideways by default. Physical remotes (wiimote_host.cpp) always take priority
// for their slot.
//
// Environment:
//   MKW_VIRTUAL_WIIMOTE=0|1   force off/on (default: on for non-MKWii games)
//   MKW_WIIMOTE_UPRIGHT=1     map the stick/d-pad without the sideways rotation
//   MKW_WIIMOTE_SCRIPT=t:btn:dur[,t:btn:dur...]  scripted presses for headless tests;
//        t/dur in seconds from the first data report, btn among A B 1 2 + - H U D L R S(shake)
//   MKW_WIIMOTE_LOG=1         log output reports from the game

#include "bluetooth/bt_controller.h"
#include "guest_abi/guest_region.h"
#include "input_bindings.h"
#include "runtime_config.h"
#include "wiimote_virtual.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Wiimote {
namespace {

using Clock = std::chrono::steady_clock;

// Core button bits as they appear in the two button bytes (big endian word).
constexpr uint16_t kLeft = 0x0100, kRight = 0x0200, kDown = 0x0400, kUp = 0x0800, kPlus = 0x1000;
constexpr uint16_t kTwo = 0x0001, kOne = 0x0002, kB = 0x0004, kA = 0x0008, kMinus = 0x0010, kHome = 0x0080;

constexpr uint8_t kAccelZero = 0x80;
constexpr uint8_t kAccelOneG = 0x9A;
constexpr size_t kEepromSize = 0x1700;

bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' && *v != '0';
}

struct ScriptPress {
    double start = 0;
    double duration = 0;
    uint16_t buttons = 0;
    bool shake = false;
};


// ---------------------------------------------------------------------------------------------
// Mapping (F10 > Controller settings > Wii Remote (virtual); Config.toml [controller])
// ---------------------------------------------------------------------------------------------
constexpr const char* kButtonKeys[Virtual::kButtonCount] = {"A", "B", "1", "2", "+", "-", "H", "S"};
constexpr const char* kButtonLabels[Virtual::kButtonCount] = {"A", "B", "1", "2", "+ (Plus)", "- (Minus)", "HOME", "Shake"};
constexpr const char* kSourceKeys[Virtual::kSourceCount] = {"None", "A", "B", "X", "Y", "Z", "Start", "L", "R", "CStick"};
constexpr const char* kSourceLabels[Virtual::kSourceCount] = {"None", "A", "B", "X", "Y", "Z", "Start", "L", "R", "C-stick (any direction)"};

std::mutex g_mappingMutex;
bool g_mappingLoaded = false;
Virtual::Mapping g_mapping;

Virtual::Mapping ParseMapping(const std::string& text, bool upright) {
    Virtual::Mapping mapping = Virtual::DefaultMapping();
    mapping.upright = upright;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(',', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string key = item.substr(0, eq);
            const std::string value = item.substr(eq + 1);
            for (int b = 0; b < Virtual::kButtonCount; ++b) {
                if (key != kButtonKeys[b]) continue;
                for (int src = 0; src < Virtual::kSourceCount; ++src) {
                    if (value == kSourceKeys[src]) mapping.source[b] = src;
                }
            }
        }
        pos = end + 1;
    }
    return mapping;
}

std::string FormatMapping(const Virtual::Mapping& mapping) {
    std::string text;
    for (int b = 0; b < Virtual::kButtonCount; ++b) {
        if (!text.empty()) text += ',';
        text += kButtonKeys[b];
        text += '=';
        text += kSourceKeys[mapping.source[b]];
    }
    return text;
}

Virtual::Mapping CurrentMapping() {
    std::lock_guard<std::mutex> lock(g_mappingMutex);
    if (!g_mappingLoaded) {
        g_mappingLoaded = true;
        g_mapping = ParseMapping(RuntimeConfigFile::VirtualWiimoteMap(),
                                 RuntimeConfigFile::VirtualWiimoteUpright(EnvFlag("MKW_WIIMOTE_UPRIGHT")));
    }
    return g_mapping;
}

bool SourceActive(int source, const PADStatus& pad) {
    const uint16_t b = pad.button;
    switch (source) {
    case Virtual::kPadA: return (b & PAD_BUTTON_A) != 0;
    case Virtual::kPadB: return (b & PAD_BUTTON_B) != 0;
    case Virtual::kPadX: return (b & PAD_BUTTON_X) != 0;
    case Virtual::kPadY: return (b & PAD_BUTTON_Y) != 0;
    case Virtual::kPadZ: return (b & PAD_TRIGGER_Z) != 0;
    case Virtual::kPadStart: return (b & PAD_BUTTON_START) != 0;
    case Virtual::kPadL: return (b & PAD_TRIGGER_L) != 0 || pad.triggerL > 200;
    case Virtual::kPadR: return (b & PAD_TRIGGER_R) != 0 || pad.triggerR > 200;
    case Virtual::kPadCStick:
        return pad.substickX > 64 || pad.substickX < -64 || pad.substickY > 64 || pad.substickY < -64;
    default: return false;
    }
}

constexpr uint16_t kButtonBits[Virtual::kButtonCount] = {0x0008, 0x0004, 0x0002, 0x0001, 0x1000, 0x0010, 0x0080, 0};

class VirtualRemote {
public:
    VirtualRemote() { Reset(); }

    void Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_eeprom.fill(0);
        // IR calibration (two copies) and accelerometer calibration (two copies), as Dolphin ships them.
        static const uint8_t kCal[] = {
            0xA1, 0xAA, 0x8B, 0x99, 0xAE, 0x9E, 0x78, 0x30, 0xA7, 0x74, 0xD3,
            0xA1, 0xAA, 0x8B, 0x99, 0xAE, 0x9E, 0x78, 0x30, 0xA7, 0x74, 0xD3,
            kAccelZero, kAccelZero, kAccelZero, 0, kAccelOneG, kAccelOneG, kAccelOneG, 0, 0, 0xA3,
            kAccelZero, kAccelZero, kAccelZero, 0, kAccelOneG, kAccelOneG, kAccelOneG, 0, 0, 0xA3,
        };
        std::memcpy(m_eeprom.data(), kCal, sizeof(kCal));
        m_speakerRegs.fill(0);
        m_irRegs.fill(0);
        m_pending.clear();
        m_mode = 0x30;
        m_continuous = false;
        m_leds = 0;
        m_irEnabled = false;
        m_speakerEnabled = false;
        m_lastButtons = 0xFFFF;
        m_haveDataReport = false;
    }

    bool PopInputReport(std::vector<uint8_t>& report) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pending.empty()) {
            report = std::move(m_pending.front());
            m_pending.pop_front();
            return true;
        }
        const auto now = Clock::now();
        if (m_haveDataReport && now - m_lastReport < std::chrono::milliseconds(5)) {
            return false;
        }
        Sample(now);
        const bool needsReport = m_continuous || m_mode != 0x30 || m_buttons != m_lastButtons ||
                                 !m_haveDataReport || now - m_lastReport > std::chrono::milliseconds(100);
        if (!needsReport) {
            return false;
        }
        BuildDataReport(report);
        m_lastButtons = m_buttons;
        m_lastReport = now;
        m_haveDataReport = true;
        return true;
    }

    void SendOutputReport(const uint8_t* data, size_t size) {
        if (size == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        static const bool s_log = EnvFlag("MKW_WIIMOTE_LOG");
        if (s_log) {
            std::string line = "[vwiimote] out";
            char hex[8];
            for (size_t i = 0; i < size && i < 22; ++i) {
                std::snprintf(hex, sizeof(hex), " %02x", data[i]);
                line += hex;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
        }
        const uint8_t id = data[0];
        const uint8_t flags = size > 1 ? data[1] : 0;
        const bool ack = (flags & 0x02) != 0;
        switch (id) {
        case 0x10: // rumble
            break;
        case 0x11: // LEDs
            m_leds = flags >> 4;
            if (ack) Ack(id, 0);
            break;
        case 0x12: // reporting mode
            if (size >= 3) {
                m_continuous = (flags & 0x04) != 0;
                m_mode = data[2];
                m_haveDataReport = false;
            }
            if (ack) Ack(id, 0);
            break;
        case 0x13: // IR pixel clock
        case 0x1A: // IR logic
            m_irEnabled = (flags & 0x04) != 0;
            if (ack) Ack(id, 0);
            break;
        case 0x14: // speaker enable
            m_speakerEnabled = (flags & 0x04) != 0;
            if (ack) Ack(id, 0);
            break;
        case 0x18: // speaker data
        case 0x19: // speaker mute
            if (ack) Ack(id, 0);
            break;
        case 0x15: // status request
            QueueStatus();
            break;
        case 0x16: // write memory
            if (size >= 7) WriteMemory(data, size);
            break;
        case 0x17: // read memory
            if (size >= 7) ReadMemory(data);
            break;
        default:
            break;
        }
    }

private:
    void Header(std::vector<uint8_t>& r, uint8_t id) const {
        r.clear();
        r.push_back(id);
        r.push_back(static_cast<uint8_t>(m_buttons >> 8));
        r.push_back(static_cast<uint8_t>(m_buttons & 0xFF));
    }

    void Ack(uint8_t report, uint8_t error) {
        std::vector<uint8_t> r;
        Header(r, 0x22);
        r.push_back(report);
        r.push_back(error);
        m_pending.push_back(std::move(r));
    }

    void QueueStatus() {
        std::vector<uint8_t> r;
        Header(r, 0x20);
        uint8_t lf = static_cast<uint8_t>(m_leds << 4);
        if (m_speakerEnabled) lf |= 0x04;
        if (m_irEnabled) lf |= 0x08;
        r.push_back(lf);
        r.push_back(0);
        r.push_back(0);
        r.push_back(0xC0); // battery level: full
        m_pending.push_back(std::move(r));
    }

    uint8_t* Region(bool registers, uint32_t address, size_t& limit) {
        if (!registers) {
            limit = kEepromSize;
            return m_eeprom.data();
        }
        switch ((address >> 16) & 0xFE) {
        case 0xA2: limit = m_speakerRegs.size(); return m_speakerRegs.data();
        case 0xB0: limit = m_irRegs.size(); return m_irRegs.data();
        default: limit = 0; return nullptr; // 0xA4: no extension attached
        }
    }

    void ReadMemory(const uint8_t* data) {
        const bool registers = (data[1] & 0x04) != 0;
        const uint32_t address = (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 8) | data[4];
        const uint32_t length = (static_cast<uint32_t>(data[5]) << 8) | data[6];
        size_t limit = 0;
        uint8_t* base = Region(registers, address, limit);
        const uint32_t offset = registers ? (address & 0xFF) : address;
        if (base == nullptr || length == 0 || offset + length > limit) {
            std::vector<uint8_t> r;
            Header(r, 0x21);
            r.push_back(static_cast<uint8_t>(0xF0 | (base == nullptr ? 7 : 8)));
            r.push_back(static_cast<uint8_t>(address >> 8));
            r.push_back(static_cast<uint8_t>(address));
            r.insert(r.end(), 16, 0);
            m_pending.push_back(std::move(r));
            return;
        }
        for (uint32_t done = 0; done < length; done += 16) {
            const uint32_t chunk = std::min<uint32_t>(16, length - done);
            std::vector<uint8_t> r;
            Header(r, 0x21);
            r.push_back(static_cast<uint8_t>((chunk - 1) << 4));
            const uint32_t a = address + done;
            r.push_back(static_cast<uint8_t>(a >> 8));
            r.push_back(static_cast<uint8_t>(a));
            for (uint32_t i = 0; i < 16; ++i) r.push_back(i < chunk ? base[offset + done + i] : 0);
            m_pending.push_back(std::move(r));
        }
    }

    void WriteMemory(const uint8_t* data, size_t size) {
        const bool registers = (data[1] & 0x04) != 0;
        const uint32_t address = (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 8) | data[4];
        const uint32_t length = std::min<uint32_t>(data[5], 16);
        size_t limit = 0;
        uint8_t* base = Region(registers, address, limit);
        const uint32_t offset = registers ? (address & 0xFF) : address;
        uint8_t error = 0;
        if (base == nullptr) {
            error = 7;
        } else if (offset + length > limit || 6 + length > size) {
            error = 8;
        } else {
            std::memcpy(base + offset, data + 6, length);
        }
        Ack(0x16, error);
    }

    static uint16_t ParseButton(const std::string& name, bool& shake) {
        if (name == "A") return kA;
        if (name == "B") return kB;
        if (name == "1") return kOne;
        if (name == "2") return kTwo;
        if (name == "+") return kPlus;
        if (name == "-") return kMinus;
        if (name == "H") return kHome;
        if (name == "U") return kUp;
        if (name == "D") return kDown;
        if (name == "L") return kLeft;
        if (name == "R") return kRight;
        if (name == "S") {
            shake = true;
            return 0;
        }
        return 0;
    }

    void LoadScript() {
        m_scriptLoaded = true;
        const char* text = std::getenv("MKW_WIIMOTE_SCRIPT");
        if (text == nullptr) return;
        const std::string all(text);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(',', pos);
            if (end == std::string::npos) end = all.size();
            const std::string item = all.substr(pos, end - pos);
            const size_t c1 = item.find(':');
            const size_t c2 = c1 == std::string::npos ? std::string::npos : item.find(':', c1 + 1);
            if (c1 != std::string::npos) {
                ScriptPress press;
                press.start = std::atof(item.substr(0, c1).c_str());
                press.duration = c2 == std::string::npos ? 0.15 : std::atof(item.substr(c2 + 1).c_str());
                const std::string name =
                    c2 == std::string::npos ? item.substr(c1 + 1) : item.substr(c1 + 1, c2 - c1 - 1);
                press.buttons = ParseButton(name, press.shake);
                m_script.push_back(press);
            }
            pos = end + 1;
        }
        std::fprintf(stderr, "[vwiimote] script with %zu presses\n", m_script.size());
    }

    void Sample(Clock::time_point now) {
        if (!m_scriptLoaded) {
            LoadScript();
            m_start = now;
        }
        static bool s_padInit = false;
        if (!s_padInit) {
            s_padInit = true;
            PADInit();
        }
        uint16_t buttons = 0;
        bool shake = false;
        PADStatus statuses[PAD_CHANMAX]{};
        PADRead(statuses);
        InputBindings::Apply(statuses);
        const PADStatus& pad = statuses[0];
        if (pad.err == PAD_ERR_NONE && !InputBindings::InputBlocked()) {
            const Virtual::Mapping mapping = CurrentMapping();
            const uint16_t b = pad.button;
            const bool up = (b & PAD_BUTTON_UP) || pad.stickY > 48;
            const bool down = (b & PAD_BUTTON_DOWN) || pad.stickY < -48;
            const bool left = (b & PAD_BUTTON_LEFT) || pad.stickX < -48;
            const bool right = (b & PAD_BUTTON_RIGHT) || pad.stickX > 48;
            if (mapping.upright) {
                if (up) buttons |= kUp;
                if (down) buttons |= kDown;
                if (left) buttons |= kLeft;
                if (right) buttons |= kRight;
            } else {
                // Held sideways (d-pad on the left): screen up is the remote's Right.
                if (up) buttons |= kRight;
                if (down) buttons |= kLeft;
                if (left) buttons |= kUp;
                if (right) buttons |= kDown;
            }
            for (int button = 0; button < Virtual::kButtonCount; ++button) {
                if (!SourceActive(mapping.source[button], pad)) continue;
                if (button == Virtual::kShake) {
                    shake = true;
                } else {
                    buttons |= kButtonBits[button];
                }
            }
        }
        const double t = std::chrono::duration<double>(now - m_start).count();
        for (const ScriptPress& press : m_script) {
            if (t >= press.start && t < press.start + press.duration) {
                buttons |= press.buttons;
                shake = shake || press.shake;
            }
        }
        m_buttons = buttons;
        m_shake = shake;
        ++m_sampleCount;
    }

    void BuildDataReport(std::vector<uint8_t>& r) {
        uint8_t mode = m_mode;
        switch (mode) {
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x3D: break;
        default: mode = 0x30; break;
        }
        Header(r, mode);
        uint8_t ax = kAccelZero, ay = kAccelZero, az = kAccelOneG;
        if (m_shake) {
            const bool phase = ((m_sampleCount / 3) & 1) != 0;
            ax = phase ? 0xE0 : 0x20;
            ay = phase ? 0xD0 : 0x30;
            az = phase ? 0xF0 : 0x40;
        }
        auto accel = [&] {
            r.push_back(ax);
            r.push_back(ay);
            r.push_back(az);
        };
        auto ir = [&](size_t n) { r.insert(r.end(), n, 0xFF); };
        auto ext = [&](size_t n) { r.insert(r.end(), n, 0x00); };
        switch (mode) {
        case 0x31: accel(); break;
        case 0x32: ext(8); break;
        case 0x33: accel(); ir(12); break;
        case 0x34: ext(19); break;
        case 0x35: accel(); ext(16); break;
        case 0x36: ir(10); ext(9); break;
        case 0x37: accel(); ir(10); ext(6); break;
        case 0x3D: r.resize(1); ext(21); break;
        default: break;
        }
    }

    std::mutex m_mutex;
    std::array<uint8_t, kEepromSize> m_eeprom{};
    std::array<uint8_t, 256> m_speakerRegs{};
    std::array<uint8_t, 256> m_irRegs{};
    std::deque<std::vector<uint8_t>> m_pending;
    uint8_t m_mode = 0x30;
    bool m_continuous = false;
    uint8_t m_leds = 0;
    bool m_irEnabled = false;
    bool m_speakerEnabled = false;
    uint16_t m_buttons = 0;
    uint16_t m_lastButtons = 0xFFFF;
    bool m_shake = false;
    bool m_upright = false;
    uint64_t m_sampleCount = 0;
    bool m_haveDataReport = false;
    Clock::time_point m_lastReport{};
    Clock::time_point m_start{};
    bool m_scriptLoaded = false;
    std::vector<ScriptPress> m_script;
};

class CompositeHost final : public Bluetooth::RemoteHost {
public:
    explicit CompositeHost(Bluetooth::RemoteHost& real) : m_real(real) {
        const char* v = std::getenv("MKW_VIRTUAL_WIIMOTE");
        if (v != nullptr && *v != '\0') {
            m_enabled = *v != '0';
        } else {
            m_enabled = !::GuestRegion::IsMarioKart();
        }
        if (m_enabled) std::fprintf(stderr, "[vwiimote] virtual Wii Remote on slot 1 (PC controller/keyboard)\n");
    }

    bool IsAttached(size_t slot) const override {
        if (Virtual(slot)) return true;
        return m_real.IsAttached(slot);
    }
    bool ConsumeWake(size_t slot) override {
        if (Virtual(slot)) {
            const bool wake = m_wake;
            m_wake = false;
            return wake;
        }
        return m_real.ConsumeWake(slot);
    }
    bool PopInputReport(size_t slot, std::vector<uint8_t>& report) override {
        if (Virtual(slot)) return m_remote.PopInputReport(report);
        return m_real.PopInputReport(slot, report);
    }
    void SendOutputReport(size_t slot, const uint8_t* report, size_t size) override {
        if (Virtual(slot)) {
            m_remote.SendOutputReport(report, size);
            return;
        }
        m_real.SendOutputReport(slot, report, size);
    }
    void OnLinkStateChanged(size_t slot, Bluetooth::LinkState state) override {
        if (Virtual(slot)) {
            if (state == Bluetooth::LinkState::Ready) {
                std::fprintf(stderr, "[vwiimote] linked to the game\n");
                m_remote.Reset();
            }
            return;
        }
        m_real.OnLinkStateChanged(slot, state);
    }
    void OnGuestDisconnect(size_t slot) override {
        if (Virtual(slot)) {
            std::fprintf(stderr, "[vwiimote] the game disconnected the remote; offering it again\n");
            m_wake = true;
            return;
        }
        m_real.OnGuestDisconnect(slot);
    }
    void OnGuestInquiry(bool active) override { m_real.OnGuestInquiry(active); }
    const char* DeviceName(size_t slot) const override {
        if (Virtual(slot)) return "Nintendo RVL-CNT-01";
        return m_real.DeviceName(slot);
    }

private:
    bool Virtual(size_t slot) const { return m_enabled && slot == 0 && !m_real.IsAttached(0); }

    Bluetooth::RemoteHost& m_real;
    VirtualRemote m_remote;
    bool m_enabled = false;
    mutable bool m_wake = true;
};

} // namespace

namespace Virtual {
const char* ButtonLabel(int button) { return button >= 0 && button < kButtonCount ? kButtonLabels[button] : "?"; }
const char* SourceLabel(int source) { return source >= 0 && source < kSourceCount ? kSourceLabels[source] : "?"; }
Mapping DefaultMapping() {
    Mapping mapping;
    // Sideways remote on a PC controller: face buttons A/B -> 2/1 (PokePark's main actions).
    mapping.source = {kPadX, kPadY, kPadB, kPadA, kPadStart, kPadZ, kPadL, kPadR};
    return mapping;
}
Mapping Get() { return CurrentMapping(); }
void Set(const Mapping& mapping) {
    {
        std::lock_guard<std::mutex> lock(g_mappingMutex);
        g_mapping = mapping;
        g_mappingLoaded = true;
    }
    RuntimeConfigFile::SetVirtualWiimoteMap(FormatMapping(mapping));
    RuntimeConfigFile::SetVirtualWiimoteUpright(mapping.upright);
}
bool Enabled() {
    const char* v = std::getenv("MKW_VIRTUAL_WIIMOTE");
    if (v != nullptr && *v != '\0') return *v != '0';
    return !::GuestRegion::IsMarioKart();
}
} // namespace Virtual

Bluetooth::RemoteHost& WrapWithVirtualRemote(Bluetooth::RemoteHost& real) {
    static CompositeHost* host = new CompositeHost(real);
    return *host;
}

} // namespace Wiimote
