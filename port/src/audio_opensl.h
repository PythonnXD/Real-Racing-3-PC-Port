#pragma once

#include "cpu_dynarmic.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class OpenSlBridge {
public:
    explicit OpenSlBridge(CpuRuntime& cpu);
    ~OpenSlBridge();

    bool Init();
    void Shutdown();

    // Resolver hook for BionicRuntime::SetCustomDlResolver("libOpenSLES.so", sym).
    uint64_t ResolveSymbol(const std::string& lib_name, const std::string& sym_name);

    uint64_t FramesPlayed() const { return frames_played_.load(std::memory_order_relaxed); }
    uint64_t EnqueueCount() const { return enqueue_count_.load(std::memory_order_relaxed); }
    int SampleRate() const { return sample_rate_; }
    int Channels() const { return channels_; }

private:
    void PumpThreadMain();
    void OpenSdlAudio(int rate, int channels);
    void CloseSdlAudio();

    CpuRuntime& cpu_;
    bool initialized_ = false;

    // Guest-readable memory page holding OpenSL IID pointers, vtables, and object instances.
    uint64_t* mem_ = nullptr;
    uint64_t sl_create_engine_stub_ = 0;

    uint64_t iid_engine_sym_ = 0;
    uint64_t iid_play_sym_ = 0;
    uint64_t iid_bufq_sym_ = 0;
    uint64_t iid_cfg_sym_ = 0;
    uint64_t iid_record_sym_ = 0;

    uint64_t iid_engine_val_ = 0;
    uint64_t iid_play_val_ = 0;
    uint64_t iid_bufq_val_ = 0;
    uint64_t iid_cfg_val_ = 0;
    uint64_t iid_record_val_ = 0;

    uint64_t engine_obj_ptr_ = 0;
    uint64_t mix_obj_ptr_ = 0;
    uint64_t player_obj_ptr_ = 0;
    uint64_t engine_itf_ptr_ = 0;
    uint64_t play_itf_ptr_ = 0;
    uint64_t bufq_itf_ptr_ = 0;
    uint64_t cfg_itf_ptr_ = 0;

    uint32_t sdl_dev_ = 0;
    int sample_rate_ = 48000;
    int channels_ = 2;
    uint32_t last_buf_bytes_ = 2048;

    std::mutex audio_mtx_;
    uint32_t play_state_ = 1; // SL_PLAYSTATE_STOPPED
    uint64_t guest_cb_fn_ = 0;
    uint64_t guest_cb_ctx_ = 0;
    uint32_t queued_count_ = 0;
    uint32_t completed_index_ = 0;

    std::atomic<bool> stop_thread_{false};
    std::atomic<uint64_t> frames_played_{0};
    std::atomic<uint64_t> enqueue_count_{0};
    std::thread pump_thread_;
};
