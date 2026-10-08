#include "n3d/platform_midi.hpp"

#include "n3d/assets.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace n3d {
namespace {

constexpr std::size_t kTuneCount = 15;
constexpr std::size_t kSlotWords = 15 * 1024; // 60 KiB, strictly below WinMM's 64 KiB limit.
constexpr std::uint16_t kSyntheticTrack = 0xffff;
constexpr std::uint32_t kShortMessage = static_cast<std::uint32_t>(MEVT_SHORTMSG) << 24u;
constexpr std::uint32_t kTempo = static_cast<std::uint32_t>(MEVT_TEMPO) << 24u;
constexpr std::uint32_t kLongMessage = MEVT_F_LONG | (static_cast<std::uint32_t>(MEVT_LONGMSG) << 24u);
constexpr std::uint32_t kNop = static_cast<std::uint32_t>(MEVT_NOP) << 24u;

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

class Reader final {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool empty() const noexcept { return position_ == bytes_.size(); }

    [[nodiscard]] std::uint8_t byte() {
        require(position_ < bytes_.size(), "Truncated validated MIDI event");
        return std::to_integer<std::uint8_t>(bytes_[position_++]);
    }

    [[nodiscard]] std::uint8_t data_byte() {
        const auto value = byte();
        require(value < 0x80u, "MIDI channel/system data byte is not seven-bit");
        return value;
    }

