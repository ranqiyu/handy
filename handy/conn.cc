#include "conn.h"
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include "logging.h"
#include "poller.h"

using namespace std;
namespace handy {

void handyUnregisterIdle(EventBase *base, const IdleId &idle);
void handyUpdateIdle(EventBase *base, const IdleId &idle);

void TcpConn::attach(EventBase *base, int fd, Ip4Addr local, Ip4Addr peer) {
    fatalif((destPort_ <= 0 && state_ != State::Invalid) || (destPort_ >= 0 && state_ != State::Handshaking),
            "you should use a new TcpConn to attach. state: %d", state_);
    base_ = base;
    state_ = State::Handshaking;
    local_ = local;
    peer_ = peer;
    if (channel_)
    {   
        trace("[%p] 原来已经有一个 channel 了，是[%p]", this, channel_);
    }
    
    delete channel_;
    channel_ = new Channel(base, fd, kWriteEvent | kReadEvent);
    trace("[%p] tcp constructed %s - %s fd: %d。附加的 channel 是[%p]", this, local_.toString().c_str(), peer_.toString().c_str(), fd, channel_);

    TcpConnPtr con = shared_from_this();
    con->channel_->onRead([=] { con->handleRead(con); });
    con->channel_->onWrite([=] { con->handleWrite(con); });
}

void TcpConn::connect(EventBase *base, const string &host, unsigned short port, int timeout, const string &localip) {
    fatalif(state_ != State::Invalid && state_ != State::Closed && state_ != State::Failed, "current state is bad state to connect. state: %d", state_);
    destHost_ = host;
    destPort_ = port;
    connectTimeout_ = timeout;
    connectedTime_ = util::timeMilli();
    localIp_ = localip;
    Ip4Addr addr(host, port);

    // 可靠连接的套接字 SOCK_STREAM
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    
    fatalif(fd < 0, "socket failed %d %s", errno, strerror(errno));
    net::setNonBlock(fd);
    int t = util::addFdFlag(fd, FD_CLOEXEC);
    fatalif(t, "addFdFlag FD_CLOEXEC failed %d %s", t, strerror(t));
    int r = 0;
    if (localip.size()) {
        Ip4Addr addr(localip, 0);
        r = ::bind(fd, (struct sockaddr *) &addr.getAddr(), sizeof(struct sockaddr));
        error("bind to %s failed error %d %s", addr.toString().c_str(), errno, strerror(errno));
    }
    if (r == 0) {
        r = ::connect(fd, (sockaddr *) &addr.getAddr(), sizeof(sockaddr_in));
        
        // 非阻塞情况下connect产生EINPROGRESS错误
        if (r != 0 && errno != EINPROGRESS) { // 但是这里不是这个错误，所以是error
            error("connect to %s error %d %s", addr.toString().c_str(), errno, strerror(errno));
        }
    }

    sockaddr_in local;
    socklen_t alen = sizeof(local);
    if (true) { // 之前是 r==0，导致无法进入
        // 在一个没有调用bind的TCP客户上，getsockname用于返回由内核赋予该连接的本地IP地址和本地端口号
        r = getsockname(fd, (sockaddr *) &local, &alen);
        if (r < 0) {
            error("getsockname failed %d %s", errno, strerror(errno));
        }

        // char sip[INET_ADDRSTRLEN] = { 0 };
	    // inet_ntop(AF_INET, &(((struct sockaddr_in *)&local)->sin_addr), sip, INET_ADDRSTRLEN);
        // Ip4Addr t1(local);
        // info("kernel assign address %s:%d, %s", sip, local.sin_port, t1.toString().c_str());
    }

    state_ = State::Handshaking;
    attach(base, fd, Ip4Addr(local), addr);
    if (timeout) {
        // 设置连接超时
        TcpConnPtr con = shared_from_this();
        timeoutId_ = base->runAfter(timeout, [con] {
            debug("[%p] connect的超时定时器已经到，关联fd %d", con.get(), con->channel_->fd());
            // 关键是这里，如果超时之后，这里还没有就断开。连上了就不用管了
            if (con->getState() == Handshaking) {
                warn("[%p] connect的超时为连接失败，将关闭，关联fd %d", con.get(), con->channel_->fd());
                con->channel_->close();
            }
        });
    }
}

void TcpConn::close() {
    if (channel_) {
        TcpConnPtr con = shared_from_this();
        getBase()->safeCall([con] {
            if (con->channel_)
                con->channel_->close();
        });
    }
}

void TcpConn::cleanup(const TcpConnPtr &con) {
    if (readcb_ && input_.size()) { // 如果读取的数据还有，则先回调出去。有必要吗？
        readcb_(con);
    }
    if (state_ == State::Handshaking) {
        state_ = State::Failed;
    } else {
        state_ = State::Closed;
    }
    trace("tcp closing %s - %s fd %d %d", local_.toString().c_str(), peer_.toString().c_str(), channel_ ? channel_->fd() : -1, errno);
    getBase()->cancel(timeoutId_);
    if (statecb_) {
        statecb_(con);
    }
    if (reconnectInterval_ >= 0 && !getBase()->exited()) {  // reconnect
        reconnect(); // 函数的实现在哪里？跳进去看一下
        return;
    }
    for (auto &idle : idleIds_) {
        handyUnregisterIdle(getBase(), idle);
    }
    // channel may have hold TcpConnPtr, set channel_ to NULL before delete
    readcb_ = writablecb_ = statecb_ = nullptr;
    Channel *ch = channel_;
    channel_ = NULL;
    if (ch)
    {
        ch->close();
        trace("[%p] 先close但不删除 channel。后续要修改，防止channel的内存泄漏", this);
    }
    //delete ch;
    
}

void TcpConn::handleRead(const TcpConnPtr &con) {
    // 这一步先将 写事件 监听 给禁用了，当需要时再启用
    trace("[%p] here,handle read。当前网络状态是 %d", this, state_);
    if (!con)
    {
        debug("不应该为空 %s", str().c_str());
    }

    assert(con); // 必须为真

    if (state_ == State::Handshaking && handleHandshake(con)) {
        return;
    }
    while (state_ == State::Connected) {
        input_.makeRoom();
        int rd = 0;
        // 打印第一个就崩溃了。说明不应该是 Connected 状态。看看在哪里赋值的
        trace("[%p] 准备读数据，他的 channel 是 [%p]", this, channel_);
        assert(channel_); // 必须有值才行

        // 尴尬。这里崩溃了。channel_ 为空，被删除了。导致崩溃
        if (!channel_)
        {
            error("[%p] channel can not be null", this);
        }
        
        if (channel_->fd() >= 0) {
            trace("[%p] debug: fd %d, %p, %p", this, channel_->fd(), input_.end(), input_.space());
            rd = readImp(channel_->fd(), input_.end(), input_.space());
            trace("[%p] channel %lld fd %d readed %d bytes", this, (long long) channel_->id(), channel_->fd(), rd);
        }
        if (rd == -1 && errno == EINTR) {
            continue;
        } else if (rd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            for (auto &idle : idleIds_) {
                handyUpdateIdle(getBase(), idle);
            }
            if (readcb_ && input_.size()) { // 这里是有读到值才会回调出来的
                trace("已经读到数据长度 %d, 内容: %s", input_.size(), input_.data());
                readcb_(con);
            }
            break;
        } 
        // 当读到为0时，表示关闭
        else if (channel_->fd() == -1 || rd == 0 || rd == -1) {
            // 这里已经判断了为 -1 的情况
            trace("[%p] channel 的 fd 已经被设置为 %d 了。将清理", this, channel_->fd());
            cleanup(con);
            break;
        } else {  // rd > 0
            input_.addSize(rd);
        }
    }
}

int TcpConn::handleHandshake(const TcpConnPtr &con) {
    fatalif(state_ != Handshaking, "handleHandshaking called when state_=%d", state_);
    
    // 这段代码测试用。用来检测是否连接成功。和后面的poll的作用一样
    if (true)
    {
        socklen_t lon;
        lon = sizeof(int); 
        int valopt = 0;
        //getsockopt(channel_->fd(),SO_ERROR,SOL_SOCKET,(void*)(&valopt), &lon);
        if (getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon) < 0) { 
                    error("Error in getsockopt() %d - %s", errno, strerror(errno)); 
                    // exit(0); 
        } else {
            // Check the value returned... 
            if (valopt) { // != 0.  为0则表示没有错误。这里若为真就是非0，失败
                trace("fd %d error connection() %d - %s\n", channel_->fd(), valopt, strerror(valopt) ); 
            } else {
                // 成功
                trace("fd %d connect success", channel_->fd());
            } 
        }
    }
    

