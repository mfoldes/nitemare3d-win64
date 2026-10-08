#pragma once

#include <cstdint>
#include <string_view>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace n3d {

struct Assets;

// Assets (including sound_bytes) must outlive this player and must not be modified
// while a sound is queued. Call all methods from the same UI thread.
class EffectsPlayer final {
public:
    explicit EffectsPlayer(const Assets& assets) noexcept;
    ~EffectsPlayer();

    EffectsPlayer(const EffectsPlayer&) = delete;
    EffectsPlayer& operator=(const EffectsPlayer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // An effect id is zero-based and addresses sound.directory[32 + effect_id].
    // A lower-priority request while a buffer is queued is deliberately suppressed.
    [[nodiscard]] bool play(unsigned effect_id, int mode, int priority) noexcept;
    [[nodiscard]] bool set_volume(unsigned percent) noexcept; // 0..100
    [[nodiscard]] bool set_muted(bool muted) noexcept;
    [[nodiscard]] bool pause() noexcept;
    [[nodiscard]] bool resume() noexcept;
    [[nodiscard]] bool stop() noexcept;
    [[nodiscard]] std::uint32_t last_native_error() const noexcept { return native_error_; }
    [[nodiscard]] std::string_view last_error() const noexcept { return error_; }

private:
    class ComApartment final {
    public:
        ComApartment() noexcept;
        ~ComApartment();
        [[nodiscard]] std::uint32_t result() const noexcept { return result_; }

    private:
        bool owns_ = false;
        std::uint32_t result_ = 0;
    };

    bool set_error(std::string_view message,
                   std::uint32_t native_code = 0) noexcept;
    [[nodiscard]] bool set_current_volume() noexcept;

    const Assets& assets_;
    ComApartment com_;
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* mastering_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    unsigned channels_ = 0;
    unsigned left_channel_ = 0;
    unsigned right_channel_ = 0;
    unsigned volume_ = 70;
    int priority_ = 0;
    bool muted_ = false;
    bool paused_ = false;
    std::string_view error_;
    std::uint32_t native_error_ = 0;
};

} // namespace n3d