    [[nodiscard]] std::uint16_t big16() {
        const auto hi = byte();
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(hi) << 8u) | byte());
    }

    [[nodiscard]] std::uint32_t big32() {
        const auto hi = big16();
        return (static_cast<std::uint32_t>(hi) << 16u) | big16();
    }

    [[nodiscard]] std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (unsigned step = 0; step < 4; ++step) {
            const auto next = byte();
            value = (value << 7u) | (next & 0x7fu);
            if ((next & 0x80u) == 0u) {
                return value;
            }
        }
        throw std::runtime_error("MIDI VLQ exceeds four bytes");
    }

    [[nodiscard]] std::span<const std::byte> take(std::size_t length) {
        require(length <= bytes_.size() - position_, "Truncated validated MIDI payload");
        const auto result = bytes_.subspan(position_, length);
        position_ += length;
        return result;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

void expect_tag(Reader& reader, const char (&tag)[5]) {
    const auto found = reader.take(4);
    require(std::memcmp(found.data(), tag, 4) == 0, "Invalid validated MIDI chunk tag");
}

struct Tune final {
    std::uint16_t division = 0;
    std::vector<MidiStreamEvent> events;
};

Tune decode_tune(const Assets& assets, std::uint8_t tune_index) {
    const auto& entry = assets.sound.directory[tune_index];
    require(entry.kind == SoundRecordKind::midi && entry.length != 0 &&
                entry.absolute_offset <= assets.sound_bytes.size() &&
                entry.length <= assets.sound_bytes.size() - entry.absolute_offset,
            "Validated MIDI directory extent is invalid");

    Reader file(std::span<const std::byte>(assets.sound_bytes).subspan(entry.absolute_offset,
                                                                      entry.length));
    expect_tag(file, "MThd");
    require(file.big32() == 6, "MIDI MThd must contain six bytes");
    require(file.big16() == 1, "MIDI stream requires a format-1 SMF");
    const auto track_count = file.big16();
    const auto division = file.big16();
    require(track_count != 0 && division != 0 && (division & 0x8000u) == 0u,
            "MIDI stream requires positive PPQN time division");

    const auto& summary = assets.sound.midi[tune_index - 1];
    require(track_count == summary.track_count && division == summary.division,
            "MIDI header differs from validated sound metadata");

    std::vector<MidiStreamEvent> parsed;
    std::uint64_t end_tick = 0;
    std::uint32_t first_tempo = 0;
    for (std::uint16_t track_index = 0; track_index < track_count; ++track_index) {
        expect_tag(file, "MTrk");
        Reader track(file.take(file.big32()));
        std::uint64_t tick = 0;
        std::uint8_t running_status = 0;
        std::uint32_t order = 0;
        bool ended = false;

        while (!track.empty()) {
            tick += track.vlq();
            const auto first = track.byte();
            const bool running = first < 0x80u;
            require(!running || running_status != 0,
                    "MIDI running status without preceding channel status");
            const auto status = running ? running_status : first;
            const auto data1 = running ? first : 0u;
            MidiStreamEvent event{};
            event.tick = tick;
            event.track_index = track_index;
            event.track_order = order++;

            if (status == 0xffu) {
                const auto meta_type = track.byte();
                const auto length = track.vlq();
                const auto payload = track.take(length);
                running_status = 0;
                if (meta_type == 0x2fu) {
                    require(payload.empty() && track.empty(), "MIDI EOT must terminate its track");
                    end_tick = std::max(end_tick, tick);
                    ended = true;
                    break;
                }
                if (meta_type == 0x51u) {
                    require(payload.size() == 3, "MIDI tempo meta requires three bytes");
                    const auto tempo = (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[0])) << 16u) |
                                       (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[1])) << 8u) |
                                       std::to_integer<std::uint8_t>(payload[2]);
                    require(tempo != 0, "MIDI tempo must be nonzero");
                    if (first_tempo == 0) {
                        first_tempo = tempo;
                    }
                    event.event_word = kTempo | tempo;
                    parsed.push_back(event);
                }
                continue; // Other meta events carry timing, but are not MIDI output.
            }

            if (status == 0xf0u || status == 0xf7u) {
                const auto payload = track.take(track.vlq());
                running_status = 0;
                const auto transmitted_length = payload.size() + (status == 0xf0u ? 1u : 0u);
                require(transmitted_length <= 0x00ffffffu, "MIDI SysEx exceeds WinMM length field");
                if (transmitted_length != 0) {
                    event.event_word = kLongMessage | static_cast<std::uint32_t>(transmitted_length);
                    event.long_data = payload;
                    event.prepend_sysex_start = status == 0xf0u;
                    parsed.push_back(event);
                }
                continue;
            }

            if (status >= 0xf0u) {
                unsigned count = 0;
                if (status == 0xf1u || status == 0xf3u) {
                    count = 1;
                } else if (status == 0xf2u) {
                    count = 2;
                } else {
                    require(status == 0xf6u || status == 0xf8u || status == 0xfau ||
                                status == 0xfbu || status == 0xfcu || status == 0xfeu,
                            "Unsupported MIDI system status");
                }
                const auto lo = count >= 1 ? track.data_byte() : 0u;
                const auto hi = count == 2 ? track.data_byte() : 0u;
                if (status < 0xf8u) {
                    running_status = 0;
                }
                event.event_word = kShortMessage | status | (lo << 8u) | (hi << 16u);
                parsed.push_back(event);
                continue;
            }

            require(status >= 0x80u && status <= 0xefu, "Invalid MIDI channel status");
            const auto hi_nibble = status & 0xf0u;
            const auto lo = running ? data1 : track.data_byte();
            const auto second = (hi_nibble == 0xc0u || hi_nibble == 0xd0u)
                                    ? 0u : track.data_byte();
            running_status = status;
            event.event_word = kShortMessage | status | (lo << 8u) | (second << 16u);
            parsed.push_back(event);
        }
        require(ended, "Validated MIDI track has no EOT event");
    }
    require(file.empty() && first_tempo == summary.tempo_us_per_quarter &&
                end_tick == summary.last_event_tick,
            "Decoded MIDI events differ from validated sound metadata");

    std::sort(parsed.begin(), parsed.end(), [](const MidiStreamEvent& a,
                                               const MidiStreamEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track_index != b.track_index) return a.track_index < b.track_index;
        return a.track_order < b.track_order;
    });

    Tune tune;
    tune.division = division;
    tune.events.reserve(parsed.size() + 1);
    std::uint64_t last_tick = 0;
    const auto append_at = [&tune, &last_tick](MidiStreamEvent event) {
        while (event.tick - last_tick > std::numeric_limits<std::uint32_t>::max()) {
            MidiStreamEvent delay{};
            delay.tick = last_tick + std::numeric_limits<std::uint32_t>::max();
            delay.delta_ticks = std::numeric_limits<std::uint32_t>::max();
            delay.event_word = kNop;
            delay.track_index = kSyntheticTrack;
            tune.events.push_back(delay);
            last_tick = delay.tick;
        }
        event.delta_ticks = static_cast<std::uint32_t>(event.tick - last_tick);
        last_tick = event.tick;
        tune.events.push_back(event);
    };
    for (const auto& event : parsed) {
        append_at(event);
    }
    // The last track may contain a silent tail after the last sounding event.
    if (tune.events.empty() || end_tick > last_tick) {
        MidiStreamEvent end{};
        end.tick = end_tick;
        end.event_word = kNop;
        end.track_index = kSyntheticTrack;
        append_at(end);
    }
    return tune;
}

} // namespace

struct MidiStreamPlayer::Impl final {
    struct Slot final {
        std::array<DWORD, kSlotWords> words{};
        MIDIHDR header{};
        std::atomic<bool> completed{false};
        bool prepared = false;
        bool queued = false;
    };

