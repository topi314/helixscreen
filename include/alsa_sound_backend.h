// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef HELIX_HAS_ALSA

#include "note_event.h"
#include "sound_backend.h"
#include "sound_synthesis.h"

#include <alsa/asoundlib.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// ALSA PCM audio backend — real waveform synthesis for Linux SBCs
/// Uses VoiceSlot for atomic note publishing and per-sample rendering.
class ALSASoundBackend : public SoundBackend {
  public:
    ALSASoundBackend();
    ~ALSASoundBackend() override;

    // SoundBackend interface
    void set_tone(float freq_hz, float amplitude, float duty_cycle) override;
    void silence() override;
    void set_waveform(Waveform w) override;
    void set_filter(const std::string& type, float cutoff) override;
    bool supports_waveforms() const override {
        return true;
    }
    bool supports_amplitude() const override {
        return true;
    }
    bool supports_filter() const override {
        return true;
    }
    float min_tick_ms() const override {
        return 1.0f;
    }

    /// Initialize the named ALSA PCM device. Returns false if it can't be opened.
    bool initialize(const std::string& device);

    bool supports_device_selection() const override {
        return true;
    }

    /// Stop render thread and close ALSA device.
    void shutdown();

    /// Duplicate mono buffer to interleaved stereo (L=R). Public for testability.
    static void mono_to_stereo(const float* mono, float* stereo, size_t frame_count);

    /// Convert float [-1,1] samples to int16 with clamping. Public for testability.
    static void float_to_s16(const float* src, int16_t* dst, size_t sample_count);

    // Voice interface (legacy — used by non-note-event callers)
    void set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) override;
    void set_voice_waveform(int slot, Waveform w) override;
    void silence_voice(int slot) override;
    int voice_count() const override {
        return MAX_VOICES;
    }

    // NoteEvent interface (primary path for synth sounds)
    void publish_note(int slot, const NoteEvent& event) override;
    bool supports_note_events() const override {
        return true;
    }

    // Render source for direct audio generation (tracker PCM playback)
    bool supports_render_source() const override {
        return true;
    }
    void set_render_source(std::function<void(float*, size_t, int)> fn) override;
    void clear_render_source() override;

  private:
    void render_loop();
    snd_pcm_sframes_t recover_xrun(snd_pcm_sframes_t err);

    snd_pcm_t* pcm_ = nullptr;
    std::thread render_thread_;
    std::atomic<bool> running_{false};

    static constexpr int MAX_VOICES = 4;

    VoiceSlot voice_slots_[MAX_VOICES];

    // Scratch buffer for mixing (sized in initialize())
    std::vector<float> mix_buf_;

    // External render source (tracker PCM playback)
    std::function<void(float*, size_t, int)> render_source_;
    std::mutex render_source_mutex_;

    // Filter for external render source (tracker) — shared, not per-voice
    std::atomic<helix::audio::FilterType> filter_type_{helix::audio::FilterType::NONE};
    std::atomic<float> filter_cutoff_{20000.0f};
    helix::audio::BiquadFilter filter_;

    // Audio format negotiated during initialize()
    unsigned int sample_rate_ = 44100;
    snd_pcm_uframes_t period_size_ = 256;
    unsigned int channels_ = 1;
    bool use_s16_ = false;

    // Underrun log rate limiter (touched only by render thread)
    uint64_t xrun_count_ = 0;
    std::chrono::steady_clock::time_point last_xrun_log_{};
};

#endif // HELIX_HAS_ALSA
