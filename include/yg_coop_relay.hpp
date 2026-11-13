#ifndef YG_COOP_RELAY_HPP
#define YG_COOP_RELAY_HPP

#include "yg_coop_net.hpp"
#include "yg_coop_smooth.hpp"
#include "yougame_net.h"
#include <atomic>
#include <chrono>
#include <mutex>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#else
#include <poll.h>
#endif
#include <string_view>
#include <thread>

namespace yg_coop {
#ifdef _WIN32
// Anonymous stdio pipes are synchronous. Peek before reading and use byte-mode
// PIPE_NOWAIT for writing so stop/shutdown retain their existing time bounds.
inline HANDLE relay_handle(int fd) {
    return reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
}
inline int relay_wait_input(unsigned timeout_ms) {
    const HANDLE input = relay_handle(0);
    if (::GetFileType(input) != FILE_TYPE_PIPE) return 1; // file or NUL
    DWORD available = 0;
    if (!::PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
        if (::GetLastError() == ERROR_BROKEN_PIPE) return 1;
        errno = EIO; return -1;
    }
    if (available) return 1;
    ::Sleep(timeout_ms);
    return 0;
}
inline int relay_read_input(char *bytes, unsigned size) {
    const HANDLE input = relay_handle(0);
    if (::GetFileType(input) == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (!::PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
            if (::GetLastError() == ERROR_BROKEN_PIPE) return 0;
            errno = EIO; return -1;
        }
        if (!available) { errno = EAGAIN; return -1; }
        if (size > available) size = available;
    }
    return ::_read(0, bytes, size);
}
inline int relay_write_output(int fd, const char *bytes, unsigned size) {
    DWORD written = 0;
    const BOOL ok = ::WriteFile(relay_handle(fd), bytes, size, &written, nullptr);
    if (ok && written) return int(written);
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    errno = (ok || error == ERROR_NO_DATA) ? EAGAIN : EPIPE;
    return -1;
}
#endif
inline bool app_stdio_enabled() {
    const char *value = std::getenv("YOUGAME_STDIO");
    return value && std::strcmp(value, "1") == 0;
}
inline bool relay_enabled() {
    const char *value = std::getenv("YG_COOP_NET");
    return value ? std::strcmp(value, "relay") == 0 : app_stdio_enabled();
}

// Run before main, only in relay mode. Every existing printf/iostream/recomp_puts
// now goes to stderr, including partial lines. Only the SDK worker owns this dup.
inline int isolate_relay_stdout() {
    if (!relay_enabled()) return -1;
    std::fflush(stdout);
#ifdef _WIN32
    if (::_setmode(0, _O_BINARY) < 0 || ::_setmode(1, _O_BINARY) < 0) return -1;
    const int protocol = ::_dup(1);
    if (protocol < 0) return -1;
    const HANDLE output = relay_handle(protocol);
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!::SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0) ||
        (::GetFileType(output) == FILE_TYPE_PIPE && !::SetNamedPipeHandleState(output, &mode, nullptr, nullptr)) ||
        ::_dup2(2, 1) < 0 || ::_setmode(1, _O_BINARY) < 0) {
        ::_close(protocol);
        return -1;
    }
    // The Windows CRT treats _IOLBF as full buffering. Flush each diagnostic
    // immediately; protocol output remains on the worker's separate raw fd.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    const int protocol = ::dup(STDOUT_FILENO);
    if (protocol < 0) return -1;
    if (::fcntl(protocol, F_SETFD, FD_CLOEXEC) < 0 ||
        ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        ::close(protocol);
        return -1;
    }
    // recomp_puts emits characters through stdout. Preserve prompt delivery of
    // its newline-terminated autopilot/input logs after redirecting that stream.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const int flags = ::fcntl(protocol, F_GETFL, 0);
    if (flags < 0 || ::fcntl(protocol, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(protocol);
        return -1;
    }
#endif
    return protocol;
}

