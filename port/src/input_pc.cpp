#include "input_pc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

PcInput::PcInput() = default;

PcInput::~PcInput() {
    Shutdown();
}

void PcInput::Init(const PcConfig& cfg) {
    cfg_ = cfg;
    initialized_ = true;
    // Open any already-attached SDL GameControllers (Xbox/XInput, DS4, DualSense, Switch Pro, virtual).
    int num_joys = SDL_NumJoysticks();
    for (int i = 0; i < num_joys; ++i) {
        if (SDL_IsGameController(i)) {
            OpenGamepad(i);
        }
    }
}

void PcInput::Shutdown() {
    for (auto& [id, pad] : pads_) {
        if (pad.gc) {
            SDL_GameControllerClose(pad.gc);
            pad.gc = nullptr;
        }
    }
    pads_.clear();
    initialized_ = false;
}

std::string PcInput::DetectControllerFamily(const std::string& name, SDL_GameControllerType type) {
    switch (type) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox/XInput";
        case SDL_CONTROLLER_TYPE_PS4: return "DualShock 4";
        case SDL_CONTROLLER_TYPE_PS5: return "DualSense";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Switch Pro";
        default: break;
    }
    std::string low = name;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low.find("dualsense") != std::string::npos || low.find("ps5") != std::string::npos) return "DualSense";
    if (low.find("dualshock") != std::string::npos || low.find("ps4") != std::string::npos) return "DualShock 4";
    if (low.find("switch") != std::string::npos || low.find("pro controller") != std::string::npos) return "Switch Pro";
    if (low.find("xbox") != std::string::npos || low.find("xinput") != std::string::npos) return "Xbox/XInput";
    return "Gamepad";
}

float PcInput::ApplyDeadzone(float v, float dz) {
    float av = std::fabs(v);
    if (av < dz) return 0.0f;
    float scaled = (av - dz) / (1.0f - dz);
    return std::clamp(v < 0.0f ? -scaled : scaled, -1.0f, 1.0f);
}

void PcInput::OpenGamepad(int joystick_index) {
    SDL_GameController* gc = SDL_GameControllerOpen(joystick_index);
    if (!gc) return;
    SDL_Joystick* joy = SDL_GameControllerGetJoystick(gc);
    if (!joy) {
        SDL_GameControllerClose(gc);
        return;
    }
    SDL_JoystickID inst_id = SDL_JoystickInstanceID(joy);
    if (inst_id < 0 || pads_.count(inst_id)) {
        SDL_GameControllerClose(gc);
        return;
    }

    PadState st;
    st.gc = gc;
    st.instance_id = inst_id;
    st.device_id = next_pad_dev_id_++;
    const char* nm = SDL_GameControllerName(gc);
    st.name = nm ? nm : "PC Gamepad";
    char guid_buf[64]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid_buf, sizeof(guid_buf));
    st.guid = guid_buf;
    st.family = DetectControllerFamily(st.name, SDL_GameControllerGetType(gc));

    std::fprintf(stdout, "[INPUT] Gamepad connected: '%s' [%s] (devId=%d, guid=%s)\n",
                 st.name.c_str(), st.family.c_str(), st.device_id, st.guid.c_str());
    pads_[inst_id] = std::move(st);
}

void PcInput::CloseGamepad(SDL_JoystickID instance_id) {
    auto it = pads_.find(instance_id);
    if (it == pads_.end()) return;
    std::fprintf(stdout, "[INPUT] Gamepad disconnected: '%s' (devId=%d)\n",
                 it->second.name.c_str(), it->second.device_id);
    if (it->second.announced) {
        pending_disconnects_.push_back(it->second.device_id);
    }
    if (it->second.gc) SDL_GameControllerClose(it->second.gc);
    pads_.erase(it);
}

std::vector<std::string> PcInput::ConnectedGamepadNames() const {
    std::vector<std::string> out;
    out.reserve(pads_.size());
    for (const auto& [id, p] : pads_) {
        out.push_back(p.name + " [" + p.family + "]");
    }
    return out;
}