    struct pollfd pfd;
    pfd.fd = channel_->fd();
    pfd.events = POLLOUT | POLLERR;

    /*
     poll函数使用pollfd类型的结构来监控一组文件句柄，
     ufds是要监控的文件句柄集合，
     nfds是监控的文件句柄数量，
     timeout是等待的毫秒数，这段时间内无论I/O是否准备好，poll都会返回。
     timeout为负数表示无线等待，timeout为0表示调用后立即返回。
     执行结果：为0表示超时前没有任何事件发生；-1表示失败；成功则返回结构体中revents不为0的文件描述符个数。
     */
    // 感觉在这里这个函数的目的，只是为了判断是否确实可写
    int r = poll(&pfd, 1, 0); // 使用hai在epoll之前的poll
    if (r == 1 && pfd.revents == POLLOUT) { // 可写
        channel_->enableReadWrite(true, false);
        state_ = State::Connected;
        if (state_ == State::Connected) {
            connectedTime_ = util::timeMilli();
            trace("tcp connected %s - %s fd %d", local_.toString().c_str(), peer_.toString().c_str(), channel_->fd());

            // {
            //     sockaddr_in local;
            //     socklen_t alen = sizeof(local);
            //     // 在一个没有调用bind的TCP客户上，getsockname用于返回由内核赋予该连接的本地IP地址和本地端口号
            //     // 即使没有connect成功也可以获取本地地址
            //     r = getsockname(channel_->fd(), (sockaddr *) &local, &alen);
            //     if (r < 0) {
            //         error("getsockname failed %d %s", errno, strerror(errno));
            //     }
            //     char sip[INET_ADDRSTRLEN] = { 0 };
            //     inet_ntop(AF_INET, &(((struct sockaddr_in *)&local)->sin_addr), sip, INET_ADDRSTRLEN);

            //     Ip4Addr t1(local);
            //     info("kernel assign address %s:%d, %s", sip, ntohs(local.sin_port), t1.toString().c_str());
            // }

            if (statecb_) {
                statecb_(con);
            }
        }
    } else {
        trace("poll fd %d return %d revents %d", channel_->fd(), r, pfd.revents);
        cleanup(con);
        return -1;
    }
    return 0; // 非0才为真，这里是假
}