// A small top-level JSON field reader for our own mpTicket answer. String values
// themselves are decoded by the native client's public ygnet_json_str helper.
inline std::string_view json_field(std::string_view object, std::string_view key) {
    size_t i = 0;
    auto space = [&] { while (i < object.size() && object[i] <= ' ') ++i; };
    auto string_end = [&]() -> bool {
        if (i >= object.size() || object[i++] != '"') return false;
        while (i < object.size()) {
            char c = object[i++];
            if (c == '"') return true;
            if (c == '\\') { if (i == object.size()) return false; ++i; }
        }
        return false;
    };
    space();
    if (i == object.size() || object[i++] != '{') return {};
    for (;;) {
        space();
        const size_t start_key = i;
        if (!string_end()) return {};
        const auto name = object.substr(start_key + 1, i - start_key - 2);
        space();
        if (i == object.size() || object[i++] != ':') return {};
        space();
        const size_t start = i;
        int depth = 0;
        while (i < object.size()) {
            char c = object[i];
            if (c == '"') { if (!string_end()) return {}; continue; }
            if (c == '[' || c == '{') ++depth;
            if ((c == ',' || c == '}') && depth == 0) break;
            if (c == ']' || c == '}') --depth;
            ++i;
        }
        size_t end = i;
        while (end > start && object[end - 1] <= ' ') --end;
        if (name == key) return object.substr(start, end - start);
        if (i == object.size() || object[i++] != ',') return {};
    }
}

// The generic two-player room broadcasts the same ordered players array to
// everyone. welcome.me identifies us; neither queue slot nor host is a seat.
inline char room_role(std::string_view start, std::string_view me) {
    if (me.empty()) return 0;
    const auto players = json_field(start, "players");
    if (players.size() < 2 || players.front() != '[' || players.back() != ']') return 0;
    std::array<std::string, 2> ids;
    size_t i = 1;
    for (auto &id : ids) {
        while (i < players.size() && players[i] <= ' ') ++i;
        const size_t begin = i;
        if (i >= players.size() || players[i] != '{') return 0;
        int depth = 0;
        bool quoted = false, escaped = false;
        for (; i < players.size(); ++i) {
            const char c = players[i];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') ++depth;
            else if ((c == '}' || c == ']') && --depth == 0) { ++i; break; }
        }
        if (depth || quoted) return 0;
        char decoded[257];
        const long n = ygnet_json_str(players.data() + begin, i - begin, "id", decoded, sizeof(decoded));
        if (n <= 0) return 0;
        id.assign(decoded, size_t(n));
        while (i < players.size() && players[i] <= ' ') ++i;
        if (&id == &ids[0]) {
            if (i >= players.size() || players[i++] != ',') return 0;
        }
    }
    if (i != players.size() - 1 || ids[0] == ids[1]) return 0;
    return ids[0] == me ? 'a' : ids[1] == me ? 'b' : 0;
}

