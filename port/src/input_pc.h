#pragma once

#include "config.h"
#include "cpu_dynarmic.h"
#include "elf_loader.h"
#include "jni_env.h"

#include <SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Verified ControllerManager.java ordinals (Phase 1 decompilation).
enum class Rr3Axis : int {
    LThumbX = 0,
    LThumbY = 1,
    RThumbX = 2,
    RThumbY = 3,
    LTrigger = 4,
    RTrigger = 5,
    MotionX = 6,
    MotionY = 7,
    Count = 8,
};

enum class Rr3Button : int {
    A = 0,
    B = 1,
    X = 2,
    Y = 3,
    L1 = 4,
    R1 = 5,
    L2 = 6,
    R2 = 7,
    DpadLeft = 8,
    DpadRight = 9,
    DpadUp = 10,
    DpadDown = 11,
    Select = 12,
    Start = 13,
    DpadCenter = 14,
    MediaPlayPause = 15,
    LThumbLeft = 16,
    LThumbRight = 17,
    LThumbUp = 18,
    LThumbDown = 19,
    LThumbPress = 20,
    RThumbLeft = 21,
    RThumbRight = 22,
    RThumbUp = 23,
    RThumbDown = 24,
    RThumbPress = 25,
    Count = 26,
};

struct KeyBindings {
    SDL_Keycode throttle = SDLK_w;
    SDL_Keycode throttle_alt = SDLK_UP;
    SDL_Keycode brake = SDLK_s;
    SDL_Keycode brake_alt = SDLK_DOWN;
    SDL_Keycode steer_left = SDLK_a;
    SDL_Keycode steer_left_alt = SDLK_LEFT;
    SDL_Keycode steer_right = SDLK_d;
    SDL_Keycode steer_right_alt = SDLK_RIGHT;
    SDL_Keycode handbrake = SDLK_SPACE; // Secondary Brake (S / Space / Down)
    SDL_Keycode camera = SDLK_c;
    SDL_Keycode rear_view = SDLK_v;
    SDL_Keycode confirm = SDLK_RETURN;
    SDL_Keycode back = SDLK_ESCAPE;
    SDL_Keycode pause = SDLK_ESCAPE;
    SDL_Keycode pause_alt = SDLK_p;
    SDL_Keycode select_btn = SDLK_TAB;
};

class PcInput {
public:
    static constexpr int32_t kKeyboardDeviceId = 1000;
    static constexpr int32_t kFullControllerFlags = 0x3FF;

    PcInput();
    ~PcInput();

    void Init(const PcConfig& cfg);
    void Shutdown();

    void SetBindings(const KeyBindings& bindings) { keys_ = bindings; }
    const KeyBindings& Bindings() const { return keys_; }

    // Process SDL keyboard/controller events (call from SDL_PollEvent loop).
    void ProcessEvent(const SDL_Event& e);

    // Direct key injection (used by automated tests or synthetic input).
    void SetKeyState(SDL_Keycode key, bool down);
    // Clears all held keyboard/gamepad buttons & axes (e.g. on Alt-Tab / focus loss).
    void ResetAllInputs();

    // Advances smooth keyboard analog steering ramp by dt_sec.
    void UpdateFrame(float dt_sec);

    // Dispatches controller connect/disconnect, axis, button, and Back key events to guest libRealRacing3.so.
    void DispatchToGuest(CpuRuntime& cpu, ElfLoader& loader, FakeJvm& jvm, uint64_t main_activity_handle);

    // Telemetry / unit test helpers.
    float KeyboardSteer() const { return kb_steer_; }
    float KeyboardThrottle() const { return kb_throttle_; }
    float KeyboardBrake() const { return kb_brake_; }
    size_t ConnectedGamepadCount() const { return pads_.size(); }
    std::vector<std::string> ConnectedGamepadNames() const;

    static std::string DetectControllerFamily(const std::string& name, SDL_GameControllerType type);

private:
    struct PadState {
        SDL_GameController* gc = nullptr;
        SDL_JoystickID instance_id = -1;
        int32_t device_id = 0;
        std::string name;
        std::string guid;
        std::string family;
        bool announced = false;
        std::array<float, static_cast<size_t>(Rr3Axis::Count)> axes{};
        std::array<float, static_cast<size_t>(Rr3Axis::Count)> prev_axes{};
        std::array<bool, static_cast<size_t>(Rr3Button::Count)> buttons{};
        std::array<bool, static_cast<size_t>(Rr3Button::Count)> prev_buttons{};
    };

    void OpenGamepad(int joystick_index);
    void CloseGamepad(SDL_JoystickID instance_id);
    static float ApplyDeadzone(float v, float dz = 0.12f);

    PcConfig cfg_{};
    KeyBindings keys_{};
    bool initialized_ = false;
    bool kb_announced_ = false;
    int32_t next_pad_dev_id_ = 2000;

    // Keyboard state (track primary and alt keys separately so releasing one never cancels the other)
    bool key_left_pri_ = false;
    bool key_left_alt_ = false;
    bool key_right_pri_ = false;
    bool key_right_alt_ = false;
    bool key_throttle_pri_ = false;
    bool key_throttle_alt_ = false;
    bool key_brake_pri_ = false;
    bool key_brake_alt_ = false;
    bool key_brake_space_ = false;
    bool key_pause_pri_ = false;
    bool key_pause_alt_ = false;
    float kb_steer_ = 0.0f;
    float kb_throttle_ = 0.0f;
    float kb_brake_ = 0.0f;
    std::array<float, static_cast<size_t>(Rr3Axis::Count)> kb_prev_axes{};
    std::array<bool, static_cast<size_t>(Rr3Button::Count)> kb_buttons{};
    std::array<bool, static_cast<size_t>(Rr3Button::Count)> kb_prev_buttons{};

    // Queued Android keycodes (e.g. Back = 4) and disconnected pad IDs
    std::vector<std::pair<int32_t, bool>> pending_android_keys_;
    std::vector<int32_t> pending_disconnects_;

    std::unordered_map<SDL_JoystickID, PadState> pads_;
};
