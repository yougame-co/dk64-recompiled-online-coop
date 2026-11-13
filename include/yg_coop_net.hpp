#ifndef YG_COOP_NET_HPP
#define YG_COOP_NET_HPP

// Shared by the native bridge and standalone test. No threads, DNS or TCP.
#include "yg_coop_wire.h"
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace yg_coop {
using Sample = std::array<uint8_t, YG_SAMPLE_SIZE>;
constexpr size_t header_size = 14;
using Packet = std::array<uint8_t, header_size + YG_SAMPLE_SIZE>;
constexpr uint8_t version = 1;

inline uint32_t get32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void put32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline float get_float(const uint8_t *p) {
    const uint32_t bits = get32(p);
    float value;
    static_assert(sizeof(value) == sizeof(bits), "requires IEEE float32");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline void clear_padding(Sample &s) {
    s[30] = s[31] = s[49] = s[50] = s[51] = 0;
}
template<class ReadByte> Sample read_sample(ReadByte read) {
    Sample s{};
    for (size_t i = 0; i < s.size(); ++i) s[i] = read(i);
    clear_padding(s);
    return s;
}
template<class WriteByte> void write_sample(const Sample &s, WriteByte write) {
    for (size_t i = 0; i < s.size(); ++i) write(i, s[i]);
}
inline bool valid_sample(const Sample &s) {
    const int32_t map = static_cast<int32_t>(get32(s.data() + YG_S_MAP));
    if (map < -1 || map > 255 || s[YG_S_KONG] >= 5 ||
        s[YG_S_MOVING] > 1 || (s[YG_S_NET_FLAGS] & ~3)) return false;
    // -1 is an explicit heartbeat during menus, transitions or missing player.
    if (map == -1) return true;
    for (size_t i : {size_t(YG_S_POSITION), size_t(YG_S_POSITION + 4),
                     size_t(YG_S_POSITION + 8), size_t(YG_S_ANIMATION_FRAME),
                     size_t(YG_S_ANIMATION_START), size_t(YG_S_ANIMATION_END),
                     size_t(YG_S_SCALE), size_t(YG_S_SCALE + 4), size_t(YG_S_SCALE + 8)}) {
        const float f = get_float(s.data() + i);
        if (!std::isfinite(f) || std::fabs(f) > 10000000.0f) return false;
    }
    return true;
}
inline bool valid_ram_address(uint32_t address, size_t ram_size) {
    // Patch statics live in extended RAM (e.g. 0x8093xxxx), beyond the N64's
    // original 8 MiB. Use the runtime's allocation, with subtraction bounds.
    return ram_size >= YG_SAMPLE_SIZE && address >= 0x80000000u &&
           uint64_t(address) - 0x80000000u <= ram_size - YG_SAMPLE_SIZE;
}
inline bool newer(uint32_t candidate, uint32_t previous) {
    const uint32_t delta = candidate - previous;
    return delta != 0 && delta < 0x80000000u;
}

enum class Drop : unsigned { none, size, magic, version, own_role, role, address, sample, stale, count };
inline const char *drop_name(Drop d) {
    constexpr const char *names[] = {"none", "size", "magic", "version", "own-role",
                                    "role", "address", "sample", "stale"};
    return names[unsigned(d)];
}
inline Packet encode(char role, uint32_t seq, uint32_t frame, const Sample &sample) {
    Packet p{};
    std::memcpy(p.data(), "YGC2", 4);
    p[4] = version; p[5] = uint8_t(role);
    put32(p.data() + 6, seq); put32(p.data() + 10, frame);
    Sample clean = sample;
    clear_padding(clean);
    std::memcpy(p.data() + header_size, clean.data(), clean.size());
    return p;
}
// Relay text payload: exactly the existing 82 bytes, as 164 lowercase hex digits.
inline std::string packet_hex(const Packet &packet) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(packet.size() * 2, '0');
    for (size_t i = 0; i < packet.size(); ++i) {
        out[2 * i] = digits[packet[i] >> 4];
        out[2 * i + 1] = digits[packet[i] & 15];
    }
    return out;
}
inline bool packet_unhex(const char *text, size_t size, Packet &packet) {
    if (!text || size != packet.size() * 2) return false;
    Packet decoded{};
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < decoded.size(); ++i) {
        const int high = nibble(text[2 * i]), low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        decoded[i] = uint8_t((high << 4) | low);
    }
    packet = decoded;
    return true;
}
inline Drop decode(const uint8_t *p, size_t size, char own_role,
                   uint32_t &seq, uint32_t &frame, Sample &sample) {
    if (size != Packet{}.size()) return Drop::size;
    if (std::memcmp(p, "YGC2", 4)) return Drop::magic;
    if (p[4] != version) return Drop::version;
    if (p[5] == uint8_t(own_role)) return Drop::own_role;
    if (p[5] != 'a' && p[5] != 'b') return Drop::role;
    Sample decoded;
    std::memcpy(decoded.data(), p + header_size, decoded.size());
    if (!valid_sample(decoded)) return Drop::sample;
    clear_padding(decoded);
    seq = get32(p + 6); frame = get32(p + 10); sample = decoded;
    return Drop::none;
}

