#include "common/Gamepad.hpp"

#include "core/Log.hpp"
#include "input/InputState.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/joystick.h>
#include <unistd.h>
#endif

namespace lumora::app {

void GamepadReader::applyDeadZone(float& x, float& y, float deadZone) {
    const float length = std::sqrt(x * x + y * y);
    if (length <= deadZone) {
        x = y = 0.0f;
        return;
    }
    // Rescale so the stick still reaches 1 at the edge and starts from 0 past the dead zone.
    const float scaled = std::min(1.0f, (length - deadZone) / (1.0f - deadZone));
    x = x / length * scaled;
    y = y / length * scaled;
}

namespace {
using Clock = std::chrono::steady_clock;
}

#if defined(__linux__)

struct GamepadReader::Impl {
    int fd = -1;
    std::string device;
    Clock::time_point nextScan{};
    float raw[8]{};  // joystick axes (xpad order): LX LY LT RX RY RT HatX HatY

    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    void scan(InputState& input) {
        nextScan = Clock::now() + std::chrono::seconds(2);
        for (int i = 0; i < 8 && fd < 0; ++i) {
            const std::string path = "/dev/input/js" + std::to_string(i);
            const int f = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (f < 0) continue;
            char name[128] = "gamepad";
            ioctl(f, JSIOCGNAME(sizeof(name)), name);
            fd = f;
            device = path;
            std::fill(std::begin(raw), std::end(raw), 0.0f);
            raw[2] = raw[5] = -1.0f;  // triggers rest at -1 in this interface
            input.setGamepadConnected(true);
            log::info("Input", "gamepad connected: {} ({})", name, path);
        }
    }
};

GamepadReader::GamepadReader() : impl_(std::make_unique<Impl>()) {}
GamepadReader::~GamepadReader() { impl_->close(); }

void GamepadReader::poll(InputState& input) {
    Impl& d = *impl_;
    if (d.fd < 0) {
        if (Clock::now() >= d.nextScan) d.scan(input);
        if (d.fd < 0) return;
    }
    static constexpr GamepadButton kButtons[] = {
        GamepadButton::A,         GamepadButton::B,           GamepadButton::X,     GamepadButton::Y,
        GamepadButton::LeftBumper, GamepadButton::RightBumper, GamepadButton::Back,  GamepadButton::Start,
        GamepadButton::Guide,     GamepadButton::LeftStick,   GamepadButton::RightStick};
    js_event event{};
    for (;;) {
        const ssize_t n = ::read(d.fd, &event, sizeof(event));
        if (n != static_cast<ssize_t>(sizeof(event))) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                log::info("Input", "gamepad disconnected ({})", d.device);
                d.close();
                input.setGamepadConnected(false);
                return;
            }
            break;
        }
        const auto type = static_cast<std::uint8_t>(event.type & ~JS_EVENT_INIT);
        if (type == JS_EVENT_BUTTON && event.number < std::size(kButtons)) {
            input.setGamepadButton(kButtons[event.number], event.value != 0);
        } else if (type == JS_EVENT_AXIS && event.number < 8) {
            d.raw[event.number] = static_cast<float>(event.value) / 32767.0f;
        }
    }
    float lx = d.raw[0], ly = d.raw[1], rx = d.raw[3], ry = d.raw[4];
    applyDeadZone(lx, ly);
    applyDeadZone(rx, ry);
    input.setGamepadAxis(GamepadAxis::LeftX, lx);
    input.setGamepadAxis(GamepadAxis::LeftY, ly);
    input.setGamepadAxis(GamepadAxis::RightX, rx);
    input.setGamepadAxis(GamepadAxis::RightY, ry);
    input.setGamepadAxis(GamepadAxis::LeftTrigger, std::clamp((d.raw[2] + 1.0f) * 0.5f, 0.0f, 1.0f));
    input.setGamepadAxis(GamepadAxis::RightTrigger, std::clamp((d.raw[5] + 1.0f) * 0.5f, 0.0f, 1.0f));
    input.setGamepadButton(GamepadButton::DPadLeft, d.raw[6] < -0.5f);
    input.setGamepadButton(GamepadButton::DPadRight, d.raw[6] > 0.5f);
    input.setGamepadButton(GamepadButton::DPadUp, d.raw[7] < -0.5f);
    input.setGamepadButton(GamepadButton::DPadDown, d.raw[7] > 0.5f);
}

