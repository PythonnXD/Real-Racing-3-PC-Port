#include "audio_opensl.h"

#include <SDL.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kSlResultSuccess = 0;
constexpr uint32_t kSlResultFeatureUnsupported = 12;
constexpr uint32_t kSlObjectStateRealized = 2;
constexpr uint32_t kSlPlayStateStopped = 1;
constexpr uint32_t kSlPlayStatePaused = 2;
constexpr uint32_t kSlPlayStatePlaying = 3;

struct SlDataSourceGuest {
    uint64_t pLocator;
    uint64_t pFormat;
};

struct SlDataFormatPcmGuest {
    uint32_t formatType;
    uint32_t numChannels;
    uint32_t samplesPerSec; // milliHertz in OpenSL ES!
    uint32_t bitsPerSample;
    uint32_t containerSize;
    uint32_t channelMask;
    uint32_t endianness;
};

struct SlBufferQueueStateGuest {
    uint32_t count;
    uint32_t index;
};

} // namespace

OpenSlBridge::OpenSlBridge(CpuRuntime& cpu) : cpu_(cpu) {}

OpenSlBridge::~OpenSlBridge() {
    Shutdown();
}

bool OpenSlBridge::Init() {
    if (initialized_) return true;

    mem_ = static_cast<uint64_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem_) return false;

    // Layout inside mem_ (each slot = 8 bytes):
    // [0..9]:   IID structs (unique dummy target addresses)
    // [10..14]: SLInterfaceID pointer variables (what dlsym("SL_IID_*") returns!)
    // [16..31]: SLObjectItf_ vtable (16 slots)
    // [32..47]: SLEngineItf_ vtable (16 slots)
    // [48..63]: SLPlayItf_ vtable (16 slots)
    // [64..79]: SLAndroidSimpleBufferQueueItf_ vtable (16 slots)
    // [80..95]: SLAndroidConfigurationItf_ vtable (16 slots)
    // [100..115]: Object / Interface double-indirected pointers
    iid_engine_val_ = reinterpret_cast<uint64_t>(&mem_[0]);
    iid_play_val_   = reinterpret_cast<uint64_t>(&mem_[2]);
    iid_bufq_val_   = reinterpret_cast<uint64_t>(&mem_[4]);
    iid_cfg_val_    = reinterpret_cast<uint64_t>(&mem_[6]);
    iid_record_val_ = reinterpret_cast<uint64_t>(&mem_[8]);

    mem_[10] = iid_engine_val_; iid_engine_sym_ = reinterpret_cast<uint64_t>(&mem_[10]);
    mem_[11] = iid_play_val_;   iid_play_sym_   = reinterpret_cast<uint64_t>(&mem_[11]);
    mem_[12] = iid_bufq_val_;   iid_bufq_sym_   = reinterpret_cast<uint64_t>(&mem_[12]);
    mem_[13] = iid_cfg_val_;    iid_cfg_sym_    = reinterpret_cast<uint64_t>(&mem_[13]);
    mem_[14] = iid_record_val_; iid_record_sym_ = reinterpret_cast<uint64_t>(&mem_[14]);

    uint64_t* obj_vt  = &mem_[16];
    uint64_t* eng_vt  = &mem_[32];
    uint64_t* play_vt = &mem_[48];
    uint64_t* bufq_vt = &mem_[64];
    uint64_t* cfg_vt  = &mem_[80];

    uint64_t ok_stub = cpu_.AllocateSvcStub("OpenSL::noop_ok", [](CpuContext& c) {
        c.SetW(0, kSlResultSuccess);
    });
    for (int i = 0; i < 16; ++i) {
        obj_vt[i] = ok_stub;
        eng_vt[i] = ok_stub;
        play_vt[i] = ok_stub;
        bufq_vt[i] = ok_stub;
        cfg_vt[i] = ok_stub;
    }

    // Object / Interface pointers (OpenSL ES interfaces are const struct XItf_ * const *)
    mem_[100] = reinterpret_cast<uint64_t>(obj_vt);  engine_obj_ptr_ = reinterpret_cast<uint64_t>(&mem_[100]);
    mem_[101] = reinterpret_cast<uint64_t>(obj_vt);  mix_obj_ptr_    = reinterpret_cast<uint64_t>(&mem_[101]);
    mem_[102] = reinterpret_cast<uint64_t>(obj_vt);  player_obj_ptr_ = reinterpret_cast<uint64_t>(&mem_[102]);
    mem_[103] = reinterpret_cast<uint64_t>(eng_vt);  engine_itf_ptr_ = reinterpret_cast<uint64_t>(&mem_[103]);
    mem_[104] = reinterpret_cast<uint64_t>(play_vt); play_itf_ptr_   = reinterpret_cast<uint64_t>(&mem_[104]);
    mem_[105] = reinterpret_cast<uint64_t>(bufq_vt); bufq_itf_ptr_   = reinterpret_cast<uint64_t>(&mem_[105]);
    mem_[106] = reinterpret_cast<uint64_t>(cfg_vt);  cfg_itf_ptr_    = reinterpret_cast<uint64_t>(&mem_[106]);

    // ---- slCreateEngine ----
    sl_create_engine_stub_ = cpu_.AllocateSvcStub("OpenSL::slCreateEngine", [this](CpuContext& c) {
        auto* pEngine = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(0)));
        if (pEngine) *pEngine = engine_obj_ptr_;
        std::fprintf(stdout, "[AUDIO] OpenSL slCreateEngine() -> OK\n");
        c.SetW(0, kSlResultSuccess);
    });

    // ---- SLObjectItf_ ----
    obj_vt[0] = ok_stub; // Realize
    obj_vt[1] = ok_stub; // Resume
    obj_vt[2] = cpu_.AllocateSvcStub("SLObject::GetState", [](CpuContext& c) {
        auto* pState = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(c.GetX(1)));
        if (pState) *pState = kSlObjectStateRealized;
        c.SetW(0, kSlResultSuccess);
    });
    obj_vt[3] = cpu_.AllocateSvcStub("SLObject::GetInterface", [this](CpuContext& c) {
        uint64_t iid = c.GetX(1);
        auto* out_itf = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(2)));
        uint64_t res_ptr = 0;
        if (iid == iid_engine_val_) res_ptr = engine_itf_ptr_;
        else if (iid == iid_play_val_) res_ptr = play_itf_ptr_;
        else if (iid == iid_bufq_val_) res_ptr = bufq_itf_ptr_;
        else if (iid == iid_cfg_val_) res_ptr = cfg_itf_ptr_;
        if (out_itf) *out_itf = res_ptr;
        c.SetW(0, res_ptr ? kSlResultSuccess : kSlResultFeatureUnsupported);
    });
    obj_vt[9] = cpu_.AllocateSvcStub("SLObject::Destroy", [this](CpuContext& c) {
        uint64_t self = c.GetX(0);
        if (self == player_obj_ptr_) {
            std::lock_guard<std::mutex> lk(audio_mtx_);
            play_state_ = kSlPlayStateStopped;
            guest_cb_fn_ = 0;
            if (sdl_dev_) SDL_PauseAudioDevice(sdl_dev_, 1);
        }
        c.SetW(0, kSlResultSuccess);
    });

    // ---- SLEngineItf_ ----
    eng_vt[2] = cpu_.AllocateSvcStub("SLEngine::CreateAudioPlayer", [this](CpuContext& c) {
        auto* pPlayer = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(1)));
        auto* pSrc = reinterpret_cast<const SlDataSourceGuest*>(static_cast<uintptr_t>(c.GetX(2)));
        int rate = 24000;
        int ch = 2;
        if (pSrc && pSrc->pFormat) {
            auto* fmt = reinterpret_cast<const SlDataFormatPcmGuest*>(static_cast<uintptr_t>(pSrc->pFormat));
            if (fmt->samplesPerSec >= 8000000) rate = static_cast<int>(fmt->samplesPerSec / 1000);
            else if (fmt->samplesPerSec >= 8000) rate = static_cast<int>(fmt->samplesPerSec);
            if (fmt->numChannels >= 1 && fmt->numChannels <= 8) ch = static_cast<int>(fmt->numChannels);
        }
        OpenSdlAudio(rate, ch);
        if (pPlayer) *pPlayer = player_obj_ptr_;
        c.SetW(0, kSlResultSuccess);
    });
    eng_vt[7] = cpu_.AllocateSvcStub("SLEngine::CreateOutputMix", [this](CpuContext& c) {
        auto* pMix = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(1)));
        if (pMix) *pMix = mix_obj_ptr_;
        c.SetW(0, kSlResultSuccess);
    });

    // ---- SLPlayItf_ ----
    play_vt[0] = cpu_.AllocateSvcStub("SLPlay::SetPlayState", [this](CpuContext& c) {
        uint32_t st = c.GetW(1);
        {
            std::lock_guard<std::mutex> lk(audio_mtx_);
            play_state_ = st;
            if (sdl_dev_) {
                SDL_PauseAudioDevice(sdl_dev_, (st == kSlPlayStatePlaying) ? 0 : 1);
            }
        }
        c.SetW(0, kSlResultSuccess);
    });
    play_vt[1] = cpu_.AllocateSvcStub("SLPlay::GetPlayState", [this](CpuContext& c) {
        auto* pState = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(c.GetX(1)));
        std::lock_guard<std::mutex> lk(audio_mtx_);
        if (pState) *pState = play_state_;
        c.SetW(0, kSlResultSuccess);
    });

    // ---- SLAndroidSimpleBufferQueueItf_ ----
    bufq_vt[0] = cpu_.AllocateSvcStub("SLBufQ::Enqueue", [this](CpuContext& c) {
        const void* buf = reinterpret_cast<const void*>(static_cast<uintptr_t>(c.GetX(1)));
        uint32_t sz = c.GetW(2);
        if (buf && sz > 0) {
            std::lock_guard<std::mutex> lk(audio_mtx_);
            last_buf_bytes_ = sz;
            if (sdl_dev_) {
                SDL_QueueAudio(sdl_dev_, buf, sz);
            }
            ++queued_count_;
            enqueue_count_.fetch_add(1, std::memory_order_relaxed);
            uint32_t frame_bytes = static_cast<uint32_t>(std::max(1, channels_) * 2);
            frames_played_.fetch_add(sz / frame_bytes, std::memory_order_relaxed);
        }
        c.SetW(0, kSlResultSuccess);
    });
    bufq_vt[1] = cpu_.AllocateSvcStub("SLBufQ::Clear", [this](CpuContext& c) {
        std::lock_guard<std::mutex> lk(audio_mtx_);
        if (sdl_dev_) SDL_ClearQueuedAudio(sdl_dev_);
        queued_count_ = 0;
        c.SetW(0, kSlResultSuccess);
    });
    bufq_vt[2] = cpu_.AllocateSvcStub("SLBufQ::GetState", [this](CpuContext& c) {
        auto* st = reinterpret_cast<SlBufferQueueStateGuest*>(static_cast<uintptr_t>(c.GetX(1)));
        if (st) {
            std::lock_guard<std::mutex> lk(audio_mtx_);
            st->count = queued_count_;
            st->index = completed_index_;
        }
        c.SetW(0, kSlResultSuccess);
    });
    bufq_vt[3] = cpu_.AllocateSvcStub("SLBufQ::RegisterCallback", [this](CpuContext& c) {
        std::lock_guard<std::mutex> lk(audio_mtx_);
        guest_cb_fn_ = c.GetX(1);
        guest_cb_ctx_ = c.GetX(2);
        c.SetW(0, kSlResultSuccess);
    });

    stop_thread_.store(false);
    pump_thread_ = std::thread([this]() { PumpThreadMain(); });
    initialized_ = true;
    return true;
}

