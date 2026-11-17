#ifndef YG_COOP_NOTICE_HPP
#define YG_COOP_NOTICE_HPP

#include <mutex>
#include <string>

namespace yg_coop {
// The relay owns publication; only the UI draw callback consumes the notice.
inline std::mutex online_notice_mutex;
inline std::string online_notice;

inline void queue_online_notice(const std::string &message) {
    std::lock_guard lock(online_notice_mutex);
    online_notice = message;
}

inline std::string take_online_notice() {
    std::unique_lock lock(online_notice_mutex, std::try_to_lock);
    std::string message;
    if (lock.owns_lock()) message.swap(online_notice);
    return message;
}
} // namespace yg_coop
#endif
