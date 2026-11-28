#pragma once

#include "config.h"
#include "dist_tools.h"
#include "elf_loader.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct HWND__;
typedef HWND__* HWND;
struct HDC__;
typedef HDC__* HDC;

class GlesBridge {
public:
    GlesBridge(CpuRuntime& cpu);
    ~GlesBridge();

    // Initializes ANGLE EGL (D3D11) + GLES 2.0 context bound to the calling thread.
    // When fixed_render_h > 0, keeps internal UI/render height locked to fixed_render_h * render_scale
    // while dynamically adjusting render_w to match the window's exact aspect ratio (zero black bars,
    // fixed UI scale, HUD/menus reflow to window edges).
    bool Init(HWND hwnd, int width, int height, const PcConfig& cfg, int fixed_render_h = 0);
    void Shutdown();

    // Updates tracked window dimensions; returns true if logical render dimensions changed.
    bool Resize(int window_w, int window_h);
    void SetVsync(bool enabled);

    // Blits scale FBO (if active) to window backbuffer and calls eglSwapBuffers.
    void SwapBuffers();

    // Computes aspect-preserving presentation rect inside the current window backbuffer.
    void GetPresentRect(int& out_x, int& out_y, int& out_w, int& out_h) const;
    bool IsFixedHeightDynamicAspect() const { return fixed_render_h_ > 0; }

    // Reads back the current rendered frame as top-down RGBA8 pixels (for --dump-frame / testing).
    bool ReadPixelsRgba(std::vector<uint8_t>& out_rgba, int& out_w, int& out_h, bool from_backbuffer = false);

    // Registers all 142 static GLES2 + 3 EGL symbols into ElfLoader.
    void RegisterSymbols(ElfLoader& loader);

    // Helper for Platform.loadTextureFromMemory & Font/GlyphVector texture creation.
    uint32_t UploadRgbaTexture(const void* pixels, int img_w, int img_h, int tex_w, int tex_h, bool flip_y);

    // Current logical render dimensions (window * render_scale).
    int RenderWidth() const { return render_w_; }
    int RenderHeight() const { return render_h_; }
    int WindowWidth() const { return window_w_; }
    int WindowHeight() const { return window_h_; }

    std::string GlVendor() const { return gl_vendor_; }
    std::string GlRenderer() const { return gl_renderer_; }
    std::string GlVersion() const { return gl_version_; }
    std::string GlExtensions() const { return gl_extensions_; }
    std::string GuestExtensions() const { return guest_extensions_; }
    bool SupportsAnisotropy() const { return max_anisotropy_ > 1.0f; }
    float AppliedAnisotropy() const { return applied_anisotropy_; }

    // Selects which GPU compressed texture extension RR3 should target (must be set before Init).
    void SetTargetGpuFormat(DepotGpuFormat fmt) { target_gpu_format_ = fmt; }
    DepotGpuFormat TargetGpuFormat() const { return target_gpu_format_; }

    // Pure CPU compressed texture decoder (ATC, ETC1, DXT1/3/5, PVRTC1 4bpp/2bpp RGB/RGBA).
    static bool DecodeCompressedTextureToRgba(uint32_t gl_format, int width, int height,
                                              const uint8_t* src, int image_size,
                                              std::vector<uint8_t>& out_rgba);

    // Optional override for glGetString(GL_RENDERER) if needed for preset matching.
    void SetRendererOverride(const std::string& override_str) { renderer_override_ = override_str; }

    // Diagnostics counters.
    uint64_t DrawCallCount() const { return draw_calls_; }
    uint64_t SwapCount() const { return swap_count_; }
    void ResetFrameCounters() { draw_calls_ = 0; }

private:
    uint64_t WrapDynamicProc(const char* name);
    void RebuildScaleFbo();
    void DestroyScaleFbo();
    void BlitScaleFboToBackbuffer();

    CpuRuntime& cpu_;
    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
    void* display_ = nullptr; // EGLDisplay
    void* surface_ = nullptr; // EGLSurface
    void* context_ = nullptr; // EGLContext

    PcConfig cfg_{};
    int window_w_ = 0;
    int window_h_ = 0;
    int render_w_ = 0;
    int render_h_ = 0;

    // Optional supersampling / fixed-height dynamic-aspect presentation FBO
    int fixed_render_h_ = 0;
    uint32_t scale_fbo_ = 0;
    uint32_t scale_color_tex_ = 0;
    uint32_t scale_depth_rb_ = 0;
    uint32_t blit_prog_ = 0;
    uint32_t blit_vbo_ = 0;

    std::string gl_vendor_;
    std::string gl_renderer_;
    std::string gl_version_;
    std::string gl_extensions_;
    std::string guest_extensions_;
    std::string renderer_override_;
    DepotGpuFormat target_gpu_format_ = DepotGpuFormat::Atc;
    bool host_supports_s3tc_ = false;
    std::unordered_set<uint32_t> cpu_decoded_dxt_tex_;
    float max_anisotropy_ = 1.0f;
    float applied_anisotropy_ = 1.0f;

    uint64_t draw_calls_ = 0;
    uint64_t swap_count_ = 0;

    std::mutex proc_mtx_;
    std::unordered_map<std::string, uint64_t> static_stubs_;
    std::unordered_map<std::string, uint64_t> dynamic_stubs_;
};