void OpenSlBridge::OpenSdlAudio(int rate, int channels) {
    std::lock_guard<std::mutex> lk(audio_mtx_);
    CloseSdlAudio();
    sample_rate_ = rate;
    channels_ = channels;

    SDL_AudioSpec want{}, have{};
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(channels);
    want.samples = 512;
    want.callback = nullptr; // push mode via SDL_QueueAudio

    sdl_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!sdl_dev_) {
        std::fprintf(stderr, "[AUDIO] SDL_OpenAudioDevice(%dHz, %dch) failed: %s (continuing silent)\n",
                     rate, channels, SDL_GetError());
        return;
    }
    sample_rate_ = have.freq;
    channels_ = have.channels;
    SDL_PauseAudioDevice(sdl_dev_, (play_state_ == kSlPlayStatePlaying) ? 0 : 1);
    std::fprintf(stdout, "[AUDIO] OpenSL ES -> SDL2 WASAPI ready (%d Hz, %d ch, S16)\n",
                 sample_rate_, channels_);
}

void OpenSlBridge::CloseSdlAudio() {
    if (sdl_dev_) {
        SDL_PauseAudioDevice(sdl_dev_, 1);
        SDL_ClearQueuedAudio(sdl_dev_);
        SDL_CloseAudioDevice(sdl_dev_);
        sdl_dev_ = 0;
    }
}