void PcInput::ResetAllInputs() {
    key_left_pri_ = key_left_alt_ = false;
    key_right_pri_ = key_right_alt_ = false;
    key_throttle_pri_ = key_throttle_alt_ = false;
    key_brake_pri_ = key_brake_alt_ = key_brake_space_ = false;
    key_pause_pri_ = key_pause_alt_ = false;
    kb_steer_ = kb_throttle_ = kb_brake_ = 0.0f;
    kb_buttons.fill(false);
    for (auto& [id, p] : pads_) {
        p.axes.fill(0.0f);
        p.buttons.fill(false);
    }
}

void PcInput::SetKeyState(SDL_Keycode key, bool down) {
    if (key == keys_.steer_left) key_left_pri_ = down;
    if (key == keys_.steer_left_alt) key_left_alt_ = down;
    if (key == keys_.steer_left || key == keys_.steer_left_alt) {
        kb_buttons[static_cast<size_t>(Rr3Button::DpadLeft)] = (key_left_pri_ || key_left_alt_);
    }
    if (key == keys_.steer_right) key_right_pri_ = down;
    if (key == keys_.steer_right_alt) key_right_alt_ = down;
    if (key == keys_.steer_right || key == keys_.steer_right_alt) {
        kb_buttons[static_cast<size_t>(Rr3Button::DpadRight)] = (key_right_pri_ || key_right_alt_);
    }
    if (key == keys_.throttle) key_throttle_pri_ = down;
    if (key == keys_.throttle_alt) key_throttle_alt_ = down;
    if (key == keys_.throttle || key == keys_.throttle_alt) {
        kb_buttons[static_cast<size_t>(Rr3Button::R2)] = (key_throttle_pri_ || key_throttle_alt_);
        if (key == SDLK_UP) kb_buttons[static_cast<size_t>(Rr3Button::DpadUp)] = down;
    }
    if (key == keys_.brake) key_brake_pri_ = down;
    if (key == keys_.brake_alt) key_brake_alt_ = down;
    if (key == keys_.handbrake) key_brake_space_ = down;
    if (key == keys_.brake || key == keys_.brake_alt || key == keys_.handbrake) {
        bool braking = (key_brake_pri_ || key_brake_alt_ || key_brake_space_);
        kb_buttons[static_cast<size_t>(Rr3Button::L2)] = braking;
        kb_buttons[static_cast<size_t>(Rr3Button::X)] = braking;
        if (key == SDLK_DOWN) kb_buttons[static_cast<size_t>(Rr3Button::DpadDown)] = down;
    }
    if (key == keys_.camera) {
        // [Android Gamepad Extended]: camera = button 3 (Y)
        kb_buttons[static_cast<size_t>(Rr3Button::Y)] = down;
    }
    if (key == keys_.rear_view) {
        // [Android Gamepad Extended]: rear_view = button 4 (L1) — pure Look Back without menu Back side-effect
        kb_buttons[static_cast<size_t>(Rr3Button::L1)] = down;
    }
    if (key == keys_.confirm) {
        kb_buttons[static_cast<size_t>(Rr3Button::A)] = down;
    }
    if (key == keys_.pause) key_pause_pri_ = down;
    if (key == keys_.pause_alt) key_pause_alt_ = down;
    if (key == keys_.pause || key == keys_.pause_alt) {
        // [Android Gamepad Extended]: pause = button 13 (Start) pauses/resumes in-race cleanly
        kb_buttons[static_cast<size_t>(Rr3Button::Start)] = (key_pause_pri_ || key_pause_alt_);
    }
    if (key == keys_.back) {
        // Android KEYCODE_BACK = 4: navigates Back in menus, verified no-op in-race (0x13cf200: ret),
        // so Esc safely pauses in-race (via Start) AND navigates Back in menus (via KEYCODE_BACK)!
        pending_android_keys_.emplace_back(4, down);
    }
    if (key == keys_.select_btn) {
        kb_buttons[static_cast<size_t>(Rr3Button::Select)] = down;
    }
}

