#include "event_base.h"
#include <fcntl.h>
#include <string.h>
#include <map>
#include "conn.h"
#include "logging.h"
#include "poller.h"
#include "util.h"
using namespace std;

namespace handy {

namespace {

struct TimerRepeatable {
    int64_t at;  // current timer timeout timestamp
    int64_t interval;
    TimerId timerid;
    Task cb;
};

struct IdleNode {
    TcpConnPtr con_;
    int64_t updated_;
    TcpCallBack cb_;
};

}  // namespace

struct IdleIdImp {
    IdleIdImp() {}
    typedef list<IdleNode>::iterator Iter;
    IdleIdImp(list<IdleNode> *lst, Iter iter) : lst_(lst), iter_(iter) {}
    list<IdleNode> *lst_;
    Iter iter_;
};

struct EventsImp {
    EventBase *base_;
    PollerBase *poller_;
    std::atomic<bool> exit_;
    int wakeupFds_[2];
    int nextTimeout_;
    SafeQueue<Task> tasks_;

    std::map<TimerId, TimerRepeatable> timerReps_; // 循环定时器
    std::map<TimerId, Task> timers_; // KV，使用map自动排序。但也不是县城安全的
    std::atomic<int64_t> timerSeq_;
    // 记录每个idle时间（单位秒）下所有的连接。链表中的所有连接，最新的插入到链表末尾。连接若有活动，会把连接从链表中移到链表尾部，做法参考memcache
    std::map<int, std::list<IdleNode>> idleConns_;
    std::set<TcpConnPtr> reconnectConns_;
    bool idleEnabled;

    EventsImp(EventBase *base, int taskCap);
    ~EventsImp();
    void init();
    void callIdles();
    IdleId registerIdle(int idle, const TcpConnPtr &con, const TcpCallBack &cb);
    void unregisterIdle(const IdleId &id);
    void updateIdle(const IdleId &id);
    void handleTimeouts();
    void refreshNearest(const TimerId *tid = NULL);
    void repeatableTimeout(TimerRepeatable *tr);

    // eventbase functions
    EventBase &exit() {
        exit_ = true;
        wakeup();
        return *base_;
    }
    bool exited() { return exit_; }
    void safeCall(Task &&task) {
        tasks_.push(move(task));
        wakeup();
    }
    void loop();
    void loop_once(int waitMs) {
        poller_->loop_once(std::min(waitMs, nextTimeout_));
        handleTimeouts();
    }
    void wakeup() {
        // 向用于唤醒的管道写数据
        int r = write(wakeupFds_[1], "", 1); // 写入空数据
        fatalif(r <= 0, "write error wd %d %d %s", r, errno, strerror(errno));
    }

