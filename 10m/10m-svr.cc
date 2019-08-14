#include <handy/handy.h>

using namespace std;
using namespace handy;

struct Report {
    long connected;
    long closed;
    long recved;
    Report() { memset(this, 0, sizeof(*this)); }
};

int main(int argc, const char *argv[]) {
    int begin_port = 25001;
    int end_port = 25005;
    int processes = 2;
    int man_port = 25000;

    if (argc < 5) {
        printf("usage: %s <begin port> <end port> <subprocesses> <management port>\n", argv[0]);
        //printf("current use default param\r\n");
        return 1;
    }
    else {
        begin_port = atoi(argv[1]);
        end_port = atoi(argv[2]);
        processes = atoi(argv[3]);
        man_port = atoi(argv[4]);
    }
    
    int pid = 1;
    for (int i = 0; i < processes; i++) {
        pid = fork();
        if (pid == 0) {  // a child process, break
            break;
        }
    }
    EventBase base;
    if (pid == 0) {          // child process
        usleep(100 * 1000);  // wait master to listen management port
        vector<TcpServerPtr> svrs;
        // 每一个进程汇总的消息，有多少连接在线，已关闭多少连接，接收了多少次消息
        long connected = 0, closed = 0, recved = 0;

        // 这个循环是为了，同一个进程在一个大的监听端口范围进行监听
        for (int i = 0; i <= end_port - begin_port; i++) {
            // reusepor为true，则多个进程可以共用一个端口
            TcpServerPtr p = TcpServer::startServer(&base, "", begin_port + i, true);
            p->onConnCreate([&] {
                // 创建一个连接（注册回调）并在最后返回
                TcpConnPtr con(new TcpConn);
                con->onState([&](const TcpConnPtr &con) {
                    auto st = con->getState();
                    if (st == TcpConn::Connected) {
                        connected++;
                    } else if (st == TcpConn::Closed || st == TcpConn::Failed) {
                        closed++;
                        connected--;
                    }
                });
                con->onMsg(new LengthCodec, [&](const TcpConnPtr &con, Slice msg) {
                    // 解码后的消息
                    recved++;
                    con->sendMsg(msg);
                });

                // 返回一个连接
                return con;
            });
            // 这里只管加，但是没有移出呀。TODO
            svrs.push_back(p);
        }

        // 这个工作进程，同时向本地的 master 进程发起连接，汇报负载信息
        TcpConnPtr report = TcpConn::createConnection(&base, "127.0.0.1", man_port, 3000);
        report->onMsg(new LineCodec, [&](const TcpConnPtr &con, Slice msg) {
            if (msg == "exit") { // 响应退出
                info("recv exit msg from master, so exit");
                base.exit();
            }
        });
        report->onState([&](const TcpConnPtr &con) {
            // 断开连接后，自己也退出
            if (con->getState() == TcpConn::Closed) {
                base.exit();
            }
        });

        // 定时上报消息
        base.runAfter(100, [&]() { 
            report->sendMsg(util::format("%d connected: %ld closed: %ld recved: %ld", getpid(), connected, closed, recved)); 
            }, 100);

        base.loop();
    } else {
        // master进程，在本地监听一个端口，响应客户端的连接
        map<int, Report> subs;
        TcpServerPtr master = TcpServer::startServer(&base, "127.0.0.1", man_port);

        // 这里并不关心哪个连接上报的，con 对象没有用。其ID，已在消息里表明
        master->onConnMsg(new LineCodec, [&](const TcpConnPtr &con, Slice msg) {
            auto fs = msg.split(' ');
            if (fs.size() != 7) {
                error("number of fields is %lu expected 7", fs.size());
                return;
            }
            Report &c = subs[atoi(fs[0].data())];
            c.connected = atoi(fs[2].data());
            c.closed = atoi(fs[4].data());
            c.recved = atoi(fs[6].data());
        });

        // 下面这里定时输出统计信息到屏幕，每3秒一次输出
        base.runAfter(3000,
                      [&]() {
                          for (auto &s : subs) {
                              Report r = s.second;
                              printf("pid: %6d connected %6ld closed: %6ld recved %6ld\n", s.first, r.connected, r.closed, r.recved);
                          }
                          printf("\n");
                      },
                      3000);
        base.loop();
    }
    info("program exited");
}