void PcInput::ProcessEvent(const SDL_Event& e) {
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        SetKeyState(e.key.keysym.sym, true);
    } else if (e.type == SDL_KEYUP) {
        SetKeyState(e.key.keysym.sym, false);
    } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
        OpenGamepad(e.cdevice.which);
    } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        CloseGamepad(e.cdevice.which);
    } else if (e.type == SDL_CONTROLLERAXISMOTION) {
        auto it = pads_.find(e.caxis.which);
        if (it == pads_.end()) return;
        auto& p = it->second;
        float v = static_cast<float>(e.caxis.value) / 32767.0f;
        switch (e.caxis.axis) {
            case SDL_CONTROLLER_AXIS_LEFTX:
                p.axes[static_cast<size_t>(Rr3Axis::LThumbX)] = ApplyDeadzone(v);
                p.buttons[static_cast<size_t>(Rr3Button::LThumbLeft)] = (v < -0.5f);
                p.buttons[static_cast<size_t>(Rr3Button::LThumbRight)] = (v > 0.5f);
                break;
            case SDL_CONTROLLER_AXIS_LEFTY:
                p.axes[static_cast<size_t>(Rr3Axis::LThumbY)] = ApplyDeadzone(v);
                p.buttons[static_cast<size_t>(Rr3Button::LThumbUp)] = (v < -0.5f);
                p.buttons[static_cast<size_t>(Rr3Button::LThumbDown)] = (v > 0.5f);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTX:
                p.axes[static_cast<size_t>(Rr3Axis::RThumbX)] = ApplyDeadzone(v);
                p.buttons[static_cast<size_t>(Rr3Button::RThumbLeft)] = (v < -0.5f);
                p.buttons[static_cast<size_t>(Rr3Button::RThumbRight)] = (v > 0.5f);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTY:
                p.axes[static_cast<size_t>(Rr3Axis::RThumbY)] = ApplyDeadzone(v);
                p.buttons[static_cast<size_t>(Rr3Button::RThumbUp)] = (v < -0.5f);
                p.buttons[static_cast<size_t>(Rr3Button::RThumbDown)] = (v > 0.5f);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT: {
                float t = std::clamp(v, 0.0f, 1.0f);
                p.axes[static_cast<size_t>(Rr3Axis::LTrigger)] = t < 0.05f ? 0.0f : t;
                p.buttons[static_cast<size_t>(Rr3Button::L2)] = (t > 0.25f);
                break;
            }
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
                float t = std::clamp(v, 0.0f, 1.0f);
                p.axes[static_cast<size_t>(Rr3Axis::RTrigger)] = t < 0.05f ? 0.0f : t;
                p.buttons[static_cast<size_t>(Rr3Button::R2)] = (t > 0.25f);
                break;
            }
            default: break;
        }
    } else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        auto it = pads_.find(e.cbutton.which);
        if (it == pads_.end()) return;
        auto& p = it->second;
        bool down = (e.type == SDL_CONTROLLERBUTTONDOWN);
        auto set_b = [&](Rr3Button b) { p.buttons[static_cast<size_t>(b)] = down; };
        switch (e.cbutton.button) {
            // Unified across Xbox (A/B/X/Y/LB/RB), PS4/PS5 (Cross/Circle/Square/Triangle/L1/R1), Switch Pro:
            // [Android Gamepad Extended] (joystick_config.txt:382-399):
            //   accel     = A (0), RT (axis 5), R2 (7)
            //   brake     = X (2), LT (axis 4), L2 (6)
            //   rear_view = B (1), L1 (4)  <- map Right Bumper (RB / R1) + B/Circle here!
            //   camera    = Y (3), R1 (5)  <- map Left Bumper  (LB / L1) + Y/Triangle here!
            //   steering  = Left Stick (axis 0) + D-Pad Left/Right (8/9)
            case SDL_CONTROLLER_BUTTON_A: set_b(Rr3Button::A); break;             // Xbox A / PS Cross -> Throttle + Select
            case SDL_CONTROLLER_BUTTON_B: set_b(Rr3Button::B); break;             // Xbox B / PS Circle -> Look Back + Back
            case SDL_CONTROLLER_BUTTON_X: set_b(Rr3Button::X); break;             // Xbox X / PS Square -> Brake
            case SDL_CONTROLLER_BUTTON_Y: set_b(Rr3Button::Y); break;             // Xbox Y / PS Triangle -> Change Camera
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: set_b(Rr3Button::R1); break; // Xbox LB / PS L1 -> Change Camera (btn 5)
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: set_b(Rr3Button::L1); break;// Xbox RB / PS R1 -> Look Back (btn 4)
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: set_b(Rr3Button::DpadLeft); break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: set_b(Rr3Button::DpadRight); break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: set_b(Rr3Button::DpadUp); break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: set_b(Rr3Button::DpadDown); break;
            case SDL_CONTROLLER_BUTTON_BACK: set_b(Rr3Button::Select); break;
            case SDL_CONTROLLER_BUTTON_START: set_b(Rr3Button::Start); break;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: set_b(Rr3Button::LThumbPress); break;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: set_b(Rr3Button::RThumbPress); break;
            default: break;
        }
    }
}