class RelayTransport {
    using Clock = std::chrono::steady_clock;
public:
    explicit RelayTransport(int protocol_fd,
        // 300 s, not 90: on a loaded machine one copy reached the relay three minutes after the other
        // (in-app run 4, 11-11), and the first copy had already given up.
        std::chrono::milliseconds setup_timeout = std::chrono::seconds(300))
        : output_(protocol_fd), setup_timeout_(setup_timeout) {
        const char *role = std::getenv("YG_COOP_ROLE");
        if (output_ < 0 || !app_stdio_enabled() ||
            (role && std::strcmp(role, "a") && std::strcmp(role, "b"))) {
            std::fprintf(stderr, "YG-COOP error relay-config\n");
            return;
        }
        if (role) set_role(role[0], "env");
        std::fprintf(stderr, "YG-COOP relay-rate max-hz=30 latest-frame-only encoding=hex packet-bytes=82\n");
        worker_ = std::thread([this] { run(); });
    }
    ~RelayTransport() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
#ifdef _WIN32
        if (output_ >= 0) ::_close(output_);
#else
        if (output_ >= 0) ::close(output_);
#endif
    }
    RelayTransport(const RelayTransport &) = delete;
    RelayTransport &operator=(const RelayTransport &) = delete;
    int mode() const { return mode_.load(); }
    char role() const { return role_.load(); }
    const Sample &remote() const { return remote_; }

    uint32_t step(const Sample &local, uint32_t frame) {
        return step(local, frame, [](const Sample &, uint32_t) {});
    }
    template<class Accept> uint32_t step(const Sample &local, uint32_t frame, Accept accept) {
        if (mode() == YG_NET_ERROR) return 0;
        if (stepped_ && frame == stepped_frame_) return status(frame, false);
        stepped_ = true; stepped_frame_ = frame;
        bool fresh = false;
        SampleInbox incoming;
        // This lock never waits on the broker, stdin, stdout or another thread.
        std::unique_lock lock(mail_mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            mail_local_ = local; mail_frame_ = frame; mail_have_local_ = true;
            if (mail_have_remote_ && (!have_remote_ || newer(mail_seq_, newest_seq_))) {
                incoming = mail_samples_;
                mail_samples_ = SampleInbox{};
                remote_ = mail_remote_; newest_seq_ = mail_seq_;
                remote_frame_ = mail_remote_frame_;
                have_remote_ = true; last_receive_frame_ = frame; fresh = true;
                if (!logged_remote_packet_) {
                    logged_remote_packet_ = true;
                    log_sample("remote-first-packet", remote_, newest_seq_, remote_frame_);
                }
                if (!logged_remote_ && get32(remote_.data()) != 0xFFFFFFFFu) {
                    logged_remote_ = true;
                    log_sample("remote-sample", remote_, newest_seq_, remote_frame_);
                }
            }
            lock.unlock();
        }
        incoming.drain(accept);
        if (fresh && !peer_live_) {
            peer_live_ = true; logged_initial_loss_ = true;
            std::fprintf(stderr, "YG-COOP peer-found frame=%u seq=%u\n", frame, newest_seq_);
        } else if (peer_live_ && frame - last_receive_frame_ >= 30) {
            peer_live_ = false;
            std::fprintf(stderr, "YG-COOP peer-lost frame=%u\n", frame);
        }
        if (!have_remote_ && !logged_initial_loss_ && frame >= 30) {
            logged_initial_loss_ = true;
            std::fprintf(stderr, "YG-COOP peer-lost frame=%u\n", frame);
        }
        if (frame % 60 == 0) {
            std::fprintf(stderr, "YG-COOP relay frame=%u role=%c received=%llu dropped=%llu newest-seq=%u frame-gap=%d remote-frame=%u remote-map=%d remote=<%.2f,%.2f,%.2f> send-seq=%u local-map=%d local=<%.2f,%.2f,%.2f> local-kong=%u local-animation=%u rtt-ms=unavailable\n",
                frame, role(), static_cast<unsigned long long>(received_.load()),
                static_cast<unsigned long long>(dropped_.load()), newest_seq_,
                have_remote_ ? int(frame - last_receive_frame_) : -1, remote_frame_,
                have_remote_ ? int32_t(get32(remote_.data())) : -1,
                get_float(remote_.data() + YG_S_POSITION), get_float(remote_.data() + YG_S_POSITION + 4),
                get_float(remote_.data() + YG_S_POSITION + 8), sent_.load(), int32_t(get32(local.data())),
                get_float(local.data() + YG_S_POSITION), get_float(local.data() + YG_S_POSITION + 4),
                get_float(local.data() + YG_S_POSITION + 8), local[YG_S_KONG],
                unsigned(local[YG_S_ANIMATION]) * 256 + local[YG_S_ANIMATION + 1]);
        }
        return status(frame, fresh);
    }
