#pragma once

#include "common/ModuleExport.hpp"

#include <memory>

namespace lumora {
class InputState;
}

namespace lumora::app {

/// Reads the first game controller into an InputState, polled once per frame: the Linux
/// joystick interface (/dev/input/js*, Xbox-style button order as the xpad driver reports it)
/// or XInput on Windows. Pads plugged in later are found (checked every two seconds).
class LUMORA_APPCOMMON_API GamepadReader {
public:
    GamepadReader();
    ~GamepadReader();
    GamepadReader(const GamepadReader&) = delete;
    GamepadReader& operator=(const GamepadReader&) = delete;

    void poll(InputState& input);

    /// Stick values -1..1 from raw ones with a radial dead zone (exposed for tests).
    static void applyDeadZone(float& x, float& y, float deadZone = 0.15f);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lumora::app