    bool cancel(TimerId timerid);
    TimerId runAt(int64_t milli, Task &&task, int64_t interval);
};

EventBase::EventBase(int taskCapacity) {
    imp_.reset(new EventsImp(this, taskCapacity));
    imp_->init();
}

EventBase::~EventBase() {}

EventBase &EventBase::exit() {
    return imp_->exit();
}

bool EventBase::exited() {
    return imp_->exited();
}

void EventBase::safeCall(Task &&task) {
    imp_->safeCall(move(task));
}

void EventBase::wakeup() {
    imp_->wakeup();
}

void EventBase::loop() {
    imp_->loop();
}

void EventBase::loop_once(int waitMs) {
    imp_->loop_once(waitMs);
}

bool EventBase::cancel(TimerId timerid) {
    return imp_ && imp_->cancel(timerid);
}

TimerId EventBase::runAt(int64_t milli, Task &&task, int64_t interval) {
    return imp_->runAt(milli, std::move(task), interval);
}

EventsImp::EventsImp(EventBase *base, int taskCap)
    : base_(base), poller_(createPoller()), exit_(false), nextTimeout_(1 << 30), tasks_(taskCap), timerSeq_(0), idleEnabled(false) {}

void EventsImp::loop() {
    while (!exit_)
        loop_once(10000);
    timerReps_.clear();
    timers_.clear();
    idleConns_.clear();
    for (auto recon : reconnectConns_) {  //重连的连接无法通过channel清理，因此单独清理
        recon->cleanup(recon);
    }
    loop_once(0);
}

void EventsImp::init() {
    // pipe函数创建匿名管道，将为输入的两个文件描述符赋值，并通过特殊的方式连接在一起，
    //写到file_descriptor[1]的数据都能从file_descriptor[0]读出来
    int r = pipe(wakeupFds_);
    fatalif(r, "pipe failed %d %s", errno, strerror(errno));
    r = util::addFdFlag(wakeupFds_[0], FD_CLOEXEC);
    fatalif(r, "addFdFlag failed %d %s", errno, strerror(errno));
    r = util::addFdFlag(wakeupFds_[1], FD_CLOEXEC);
    fatalif(r, "addFdFlag failed %d %s", errno, strerror(errno));
    trace("wakeup pipe created %d %d", wakeupFds_[0], wakeupFds_[1]);

    // 监听匿名管道[0]读事件kReadEvent。原始指针似乎没有delete?
    Channel *ch = new Channel(base_, wakeupFds_[0], kReadEvent);
    ch->onRead([=] {
        char buf[1024] = {0};
        int r = ch->fd() >= 0 ? ::read(ch->fd(), buf, sizeof buf) : 0;
        if (r > 0) {
            info("read[%d] %s", r, buf);
            // 这个管道的目的是用来通知做任务的，现在不需要里面有什么数据。但是，ch->fd() 是在另外一个地方赋值
            Task task;
            // 如果有任务就一直执行
            while (tasks_.pop_wait(&task, 0)) {
                task();
            }
        } else if (r == 0) {
            delete ch;
        } else if (errno == EINTR) {
        } else {
            fatal("wakeup channel read error %d %d %s", r, errno, strerror(errno));
        }
    });
}

void EventsImp::handleTimeouts() {
    int64_t now = util::timeMilli();
    TimerId tid{now, 1L << 62};
    
    // 回调方法，并移出。遍历，会导致延迟。若使用多县城呢
    while (timers_.size() && timers_.begin()->first < tid) {
        Task task = move(timers_.begin()->second);
        timers_.erase(timers_.begin());
        task();
    }
    refreshNearest();
}

EventsImp::~EventsImp() {
    delete poller_;
    ::close(wakeupFds_[1]);
}

void EventsImp::callIdles() {
    int64_t now = util::timeMilli() / 1000;
    for (auto &l : idleConns_) {
        int idle = l.first;
        auto lst = l.second;
        while (lst.size()) {
            IdleNode &node = lst.front();
            if (node.updated_ + idle > now) {
                break;
            }
            node.updated_ = now;
            lst.splice(lst.end(), lst, lst.begin());
            node.cb_(node.con_);
        }
    }
}

IdleId EventsImp::registerIdle(int idle, const TcpConnPtr &con, const TcpCallBack &cb) {
    if (!idleEnabled) {
        base_->runAfter(1000, [this] { callIdles(); }, 1000);
        idleEnabled = true;
    }
    auto &lst = idleConns_[idle];
    lst.push_back(IdleNode{con, util::timeMilli() / 1000, move(cb)});
    trace("register idle");
    return IdleId(new IdleIdImp(&lst, --lst.end()));
}

void EventsImp::unregisterIdle(const IdleId &id) {
    trace("unregister idle");
    id->lst_->erase(id->iter_);
}

void EventsImp::updateIdle(const IdleId &id) {
    trace("update idle");
    id->iter_->updated_ = util::timeMilli() / 1000;
    id->lst_->splice(id->lst_->end(), *id->lst_, id->iter_);
}

void EventsImp::refreshNearest(const TimerId *tid) {
    if (timers_.empty()) {
        nextTimeout_ = 1 << 30;
    } else {
        const TimerId &t = timers_.begin()->first;
        nextTimeout_ = t.first - util::timeMilli(); // 计算得到下一次唤醒时间
        nextTimeout_ = nextTimeout_ < 0 ? 0 : nextTimeout_;
    }
}

void EventsImp::repeatableTimeout(TimerRepeatable *tr) {
    tr->at += tr->interval;
    tr->timerid = {tr->at, ++timerSeq_};
    timers_[tr->timerid] = [this, tr] { repeatableTimeout(tr); };
    refreshNearest(&tr->timerid);
    tr->cb();
}

TimerId EventsImp::runAt(int64_t milli, Task &&task, int64_t interval) {
    if (exit_) {
        return TimerId();
    }
    if (interval) {
        TimerId tid{-milli, ++timerSeq_};
        TimerRepeatable &rtr = timerReps_[tid];
        rtr = {milli, interval, {milli, ++timerSeq_}, move(task)};
        TimerRepeatable *tr = &rtr;
        timers_[tr->timerid] = [this, tr] { repeatableTimeout(tr); };
        refreshNearest(&tr->timerid);
        return tid;
    } else {
        TimerId tid{milli, ++timerSeq_};
        timers_.insert({tid, move(task)});
        refreshNearest(&tid);
        return tid;
    }
}

bool EventsImp::cancel(TimerId timerid) {
    if (timerid.first < 0) {
        auto p = timerReps_.find(timerid);
        auto ptimer = timers_.find(p->second.timerid);
        if (ptimer != timers_.end()) {
            timers_.erase(ptimer);
        }
        timerReps_.erase(p);
        return true;
    } else {
        auto p = timers_.find(timerid);
        if (p != timers_.end()) {
            timers_.erase(p);
            return true;
        }
        return false;
    }
}

void MultiBase::loop() {
    int sz = bases_.size();
    vector<thread> ths(sz - 1);
    for (int i = 0; i < sz - 1; i++) {
        thread t([this, i] { bases_[i].loop(); });
        ths[i].swap(t); // 这样效率更高，相比 ths[i] = t; 
    }
    bases_.back().loop(); // 当前县城也执行一个loop，使少用一个县城
    for (int i = 0; i < sz - 1; i++) {
        ths[i].join();
    }
}

// ==========================================、
// 将 conn.h 定义的方法放一部分这里了

Channel::Channel(EventBase *base, int fd, int events) : base_(base), fd_(fd), events_(events) {
    trace("[%p] channel constructor", this);
    fatalif(net::setNonBlock(fd_) < 0, "channel set non block failed");
    static atomic<int64_t> id(0); // 静态的
    id_ = ++id;
    poller_ = base_->imp_->poller_;
    poller_->addChannel(this);
}

Channel::~Channel() {
    trace("[%p] channel desctructor11", this);
    close();
    trace("[%p] channel 析构完成");
}

void Channel::enableRead(bool enable) {
    if (enable) {
        events_ |= kReadEvent;
    } else {
        events_ &= ~kReadEvent;
    }
    poller_->updateChannel(this);
}

void Channel::enableWrite(bool enable) {
    if (enable) {
        events_ |= kWriteEvent;
    } else {
        events_ &= ~kWriteEvent;
    }
    poller_->updateChannel(this);
}

void Channel::enableReadWrite(bool readable, bool writable) {
    if (readable) {
        events_ |= kReadEvent;
    } else {
        events_ &= ~kReadEvent;
    }
    if (writable) {
        events_ |= kWriteEvent;
    } else {
        events_ &= ~kWriteEvent;
    }
    poller_->updateChannel(this);
}

void Channel::close() {
    if (fd_ >= 0) {
        trace("close channel %ld fd %d", (long) id_, fd_);
        poller_->removeChannel(this);
        ::close(fd_);
        fd_ = -1;
        // 这里要回调出去。到 con->handleRead(con);
        handleRead();
    } else {
        trace("%ld channel not fd", (long)id_);
    }
}

bool Channel::readEnabled() {
    return events_ & kReadEvent;
}
bool Channel::writeEnabled() {
    return events_ & kWriteEvent;
}

void handyUnregisterIdle(EventBase *base, const IdleId &idle) {
    base->imp_->unregisterIdle(idle);
}

void handyUpdateIdle(EventBase *base, const IdleId &idle) {
    base->imp_->updateIdle(idle);
}

TcpConn::TcpConn()
    : base_(NULL), channel_(NULL), state_(State::Invalid), destPort_(-1), connectTimeout_(0), reconnectInterval_(-1), connectedTime_(util::timeMilli()) 
    {
        trace("[%p] tcp construct", this);
    }

TcpConn::~TcpConn() {
    trace("[%p] tcp destroyed %s - %s", this, local_.toString().c_str(), peer_.toString().c_str());
    delete channel_;
}

void TcpConn::addIdleCB(int idle, const TcpCallBack &cb) {
    if (channel_) {
        idleIds_.push_back(getBase()->imp_->registerIdle(idle, shared_from_this(), cb));
    }
}

// 哪些地方会触发重连呢？
// 尴噶，TcpConn::reconnect 的声明在 conn.h。而实现在这个 event_base.cc文件
void TcpConn::reconnect() {
    auto con = shared_from_this();
    getBase()->imp_->reconnectConns_.insert(con);
    long long interval = reconnectInterval_ - (util::timeMilli() - connectedTime_);
    interval = interval > 0 ? interval : 0;
    info("[%p] tcp reconnect interval: %d will reconnect after %lld ms", this, reconnectInterval_, interval);
    getBase()->runAfter(interval, [this, con]() {
        getBase()->imp_->reconnectConns_.erase(con);
        connect(getBase(), destHost_, (unsigned short) destPort_, connectTimeout_, localIp_);
    });
    if (channel_)
    {
        info("[%p] 将要删除 channel，它是[%p]", this, channel_);
    }
    {
        channel_->close(); // 内部也handle出来
    info("[%p] 先不删除，直接close并且NULL.后面要修改", this);
    }
    //delete channel_;
    channel_ = NULL;
}

}  // namespace handy