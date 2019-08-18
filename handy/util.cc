#include "util.h"
#include <fcntl.h>
#include <stdarg.h>
#include <algorithm>
#include <chrono>
#include <memory>

using namespace std;

namespace handy {

string util::format(const char *fmt, ...) {
    char buffer[500];
    unique_ptr<char[]> release1;
    char *base;
    for (int iter = 0; iter < 2; iter++) {
        int bufsize;
        if (iter == 0) {
            bufsize = sizeof(buffer);
            base = buffer;
        } else {
            bufsize = 30000;
            base = new char[bufsize];
            release1.reset(base);
        }
        char *p = base;
        char *limit = base + bufsize;
        if (p < limit) {
            va_list ap;
            va_start(ap, fmt);
            p += vsnprintf(p, limit - p, fmt, ap);
            va_end(ap);
        }
        // Truncate to available space if necessary
        if (p >= limit) {
            if (iter == 0) {
                continue;  // Try again with larger buffer
            } else {
                p = limit - 1;
                *p = '\0';
            }
        }
        break;
    }
    return base;
}

int64_t util::timeMicro() {
    // system_clock：用在需要得到绝对时点的场景
    // 刻度是1纳秒
    chrono::time_point<chrono::system_clock> p = chrono::system_clock::now();
    return chrono::duration_cast<chrono::microseconds>(p.time_since_epoch()).count();
}
int64_t util::steadyMicro() {
    // steady_clock：用在需要得到时间间隔，并且这个时间间隔不会因为修改系统时间而受影响的场景,一般是系统启动时间
    // 刻度是1纳秒
    chrono::time_point<chrono::steady_clock> p = chrono::steady_clock::now();
    return chrono::duration_cast<chrono::microseconds>(p.time_since_epoch()).count();
}

std::string util::readableTime(time_t t) {
    struct tm tm1;
    localtime_r(&t, &tm1);
    return format("%04d-%02d-%02d %02d:%02d:%02d", tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday, tm1.tm_hour, tm1.tm_min, tm1.tm_sec);
}

int util::addFdFlag(int fd, int flag) {
    int ret = fcntl(fd, F_GETFD);
    return fcntl(fd, F_SETFD, ret | flag);
}

}  // namespace handy