void TcpConn::handleWrite(const TcpConnPtr &con) {
    if (state_ == State::Handshaking) {
        // 当客户端连接失败也会到这里来
        handleHandshake(con);
    } else if (state_ == State::Connected) {
        ssize_t sended = isend(output_.begin(), output_.size());
        if (sended == -1)
        {
            cleanup(con);
            return;
        }
        
        output_.consume(sended);

        // 只有当为空的时候才回调给客户端。否则说明数据没有写完，不能再写了
        if (output_.empty() && writablecb_) {
            writablecb_(con);
        }
        // 当没有缓存的数据后，就取消监听可写事件
        // 如果客户端有数据要写，则这里不会为空
        if (output_.empty() && channel_->writeEnabled()) {  // writablecb_ may write something
            channel_->enableWrite(false);
        }
    } else {
        error("handle write unexpected");
    }
}

ssize_t TcpConn::isend(const char *buf, size_t len) {
    size_t sended = 0;
    while (len > sended) { // 当 等于 时就退出循化，说明写完了
        ssize_t wd = writeImp(channel_->fd(), buf + sended, len - sended);
        trace("channel %lld fd %d write %ld bytes", (long long) channel_->id(), channel_->fd(), wd);
        if (wd > 0) {
            sended += wd;
            continue;
        } else if (wd == -1 && errno == EINTR) { // 系统调用被中断
            continue;
        } else if (wd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) { // 已经写满了不能再写
            if (!channel_->writeEnabled()) { // 如果但前没有注册可写事件，则注册
                channel_->enableWrite(true);
            }
            break;
        } else { // 这里还有一些未处理掉的，如 SIGPIPE。表示往对端已经关闭的socket上写数据时就会触发
            error("write error: channel %lld fd %d wd %ld %d %s", (long long) channel_->id(), channel_->fd(), wd, errno, strerror(errno));
            // ECONNRESET 104 /* Connection reset by peer */
            if ((errno == ECONNRESET) || (errno == ETIMEDOUT) || (errno == EPIPE))
            {
                // 远程已经断开连接没有必要继续了
                // 在服务端依然会崩溃，加入ETIMEDOUT 和 EPIPE 的判断
                return -1;
            }
            
            break;
        }
    }
    return sended;
}

