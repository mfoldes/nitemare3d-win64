#include "n3d/platform_effects.hpp"

#include "n3d/assets.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <Windows.h>
#include <mmreg.h>
#include <xaudio2.h>

namespace n3d {
namespace {

// Mastering-voice channels are ordered by ascending speaker-mask bit.
unsigned channel_index(DWORD mask, DWORD speaker) noexcept {
    return std::popcount(mask & (speaker - 1u));
}

} // namespace

EffectsPlayer::ComApartment::ComApartment() noexcept {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_FALSE increments the same per-thread COM count as S_OK.
    owns_ = SUCCEEDED(hr);
    // An existing MTA is already suitable for XAudio2; do not change or own it.
    result_ = static_cast<std::uint32_t>(hr == RPC_E_CHANGED_MODE ? S_OK : hr);
}

EffectsPlayer::ComApartment::~ComApartment() {
    if (owns_) {
        CoUninitialize();
    }
}

bool EffectsPlayer::set_error(std::string_view message,
                             std::uint32_t native_code) noexcept {
    error_ = message;
    native_error_ = native_code;
    return false;
}

EffectsPlayer::EffectsPlayer(const Assets& assets) noexcept : assets_(assets), com_() {
    const auto com_result = com_.result();
    if (FAILED(static_cast<HRESULT>(com_result))) {
        set_error("COM initialization failed for XAudio2", com_result);
        return;
    }

    HRESULT result = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(result)) {
        set_error("XAudio2 engine creation failed", static_cast<std::uint32_t>(result));
        return;
    }

    result = engine_->CreateMasteringVoice(&mastering_);
    if (FAILED(result)) {
        set_error("XAudio2 mastering voice creation failed",
                  static_cast<std::uint32_t>(result));
        return;
    }

    XAUDIO2_VOICE_DETAILS details{};
    mastering_->GetVoiceDetails(&details);
    channels_ = details.InputChannels;
    if (channels_ == 0 || channels_ > XAUDIO2_MAX_AUDIO_CHANNELS) {
        set_error("Unsupported XAudio2 output channel count");
        return;
    }
    if (channels_ > 1) {
        DWORD mask = 0;
        result = mastering_->GetChannelMask(&mask);
        if (FAILED(result)) {
            set_error("XAudio2 output speaker query failed",
                      static_cast<std::uint32_t>(result));
            return;
        }
        // For an unspecified stereo mask, use the documented stereo channel order.
        if (mask == 0 && channels_ == 2) {
            left_channel_ = 0;
            right_channel_ = 1;
        } else if (static_cast<unsigned>(std::popcount(mask)) == channels_ &&
                   (mask & SPEAKER_FRONT_LEFT) != 0 &&
                   (mask & SPEAKER_FRONT_RIGHT) != 0) {
            left_channel_ = channel_index(mask, SPEAKER_FRONT_LEFT);
            right_channel_ = channel_index(mask, SPEAKER_FRONT_RIGHT);
        } else {
            set_error("XAudio2 output has no front-left/front-right speaker pair");
            return;
        }
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 11025;
    format.wBitsPerSample = 8;
    format.nBlockAlign = 1;
    format.nAvgBytesPerSec = 11025;
    result = engine_->CreateSourceVoice(&source_, &format);
    if (FAILED(result)) {
        set_error("XAudio2 PCM source voice creation failed",
                  static_cast<std::uint32_t>(result));
        return;
    }
    if (!set_current_volume()) return;
    error_ = {};
    native_error_ = 0;
}

EffectsPlayer::~EffectsPlayer() {
    if (source_) source_->DestroyVoice();
    if (mastering_) mastering_->DestroyVoice();
    if (engine_) engine_->Release();
}

bool EffectsPlayer::ready() const noexcept {
    return source_ && error_.empty();
}

bool EffectsPlayer::set_current_volume() noexcept {
    if (!source_) {
        return set_error("XAudio2 effect device unavailable");
    }
    const float gain = muted_ ? 0.0f : static_cast<float>(volume_) / 100.0f;
    const HRESULT result = source_->SetVolume(gain);
    if (FAILED(result)) {
        return set_error("XAudio2 effect volume update failed", static_cast<std::uint32_t>(result));
    }
    error_ = {};
    native_error_ = 0;
    return true;
}

bool EffectsPlayer::set_volume(unsigned percent) noexcept {
    if (percent > 100) {
        return set_error("Effect volume must be between 0 and 100");
    }
    if (!source_) {
        return set_error("XAudio2 effect device unavailable");
    }
    const unsigned previous = volume_;
    volume_ = percent;
    if (!set_current_volume()) {
        volume_ = previous;
        return false;
    }
    return true;
}

bool EffectsPlayer::set_muted(bool muted) noexcept {
    if (!source_) {
        return set_error("XAudio2 effect device unavailable");
    }
    const bool previous = muted_;
    muted_ = muted;
    if (!set_current_volume()) {
        muted_ = previous;
        return false;
    }
    return true;
}

bool EffectsPlayer::play(unsigned effect_id, int mode, int priority) noexcept {
    if (!source_) {
        return set_error("XAudio2 effect device unavailable");
    }
    constexpr unsigned first_effect = 32;
    constexpr unsigned effect_slots = 79;
    if (effect_id >= effect_slots) {
        return set_error("Effect id is outside the sound directory");
    }

    const auto& entry = assets_.sound.directory[first_effect + effect_id];
    if (entry.kind != SoundRecordKind::pcm ||
        entry.absolute_offset > assets_.sound_bytes.size() ||
        entry.length > assets_.sound_bytes.size() - entry.absolute_offset) {
        return set_error("Effect sample is missing or outside sound_bytes");
    }
    // Original 1010:e3d1-e3d4 returns before preempting for an empty PCM slot.
    if (entry.length == 0) {
        error_ = {};
        native_error_ = 0;
        return true;
    }

    XAUDIO2_VOICE_STATE state{};
    source_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued != 0 && priority < priority_) {
        error_ = {};
        native_error_ = 0;
        return true;
    }