void PcInput::UpdateFrame(float dt_sec) {
    float dt = std::clamp(dt_sec, 0.0f, 0.1f);
    bool left = key_left_pri_ || key_left_alt_;
    bool right = key_right_pri_ || key_right_alt_;
    bool throttle = key_throttle_pri_ || key_throttle_alt_;
    bool brake = key_brake_pri_ || key_brake_alt_ || key_brake_space_;
    float target_steer = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
    if (target_steer != 0.0f) {
        // Faster snap when reversing steering direction across center.
        float rate = (kb_steer_ * target_steer < 0.0f)
                         ? (cfg_.steer_ramp_rate + cfg_.steer_return_rate)
                         : cfg_.steer_ramp_rate;
        float step = rate * dt;
        if (kb_steer_ < target_steer) kb_steer_ = std::min(target_steer, kb_steer_ + step);
        else kb_steer_ = std::max(target_steer, kb_steer_ - step);
    } else {
        float step = cfg_.steer_return_rate * dt;
        if (kb_steer_ > 0.0f) kb_steer_ = std::max(0.0f, kb_steer_ - step);
        else if (kb_steer_ < 0.0f) kb_steer_ = std::min(0.0f, kb_steer_ + step);
    }
    kb_throttle_ = throttle ? 1.0f : 0.0f;
    kb_brake_ = brake ? 1.0f : 0.0f;
}