#elif defined(_WIN32)

struct GamepadReader::Impl {
    using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    HMODULE library = nullptr;
    GetState getState = nullptr;
    int pad = -1;
    Clock::time_point nextScan{};
};

GamepadReader::GamepadReader() : impl_(std::make_unique<Impl>()) {
    for (const wchar_t* name : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"}) {
        impl_->library = LoadLibraryW(name);
        if (impl_->library != nullptr) break;
    }
    if (impl_->library != nullptr) {
        impl_->getState = reinterpret_cast<Impl::GetState>(GetProcAddress(impl_->library, "XInputGetState"));
    }
}

GamepadReader::~GamepadReader() {
    if (impl_->library != nullptr) FreeLibrary(impl_->library);
}

void GamepadReader::poll(InputState& input) {
    Impl& d = *impl_;
    if (d.getState == nullptr) return;
    XINPUT_STATE state{};
    if (d.pad < 0) {
        if (Clock::now() < d.nextScan) return;
        d.nextScan = Clock::now() + std::chrono::seconds(2);  // probing absent pads is slow
        for (DWORD i = 0; i < 4 && d.pad < 0; ++i) {
            if (d.getState(i, &state) == ERROR_SUCCESS) {
                d.pad = static_cast<int>(i);
                input.setGamepadConnected(true);
                log::info("Input", "gamepad connected (XInput {})", i);
            }
        }
        if (d.pad < 0) return;
    }
    if (d.getState(static_cast<DWORD>(d.pad), &state) != ERROR_SUCCESS) {
        log::info("Input", "gamepad disconnected");
        d.pad = -1;
        input.setGamepadConnected(false);
        return;
    }
    const XINPUT_GAMEPAD& g = state.Gamepad;
    const std::pair<WORD, GamepadButton> buttons[] = {
        {XINPUT_GAMEPAD_A, GamepadButton::A}, {XINPUT_GAMEPAD_B, GamepadButton::B},
        {XINPUT_GAMEPAD_X, GamepadButton::X}, {XINPUT_GAMEPAD_Y, GamepadButton::Y},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, GamepadButton::LeftBumper},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, GamepadButton::RightBumper}, {XINPUT_GAMEPAD_BACK, GamepadButton::Back},
        {XINPUT_GAMEPAD_START, GamepadButton::Start}, {XINPUT_GAMEPAD_LEFT_THUMB, GamepadButton::LeftStick},
        {XINPUT_GAMEPAD_RIGHT_THUMB, GamepadButton::RightStick}, {XINPUT_GAMEPAD_DPAD_UP, GamepadButton::DPadUp},
        {XINPUT_GAMEPAD_DPAD_DOWN, GamepadButton::DPadDown}, {XINPUT_GAMEPAD_DPAD_LEFT, GamepadButton::DPadLeft},
        {XINPUT_GAMEPAD_DPAD_RIGHT, GamepadButton::DPadRight}};
    for (const auto& [mask, button] : buttons) input.setGamepadButton(button, (g.wButtons & mask) != 0);
    // XInput's y points up; the engine's down, like the screen.
    float lx = g.sThumbLX / 32767.0f, ly = -g.sThumbLY / 32767.0f;
    float rx = g.sThumbRX / 32767.0f, ry = -g.sThumbRY / 32767.0f;
    applyDeadZone(lx, ly);
    applyDeadZone(rx, ry);
    input.setGamepadAxis(GamepadAxis::LeftX, std::clamp(lx, -1.0f, 1.0f));
    input.setGamepadAxis(GamepadAxis::LeftY, std::clamp(ly, -1.0f, 1.0f));
    input.setGamepadAxis(GamepadAxis::RightX, std::clamp(rx, -1.0f, 1.0f));
    input.setGamepadAxis(GamepadAxis::RightY, std::clamp(ry, -1.0f, 1.0f));
    input.setGamepadAxis(GamepadAxis::LeftTrigger, g.bLeftTrigger / 255.0f);
    input.setGamepadAxis(GamepadAxis::RightTrigger, g.bRightTrigger / 255.0f);
}

#else

struct GamepadReader::Impl {};
GamepadReader::GamepadReader() : impl_(std::make_unique<Impl>()) {}
GamepadReader::~GamepadReader() = default;
void GamepadReader::poll(InputState&) {}

#endif

} // namespace lumora::app
