#include <handy/handy.h>
using namespace handy;

int main(int argc, const char *argv[]) {
    setloglevel("TRACE");
    EventBase base;

    // 键盘的中断信号
    Signal::signal(SIGINT, [&] { base.exit(); });

    // 启动一个服务器
    TcpServerPtr svr = TcpServer::startServer(&base, "", 2099);
    exitif(svr == NULL, "start tcp server failed");
    svr->onConnState([&](const TcpConnPtr &con) {  // 200ms后关闭连接
        if (con->getState() == TcpConn::Connected)
            base.runAfter(200, [con]() {
                info("close con after 200ms");
                con->close();
            });
    });

    info("==================create client connection ...");

    // 连接
    //TcpConnPtr con1 = TcpConn::createConnection(&base, "localhost", 2099);
    //con1->setReconnectInterval(300);
    TcpConnPtr con2 = TcpConn::createConnection(&base, "localhost", 1, 1000);
    con2->setReconnectInterval(2000);

    base.runAfter(6000, [&]() { base.exit(); });
    base.loop();

    info("program exit\r\n");
}