private:
    void set_role(char value, const char *source) {
        role_ = value;
        mode_ = value == 'a' ? YG_NET_ROLE_A : YG_NET_ROLE_B;
        std::fprintf(stderr, "YG-COOP relay-start role=%c source=%s\n", value, source);
    }
    void start_room() {
        if (room_started_ || pending_start_.empty()) return;
        if (!role()) {
            if (me_.empty()) return;
            const char value = room_role(pending_start_, me_);
            if (!value) { fail("role"); return; }
            set_role(value, "room");
        }
        room_started_ = true;
        pending_start_.clear();
    }
    uint32_t status(uint32_t frame, bool fresh) const {
        return (have_remote_ ? YG_NET_HAS_SAMPLE : 0) | (fresh ? YG_NET_FRESH : 0) |
            (have_remote_ && frame - last_receive_frame_ < 30 ? YG_NET_LIVE : 0);
    }
    void log_sample(const char *label, const Sample &sample, uint32_t seq, uint32_t frame) const {
        std::fprintf(stderr, "YG-COOP %s role=%c seq=%u sender-frame=%u map=%d position=<%.2f,%.2f,%.2f> kong=%u animation=%u\n",
            label, label[0] == 'l' ? role() : (role() == 'a' ? 'b' : 'a'), seq, frame,
            int32_t(get32(sample.data())), get_float(sample.data() + YG_S_POSITION),
            get_float(sample.data() + YG_S_POSITION + 4), get_float(sample.data() + YG_S_POSITION + 8),
            sample[YG_S_KONG], unsigned(sample[YG_S_ANIMATION]) * 256 + sample[YG_S_ANIMATION + 1]);
    }
    static int write_line(void *user, const char *line, size_t size) {
        auto &self = *static_cast<RelayTransport *>(user);
        if (self.write_broken_) return -1;
        const auto deadline = Clock::now() + std::chrono::milliseconds(250);
        size_t offset = 0;
        while (offset < size && (!self.stop_ || self.closing_)) {
#ifdef _WIN32
            const int n = relay_write_output(self.output_, line + offset, unsigned(size - offset));
#else
            const ssize_t n = ::write(self.output_, line + offset, size - offset);
#endif
            if (n > 0) { offset += size_t(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && Clock::now() < deadline) {
#ifdef _WIN32
                ::Sleep(5);
#else
                pollfd fd{self.output_, POLLOUT, 0};
                ::poll(&fd, 1, 5);
#endif
                continue;
            }
            self.failed_ = true;
            self.mode_ = YG_NET_ERROR;
            self.write_broken_ = offset != 0; // Never append JSON to a partial line.
            std::fprintf(stderr, "YG-COOP error relay-write\n");
            return -1; // End this worker: never append a new request to a partial line.
        }
        if (offset != size && offset) self.write_broken_ = true;
        return offset == size ? 0 : -1;
    }
    static void on_line(void *user, char *line, size_t size) {
        auto &self = *static_cast<RelayTransport *>(user);
        if (self.closing_ || self.failed_ || self.stop_ ||
            size < 3 || std::memcmp(line, "yg ", 3) || self.ticket_id_ <= 0) return;
        const std::string_view json(line + 3, size - 3);
        if (json_field(json, "re") != std::to_string(self.ticket_id_)) return;
        self.ticket_id_ = 0;
        const auto result = json_field(json, "result");
        char ticket[4097];
        const long n = ygnet_json_str(result.data(), result.size(), "ticket", ticket, sizeof(ticket));
        if (json_field(json, "ok") != "true" || n <= 0) {
            self.failed_ = true;
            self.mode_ = YG_NET_ERROR;
            // Log only known codes, never an arbitrary app/server string.
            char code[33];
            const long length = ygnet_json_str(json.data(), json.size(), "code", code, sizeof(code));
            const char *safe = length > 0 &&
                (!std::strcmp(code, "staging_access") || !std::strcmp(code, "ticket_failed") ||
                 !std::strcmp(code, "unavailable")) ? code : "unknown";
            std::fprintf(stderr, "YG-COOP error relay-ticket code=%s\n", safe);
            return;
        }
        self.ticket_.assign(ticket, size_t(n));
        std::fprintf(stderr, "YG-COOP relay-ticket ok\n");
        self.open_queue();
    }
    void open_queue() {
        queue_conn_ = ygnet_open(net_, ticket_.c_str(), nullptr);
        if (queue_conn_ < 0) fail("open-queue");
    }
    static void on_open(void *user, int conn) {
        auto &self = *static_cast<RelayTransport *>(user);
        if (self.closing_ || self.failed_ || self.stop_) return;
        if (conn == self.room_conn_) {
            self.room_open_ = true;
            std::fprintf(stderr, "YG-COOP relay-open ok\n");
            // Room lifecycle: ready is a control frame, sent once before samples.
            if (ygnet_send(self.net_, conn, "{\"t\":\"ready\"}") < 0) self.fail("ready");
            self.next_send_ = Clock::now() + std::chrono::microseconds(33334);
        }
    }
    static void on_message(void *user, int conn, const char *data, size_t size) {
        auto &self = *static_cast<RelayTransport *>(user);
        if (self.closing_ || self.failed_ || self.stop_) return;
        char type[32];
        if (ygnet_json_str(data, size, "t", type, sizeof(type)) < 0) return;
        if (conn == self.queue_conn_ && std::strcmp(type, "matched") == 0 && !self.room_conn_) {
            char room[13];
            if (ygnet_json_str(data, size, "room", room, sizeof(room)) < 0) { self.fail("matched"); return; }
            ygnet_open_opts opts{}; opts.room = room;
            self.room_conn_ = ygnet_open(self.net_, self.ticket_.c_str(), &opts);
            if (self.room_conn_ < 0) self.fail("open-room");
            // netClose uses the public header even when the queue server closes first.
            ygnet_close(self.net_, self.queue_conn_, 1000, nullptr);
            return;
        }
        if (conn != self.room_conn_) return;
        if (std::strcmp(type, "welcome") == 0 && !self.room_started_) {
            char me[257];
            const long n = ygnet_json_str(data, size, "me", me, sizeof(me));
            if (n > 0) self.me_.assign(me, size_t(n));
            self.start_room();
        }
        if (std::strcmp(type, "start") == 0 && !self.room_started_) {
            self.pending_start_.assign(data, size);
            self.start_room();
        }
        if (std::strcmp(type, "msg")) return;
        if (!self.room_started_ || !self.role()) return;
        char hex[165]; Packet packet{};
        const long n = ygnet_json_str(data, size, "d", hex, sizeof(hex));
        if (n != 164 || !packet_unhex(hex, size_t(n), packet)) { ++self.dropped_; return; }
        Sample sample{}; uint32_t seq = 0, frame = 0;
        const Drop why = decode(packet.data(), packet.size(), self.role(), seq, frame, sample);
        if (why != Drop::none || (self.worker_have_remote_ && !newer(seq, self.worker_seq_))) {
            ++self.dropped_; return;
        }
        self.worker_have_remote_ = true; self.worker_seq_ = seq;
        {
            std::lock_guard lock(self.mail_mutex_);
            self.mail_remote_ = sample; self.mail_seq_ = seq;
            self.mail_remote_frame_ = frame; self.mail_have_remote_ = true;
            self.mail_samples_.push(sample, frame);
        }
        ++self.received_;
    }
    static void on_error(void *user, int conn, const char *, const char *) {
        auto &self = *static_cast<RelayTransport *>(user);
        if (self.closing_) return; // Retry a refused close while draining below.
        // Queue shutdown after matched can race netClose; the room is independent.
        if (conn == self.queue_conn_ && self.room_conn_ > 0) return;
        self.fail("broker"); // Never print server-supplied error text or credentials.
    }
    static void on_close(void *user, int conn, int code, const char *) {
        auto &self = *static_cast<RelayTransport *>(user);
        if (conn == self.room_conn_) {
            self.room_conn_ = 0;
            self.room_open_ = false;
            if (self.closing_) return;
            self.failed_ = true;
            self.mode_ = YG_NET_ERROR;
            std::fprintf(stderr, "YG-COOP error relay-close code=%d\n", code);
        } else if (conn == self.queue_conn_) {
            self.queue_conn_ = 0;
            if (!self.closing_ && self.room_conn_ <= 0) self.fail("queue-close");
        }
    }
    void fail(const char *what) {
        failed_ = true;
        mode_ = YG_NET_ERROR;
        std::fprintf(stderr, "YG-COOP error relay-%s\n", what);
    }
    void close_connections() {
        closing_ = true;
        mode_ = YG_NET_ERROR;
        ticket_id_ = 0;
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while ((queue_conn_ > 0 || room_conn_ > 0) && Clock::now() < deadline) {
            for (int *conn : {&queue_conn_, &room_conn_}) {
                if (*conn <= 0) continue;
                const int result = ygnet_close(net_, *conn, 1000, nullptr);
                if (result == YGNET_ENOCONN || result == YGNET_EENDED) *conn = 0;
            }
            if (queue_conn_ <= 0 && room_conn_ <= 0) break;
#ifdef _WIN32
            const int ready = relay_wait_input(5);
#else
            pollfd fd{STDIN_FILENO, POLLIN, 0};
            const int ready = ::poll(&fd, 1, 5);
#endif
            if (ready < 0 && errno != EINTR) break;
            if (ready <= 0) continue;
            char bytes[8192];
            // Feed SDK acknowledgements/pending opens; callbacks ignore traffic.
            // Keep consuming even on SDK parse failure so the app's pipe drains.
            for (int batch = 0; batch < 32 && Clock::now() < deadline; ++batch) {
#ifdef _WIN32
                const int n = relay_read_input(bytes, sizeof(bytes));
#else
                const ssize_t n = ::read(STDIN_FILENO, bytes, sizeof(bytes));
#endif
                if (n > 0) { (void)ygnet_feed(net_, bytes, size_t(n)); }
                else if (n == 0) { ygnet_eof(net_); return; }
                else if (errno == EINTR) continue;
                else break;
            }
        }
    }
    void run() {
        ygnet_config cfg{};
        cfg.user = this; cfg.write = write_line; cfg.on_line = on_line;
        cfg.on_open = on_open; cfg.on_message = on_message;
        cfg.on_close = on_close; cfg.on_error = on_error;
        cfg.max_line = 2 * 1024 * 1024;
        net_ = ygnet_create(&cfg);
#ifdef _WIN32
        if (!net_ || ::_setmode(0, _O_BINARY) < 0) {
#else
        const int input_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
        if (!net_ || input_flags < 0 || ::fcntl(STDIN_FILENO, F_SETFL, input_flags | O_NONBLOCK) < 0) {
#endif
            fail("stdin");
            if (net_) ygnet_destroy(net_);
            return;
        }
        // A leading newline also isolates any earlier TU initializer's partial log.
        if (write_line(this, "\n", 1) || ygnet_hello(net_) < 0) fail("hello");
        bool asked = false;
        const auto setup_deadline = Clock::now() + setup_timeout_;
        uint32_t last_sent_frame = 0; bool have_sent = false;
        while (!stop_ && !failed_) {
#ifdef _WIN32
            const int ready = relay_wait_input(5);
#else
            pollfd fd{STDIN_FILENO, POLLIN, 0};
            const int ready = ::poll(&fd, 1, 5);
#endif
            if (ready < 0 && errno != EINTR) { fail("poll"); break; }
#ifdef _WIN32
            if (ready > 0) {
#else
            if (ready > 0 && (fd.revents & (POLLIN | POLLHUP | POLLERR))) {
#endif
                char bytes[8192];
                // Bounded drain: outbound and stop checks still run during a flood.
                for (int batch = 0; batch < 32 && !failed_; ++batch) {
#ifdef _WIN32
                    const int n = relay_read_input(bytes, sizeof(bytes));
#else
                    const ssize_t n = ::read(STDIN_FILENO, bytes, sizeof(bytes));
#endif
                    if (n > 0) { if (ygnet_feed(net_, bytes, size_t(n)) < 0) fail("feed"); }
                    else if (n == 0) { ygnet_eof(net_); fail("eof"); break; }
                    else if (errno == EINTR) continue;
                    else { if (errno != EAGAIN && errno != EWOULDBLOCK) fail("read"); break; }
                }
            }
            if (failed_ || stop_) break;
            const int sdk_status = ygnet_status(net_);
            if (sdk_status < 0) { fail("hello-broker"); break; }
            if (sdk_status == 1 && !asked) {
                asked = true;
                ticket_id_ = ygnet_next_id(net_);
                const auto request = "yg {\"id\":" + std::to_string(ticket_id_) +
                    ",\"type\":\"mpTicket\",\"mode\":\"duel\",\"players\":2}\n";
                if (write_line(this, request.data(), request.size())) fail("ticket-write");
            }
            if (!room_started_ && Clock::now() > setup_deadline) { fail("setup-timeout"); break; }
            if (!room_open_ || !room_started_ || !role() || Clock::now() < next_send_) continue;
            Sample local{}; uint32_t frame = 0; bool have_local = false;
            {
                std::lock_guard lock(mail_mutex_);
                local = mail_local_; frame = mail_frame_; have_local = mail_have_local_;
            }
            if (!have_local || (have_sent && frame == last_sent_frame)) continue;
            const uint32_t seq = sent_.load() + 1;
            const auto text = "{\"t\":\"msg\",\"d\":\"" + packet_hex(encode(role(), seq, frame, local)) + "\"}";
            const int result = ygnet_send(net_, room_conn_, text.c_str());
            next_send_ = Clock::now() + std::chrono::microseconds(33334);
            if (result == YGNET_EBUSY) continue; // Drop, never queue old positions.
            if (result < 0) { fail("send"); break; }
            sent_ = seq; last_sent_frame = frame; have_sent = true;
            if (!logged_local_packet_) { logged_local_packet_ = true; log_sample("local-first-packet", local, seq, frame); }
            if (!logged_local_ && get32(local.data()) != 0xFFFFFFFFu) {
                logged_local_ = true; log_sample("local-sample", local, seq, frame);
            }
        }
        // Worker-only bounded shutdown, for both failures and normal teardown.
        // Pending opens are closed as soon as the app supplies their IDs.
        close_connections();
        ygnet_destroy(net_); net_ = nullptr;
#ifndef _WIN32
        ::fcntl(STDIN_FILENO, F_SETFL, input_flags);
#endif
    }
    int output_ = -1;
    const std::chrono::milliseconds setup_timeout_;
    std::atomic<int> mode_{YG_NET_ERROR};
    std::atomic<char> role_{0};
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> received_{0}, dropped_{0};
    std::atomic<uint32_t> sent_{0};
    std::mutex mail_mutex_;
    Sample mail_local_{}, mail_remote_{}, remote_{};
    SampleInbox mail_samples_;
    uint32_t mail_frame_ = 0, mail_seq_ = 0, mail_remote_frame_ = 0;
    bool mail_have_local_ = false, mail_have_remote_ = false;
    uint32_t newest_seq_ = 0, remote_frame_ = 0, last_receive_frame_ = 0, stepped_frame_ = 0;
    bool stepped_ = false, have_remote_ = false, peer_live_ = false;
    bool logged_initial_loss_ = false, logged_remote_packet_ = false, logged_remote_ = false;
    // Worker-owned state below: every ygnet call, including callbacks, is on it.
    ygnet *net_ = nullptr;
    long long ticket_id_ = 0;
    int queue_conn_ = 0, room_conn_ = 0;
    std::string ticket_, me_, pending_start_;
    Clock::time_point next_send_{};
    bool failed_ = false, closing_ = false, write_broken_ = false;
    bool room_open_ = false, room_started_ = false;
    bool worker_have_remote_ = false, logged_local_packet_ = false, logged_local_ = false;
    uint32_t worker_seq_ = 0;
};
} // namespace yg_coop
#endif