void TcpConn::send(Buffer &buf) {
    if (channel_) {
        // 如果当前已经监听了可写事件，则说明之前已经有其他县城触发了写
        if (channel_->writeEnabled()) {  // just full
            output_.absorb(buf); //合并
        }
        if (buf.size()) { // 如果合并字符串了，这里就会为false，就不会进来
            ssize_t sended = isend(buf.begin(), buf.size());
            if (sended == -1)
            {
                TcpConnPtr con1 = shared_from_this();
                cleanup(con1);
                return;
            }

            buf.consume(sended); // 将消费了的字符串丢掉
        }
        if (buf.size()) { // 如果本次没有消费完，则保留到 output
            output_.absorb(buf);
            if (!channel_->writeEnabled()) {
                channel_->enableWrite(true);
            }
        }
    } else {
        trace("connection %s - %s closed, but still writing %lu bytes", local_.toString().c_str(), peer_.toString().c_str(), buf.size());
    }
}

void TcpConn::send(const char *buf, size_t len) {
    if (channel_) {
        if (output_.empty()) {
            ssize_t sended = isend(buf, len);
            if (sended == -1)
            {
                TcpConnPtr con1 = shared_from_this();
                cleanup(con1);
                return;
            }
            
            buf += sended;
            len -= sended;
        }
        if (len) {
            output_.append(buf, len);
        }
    } else {
        trace("connection %s - %s closed, but still writing %lu bytes", local_.toString().c_str(), peer_.toString().c_str(), len);
    }
}

void TcpConn::onMsg(CodecBase *codec, const MsgCallBack &cb) {
    assert(!readcb_);
    codec_.reset(codec);
    onRead([cb](const TcpConnPtr &con) {
        int r = 1;
        while (r) {  // 当下次解析出来的为 0 时/false，就退出循环了
            Slice msg;
            r = con->codec_->tryDecode(con->getInput(), msg);
            if (r < 0) {
                con->channel_->close();
                break;
            } else if (r > 0) {
                debug("消息解码成功. 原始长度 %d，消息长度 %ld", r, msg.size());
                cb(con, msg);
                con->getInput().consume(r);
            } 
            // 无意义的打印，当完全解析后肯定就为0了
            /*
            else {
                std::string s = msg;
                warn("[%s] 解码出来的数据长度为 len 0，但是待解码数据长度为 %d", s.c_str(), con->getInput().size());
            }*/
        }
    });
}

void TcpConn::sendMsg(Slice msg) {
    codec_->encode(msg, getOutput());
    sendOutput();
}

TcpServer::TcpServer(EventBases *bases) : base_(bases->allocBase()), bases_(bases), listen_channel_(NULL), createcb_([] { return TcpConnPtr(new TcpConn); }) {}

