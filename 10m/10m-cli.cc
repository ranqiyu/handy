#include <handy/handy.h>
#include <sys/wait.h>
#include <atomic>

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
                info("%s", s.c_str());

                if (st == TcpConn::Closed) {
                }
            }
        });

        base.loop();
}
int main(int argc, const char *argv[]) {

    string program = argv[0];
    string logfile = program + ".log";

    if (false) // mytest
    {
        test_one();
        return 0;
    }
    
    int c = 1;
    std::string loglevel = "trace";

    string host = "39.106.230.84"; // 服务器IP
    //string host = "192.168.0.3"; // 服务器IP
    int begin_port = 9500; 
    int end_port = 9500;
    
    int conn_count = 1;  // 总的连接数
    int reconnect_interval = -1; // 重连时间间隔，毫秒。-1表示不重连， 0表示立即重连
    int connect_timedout = 0; // 重连的超时时间，毫秒。0表示不设置
    int processes = 1; // 连接一共用多少个进程创建
    int create_rate_mils = 5000; // 创建连接的速率。每隔多少一次IO。单位毫秒
    int concur_num_per_tms = 1000; // 每次的并发IO数 

    int heartbeat_interval = 5000;//60 * 1000; // 心跳间隔时间，毫秒
    int bsz = 64; // 心跳包大小
    int data_protol = 2; // 心跳包数据协议，1换行符,2长度
    int man_port = 3031;


    if (argc <= 14) {
        printf("usage %s <log level> <remote host> <remote begin port> <remote end port> <concur connect> <concur io interval> <concur io number> <fork work process> <hearbeat interval> <data size> <data protol> <management port>\n",
               argv[0]);
        printf("    <log level>: 设置日志级别，可以取值如 trace, debug, info, error\n");
        printf("    <remote host>: 指定远程服务端的主机或者IP地址\n");
        printf("    <remote begin port>: 远程服务端的监听端口，指定开始端口\n");
        printf("    <remote end port>: 远程服务端的监听端口，指定结束端口。end port可以等于begin port表示只监听这个端口，否则为一个连续端口\n");
        printf("    <concur connect>: 总的并发连接数\n");
        printf("    <reconnect interval>: 重连间隔时间，毫秒。-1表示不重连， 0表示立即重连\n");
        printf("    <connect timedout>: 连接超时时间，毫秒。0表示不设置\n");
        printf("    <concur io interval>: 每次并发IO的间隔时间，毫秒\n");
        printf("    <concur io number>: 每次并发IO的数量\n");
        printf("    <fork work process>: 摊派到子进程的数量\n");
        printf("    <hearbeat interval>: 发送心跳的间隔时间，毫秒。为0表示不并发送心跳\n");
        printf("    <data size>: 发送的数据包大小（如心跳数据包），字节\n");
        printf("    <data protol>: 数据的格式/协议。1表示换行符结束，2使用{}，其它表示以长度解码\n");
        printf("    <management port>: 多个进程时本地的管理端口\n");
        printf("使用测试的默认值\n");
    }
    else {
        // 在vscode中传参数，例如
        
         loglevel = argv[c++];
         host = argv[c++];
         begin_port = atoi(argv[c++]);
         end_port = atoi(argv[c++]);
         conn_count = atoi(argv[c++]);
         reconnect_interval = atoi(argv[c++]);
         connect_timedout = atoi(argv[c++]);
         create_rate_mils = atoi(argv[c++]);
         concur_num_per_tms = atoi(argv[c++]);

         processes = atoi(argv[c++]);
         heartbeat_interval = atoi(argv[c++]);
         bsz = atoi(argv[c++]);
         data_protol = atoi(argv[c++]);

         man_port = atoi(argv[c++]);

        info("使用传入的参数值，将创建 %d 个连接", conn_count);
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
    
    assert(conn_count >= processes);
    int every_process_conn_count = conn_count / processes; // 每个进程创建的连接个数
    int create_timer_cnt = every_process_conn_count / concur_num_per_tms; // 一共用这么多次的定时器
    if (create_timer_cnt <= 0)
    {
        create_timer_cnt = 1;
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
    }
    else { // 只有一个进程就不用fork了
        pid = 0;
    }
    
    EventBase base;

    // 捕获信号SIGPIPE，防止进程异常退出
    Signal::signal(SIGPIPE, [] {
        info("捕获信号 SIGPIPE");
    });
    Signal::signal(SIGINT, [&] {
        // ctrl + c 
        info("捕获终止信号 SIGINT，将要退出");
        base.exit();
    });

    if (pid == 0) {  // child process
        char *buf = new char[bsz];
        ExitCaller ec1([=] { delete[] buf; });
        memset(buf, 'a', bsz);
        const char *tbuf = "heartbeat";
        memcpy(buf, tbuf, std::min((int)(strlen(tbuf)), bsz));

        Slice msg(buf, bsz);
        unsigned int send = 0;
        unsigned int connected = 0;
        unsigned int retry = 0;
        unsigned int recved = 0;

        std::map<TcpConnPtr, int64_t> active_con; // 受到服务端心跳的连接
        vector<TcpConnPtr> allConns;
       
        info("process %d will create %d connect, with %d timer", getpid(), every_process_conn_count, create_timer_cnt);
        
        int total_cnt = 0;

        for (int k = 0; k < create_timer_cnt; k++) {
            debug("进入循环 %d", k);
            
            // 一次性创建了很多的定时器
            int tk = k;
            base.runAfter(create_rate_mils * k, [&, tk] {

                debug("%d 定时器 %d 已经到达，共将创建 %d 个连接", getpid(), tk, concur_num_per_tms);

                for (int i = 0; i < concur_num_per_tms; i++) {
                    // 这里有一个轮回，端口会被重复使用
                    unsigned short port = begin_port;
                    if(begin_port != end_port){
                        port = begin_port + (i % (end_port - begin_port));
                    }

                    // 这里连接的超时时间
                    auto con = TcpConn::createConnection(&base, host, port, connect_timedout);

                    debug("%d 定时器 %d 将创建第 %d 个连接 %s", getpid(), tk, i, con->str().c_str());

                    allConns.push_back(con);
                    con->setReconnectInterval(reconnect_interval);

                    // 使用LIneCodec编码可以用来测试echo server
                    con->onMsg(cd, [&](const TcpConnPtr &con, const Slice &msg) {
                        std::string t = msg;
                        info("[%s]收到服务端[%s]消息： %s", con->local_.toString().c_str(), con->peer_.toString().c_str(), t.c_str());

                        // 如果有心跳了，这里不echo，只收
                        /*
                        if (heartbeat_interval == 0) {  // echo the msg if no interval
                            con->sendMsg(msg);
                            send++;
                        }*/
                        recved++;
                    });
                    con->onState([&, i](const TcpConnPtr &con) {
                        TcpConn::State st = con->getState();
                        if (st == TcpConn::Connected) {
                            connected++;
                            info("本地 %s，远程 %s，tcp连接成功, 当前共有 %d 个连接", con->local_.toString().c_str(), con->peer_.toString().c_str(), connected);

                            //                            send ++;
                            //                            con->sendMsg(msg);
                        } else if (st == TcpConn::Failed || st == TcpConn::Closed) {  //Failed表示连接出错
                            if (st == TcpConn::Closed) {
                                connected--;
                            }
                            if (reconnect_interval != -1) // 内部只在指定了重连时才会重连
                            {
                                retry++;                                
                            }

                            info("本地 %s，远程 %s，tcp连接异常[%d], 还有 %d 个", con->local_.toString().c_str(), con->peer_.toString().c_str(), st, connected);
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
        
        if (heartbeat_interval) {
            // 如果设置了心跳间隔，则发送心跳
            debug("%d 将要启动心跳定时期", getpid());

            base.runAfter(heartbeat_interval,
                          [&] {
                            // 这么多连接要分多少次定时期来发
                            int timer_cnt = allConns.size() / concur_num_per_tms; // 一共用这么多次的定时器
                            timer_cnt = std::max(timer_cnt, 1);
                            debug("%d 共有 %d 个连接，要分 %d 次定时器来发送心跳，每次发 %d 个连接", getpid(), allConns.size(), timer_cnt, concur_num_per_tms);

                              for (int i = 0; i < timer_cnt; i++) {

                                  base.runAfter(i * create_rate_mils, [&, i] {

                                      // 心跳也是一批一批发
                                      for (size_t j = i*concur_num_per_tms; (j < allConns.size()) && (i < concur_num_per_tms); j++) {

                                          if (allConns[j]->getState() == TcpConn::Connected) {
                                              
                                            static std::atomic<uint32_t> idx(0);
                                            idx++;
                                            
                                            std::string s1 =std::to_string(idx);
                                            memcpy(buf, s1.c_str(), std::min((int)s1.length(), bsz));      
                                            Slice msg(buf, bsz);

                                            info("[%s]向服务端[%s]发送消息：%s", allConns[j]->local_.toString().c_str(), allConns[j]->peer_.toString().c_str(), buf);

                                            allConns[j]->sendMsg(msg);
                                            send++;
                                          }
                                      }
                                  });
                              }
                          },
                          heartbeat_interval);
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
                error("销毁向本地master上报负载的tcp连接");
                base.exit();
            }
        });
        
        // 每隔2秒上报一次负载信息。当前进程有多少连接，连接失败重试了多少，发送多少，接收了多少次
        base.runAfter(2000,
                      [&]() {
                          std::string s = util::format("%d connected: %d, retry: %d, send: %d, recved: %d", getpid(), connected, retry, send, recved);
                          info("上报负载： %s", s.c_str());
                          //info("进程 %d 负载情况，connected: %d ，retry: %d ，send: %d ，recved: %d", getpid(), connected, retry, send, recved);
                          if (report->getState() == TcpConn::Connected)
                          {
                                report->sendMsg(s); 
                          }
                          else {
                              debug("上报负载的本地socket没有连接成功，不会上报");
                          }
                          
                           },
                      2500);
        info("%d 进程启动事件循环", getpid());
        base.loop();
        info("%d 进程即将退出", getpid());
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
                              info("pid: %d, connected %d, retry %d, sended %d, recved %d", (long) s.first, r.connected, r.retry, r.sended, r.recved);
                          }
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
