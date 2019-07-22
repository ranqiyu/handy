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

void test_one(){

    Signal::signal(SIGPIPE, [] {});

    EventBase base;
    
        TcpConnPtr con = TcpConn::createConnection(&base, "182.61.30.122", 10000, 3000);
        con->setReconnectInterval(20 * 1000);
        con->onMsg(new LengthCodec, [&](const TcpConnPtr &con, const Slice &msg) {
            if (false) { 
                con->sendMsg(msg);
            }
        });
        con->onState([&](const TcpConnPtr &con) {
            TcpConn::State st = con->getState();
            if (st == TcpConn::Connected) {
                std::string s = util::format("pid %d 连接成功, %s", getpid(), con->str().c_str());
                info("%s", s.c_str());
                //                            send ++;
                //                            con->sendMsg(msg);
            } else if (st == TcpConn::Failed || st == TcpConn::Closed) {  //Failed表示连接出错
                std::string s = util::format("pid %d 连接异常 %d, %s", getpid(), st, con->str().c_str());

                if (st == TcpConn::Closed) {
                }
            }
        });

        base.loop();
}
int main(int argc, const char *argv[]) {

    string program = argv[0];
    string logfile = program + ".log";

    setlogfile(logfile);
    info("process run  at %s", argv[0]);

    if (false) // mytest
    {
        test_one();
        return 0;
    }
    
    int c = 1;

    string host = "182.61.30.122"; // 服务器IP
    int begin_port = 10000; 
    int end_port = 10005;
    
    int conn_count = 100;  // 总的连接数
    int processes = 1; // 连接一共用多少个进程创建
    int create_rate_mils = 50; // 创建连接的速率
    int concur_num_per_tms = 1000; // 每次的并发IO数 

    int heartbeat_interval = 60; // 心跳间隔时间，毫秒
    int bsz = 64; // 心跳包大小

    int man_port = 10301;

    assert(conn_count >= processes);
    int every_process_conn_count = conn_count / processes; // 每个进程创建的连接个数
    int create_timer_cnt = every_process_conn_count / concur_num_per_tms; // 一共用这么多次的定时器
    if (create_timer_cnt <= 0)
    {
        create_timer_cnt = 1;
    }
    

    if (argc < 9) {
        printf("usage %s <host> <begin port> <end port> <conn count> <create seconds> <subprocesses> <hearbeat interval> <send size> <management port>\n",
               argv[0]);
        printf("use default val\r\n");
        //return 1;
    }
    else {
        // host = argv[c++];
        // begin_port = atoi(argv[c++]);
        // end_port = atoi(argv[c++]);
        // conn_count = atoi(argv[c++]);
        // create_seconds = atof(argv[c++]);
        // processes = atoi(argv[c++]);
        // conn_count = conn_count / processes; // 总的连接数分摊得每一个进程上

        // heartbeat_interval = atoi(argv[c++]);
        // bsz = atoi(argv[c++]);
        // man_port = atoi(argv[c++]);
    }


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
       
        info("process %d will create %d connect, with %d timer", getpid(), every_process_conn_count, create_timer_cnt);
        
        int total_cnt = 0;

        for (int k = 0; k < create_timer_cnt; k++) {
            info("进入循环 %d", k);
            
            // 一次性创建了很多的定时器
            base.runAfter(create_rate_mils * k, [&] {

                info("%d 定时器 %d 已经到达，共将创建 %d 个连接", getpid(), k, concur_num_per_tms);

                for (int i = 0; i < concur_num_per_tms; i++) {
                    // 这里有一个轮回，端口会被重复使用
                    unsigned short port = begin_port + (i % (end_port - begin_port));
                    auto con = TcpConn::createConnection(&base, host, port, 20 * 1000);

                    info("%d 将创建第 %d 个连接 %s", getpid(), i, con->str().c_str());

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
                            std::string s = util::format("pid %d 连接成功, %s", getpid(), con->str().c_str());
                            info("%s", s.c_str());
                            connected++;
                            //                            send ++;
                            //                            con->sendMsg(msg);
                        } else if (st == TcpConn::Failed || st == TcpConn::Closed) {  //Failed表示连接出错
                            std::string s = util::format("pid %d 连接异常 %d, %s", getpid(), st, con->str().c_str());
                            warn("%s", s.c_str());
                            if (st == TcpConn::Closed) {
                                connected--;
                            }
                            retry++;
                        }
                    });

                    if ((++total_cnt) >= every_process_conn_count)
                    {
                        info("进程 %d  已完成创建连接总量 %d ", getpid(), every_process_conn_count);
                        break;
                    }
                    
                }
            });
        }
        /*
        if (heartbeat_interval) {
            // 如果设置了心跳间隔，则发送心跳
            base.runAfter(heartbeat_interval,
                          [&] {
                              for (int i = 0; i < heartbeat_interval * 10; i++) {
                                  // 发送一次心跳。也是创建了很多定时器
                                  base.runAfter(i * 100, [&, i] {
                                      // 心跳也是一批一批发
                                      size_t block = allConns.size() / heartbeat_interval / 10;
                                      for (size_t j = i * block; j < (i + 1) * block && j < allConns.size(); j++) {
                                          if (allConns[j]->getState() == TcpConn::Connected) {
                                              
                                              info("%d 将向 %s 发送心跳包", getpid(), allConns[i]->str().c_str());

                                              allConns[j]->sendMsg(msg);
                                              send++;
                                          }
                                      }
                                  });
                              }
                          },
                          heartbeat_interval);
        }*/

        TcpConnPtr report = TcpConn::createConnection(&base, "127.0.0.1", man_port, 3000);
        report->onMsg(new LineCodec, [&](const TcpConnPtr &con, Slice msg) {
            if (msg == "exit") {
                info("recv exit msg from master, so exit");
                base.exit();
            }
        });
        report->onState([&](const TcpConnPtr &con) {
            if (con->getState() == TcpConn::Closed) {
                error("销毁向本地master上报负载的tcp连接");
                base.exit();
            }
        });
        
        // 每隔2秒上报一次负载信息。当前进程有多少连接，连接失败重试了多少，发送多少，接收了多少次
        base.runAfter(2000,
                      [&]() {
                          std::string s = util::format("%d connected: %ld retry: %ld send: %ld recved: %ld", getpid(), connected, retry, send, recved);
                          info("上报负载： %s", s.c_str());
                           report->sendMsg(s); 
                           },
                      100);
        base.loop();
        info("%d 子进程即将退出", getpid());
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