int TcpServer::bind(const std::string &host, unsigned short port, bool reusePort) {
    info("[%p] 将要启动tcp监听 %s:%d", this, host.c_str(), port);

    addr_ = Ip4Addr(host, port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int r = net::setReuseAddr(fd);
    fatalif(r, "set socket reuse option failed");
    r = net::setReusePort(fd, reusePort);
    fatalif(r, "set socket reuse port option failed");
    r = util::addFdFlag(fd, FD_CLOEXEC);
    fatalif(r, "addFdFlag FD_CLOEXEC failed");
    r = ::bind(fd, (struct sockaddr *) &addr_.getAddr(), sizeof(struct sockaddr));
    if (r) {
        close(fd);
        error("bind to %s failed %d %s", addr_.toString().c_str(), errno, strerror(errno));
        return errno;
    }
    r = listen(fd, 20);
    fatalif(r, "listen failed %d %s", errno, strerror(errno));
    info("fd %d listening at %s", fd, addr_.toString().c_str());
    listen_channel_ = new Channel(base_, fd, kReadEvent);
    listen_channel_->onRead([this] { handleAccept(); });
    return 0;
}

TcpServerPtr TcpServer::startServer(EventBases *bases, const std::string &host, unsigned short port, bool reusePort) {
    TcpServerPtr p(new TcpServer(bases));
    int r = p->bind(host, port, reusePort);
    if (r) {
        error("bind to %s:%d failed %d %s", host.c_str(), port, errno, strerror(errno));
    }
    return r == 0 ? p : NULL;
}

void TcpServer::handleAccept() {
    struct sockaddr_in raddr;
    socklen_t rsz = sizeof(raddr);
    int lfd = listen_channel_->fd();
    int cfd;
    while (lfd >= 0 && (cfd = accept(lfd, (struct sockaddr *) &raddr, &rsz)) >= 0) {
        sockaddr_in peer, local;
        socklen_t alen = sizeof(peer);
        int r = getpeername(cfd, (sockaddr *) &peer, &alen);
        if (r < 0) {
            error("get peer name failed %d %s", errno, strerror(errno));
            continue;
        }
        r = getsockname(cfd, (sockaddr *) &local, &alen);
        if (r < 0) {
            error("getsockname failed %d %s", errno, strerror(errno));
            continue;
        }
        r = util::addFdFlag(cfd, FD_CLOEXEC);
        fatalif(r, "addFdFlag FD_CLOEXEC failed");
        //r = net::setKeepAlived(cfd, 30, 30, 10);
        //fatalif(r, "set keepalived failed");

        EventBase *b = bases_->allocBase();
        auto addcon = [=] {
            TcpConnPtr con = createcb_();
            con->attach(b, cfd, local, peer);
            if (statecb_) {
                con->onState(statecb_);
            }
            if (readcb_) {
                con->onRead(readcb_);
            }
            if (msgcb_) {
                con->onMsg(codec_->clone(), msgcb_);
            }
        };
        if (b == base_) {
            addcon();
        } else {
            b->safeCall(move(addcon));
        }
    }
    if (lfd >= 0 && errno != EAGAIN && errno != EINTR) {
        warn("accept return %d  %d %s", cfd, errno, strerror(errno));
    }
}

HSHAPtr HSHA::startServer(EventBase *base, const std::string &host, unsigned short port, int threads) {
    HSHAPtr p = HSHAPtr(new HSHA(threads));
    p->server_ = TcpServer::startServer(base, host, port);
    return p->server_ ? p : NULL;
}

void HSHA::onMsg(CodecBase *codec, const RetMsgCallBack &cb) {
    server_->onConnMsg(codec, [this, cb](const TcpConnPtr &con, Slice msg) {
        std::string input = msg;
        threadPool_.addTask([=] {
            std::string output = cb(con, input); // 处理完消息后的 response
            server_->getBase()->safeCall([=] {
                if (output.size()) // 将 写入消息 加入县城任务队列
                    con->sendMsg(output);
            });
        });
    });
}

}  // namespace handy
