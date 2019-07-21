#include "port_posix.h"
#include <netdb.h>
#include <pthread.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace handy {
namespace port {
#ifdef OS_LINUX
struct in_addr getHostByName(const std::string &host) {
    struct in_addr addr;
    char buf[1024];
    struct hostent hent;
    struct hostent *he = NULL;
    int herrno = 0;
    memset(&hent, 0, sizeof hent);
    int r = gethostbyname_r(host.c_str(), &hent, buf, sizeof buf, &he, &herrno);
    if (r == 0 && he && he->h_addrtype == AF_INET) {
        addr = *reinterpret_cast<struct in_addr *>(he->h_addr);
    } else {
        addr.s_addr = INADDR_NONE;
    }
    return addr;
}
uint64_t gettid() {
    /**
     *     在linux下每一个进程都一个进程id，类型pid_t，可以由 getpid（）获取。
      POSIX线程也有线程id，类型pthread_t，可以由 pthread_self（）获取，线程id由线程库维护。
      但是各个进程独立，所以会有不同进程中线程号相同节的情况。
      那么这样就会存在一个问题，我的进程p1中的线程pt1要与进程p2中的线程pt2通信怎么办，进程id不可以，线程id又可能重复，所以这里会有一个真实的线程id唯一标识，tid。
      glibc没有实现gettid的函数，所以我们可以通过linux下的系统调用 syscall(SYS_gettid) 来获得。
     */
    return syscall(SYS_gettid);
}
#elif defined(OS_MACOSX)
struct in_addr getHostByName(const std::string &host) {
    struct in_addr addr;
    struct hostent *he = gethostbyname(host.c_str());
    if (he && he->h_addrtype == AF_INET) {
        addr = *reinterpret_cast<struct in_addr *>(he->h_addr);
    } else {
        addr.s_addr = INADDR_NONE;
    }
    return addr;
}
uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t uid = 0;
    memcpy(&uid, &tid, std::min(sizeof(tid), sizeof(uid)));
    return uid;
}
#endif

}  // namespace port
}  // namespace handy