inline bool parse_port(const char *text, uint16_t &port) {
    if (!text || !*text) return false;
    uint32_t value = 0;
    for (const char *c = text; *c; ++c) {
        if (*c < '0' || *c > '9') return false;
        value = value * 10 + unsigned(*c - '0');
        if (value > 65535) return false;
    }
    if (!value) return false;
    port = uint16_t(value);
    return true;
}
inline bool loopback_address(const char *address, uint16_t port, sockaddr_in &out) {
    if (!address || std::strcmp(address, "127.0.0.1") || !port) return false;
    out = {};
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = htonl(0x7F000001u);
    out.sin_port = htons(port);
    return true;
}
struct Config {
    int mode = YG_NET_ECHO;
    char role = 0;
    uint16_t port = 0, peer_port = 0;
    sockaddr_in local{}, peer{};
};
inline Config config(const char *enabled, const char *role, const char *port, const char *peer_port,
                     const char *local_address = "127.0.0.1", const char *peer_address = "127.0.0.1") {
    Config c;
    if (!enabled || std::strcmp(enabled, "1")) return c;
    c.mode = YG_NET_ERROR;
    if (!role || (std::strcmp(role, "a") && std::strcmp(role, "b")) ||
        !parse_port(port, c.port) || !parse_port(peer_port, c.peer_port) ||
        c.port == c.peer_port || !loopback_address(local_address, c.port, c.local) ||
        !loopback_address(peer_address, c.peer_port, c.peer)) return c;
    c.role = role[0];
    c.mode = c.role == 'a' ? YG_NET_ROLE_A : YG_NET_ROLE_B;
    return c;
}

class Transport {
public:
    explicit Transport(Config c) : config_(c) {
        if (c.mode == YG_NET_ECHO) return;
        if (c.mode == YG_NET_ERROR ||
            (c.mode != YG_NET_ROLE_A && c.mode != YG_NET_ROLE_B) ||
            c.role != (c.mode == YG_NET_ROLE_A ? 'a' : 'b') ||
            !c.port || !c.peer_port || c.port == c.peer_port ||
            c.local.sin_family != AF_INET || c.peer.sin_family != AF_INET ||
            c.local.sin_addr.s_addr != htonl(0x7F000001u) ||
            c.peer.sin_addr.s_addr != htonl(0x7F000001u) ||
            c.local.sin_port != htons(c.port) || c.peer.sin_port != htons(c.peer_port)) {
            config_.mode = YG_NET_ERROR;
            std::fprintf(stderr, "YG-COOP error net-config role/ports/address invalid (require a|b, distinct ports 1..65535, 127.0.0.1 only)\n");
            return;
        }
#ifdef _WIN32
        WSADATA data{};
        const int startup = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (startup) { fail("startup", startup); return; }
        winsock_started_ = true;
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ == INVALID_SOCKET) { fail("socket"); return; }
        u_long nonblocking = 1;
        if (::ioctlsocket(fd_, FIONBIO, &nonblocking) ||
            !::SetHandleInformation(reinterpret_cast<HANDLE>(fd_), HANDLE_FLAG_INHERIT, 0)) {
            fail("nonblocking"); return;
        }
#else
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { fail("socket"); return; }
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) { fail("nonblocking"); return; }
#endif
        // Deliberately no SO_REUSEADDR: a second owner of the port is an error.
        if (::bind(fd_, reinterpret_cast<const sockaddr *>(&c.local), sizeof(c.local)) < 0) {
            fail("bind"); return;
        }
        std::printf("YG-COOP net-start role=%c bind=127.0.0.1:%u peer=127.0.0.1:%u version=%u packet-bytes=%zu\n",
                    c.role, c.port, c.peer_port, version, Packet{}.size());
        std::fflush(stdout);
    }
#ifdef _WIN32
    ~Transport() {
        if (fd_ != INVALID_SOCKET) ::closesocket(fd_);
        if (winsock_started_) ::WSACleanup();
    }
#else
    ~Transport() { if (fd_ >= 0) ::close(fd_); }
