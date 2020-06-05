#include <handy/handy.h>
using namespace handy;

int main(int argc, const char *argv[]) {
    setloglevel("TRACE");
    EventBase base;
    Signal::signal(SIGINT, [&] { base.exit(); });
    TcpServerPtr svr = TcpServer::startServer(&base, "", 2099);
    exitif(svr == NULL, "start tcp server failed");
    svr->onConnState([](const TcpConnPtr &con) {
        if (con->getState() == TcpConn::Connected) {
            // 针对这个一个 连接。一个连接可以设置多个 idle 回调？感觉没有多大意义
            con->addIdleCB(2, [](const TcpConnPtr &con) {
                info("idle for 2 seconds, close connection");
                con->close();
            });
        }
    });
    auto con = TcpConn::createConnection(&base, "localhost", 2099);
    base.runAfter(3000, [&]() { base.exit(); });
    base.loop();
}