void OpenSlBridge::PumpThreadMain() {
    cpu_.EnsureThreadInit();
    while (!stop_thread_.load(std::memory_order_relaxed)) {
        uint64_t cb_fn = 0;
        uint64_t cb_ctx = 0;
        bool should_fire = false;

        {
            std::lock_guard<std::mutex> lk(audio_mtx_);
            if (play_state_ == kSlPlayStatePlaying && guest_cb_fn_ != 0 && queued_count_ > 0) {
                uint32_t target_bytes = std::max<uint32_t>(last_buf_bytes_ * 3, 4096);
                uint32_t sdl_queued = sdl_dev_ ? SDL_GetQueuedAudioSize(sdl_dev_) : 0;
                if (sdl_queued < target_bytes) {
                    --queued_count_;
                    ++completed_index_;
                    cb_fn = guest_cb_fn_;
                    cb_ctx = guest_cb_ctx_;
                    should_fire = true;
                }
            }
        }

        if (should_fire && cb_fn != 0) {
            cpu_.CallGuest(cb_fn, {bufq_itf_ptr_, cb_ctx});
            if (sdl_dev_ == 0) {
                // Silent fallback pacing (avoid 100% CPU busy-spin when no audio device exists).
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
}

void OpenSlBridge::Shutdown() {
    if (!initialized_) return;
    stop_thread_.store(true);
    if (pump_thread_.joinable()) {
        pump_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lk(audio_mtx_);
        CloseSdlAudio();
    }
    if (mem_) {
        VirtualFree(mem_, 0, MEM_RELEASE);
        mem_ = nullptr;
    }
    initialized_ = false;
}

uint64_t OpenSlBridge::ResolveSymbol(const std::string& lib_name, const std::string& sym_name) {
    if (lib_name.find("OpenSLES") == std::string::npos) return 0;
    if (!initialized_) Init();
    if (sym_name == "slCreateEngine") return sl_create_engine_stub_;
    if (sym_name == "SL_IID_ENGINE") return iid_engine_sym_;
    if (sym_name == "SL_IID_PLAY") return iid_play_sym_;
    if (sym_name == "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") return iid_bufq_sym_;
    if (sym_name == "SL_IID_ANDROIDCONFIGURATION") return iid_cfg_sym_;
    if (sym_name == "SL_IID_RECORD") return iid_record_sym_;
    return 0;
}
