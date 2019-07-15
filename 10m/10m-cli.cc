#include <handy/handy.h>
#include <sys/wait.h>

using namespace std;
using namespace handy;

struct Report {
    long connected;
    long retry;
    long sended;
    long recved;
    Report() { memset(this, 0, sizeof(*this)); }
};

int main(int argc, const char *argv[]) {
    if (argc < 9) {
        printf("usage %s <host> <begin port> <end port> <conn count> <create seconds> <subprocesses> <hearbeat interval> <send size> <management port>\n",
               argv[0]);
        return 1;
    }

    int c = 1;
    string host = argv[c++];
    int begin_port = atoi(argv[c++]);
    int end_port = atoi(argv[c++]);
    int conn_count = atoi(argv[c++]);
    float create_seconds = atof(argv[c++]);
    int processes = atoi(argv[c++]);
    conn_count = conn_count / processes; // 总的连接数分摊得每一个进程上

    int heartbeat_interval = atoi(argv[c++]);
    int bsz = atoi(argv[c++]);
    int man_port = atoi(argv[c++]);

    int pid = 1;
    for (int i = 0; i < processes; i++) {
        pid = fork();
        if (pid == 0) {  // a child process, break
            sleep(1);
            break;
        }
    }
    
    // 捕获信号SIGPIPE，防止进程异常退出
    Signal::signal(SIGPIPE, [] {});

    EventBase base;
    if (pid == 0) {  // child process
        char *buf = new char[bsz];
        ExitCaller ec1([=] { delete[] buf; });
        Slice msg(buf, bsz);
        char heartbeat[] = "heartbeat";
        int send = 0;
        int connected = 0;
        int retry = 0;
        int recved = 0;

        vector<TcpConnPtr> allConns;
        info("process %d creating %d connections", getpid(), conn_count);

        // 一共用多少秒来创建这些连接
        for (int k = 0; k < create_seconds * 10; k++) {
            // 但定时器是按 100 毫秒
            base.runAfter(100 * k, [&] {

                // 当前进程的全部连接数 分摊到 这么多秒来创建，则每一次应该创建下面的 c 个tcp客户端
                // 因为最外层的循环 x10了，所以这里要 除掉 10
                int c = conn_count / create_seconds / 10;
                for (int i = 0; i < c; i++) {
                    // 这里有一个轮回，端口会被重复使用
                    unsigned short port = begin_port + (i % (end_port - begin_port));
                    auto con = TcpConn::createConnection(&base, host, port, 20 * 1000);
                    allConns.push_back(con);
                    con->setReconnectInterval(20 * 1000);
                    con->onMsg(new LengthCodec, [&](const TcpConnPtr &con, const Slice &msg) {
                        // 如果有心跳了，这里不echo，只收
                        if (heartbeat_interval == 0) {  // echo the msg if no interval
                            con->sendMsg(msg);
                            send++;
                        }
                        recved++;
                    });
                    con->onState([&, i](const TcpConnPtr &con) {
                        TcpConn::State st = con->getState();
                        if (st == TcpConn::Connected) {
                            connected++;
                            //                            send ++;
                            //                            con->sendMsg(msg);
                        } else if (st == TcpConn::Failed || st == TcpConn::Closed) {  //Failed表示连接出错
                            if (st == TcpConn::Closed) {
                                connected--;
                            }
                            retry++;
                        }
                    });
                }
            });
        }
        if (heartbeat_interval) {
            // 如果设置了心跳间隔，则发送心跳
            base.runAfter(heartbeat_interval * 1000,
                          [&] {
                              for (int i = 0; i < heartbeat_interval * 10; i++) {
                                  // 发送一次心跳
                                  base.runAfter(i * 100, [&, i] {
                                      // 心跳也是一批一批发
                                      size_t block = allConns.size() / heartbeat_interval / 10;
                                      for (size_t j = i * block; j < (i + 1) * block && j < allConns.size(); j++) {
                                          if (allConns[j]->getState() == TcpConn::Connected) {
                                              allConns[j]->sendMsg(msg);
                                              send++;
                                          }
                                      }
                                  });
                              }
                          },
                          heartbeat_interval * 1000);
        }

        TcpConnPtr report = TcpConn::createConnection(&base, "127.0.0.1", man_port, 3000);
        report->onMsg(new LineCodec, [&](const TcpConnPtr &con, Slice msg) {
            if (msg == "exit") {
                info("recv exit msg from master, so exit");
                base.exit();
            }
        });
        report->onState([&](const TcpConnPtr &con) {
            if (con->getState() == TcpConn::Closed) {
                base.exit();
            }
        });
        base.runAfter(2000,
                      [&]() {
                          // 每隔2秒上报一次负载信息。当前进程有多少连接，连接失败重试了多少，发送多少，接收了多少次
                           report->sendMsg(util::format("%d connected: %ld retry: %ld send: %ld recved: %ld", getpid(), connected, retry, send, recved)); 
                           },
                      100);
        base.loop();
    } else {  // master process
        map<int, Report> subs;
        TcpServerPtr master = TcpServer::startServer(&base, "127.0.0.1", man_port);
        master->onConnMsg(new LineCodec, [&](const TcpConnPtr &con, Slice msg) {
            auto fs = msg.split(' ');
            if (fs.size() != 9) {
                error("number of fields is %lu expected 7", fs.size());
                return;
            }
            Report &c = subs[atoi(fs[0].data())];
            c.connected = atoi(fs[2].data());
            c.retry = atoi(fs[4].data());
            c.sended = atoi(fs[6].data());
            c.recved = atoi(fs[8].data());
        });
        base.runAfter(3000,
                      [&]() {
                          for (auto &s : subs) {
                              Report &r = s.second;
                              printf("pid: %6ld connected %6ld retry %6ld sended %6ld recved %6ld\n", (long) s.first, r.connected, r.retry, r.sended, r.recved);
                          }
                          printf("\n");
                      },
                      3000);

        // 主进程处理信号 SIGCHLD，
        Signal::signal(SIGCHLD, [] {
            // 等待子进程退出
            int status = 0;
            wait(&status);
            error("wait result: status: %d is signaled: %d signal: %d", status, WIFSIGNALED(status), WTERMSIG(status));
        });
        base.loop();
    }
    info("program exited");
}