    explicit Impl(const Assets& assets) {
        for (std::uint8_t index = 1; index <= kTuneCount; ++index) {
            tunes[index] = decode_tune(assets, index);
        }
    }

    ~Impl() {
        if (stream) {
            // No application locks are held: MOM_DONE only stores to Slot::completed.
            midiStreamStop(stream);
            midiOutReset(reinterpret_cast<HMIDIOUT>(stream));
            for (auto& slot : slots) {
                if (slot.prepared) {
                    midiOutUnprepareHeader(reinterpret_cast<HMIDIOUT>(stream), &slot.header,
                                           sizeof(slot.header));
                }
            }
            midiStreamClose(stream);
        }
    }

    static void CALLBACK callback(HMIDIOUT, UINT message, DWORD_PTR, DWORD_PTR param1,
                                  DWORD_PTR) noexcept {
        if (message == MOM_DONE) {
            auto* header = reinterpret_cast<MIDIHDR*>(param1);
            auto* slot = reinterpret_cast<Slot*>(header->dwUser);
            slot->completed.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] bool fail(const char* reason, MMRESULT native = MMSYSERR_NOERROR) noexcept {
        error = reason;
        native_error = native;
        return false;
    }

    void clear_error() noexcept {
        error = "";
        native_error = MMSYSERR_NOERROR;
    }

    [[nodiscard]] bool apply_volume(unsigned level, bool silence) {
        if (!stream) return true;
        const auto channel = static_cast<DWORD>(silence ? 0u : level * 0xffffu / 100u);
        const auto result = midiOutSetVolume(reinterpret_cast<HMIDIOUT>(stream),
                                             channel | (channel << 16u));
        if (result == MMSYSERR_NOTSUPPORTED) {
            return fail("MIDI mapper does not support per-stream volume/mute", result);
        }
        if (result != MMSYSERR_NOERROR) {
            return fail("midiOutSetVolume failed", result);
        }
        return true;
    }

    [[nodiscard]] bool ensure_open() {
        if (stream) return true;
        UINT device = MIDI_MAPPER;
        const auto result = midiStreamOpen(&stream, &device, 1,
                                           reinterpret_cast<DWORD_PTR>(&callback), 0,
                                           CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            stream = nullptr;
            if (result == MMSYSERR_BADDEVICEID || result == MMSYSERR_NODRIVER) {
                return fail("MIDI_MAPPER unavailable: midiStreamOpen failed", result);
            }
            return fail("midiStreamOpen(MIDI_MAPPER) failed", result);
        }
        if (!apply_volume(volume, muted)) {
            midiStreamClose(stream);
            stream = nullptr;
            return false;
        }
        return true;
    }

    // Only called with a stopped stream and with both headers released.
    [[nodiscard]] bool set_timing(const Tune& tune) {
        MIDIPROPTIMEDIV time_division{};
        time_division.cbStruct = sizeof(time_division);
        time_division.dwTimeDiv = tune.division;
        auto result = midiStreamProperty(stream, reinterpret_cast<LPBYTE>(&time_division),
                                         MIDIPROP_SET | MIDIPROP_TIMEDIV);
        if (result != MMSYSERR_NOERROR) {
            return fail("midiStreamProperty(MIDIPROP_TIMEDIV) failed", result);
        }
        // A new pass must never inherit the tempo left by the previous pass.
        MIDIPROPTEMPO default_tempo{};
        default_tempo.cbStruct = sizeof(default_tempo);
        default_tempo.dwTempo = 500000;
        result = midiStreamProperty(stream, reinterpret_cast<LPBYTE>(&default_tempo),
                                    MIDIPROP_SET | MIDIPROP_TEMPO);
        if (result != MMSYSERR_NOERROR) {
            return fail("midiStreamProperty(MIDIPROP_TEMPO) failed", result);
        }
        return true;
    }

    // Writes complete DWORD-aligned MIDIEVENTs into a fixed lifetime-stable buffer.
    [[nodiscard]] bool fill(Slot& slot) {
        auto& words = slot.words;
        std::size_t used = 0;
        const auto& events = tunes[active_tune].events;
        while (next_event < events.size()) {
            const auto& event = events[next_event];
            if (kSlotWords - used < 3) break;
            const bool long_message = (event.event_word & MEVT_F_LONG) != 0;
            if (!long_message) {
                words[used++] = event.delta_ticks;
                words[used++] = 0;
                words[used++] = event.event_word;
                ++next_event;
                continue;
            }

            const auto total = event.long_data.size() + (event.prepend_sysex_start ? 1u : 0u);
            const auto capacity = (kSlotWords - used - 3) * sizeof(DWORD);
            if (capacity == 0) break;
            const auto count = std::min(total - long_offset, capacity);
            words[used++] = long_offset == 0 ? event.delta_ticks : 0;
            words[used++] = 0;
            words[used++] = (event.event_word & 0xff000000u) | static_cast<DWORD>(count);
            auto* destination = reinterpret_cast<std::byte*>(words.data() + used);
            const auto padded = (count + 3u) & ~std::size_t{3};
            std::memset(destination, 0, padded);
            if (event.prepend_sysex_start && long_offset == 0) {
                destination[0] = std::byte{0xf0};
                if (count > 1) {
                    std::memcpy(destination + 1, event.long_data.data(), count - 1);
                }
            } else {
                const auto source_offset = long_offset - (event.prepend_sysex_start ? 1u : 0u);
                std::memcpy(destination, event.long_data.data() + source_offset, count);
            }
            used += padded / sizeof(DWORD);
            long_offset += count;
            if (long_offset == total) {
                long_offset = 0;
                ++next_event;
            }
        }
        if (used == 0) return false;
        slot.header = {};
        slot.header.lpData = reinterpret_cast<LPSTR>(words.data());
        slot.header.dwBufferLength = static_cast<DWORD>(used * sizeof(DWORD));
        slot.header.dwBytesRecorded = slot.header.dwBufferLength;
        slot.header.dwUser = reinterpret_cast<DWORD_PTR>(&slot);
        slot.header.dwFlags = MHDR_ISSTRM;
        return true;
    }

    [[nodiscard]] bool queue_more() {
        for (auto& slot : slots) {
            if (slot.queued || next_event == tunes[active_tune].events.size()) continue;
            if (slot.prepared) {
                return fail("MIDI header still prepared while refilling stream");
            }
            if (!fill(slot)) {
                return fail("MIDI event could not fit in an empty stream buffer");
            }
            slot.completed.store(false, std::memory_order_relaxed);
            auto result = midiOutPrepareHeader(reinterpret_cast<HMIDIOUT>(stream), &slot.header,
                                               sizeof(slot.header));
            if (result != MMSYSERR_NOERROR) {
                return fail("midiOutPrepareHeader failed", result);
            }
            slot.prepared = true;
            result = midiStreamOut(stream, &slot.header, sizeof(slot.header));
            if (result != MMSYSERR_NOERROR) {
                const auto unprepared = midiOutUnprepareHeader(reinterpret_cast<HMIDIOUT>(stream),
                                                                &slot.header, sizeof(slot.header));
                if (unprepared == MMSYSERR_NOERROR) slot.prepared = false;
                return fail("midiStreamOut failed", result);
            }
            slot.queued = true;
        }
        return true;
    }

    [[nodiscard]] bool stop(bool clear_on_success = true) {
        if (!stream) {
            active_tune = 0;
            paused = false;
            if (clear_on_success) clear_error();
            return true;
        }
        const auto stopped = midiStreamStop(stream);
        if (stopped != MMSYSERR_NOERROR) {
            return fail("midiStreamStop failed", stopped);
        }
        // midiOutReset releases sustain and sends note-offs on every MIDI channel.
        const auto reset = midiOutReset(reinterpret_cast<HMIDIOUT>(stream));
        MMRESULT unprepare_error = MMSYSERR_NOERROR;
        for (auto& slot : slots) {
            if (slot.prepared) {
                const auto result = midiOutUnprepareHeader(reinterpret_cast<HMIDIOUT>(stream),
                                                            &slot.header, sizeof(slot.header));
                if (result != MMSYSERR_NOERROR && unprepare_error == MMSYSERR_NOERROR) {
                    unprepare_error = result;
                } else if (result == MMSYSERR_NOERROR) {
                    slot.prepared = false;
                    slot.queued = false;
                    slot.completed.store(false, std::memory_order_relaxed);
                }
            }
        }
        if (reset != MMSYSERR_NOERROR) return fail("midiOutReset failed", reset);
        if (unprepare_error != MMSYSERR_NOERROR) {
            return fail("midiOutUnprepareHeader failed after stream stop", unprepare_error);
        }
        active_tune = 0;
        paused = false;
        next_event = 0;
        long_offset = 0;
        if (clear_on_success) clear_error();
        return true;
    }

    [[nodiscard]] bool start(std::uint8_t index) {
        if (!ensure_open() || !set_timing(tunes[index])) return false;
        active_tune = index;
        next_event = 0;
        long_offset = 0;
        paused = false;
        if (!queue_more()) {
            const auto* reason = error;
            const auto native = native_error;
            static_cast<void>(stop(false));
            return fail(reason, native);
        }
        const auto result = midiStreamRestart(stream);
        if (result != MMSYSERR_NOERROR) {
            static_cast<void>(stop(false));
            return fail("midiStreamRestart failed", result);
        }
        clear_error();
        return true;
    }

    std::array<Tune, kTuneCount + 1> tunes{};
    std::array<Slot, 2> slots{};
    HMIDISTRM stream = nullptr;
    std::size_t next_event = 0;
    std::size_t long_offset = 0;
    std::uint8_t active_tune = 0;
    unsigned volume = 100;
    bool muted = false;
    bool paused = false;
    const char* error = "";
    MMRESULT native_error = MMSYSERR_NOERROR;
};

MidiStreamPlayer::MidiStreamPlayer(const Assets& assets) : impl_(std::make_unique<Impl>(assets)) {}
MidiStreamPlayer::~MidiStreamPlayer() = default;

bool MidiStreamPlayer::play(std::uint8_t tune) {
    if (tune == 0) return stop();
    if (tune > kTuneCount) return impl_->fail("MIDI tune index must be 0..15");
    if (impl_->active_tune != 0 && !impl_->stop()) return false;
    return impl_->start(tune);
}

bool MidiStreamPlayer::stop() { return impl_->stop(); }

bool MidiStreamPlayer::pause() {
    if (impl_->active_tune == 0) return impl_->fail("No active MIDI tune to pause");
    if (impl_->paused) return true;
    const auto result = midiStreamPause(impl_->stream);
    if (result != MMSYSERR_NOERROR) return impl_->fail("midiStreamPause failed", result);
    impl_->paused = true;
    impl_->clear_error();
    return true;
}

bool MidiStreamPlayer::resume() {
    if (impl_->active_tune == 0) return impl_->fail("No active MIDI tune to resume");
    if (!impl_->paused) return true;
    const auto result = midiStreamRestart(impl_->stream);
    if (result != MMSYSERR_NOERROR) return impl_->fail("midiStreamRestart failed", result);
    impl_->paused = false;
    impl_->clear_error();
    return true;
}

bool MidiStreamPlayer::update() {
    if (impl_->active_tune == 0) return true;
    for (auto& slot : impl_->slots) {
        if (!slot.queued || !slot.completed.exchange(false, std::memory_order_acquire)) continue;
        const auto result = midiOutUnprepareHeader(reinterpret_cast<HMIDIOUT>(impl_->stream),
                                                   &slot.header, sizeof(slot.header));
        if (result != MMSYSERR_NOERROR) {
            slot.completed.store(true, std::memory_order_relaxed);
            return impl_->fail("midiOutUnprepareHeader failed after MOM_DONE", result);
        }
        slot.prepared = false;
        slot.queued = false;
    }
    if (impl_->next_event < impl_->tunes[impl_->active_tune].events.size()) {
        if (impl_->queue_more()) return true;
        const auto* reason = impl_->error;
        const auto native = impl_->native_error;
        static_cast<void>(impl_->stop(false));
        return impl_->fail(reason, native);
    }
    if (impl_->paused) return true;
    for (const auto& slot : impl_->slots) {
        if (slot.queued) return true;
    }
    const auto tune = impl_->active_tune;
    if (!impl_->stop()) return false;
    return impl_->start(tune);
}

bool MidiStreamPlayer::set_volume(unsigned percent) {
    if (percent > 100u) return impl_->fail("MIDI volume must be 0..100");
    if (!impl_->apply_volume(percent, impl_->muted)) return false;
    impl_->volume = percent;
    impl_->clear_error();
    return true;
}

bool MidiStreamPlayer::set_muted(bool muted) {
    if (!impl_->apply_volume(impl_->volume, muted)) return false;
    impl_->muted = muted;
    impl_->clear_error();
    return true;
}

std::string_view MidiStreamPlayer::last_error() const noexcept { return impl_->error; }
std::uint32_t MidiStreamPlayer::last_native_error() const noexcept { return impl_->native_error; }

std::span<const MidiStreamEvent> MidiStreamPlayer::events(std::uint8_t tune) const noexcept {
    if (tune == 0 || tune > kTuneCount) return {};
    return impl_->tunes[tune].events;
}

std::uint16_t MidiStreamPlayer::division(std::uint8_t tune) const noexcept {
    if (tune == 0 || tune > kTuneCount) return 0;
    return impl_->tunes[tune].division;
}

} // namespace n3d