void PcInput::DispatchToGuest(CpuRuntime& cpu, ElfLoader& loader, FakeJvm& jvm, uint64_t main_activity_handle) {
    const uint64_t env_ptr = jvm.GetJniEnvGuestPtr();
    const uint64_t cm_cls = jvm.FindClassHandle("com/firemint/realracing/ControllerManager");
    const uint64_t fn_conn = loader.FindExport("Java_com_firemint_realracing_ControllerManager_ControllerConnectedJNI");
    const uint64_t fn_disc = loader.FindExport("Java_com_firemint_realracing_ControllerManager_ControllerDisconnectedJNI");
    const uint64_t fn_axis = loader.FindExport("Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI");
    const uint64_t fn_btn  = loader.FindExport("Java_com_firemint_realracing_ControllerManager_SetButtonValueJNI");
    const uint64_t fn_kdown = loader.FindExport("Java_com_firemint_realracing_MainActivity_onKeyPressed");
    const uint64_t fn_kup   = loader.FindExport("Java_com_firemint_realracing_MainActivity_onKeyReleased");

    // 1. Announce virtual keyboard controller once
    if (!kb_announced_ && fn_conn && cm_cls) {
        uint64_t jname = jvm.NewStringUtf("PC Keyboard Controller");
        uint64_t jdesc = jvm.NewStringUtf("pc-keyboard-virtual-0001");
        cpu.CallGuest(fn_conn, {
            env_ptr, cm_cls, jname, jdesc,
            static_cast<uint64_t>(kKeyboardDeviceId),
            static_cast<uint64_t>(kFullControllerFlags)
        });
        kb_announced_ = true;
    }

    // 2. Dispatch keyboard analog axes & buttons on change
    if (fn_axis && cm_cls) {
        std::array<float, static_cast<size_t>(Rr3Axis::Count)> cur{};
        cur[static_cast<size_t>(Rr3Axis::LThumbX)] = kb_steer_;
        cur[static_cast<size_t>(Rr3Axis::LTrigger)] = kb_brake_;
        cur[static_cast<size_t>(Rr3Axis::RTrigger)] = kb_throttle_;
        for (size_t i = 0; i < cur.size(); ++i) {
            if (std::fabs(cur[i] - kb_prev_axes[i]) > 1e-4f) {
                cpu.CallGuestWithF32(fn_axis,
                                     {env_ptr, cm_cls, static_cast<uint64_t>(kKeyboardDeviceId), static_cast<uint64_t>(i)},
                                     {cur[i]});
                kb_prev_axes[i] = cur[i];
            }
        }
    }
    if (fn_btn && cm_cls) {
        // ControllerManager.java:87: SetButtonValueJNI(int deviceId, boolean pressed, int buttonOrdinal)
        for (size_t i = 0; i < kb_buttons.size(); ++i) {
            if (kb_buttons[i] != kb_prev_buttons[i]) {
                cpu.CallGuest(fn_btn, {
                    env_ptr, cm_cls,
                    static_cast<uint64_t>(kKeyboardDeviceId),
                    kb_buttons[i] ? 1ULL : 0ULL,
                    static_cast<uint64_t>(i)
                });
                kb_prev_buttons[i] = kb_buttons[i];
            }
        }
    }

    // 3. Dispatch disconnected physical gamepads
    if (fn_disc && cm_cls) {
        for (int32_t dev_id : pending_disconnects_) {
            cpu.CallGuest(fn_disc, {env_ptr, cm_cls, static_cast<uint64_t>(dev_id)});
        }
    }
    pending_disconnects_.clear();

    // 4. Announce and dispatch physical gamepads
    for (auto& [inst_id, pad] : pads_) {
        if (!pad.announced && fn_conn && cm_cls) {
            uint64_t jname = jvm.NewStringUtf(pad.name);
            uint64_t jdesc = jvm.NewStringUtf(pad.guid);
            cpu.CallGuest(fn_conn, {
                env_ptr, cm_cls, jname, jdesc,
                static_cast<uint64_t>(pad.device_id),
                static_cast<uint64_t>(kFullControllerFlags)
            });
            pad.announced = true;
        }
        if (fn_axis && cm_cls) {
            for (size_t i = 0; i < pad.axes.size(); ++i) {
                if (std::fabs(pad.axes[i] - pad.prev_axes[i]) > 1e-4f) {
                    cpu.CallGuestWithF32(fn_axis,
                                         {env_ptr, cm_cls, static_cast<uint64_t>(pad.device_id), static_cast<uint64_t>(i)},
                                         {pad.axes[i]});
                    pad.prev_axes[i] = pad.axes[i];
                }
            }
        }
        if (fn_btn && cm_cls) {
            // ControllerManager.java:87: SetButtonValueJNI(int deviceId, boolean pressed, int buttonOrdinal)
            for (size_t i = 0; i < pad.buttons.size(); ++i) {
                if (pad.buttons[i] != pad.prev_buttons[i]) {
                    cpu.CallGuest(fn_btn, {
                        env_ptr, cm_cls,
                        static_cast<uint64_t>(pad.device_id),
                        pad.buttons[i] ? 1ULL : 0ULL,
                        static_cast<uint64_t>(i)
                    });
                    pad.prev_buttons[i] = pad.buttons[i];
                }
            }
        }
    }

    // 5. Dispatch Android keycodes (e.g. Back = 4)
    for (auto [kc, down] : pending_android_keys_) {
        uint64_t fn = down ? fn_kdown : fn_kup;
        if (fn && main_activity_handle) {
            cpu.CallGuest(fn, {env_ptr, main_activity_handle, static_cast<uint64_t>(kc)});
        }
    }
    pending_android_keys_.clear();
}