#endif
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;
    int mode() const { return config_.mode; }
    char role() const { return config_.role; }
    int error_number() const { return error_; }
    const Sample &remote() const { return remote_; }
    uint32_t newest_seq() const { return newest_seq_; }
    uint64_t received() const { return received_; }
    uint64_t dropped() const { return dropped_; }

    uint32_t step(const Sample &local, uint32_t frame) {
        return step(local, frame, [](const Sample &, uint32_t) {});
    }
    // Visit every accepted sample, including packets drained in one game frame.
    template<class Accept> uint32_t step(const Sample &local, uint32_t frame, Accept accept) {
#ifdef _WIN32
        if (fd_ == INVALID_SOCKET) return 0;
#else
        if (fd_ < 0) return 0;
#endif
        // Guard against accidental duplicate calls from the same game frame.
        if (stepped_ && frame == stepped_frame_) return status(frame, false);
        stepped_ = true; stepped_frame_ = frame;
        const Packet p = encode(config_.role, ++send_seq_, frame, local);
#ifdef _WIN32
        const int sent = ::sendto(fd_, reinterpret_cast<const char *>(p.data()), int(p.size()), 0,
                                  reinterpret_cast<const sockaddr *>(&config_.peer), sizeof(config_.peer));
        if (sent != int(p.size()) && !logged_send_) {
            logged_send_ = true;
            std::fprintf(stderr, "YG-COOP error net-send errno=%d\n", ::WSAGetLastError());
        }
#else
        const auto sent = ::sendto(fd_, p.data(), p.size(), 0,
                                  reinterpret_cast<const sockaddr *>(&config_.peer), sizeof(config_.peer));
        if (sent != static_cast<ssize_t>(p.size()) && !logged_send_) {
            logged_send_ = true;
            std::fprintf(stderr, "YG-COOP error net-send errno=%d\n", errno);
        }
#endif
        if (!logged_local_packet_) {
            logged_local_packet_ = true;
            log_sample("local-first-packet", local, send_seq_, frame);
        }
        if (!logged_local_ && get32(local.data() + YG_S_MAP) != 0xFFFFFFFFu) {
            logged_local_ = true;
            log_sample("local-sample", local, send_seq_, frame);
        }
        bool fresh = false;
        // Read until EAGAIN; a large buffer distinguishes oversized datagrams.
        for (;;) {
            std::array<uint8_t, 2048> bytes{};
            sockaddr_in from{};
#ifdef _WIN32
            int from_size = sizeof(from);
            const int n = ::recvfrom(fd_, reinterpret_cast<char *>(bytes.data()), int(bytes.size()), 0,
                                     reinterpret_cast<sockaddr *>(&from), &from_size);
            if (n < 0) {
                const int error = ::WSAGetLastError();
                if (error == WSAEINTR) continue;
                if (error == WSAEMSGSIZE) { drop(Drop::size); continue; }
                if (error != WSAEWOULDBLOCK && !logged_receive_) {
                    logged_receive_ = true;
                    std::fprintf(stderr, "YG-COOP error net-receive errno=%d\n", error);
                }
                break;
            }
#else
            socklen_t from_size = sizeof(from);
            const ssize_t n = ::recvfrom(fd_, bytes.data(), bytes.size(), 0,
                                        reinterpret_cast<sockaddr *>(&from), &from_size);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK && !logged_receive_) {
                    logged_receive_ = true;
                    std::fprintf(stderr, "YG-COOP error net-receive errno=%d\n", errno);
                }
                break;
            }
