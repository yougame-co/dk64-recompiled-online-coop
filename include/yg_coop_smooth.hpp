#ifndef YG_COOP_SMOOTH_HPP
#define YG_COOP_SMOOTH_HPP

#include "yg_coop_net.hpp"
#include <algorithm>

namespace yg_coop {
// Both transports deliver accepted samples on the game thread. The relay worker
// uses this bounded inbox under its existing mutex; no allocation or waiting.
struct RemoteSample { Sample sample{}; uint32_t frame = 0; };
class SampleInbox {
public:
    void push(const Sample &sample, uint32_t frame) {
        if (size_ == entries_.size()) {
            begin_ = (begin_ + 1) % entries_.size();
            --size_;
        }
        entries_[(begin_ + size_++) % entries_.size()] = {sample, frame};
    }
    template<class Accept> void drain(Accept accept) {
        for (size_t i = 0; i < size_; ++i) {
            const auto &entry = entries_[(begin_ + i) % entries_.size()];
            accept(entry.sample, entry.frame);
        }
        size_ = begin_ = 0;
    }
private:
    std::array<RemoteSample, 32> entries_{};
    size_t begin_ = 0, size_ = 0;
};

enum class SmoothSnap : unsigned { spawn, map, kong, jump, backwards, local_spawn, peer_resume, count };
inline const char *smooth_snap_name(SmoothSnap reason) {
    constexpr const char *names[] = {"spawn", "map-change", "kong-change", "big-jump",
                                    "sender-frame-backwards", "local-spawn", "peer-resume"};
    return names[unsigned(reason)];
}
inline uint16_t smooth_get16(const uint8_t *p) { return uint16_t(unsigned(p[0]) * 256 + p[1]); }
inline void smooth_put16(uint8_t *p, uint16_t value) { p[0] = uint8_t(value >> 8); p[1] = uint8_t(value); }
inline void smooth_put_float(uint8_t *p, float value) {
    uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); put32(p, bits);
}
// DK64 stores angles in s16 fields but one revolution is 4096, not 65536.
inline int smooth_yaw_delta(int from, int to) { return ((to - from + 2048) & 4095) - 2048; }
inline double smooth_distance(const Sample &a, const Sample &b) {
    double squared = 0;
    for (size_t i = 0; i < 3; ++i) {
        const double delta = double(get_float(a.data() + YG_S_POSITION + 4 * i)) -
                             get_float(b.data() + YG_S_POSITION + 4 * i);
        squared += delta * delta;
    }
    return std::sqrt(squared);
}

class Smoother {
public:
    static constexpr double snap_units = 120.0;
    // Zero prediction is deliberate: exhausted buffers hold the exact newest
    // position, including during a long outage. This is within the <=4 cap.
    static constexpr unsigned extrapolation_cap = 0;
    // DK64 game ticks are 30 Hz (frame counter; walk dumps +5 s and +15 s are 300 apart), and both
    // transports send one sample per tick. Relay run 3 (11-02) still stalled 2-6 ticks a minute with one
    // tick of jitter, so two ticks keep the delay near 100 ms.
    static constexpr unsigned jitter_frames = 2;
    explicit Smoother(unsigned send_interval, bool logging = true)
        : delay_(send_interval + jitter_frames), logging_(logging) {}
    unsigned delay() const { return delay_; }
    uint64_t snaps() const { return total_snaps_; }
    const Sample &sample() const { return output_; }
    const Sample &newest() const { return history_[size_ - 1].sample; }

    void accept(const Sample &sample, uint32_t sender_frame, uint32_t consumer_frame) {
        SmoothSnap reason = SmoothSnap::count;
        if (!size_) reason = SmoothSnap::spawn;
        else {
            const auto &previous = history_[size_ - 1];
            if (sender_frame < previous.frame) reason = SmoothSnap::backwards;
            else if (get32(sample.data() + YG_S_MAP) != get32(previous.sample.data() + YG_S_MAP)) reason = SmoothSnap::map;
            else if (sample[YG_S_KONG] != previous.sample[YG_S_KONG]) reason = SmoothSnap::kong;
            else if (sample[YG_S_SPAWN] != previous.sample[YG_S_SPAWN]) reason = SmoothSnap::spawn;
            else if (get32(sample.data() + YG_S_MAP) != 0xFFFFFFFFu &&
                     smooth_distance(sample, previous.sample) > snap_units) reason = SmoothSnap::jump;
        }
        if (reason != SmoothSnap::count) {
            size_ = 0;
            mark_snap(reason, consumer_frame);
            anchor(sender_frame, consumer_frame);
        } else if (sender_frame == history_[size_ - 1].frame) {
            history_[size_ - 1].sample = sample;
            return;
        } else if (rendered_ && cursor_ > double(history_[size_ - 1].frame) + 4) {
            // Refill after a long starvation without chasing an obsolete clock.
            // This is a discontinuity, not extrapolation or an arrival-time lerp.
            size_ = 0;
            mark_snap(SmoothSnap::peer_resume, consumer_frame);
            anchor(sender_frame, consumer_frame);
        }
        if (size_ == history_.size()) {
            for (size_t i = 1; i < size_; ++i) history_[i - 1] = history_[i];
            --size_;
        }
        history_[size_++] = {sample, sender_frame};
    }

