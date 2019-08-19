#include <handy/handy.h>

using namespace std;
using namespace handy;

struct Report {
    unsigned long connected;
    unsigned long closed;
    unsigned long recved;
    Report() { memset(this, 0, sizeof(*this)); }
};

int main(int argc, const char *argv[]) {

    string program = argv[0];
    string logfile = program + ".log";

    int begin_port = 9500;
    int end_port = 9500;
    int processes = 1;
    int data_protol = 2; 
    int man_port = 3031;

    std::string loglevel = "error";

    if (argc < 7) {
        printf("usage: %s <log level> <begin port> <end port> <fork work process> <management port>\n", argv[0]);
        //printf("current use default param\r\n");
        printf("    <log level>: 设置日志级别，可以取值如 trace, debug, info, error\n");
        printf("    <begin port>: 远程服务端的监听端口，指定开始端口\n");
        printf("    <end port>: 远程服务端的监听端口，指定结束端口。end port可以等于begin port表示只监听这个端口，否则为一个连续端口\n");
        printf("    <fork work process>: 摊派到子进程的数量\n");
        printf("    <data protol>: 数据的格式/协议。1表示换行符结束，2使用{}，其它表示以长度解码\n");
        printf("    <management port>: 多个进程时本地的管理端口\n");
        if (argc != 1)
        {
            return 0;
        }
        
        printf("使用测试的默认值\n");        
        //return 1;
    }
    else {
        int c = 1;
        loglevel = argv[c++];
        begin_port = atoi(argv[c++]);
        end_port = atoi(argv[c++]);
        processes = atoi(argv[c++]);
        data_protol = atoi(argv[c++]);
        man_port = atoi(argv[c++]);
    }
    
    setlogfile(logfile);
    setloglevel(loglevel);

    info("%d 主进程启动，在位置 %s", getpid(), argv[0]);

    CodecBase* cd = nullptr;
    info("数据包协议 %d", data_protol);
    switch (data_protol)
    {
    case 1: cd = new LineCodec(); break;
    case 2: cd = new BracketCodec(); break;
    default: cd = new LengthCodec(); break;
    }

    int pid = 1;
    if (processes > 1)
    {
        for (int i = 0; i < processes; i++) {
            pid = fork();
            if (pid == 0) {  // a child process, break
                        // 给子进程指定日志文件
                std::string child_log_file = program + "-" + std::to_string(getpid()) + ".log";
                setlogfile(child_log_file);
                info("=========子进程 %d 启动========", getpid());
                sleep(1);
                break;
            }
        }
    } else {
        pid = 0;
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
                        info("本地 %s，远程 %s，tcp连接成功, 当前共有 %d 个连接", con->local_.toString().c_str(), con->peer_.toString().c_str(), connected);
                    } else if (st == TcpConn::Closed || st == TcpConn::Failed) {
                        closed++;
                        connected--;
                        info("本地 %s，远程 %s，tcp连接异常[%d], 还有 %d 个", con->local_.toString().c_str(), con->peer_.toString().c_str(), st, connected);
                    }
                });
                con->onMsg(cd, [&](const TcpConnPtr &con, Slice msg) {
                    // 解码后的消息
                    std::string str = msg;
                    info("收到客户端[%s]消息: %s", con->peer_.toString().c_str(), str.c_str());
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
            std::string ss = util::format("%d, connected: %ld, closed: %ld, recved: %ld", getpid(), connected, closed, recved);
            info("%s", ss.c_str());

            if (report->getState() == TcpConn::Connected)
            {
                report->sendMsg(ss);                 
            }
        }, 2000);

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
                              info("pid: %6d, connected: %6ld, closed: %6ld, recved %6ld", s.first, r.connected, r.closed, r.recved);
                          }
                      },
                      3000);
        base.loop();
    }
    info("program exited");
}