#endif
            if (from_size != sizeof(from) || from.sin_family != AF_INET ||
                from.sin_addr.s_addr != config_.peer.sin_addr.s_addr ||
                from.sin_port != config_.peer.sin_port) { drop(Drop::address); continue; }
            uint32_t seq = 0, sender_frame = 0;
            Sample decoded{};
            const Drop why = decode(bytes.data(), size_t(n), config_.role, seq, sender_frame, decoded);
            if (why != Drop::none) { drop(why); continue; }
            if (have_remote_ && !newer(seq, newest_seq_)) { drop(Drop::stale); continue; }
            accept(decoded, sender_frame);
            remote_ = decoded;
            newest_seq_ = seq; remote_frame_ = sender_frame;
            have_remote_ = true; last_receive_frame_ = frame;
            ++received_; fresh = true;
            if (!logged_remote_packet_) {
                logged_remote_packet_ = true;
                log_sample("remote-first-packet", remote_, seq, sender_frame);
            }
            // Menus send explicit invalid samples; log the first renderable
            // remote so positions can be compared against the peer's log.
            if (!logged_remote_ && get32(remote_.data() + YG_S_MAP) != 0xFFFFFFFFu) {
                logged_remote_ = true;
                log_sample("remote-sample", remote_, seq, sender_frame);
            }
        }
        if (fresh && !peer_live_) {
            peer_live_ = true;
            logged_initial_loss_ = true;
            std::printf("YG-COOP peer-found frame=%u seq=%u\n", frame, newest_seq_);
        } else if (peer_live_ && frame - last_receive_frame_ >= 30) {
            peer_live_ = false;
            std::printf("YG-COOP peer-lost frame=%u\n", frame);
        }
        if (!have_remote_ && !logged_initial_loss_ && frame >= 30) {
            logged_initial_loss_ = true;
            std::printf("YG-COOP peer-lost frame=%u\n", frame);
        }
        if (frame % 60 == 0) {
            const auto *s = remote_.data();
            std::printf("YG-COOP net frame=%u role=%c received=%llu dropped=%llu newest-seq=%u frame-gap=%d remote-frame=%u remote-map=%d remote=<%.2f,%.2f,%.2f> send-seq=%u local-map=%d local=<%.2f,%.2f,%.2f> local-kong=%u local-animation=%u\n",
                        frame, config_.role, static_cast<unsigned long long>(received_),
                        static_cast<unsigned long long>(dropped_), newest_seq_,
                        have_remote_ ? int(frame - last_receive_frame_) : -1, remote_frame_,
                        have_remote_ ? int32_t(get32(s + YG_S_MAP)) : -1,
                        get_float(s + YG_S_POSITION), get_float(s + YG_S_POSITION + 4),
                        get_float(s + YG_S_POSITION + 8), send_seq_,
                        int32_t(get32(local.data() + YG_S_MAP)),
                        get_float(local.data() + YG_S_POSITION),
                        get_float(local.data() + YG_S_POSITION + 4),
                        get_float(local.data() + YG_S_POSITION + 8), local[YG_S_KONG],
                        (unsigned(local[YG_S_ANIMATION]) << 8) | local[YG_S_ANIMATION + 1]);
            std::fflush(stdout);
        }
        return status(frame, fresh);
    }
private:
    uint32_t status(uint32_t frame, bool fresh) const {
        return (have_remote_ ? YG_NET_HAS_SAMPLE : 0) | (fresh ? YG_NET_FRESH : 0) |
               (have_remote_ && frame - last_receive_frame_ < 30 ? YG_NET_LIVE : 0);
    }
#ifdef _WIN32
    void fail(const char *operation, int error = ::WSAGetLastError()) {
        error_ = error;
        std::fprintf(stderr, "YG-COOP error net-config %s errno=%d\n", operation, error_);
        if (fd_ != INVALID_SOCKET) ::closesocket(fd_);
        fd_ = INVALID_SOCKET; config_.mode = YG_NET_ERROR;
    }
#else
    void fail(const char *operation) {
        error_ = errno;
        std::fprintf(stderr, "YG-COOP error net-config %s errno=%d\n", operation, error_);
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1; config_.mode = YG_NET_ERROR;
    }
#endif
    void drop(Drop why) {
        ++dropped_;
        const auto i = unsigned(why);
        if (!logged_drop_[i]) {
            logged_drop_[i] = true;
            std::printf("YG-COOP net-drop kind=%s\n", drop_name(why));
        }
    }
    void log_sample(const char *label, const Sample &s, uint32_t seq, uint32_t frame) const {
        std::printf("YG-COOP %s role=%c seq=%u sender-frame=%u map=%d position=<%.2f,%.2f,%.2f> kong=%u animation=%u\n",
                    label, label[0] == 'l' ? config_.role : (config_.role == 'a' ? 'b' : 'a'),
                    seq, frame, int32_t(get32(s.data() + YG_S_MAP)),
                    get_float(s.data() + YG_S_POSITION), get_float(s.data() + YG_S_POSITION + 4),
                    get_float(s.data() + YG_S_POSITION + 8), s[YG_S_KONG],
                    (unsigned(s[YG_S_ANIMATION]) << 8) | s[YG_S_ANIMATION + 1]);
        std::fflush(stdout);
    }
    Config config_;
#ifdef _WIN32
    SOCKET fd_ = INVALID_SOCKET;
    bool winsock_started_ = false;
    int error_ = 0;
#else
    int fd_ = -1, error_ = 0;
#endif
    Sample remote_{};
    uint32_t send_seq_ = 0, newest_seq_ = 0, remote_frame_ = 0;
    uint32_t last_receive_frame_ = 0, stepped_frame_ = 0;
    uint64_t received_ = 0, dropped_ = 0;
    bool have_remote_ = false, peer_live_ = false, stepped_ = false;
    bool logged_local_ = false, logged_remote_ = false, logged_send_ = false, logged_receive_ = false;
    bool logged_local_packet_ = false, logged_remote_packet_ = false, logged_initial_loss_ = false;
    std::array<bool, unsigned(Drop::count)> logged_drop_{};
};
} // namespace yg_coop
#endif