    void snap_to_newest(SmoothSnap reason, uint32_t frame) {
        if (!size_ || pending_snap_) return;
        const RemoteSample last = history_[size_ - 1];
        history_[0] = last; size_ = 1;
        anchor(last.frame, frame);
        mark_snap(reason, frame);
    }

    // Advance once per *game* frame; interpolation weights use sender ticks.
    // Keep a short history so the jitter buffer can bracket a tick even when
    // multiple newer samples were delivered together. Never mix actor metadata.
    void render(uint32_t frame, bool visible = true) {
        if (rendered_ && frame == rendered_frame_) return;
        if (!logged_config_ && logging_) {
            std::fprintf(stderr, "YG-COOP smooth-config delay-frames=%u jitter-frames=%u snap-units=120 extrapolation-cap=0 yaw-period=4096\n", delay_, jitter_frames);
            logged_config_ = true;
        }
        if (size_) {
            cursor_ += uint32_t(frame - clock_frame_);
            clock_frame_ = frame;
            const auto &last = history_[size_ - 1];
            output_ = last.sample; // animation/scale/hands/control always newest
            if (!pending_snap_ && cursor_ < last.frame) {
                const RemoteSample *left = &history_[0], *right = left;
                for (size_t i = 1; i < size_; ++i) {
                    right = &history_[i];
                    if (cursor_ <= right->frame) break;
                    left = right;
                }
                const double span = double(right->frame) - left->frame;
                const double alpha = span > 0 ? std::clamp((cursor_ - left->frame) / span, 0.0, 1.0) : 0.0;
                for (size_t i = 0; i < 3; ++i) {
                    const size_t offset = YG_S_POSITION + 4 * i;
                    const double a = get_float(left->sample.data() + offset);
                    const double b = get_float(right->sample.data() + offset);
                    smooth_put_float(output_.data() + offset, float(a + (b - a) * alpha));
                }
                const int a = smooth_get16(left->sample.data() + YG_S_ROTATION + 2);
                const int b = smooth_get16(right->sample.data() + YG_S_ROTATION + 2);
                const int yaw = int(std::lround(a + smooth_yaw_delta(a, b) * alpha)) & 4095;
                smooth_put16(output_.data() + YG_S_ROTATION + 2, uint16_t(yaw));
            }
            if (pending_snap_) {
                // A batch may contain more samples after a discontinuity. Snap
                // to its newest pose and refill, never rewind on the next frame.
                history_[0] = last; size_ = 1;
                anchor(history_[0].frame, frame);
            }
        }
        const bool measuring = size_ && visible && get32(output_.data() + YG_S_MAP) != 0xFFFFFFFFu;
        if (measuring && measured_previous_) {
            const double step = smooth_distance(output_, previous_output_);
            max_step_ = std::max(max_step_, step);
            if (step < 0.0001 && newest()[YG_S_MOVING] && !pending_snap_) ++stalls_;
        }
        previous_output_ = output_; measured_previous_ = measuring;
        pending_snap_ = false;
        rendered_ = true; rendered_frame_ = frame;
        if (frame % 60 == 0) {
            if (logging_) std::fprintf(stderr,
                "YG-COOP smooth frame=%u delay-frames=%u max-step=%.4f stalls=%llu snaps=%llu extrapolated=0\n",
                frame, delay_, max_step_, static_cast<unsigned long long>(stalls_),
                static_cast<unsigned long long>(window_snaps_));
            max_step_ = 0; stalls_ = window_snaps_ = 0;
        }
    }
private:
    void anchor(uint32_t sender_frame, uint32_t consumer_frame) {
        cursor_ = double(sender_frame) - delay_;
        clock_frame_ = consumer_frame;
    }
    void mark_snap(SmoothSnap reason, uint32_t frame) {
        pending_snap_ = true;
        ++total_snaps_; ++window_snaps_;
        const size_t i = unsigned(reason);
        if (logging_ && (!logged_snap_[i] || frame - last_snap_log_[i] >= 60)) {
            std::fprintf(stderr, "YG-COOP smooth-snap reason=%s\n", smooth_snap_name(reason));
            logged_snap_[i] = true; last_snap_log_[i] = frame;
        }
    }
    std::array<RemoteSample, 32> history_{};
    size_t size_ = 0;
    unsigned delay_;
    bool logging_, logged_config_ = false, pending_snap_ = false, rendered_ = false;
    bool measured_previous_ = false;
    uint32_t clock_frame_ = 0, rendered_frame_ = 0;
    double cursor_ = 0, max_step_ = 0;
    uint64_t total_snaps_ = 0, window_snaps_ = 0, stalls_ = 0;
    Sample output_{}, previous_output_{};
    std::array<bool, unsigned(SmoothSnap::count)> logged_snap_{};
    std::array<uint32_t, unsigned(SmoothSnap::count)> last_snap_log_{};
};
} // namespace yg_coop
#endif
