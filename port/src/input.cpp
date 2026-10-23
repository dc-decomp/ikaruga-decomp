// The controller and its units, natively: Shinobi's peripheral library at the functions the
// game calls (pd*). Shinobi read each port's condition from the Maple device layer at the
// vertical blank and converted it into the records that pdGetPeripheral returns. The server here,
// called at the same point, converts the controller state that the launcher keeps (the window's
// controls, a recording or the autoplay) instead, as Shinobi did: the game copies the whole record
// each frame. The setup is fixed: a controller in port A and a memory card with a screen in its
// first slot, as the save directory stands for (saves.cpp). The device layer is never started, so
// no Maple frame goes out.
#include <cstddef>

#include "decomp.h"
#include "hw.h"

#include "dream/runtime/maple/maple.h"

namespace game {
namespace {

// Shinobi's state for the 24 ports (4 connectors of 6 units), in its own memory.
constexpr uint32_t kRecords = 0x8C10EE80u;  // PDS_PERIPHERAL, 0x34 bytes each
constexpr uint32_t kOptimize = 0x8C10F360u;  // pdSetOptimize: buttons stay active low
constexpr uint32_t kThresholdOn = 0x8C10F364u, kThresholdOff = 0x8C10F365u;  // pdSetThreshold
constexpr uint32_t kLeftDown = 0x8C10F366u, kRightDown = 0x8C10F37Eu;  // per port: trigger as a button
constexpr uint32_t kNoDevice = 0x8C0CAE74u;  // "(no device)"
constexpr uint32_t kServerOn = 0x8C10EE74u, kDeviceLayer = 0x8C10EE78u, kServerFlag = 0x8C10EE7Cu;
constexpr uint32_t kCard = 1;  // port A's first slot

// A standard controller: the buttons, both triggers and one stick.
constexpr uint32_t kController = 0x000F06FEu;
constexpr uint32_t kRight = 0x10000u, kLeft = 0x20000u;

void clear(uint32_t r, bool optimize) {
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, id), 0);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, support), 0);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, on), optimize ? ~0u : 0u);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, off), optimize ? 0u : ~0u);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, press), optimize ? ~0u : 0u);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, release), optimize ? ~0u : 0u);
    for (uint32_t o = offsetof(PDS_PERIPHERAL, r); o < offsetof(PDS_PERIPHERAL, name); o += 2)
        guest::store<uint16_t>(r + o, 0);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, extend), 0);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, name), kNoDevice);
}

// A trigger pressed past the upper threshold counts as a button until it drops below the lower one.
bool trigger_down(uint8_t value, uint32_t state) {
    const uint8_t on = guest::load<uint8_t>(kThresholdOn), off = guest::load<uint8_t>(kThresholdOff);
    bool down = guest::load<uint8_t>(state) != 0;
    if (value >= on)
        down = true;
    else if (value <= off)
        down = false;
    guest::store<uint8_t>(state, down ? 1 : 0);
    return down;
}

void controller(uint32_t r, uint32_t port, const dream::maple::ControllerState& pad, bool optimize) {
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, id), kController);
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, support), kController);
    // The bus carries the buttons active low; the triggers add bits 16 and 17.
    uint32_t bits = pad.buttons | 0xFFFF0000u;
    if (trigger_down(pad.ltrigger, kLeftDown + port))
        bits &= ~kLeft;
    if (trigger_down(pad.rtrigger, kRightDown + port))
        bits &= ~kRight;
    guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, on), optimize ? bits : ~bits);
    guest::store<uint16_t>(r + offsetof(PDS_PERIPHERAL, r), pad.rtrigger);
    guest::store<uint16_t>(r + offsetof(PDS_PERIPHERAL, l), pad.ltrigger);
    guest::store<int16_t>(r + offsetof(PDS_PERIPHERAL, x1), static_cast<int16_t>(pad.joy_x - 0x80));
    guest::store<int16_t>(r + offsetof(PDS_PERIPHERAL, y1), static_cast<int16_t>(pad.joy_y - 0x80));
    guest::store<int16_t>(r + offsetof(PDS_PERIPHERAL, x2), 0);
    guest::store<int16_t>(r + offsetof(PDS_PERIPHERAL, y2), 0);
}

}  // namespace

// No code reads a record's name or device information, so each record keeps the "(no device)" name
// that Shinobi gives an empty port, and no information.
void pdInitPeripheral(Sint32 logic, gptr<void>, gptr<void>) {
    guest::store<uint32_t>(kDeviceLayer, 0);
    guest::store<uint32_t>(kServerOn, 1);
    guest::store<uint32_t>(kServerFlag, 0);
    guest::store<uint32_t>(kOptimize, static_cast<uint32_t>(logic));
    for (uint32_t port = 0; port < 24; ++port) {
        const uint32_t r = kRecords + port * sizeof(PDS_PERIPHERAL);
        clear(r, logic != 0);
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, old), 0);
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, info), 0);
        guest::store<uint8_t>(kLeftDown + port, 0);
        guest::store<uint8_t>(kRightDown + port, 0);
    }
    guest::store<uint8_t>(kThresholdOn, 0xC0);
    guest::store<uint8_t>(kThresholdOff, 0xA0);
}

void pdExitPeripheral() {
    const bool optimize = guest::load<uint32_t>(kOptimize) != 0;
    for (uint32_t port = 0; port < 24; ++port) {
        const uint32_t r = kRecords + port * sizeof(PDS_PERIPHERAL);
        clear(r, optimize);
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, old), 0);
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, info), 0);
    }
}

void pdExecPeripheralServerUser() {
    const bool optimize = guest::load<uint32_t>(kOptimize) != 0;
    for (uint32_t port = 0; port < 24; port += 6) {
        const uint32_t r = kRecords + port * sizeof(PDS_PERIPHERAL);
        const uint32_t old = guest::load<uint32_t>(r + offsetof(PDS_PERIPHERAL, on));
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, old), old);
        if (port == 0)
            controller(r, port, hw::pad(), optimize);
        else
            clear(r, optimize);
        const uint32_t on = guest::load<uint32_t>(r + offsetof(PDS_PERIPHERAL, on));
        const uint32_t off = ~on;
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, off), off);
        const uint32_t changed = on ^ old;
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, press), optimize ? ~changed | on : changed & on);
        guest::store<uint32_t>(r + offsetof(PDS_PERIPHERAL, release), optimize ? ~changed | off : changed & off);
    }
}

// The memory card's screen: 48 by 32 dots, which the game draws its logo on. Nothing shows it.
Sint32 pdVmsLcdIsReady(Uint32 port) { return port == kCard ? 1 : 0; }
Sint32 pdVmsLcdWrite1(Uint32 port, gptr<void>) { return port == kCard ? 0 : -2; }
// The card's screen is the right way up when the controller is, which Shinobi gives as 1.
Sint32 pdLcdGetDirection(Uint32) { return 1; }

// No vibration unit: the game's calls return "not ready" without a request.
Sint32 pdVibIsReady(Uint32) { return 0; }

}  // namespace game