    // One voice and at most one queued buffer. The Assets vector is immutable and
    // outlives this player, so XAudio2 can read it until playback or Flush completes.
    HRESULT result = source_->Stop();
    if (FAILED(result)) {
        return set_error("XAudio2 effect voice stop failed", static_cast<std::uint32_t>(result));
    }
    result = source_->FlushSourceBuffers();
    if (FAILED(result)) {
        return set_error("XAudio2 effect voice flush failed", static_cast<std::uint32_t>(result));
    }
    priority_ = 0;

    std::array<float, XAUDIO2_MAX_AUDIO_CHANNELS> matrix{};
    // On mono devices panning cannot be expressed: every mode plays full-level mono.
    if (channels_ == 1) {
        matrix[0] = 1.0f;
    } else {
        if (mode != 1) matrix[left_channel_] = 1.0f;
        if (mode == 0 || mode == 1) matrix[right_channel_] = 1.0f;
    }
    result = source_->SetOutputMatrix(mastering_, 1, channels_, matrix.data());
    if (FAILED(result)) {
        return set_error("XAudio2 effect panning update failed", static_cast<std::uint32_t>(result));
    }

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = entry.length;
    buffer.pAudioData = reinterpret_cast<const BYTE*>(assets_.sound_bytes.data() + entry.absolute_offset);
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    result = source_->SubmitSourceBuffer(&buffer);
    if (FAILED(result)) {
        return set_error("XAudio2 effect buffer submission failed", static_cast<std::uint32_t>(result));
    }
    priority_ = priority;
    if (!paused_) {
        result = source_->Start();
        if (FAILED(result)) {
            // Assets still owns the submitted bytes; discard the failed request.
            const HRESULT flushed = source_->FlushSourceBuffers();
            priority_ = 0;
            return FAILED(flushed)
                ? set_error("XAudio2 effect start and flush failed", static_cast<std::uint32_t>(flushed))
                : set_error("XAudio2 effect start failed", static_cast<std::uint32_t>(result));
        }
    }
    error_ = {};
    native_error_ = 0;
    return true;
}

bool EffectsPlayer::pause() noexcept {
    if (!source_) {
        return set_error("XAudio2 effect device unavailable");
    }
    const HRESULT result = source_->Stop();
    if (FAILED(result)) {
        return set_error("XAudio2 effect pause failed", static_cast<std::uint32_t>(result));
    }
    paused_ = true;
    error_ = {};
    native_error_ = 0;
    return true;
}

bool EffectsPlayer::resume() noexcept {
    if (!source_) {
        return set_error("XAudio2 effect device unavailable");
    }
    XAUDIO2_VOICE_STATE state{};
    source_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued != 0) {
        const HRESULT result = source_->Start();
        if (FAILED(result))
            return set_error("XAudio2 effect resume failed", static_cast<std::uint32_t>(result));
    }
    paused_ = false;
    error_ = {};
    native_error_ = 0;
    return true;
}

bool EffectsPlayer::stop() noexcept {
    if (!source_) {
        return set_error("XAudio2 effect device unavailable");
    }
    HRESULT result = source_->Stop();
    if (FAILED(result)) {
        return set_error("XAudio2 effect stop failed", static_cast<std::uint32_t>(result));
    }
    result = source_->FlushSourceBuffers();
    if (FAILED(result)) {
        return set_error("XAudio2 effect flush failed", static_cast<std::uint32_t>(result));
    }
    priority_ = 0;
    paused_ = false;
    error_ = {};
    native_error_ = 0;
    return true;
}

} // namespace n3d
