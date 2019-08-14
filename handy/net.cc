#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <string>
#include "logging.h"
#include "util.h"

using namespace std;
namespace handy {

int net::setNonBlock(int fd, bool value) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return errno;
    }
    if (value) {
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int net::setReuseAddr(int fd, bool value) {
    int flag = value;
    int len = sizeof flag;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, len);
}

int net::setReusePort(int fd, bool value) {
#ifndef SO_REUSEPORT
    fatalif(value, "SO_REUSEPORT not supported");
    return 0;
#else
    int flag = value;
    int len = sizeof flag;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, len);
#endif
}

int net::setNoDelay(int fd, bool value) {
    int flag = value;
    int len = sizeof flag;
    return setsockopt(fd, SOL_SOCKET, TCP_NODELAY, &flag, len);
}

int net::setKeepAlived(int fd, int idle, int interval, int keepcnt) {
    int yes = 1;
    // 启用 keepalive
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(int)) == -1)
    {
        return -1;
    }
    // TCP_KEEPIDLE：覆盖内核参数 tcp_keepalive_time
    // 在TCP保活打开的情况下，最后一次数据交换到TCP发送第一个保活探测消息的时间，即允许的持续空闲时间。默认值为7200s（2h）。
    if(setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(int)) == -1){
        return -1;
    }

    // TCP_KEEPINTVL：覆盖 tcp_keepalive_intvl
    // 保活探测消息的发送频率。默认值为75s。
    // 发送频率tcp_keepalive_intvl乘以发送次数tcp_keepalive_probes，就得到了从开始探测直到放弃探测确定连接断开的时间，大约为11min。
    if(setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(int)) == -1){
        return -1;
    }

    // TCP_KEEPCNT：覆盖内核参数 tcp_keepalive_probes
    // TCP发送保活探测消息以确定连接是否已断开的次数。默认值为9（次）
    return setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(int));
}
    
Ip4Addr::Ip4Addr(const string &host, unsigned short port) {
    memset(&addr_, 0, sizeof addr_);
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    if (host.size()) {
        addr_.sin_addr = port::getHostByName(host);
    } else {
        addr_.sin_addr.s_addr = INADDR_ANY;
    }
    if (addr_.sin_addr.s_addr == INADDR_NONE) {
        error("cannot resove %s to ip", host.c_str());
    }
}

string Ip4Addr::toString() const {
    uint32_t uip = addr_.sin_addr.s_addr;
    return util::format("%d.%d.%d.%d:%d", (uip >> 0) & 0xff, (uip >> 8) & 0xff, (uip >> 16) & 0xff, (uip >> 24) & 0xff, ntohs(addr_.sin_port));
}

string Ip4Addr::ip() const {
    uint32_t uip = addr_.sin_addr.s_addr;
    return util::format("%d.%d.%d.%d", (uip >> 0) & 0xff, (uip >> 8) & 0xff, (uip >> 16) & 0xff, (uip >> 24) & 0xff);
}

unsigned short Ip4Addr::port() const {
    return (unsigned short)ntohs(addr_.sin_port);
}

unsigned int Ip4Addr::ipInt() const {
    return ntohl(addr_.sin_addr.s_addr);
}
bool Ip4Addr::isIpValid() const {
    return addr_.sin_addr.s_addr != INADDR_NONE;
}

char *Buffer::makeRoom(size_t len) {
    if (e_ + len <= cap_) {
    } else if (size() + len < cap_ / 2) {
        moveHead();
    } else {
        expand(len);
    }
    return end();
}

void Buffer::expand(size_t len) {
    size_t ncap = std::max(exp_, std::max(2 * cap_, size() + len));
    char *p = new char[ncap];
    std::copy(begin(), end(), p);
    e_ -= b_;
    b_ = 0;
    delete[] buf_;
    buf_ = p;
    cap_ = ncap;
}

void Buffer::copyFrom(const Buffer &b) {
    memcpy(this, &b, sizeof b);
    if (b.buf_) {
        buf_ = new char[cap_];
        memcpy(data(), b.begin(), b.size());
    }
}

// 吞并。之后buf为空
Buffer &Buffer::absorb(Buffer &buf) {
    if (&buf != this) {
        if (size() == 0) {
            char b[sizeof buf];
            memcpy(b, this, sizeof b);
            memcpy(this, &buf, sizeof b);
            memcpy(&buf, b, sizeof b);
            std::swap(exp_, buf.exp_);  // keep the origin exp_
        } else {
            append(buf.begin(), buf.size());
            buf.clear();
        }
    }
    return *this;
}

}  // namespace handy