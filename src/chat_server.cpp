// 聊天服务器（多线程：1个监听 + N个worker）
// 线程模型：主线程只负责accept，然后把fd分发给某个worker；
// 每个worker拥有自己的 event_base，负责该worker名下连接的读写与回调。
// 关键：bufferevent 用 BEV_OPT_THREADSAFE，允许跨线程写（广播时可直接 write）。

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <sstream>
#include <cctype>
#include <thread>
#include <mutex>
#include <deque>
#include <netinet/tcp.h>

using json = nlohmann::json;
constexpr int PORT = 12345;

// ===== 背压参数 =====
constexpr size_t WRITE_BLOCK  = 1 << 20;    // 1MB：开始对该接收者排队，不再直接写
constexpr size_t WRITE_RESUME = 256 << 10;  // 256KB：触发 write_cb，继续从队列刷
constexpr size_t WRITE_CUTOFF = 4 << 20;    // 4MB：判定极慢（这里仍做断开）
constexpr size_t BACKLOG_MAX  = 256;        // 每连接最多排队 256 条；溢出则丢弃该条

// 每连接的待发送队列（慢接收者排队区）
static std::unordered_map<bufferevent*, std::deque<std::string>> g_sendq;
static inline size_t outbuf_len(bufferevent* bev) {
    evbuffer* out = bufferevent_get_output(bev);
    return evbuffer_get_length(out);
}

// ---------------- 全局并发控制 ----------------
std::mutex g_mtx;  // 保护 g_rooms / g_subs / g_users / g_clients / g_sendq

// ---------------- 连接表（仅用于统计在线数） ----------------
std::vector<bufferevent*> g_clients;

// ---------------- 房间数据结构 ----------------
struct Room {
    std::string id;
    std::unordered_set<bufferevent*> members; // O(1) 插删
};

std::unordered_map<std::string, Room> g_rooms;  // roomId -> Room
std::unordered_map<bufferevent*, std::unordered_set<std::string>> g_subs; // bev -> {roomId,...}

// ---------------- 用户（UID / 昵称） ----------------
struct User {
    std::string id;    // 稳定短ID
    std::string name;  // 昵称
};
std::unordered_map<bufferevent*, User> g_users;
std::atomic<uint64_t> g_uid_counter{1};

std::unordered_map<std::string, bufferevent*> g_uid2bev;

static std::string gen_uid_hex() {
    uint64_t n = g_uid_counter.fetch_add(1);
    std::ostringstream oss;
    oss << std::hex << n;
    return oss.str(); // "1","2","3"... hex 字符串
}

static bool valid_nick(const std::string& s) {
    if (s.empty() || s.size() > 20) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

// ---------------- 工具函数（线程安全用法） ----------------
static json user_snapshot_unsafe(bufferevent* bev) {
    // 只在已加锁的情况下调用；外部不再对 user_json 进行二次加锁
    auto it = g_users.find(bev);
    if (it == g_users.end()) return {{"id","?"},{"name","?"}};
    return json{{"id", it->second.id}, {"name", it->second.name}};
}

// 统一的“关闭并清理”函数（可在任意线程调用）
static void close_client_and_cleanup(bufferevent* bev, const char* why) {
    std::vector<std::string> rooms;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (auto it = g_subs.find(bev); it != g_subs.end()) {
            
            rooms.assign(it->second.begin(), it->second.end());
        }
    }
    for (auto& r : rooms) {
        // 锁外逐房退订 + 广播
        extern void leave_room(bufferevent*, const std::string&, bool);
        leave_room(bev, r, /*silent=*/false);
    }
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (auto it = g_users.find(bev); it != g_users.end()) {
            g_uid2bev.erase(it->second.id);
        }

        g_sendq.erase(bev);
        g_subs.erase(bev);
        g_users.erase(bev);
        g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), bev), g_clients.end());
        total = g_clients.size();
    }
    bufferevent_free(bev);
    std::cout << "Client closed (" << why << "), total: " << total << '\n';
}

// 包装发送：根据 outbuf 大小决定 直接写 / 入队 / 丢弃 / 断开
bool send_json(bufferevent* bev, const json& j) {
    std::string s = j.dump();
    s.push_back('\n');

    const size_t outlen = outbuf_len(bev);

    // 极慢：达到截断阈值 → 断开（保命策略）
    if (outlen > WRITE_CUTOFF) {
        close_client_and_cleanup(bev, "write cutoff");
        return false;
    }

    // 慢：达到阻塞阈值 → 入队；若队列也满了 → 丢弃该条（不踢人）
    if (outlen > WRITE_BLOCK) {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto& q = g_sendq[bev];
        if (q.size() >= BACKLOG_MAX) {
            // 排队溢出：丢弃该条
            return false;
        }
        q.push_back(std::move(s));
        return false;
    }

    // 快：直接写
    bufferevent_write(bev, s.data(), s.size());
    return true;
}

static inline void send_err(bufferevent* bev, const std::string& info, const std::string& room = "") {
    json j = {{"type","error"},{"info",info}};
    if (!room.empty()) j["room"] = room;
    send_json(bev, j);
}

void room_broadcast(const std::string& room, const json& j, bufferevent* except = nullptr) {
    // 锁内收集 -> 锁外发送
    std::vector<bufferevent*> recipients;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_rooms.find(room);
        if (it == g_rooms.end()) return;
        recipients.reserve(it->second.members.size());
        for (auto* b : it->second.members)
            if (b != except) recipients.push_back(b);
    }
    for (auto* b : recipients) send_json(b, j);
}

inline bool in_room(bufferevent* bev, const std::string& r) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_rooms.find(r);
    return it != g_rooms.end() && it->second.members.count(bev) > 0;
}

void join_room(bufferevent* bev, const std::string& r) {
    bool already = false;
    int members_after = 0;
    json u;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto& room = g_rooms[r];
        if (room.id.empty()) room.id = r;

        already = !room.members.insert(bev).second;
        members_after = (int)room.members.size();
        g_subs[bev].insert(r);
        u = user_snapshot_unsafe(bev); // 锁内拍快照
    }

    if (already) {
        send_json(bev, {{"type","sys"},{"room",r},{"info","already in room"},
                        {"members", members_after},
                        {"user", u}});
        return;
    }

    room_broadcast(r, {
        {"type","sys"},{"room",r},{"info","joined"},
        {"members", members_after},
        {"user", u}
    });
}

void leave_room(bufferevent* bev, const std::string& r, bool silent=false) {
    bool existed = false;
    int members_after = 0;
    bool room_empty = false;
    json u;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_rooms.find(r);
        if (it == g_rooms.end()) return;

        auto& room = it->second;
        existed = (room.members.erase(bev) > 0);
        if (existed) {
            g_subs[bev].erase(r);
            members_after = (int)room.members.size();
            u = user_snapshot_unsafe(bev);
            if (room.members.empty()) {
                g_rooms.erase(it);
                room_empty = true;
            }
        }
    }

    if (!existed) return;
    if (!silent) {
        room_broadcast(r, {
            {"type","sys"},{"room",r},{"info","left"},
            {"members", members_after},
            {"user", u}
        });
    }
    (void)room_empty; // 这里不需要额外动作，已回收空房
}

// ---------------- 读回调：按行取完整 JSON ----------------
void read_cb(bufferevent* bev, void*) {
    evbuffer* in = bufferevent_get_input(bev);

    while (true) {
        evbuffer_ptr eol = evbuffer_search_eol(in, nullptr, nullptr, EVBUFFER_EOL_LF);
        if (eol.pos == -1) break;

        size_t len = eol.pos + 1; // 包含 '\n'
        std::string line(len, '\0');
        evbuffer_remove(in, line.data(), len);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();

        json msg;
        try {
            msg = json::parse(line);
        } catch (const std::exception& e) {
            send_err(bev, e.what());
            continue;
        }

        if (!msg.contains("type") || !msg["type"].is_string()) {
            send_err(bev, "missing or invalid 'type'"); continue;
        }

        const std::string type = msg["type"].get<std::string>();

        if (type == "nick") {
            if (!msg.contains("name") || !msg["name"].is_string()) {
                send_err(bev, "missing 'name' for nick"); continue;
            }
            std::string name = msg["name"].get<std::string>();
            if (!valid_nick(name)) {
                send_err(bev, "invalid nickname (1-20 chars, [A-Za-z0-9_-])"); continue;
            }

            // 锁内：写昵称并快照订阅与用户；锁外：通知
            std::vector<std::string> subs_snapshot;
            json u;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_users[bev].name = name;
                auto it_sub = g_subs.find(bev);
                if (it_sub != g_subs.end())
                    subs_snapshot.assign(it_sub->second.begin(), it_sub->second.end());
                u = user_snapshot_unsafe(bev);
            }

            send_json(bev, {{"type","sys"},{"info","nick ok"},{"user", u}});
            for (const auto& r : subs_snapshot) {
                room_broadcast(r, {{"type","sys"},{"room",r},{"info","renamed"},{"user", u}});
            }
        }
        else if (type == "join") {
            if (!msg.contains("room") || !msg["room"].is_string()) { send_err(bev, "missing 'room' for join"); continue; }
            std::string r = msg["room"].get<std::string>();
            if (r.empty()) { send_err(bev, "room id empty"); continue; }
            join_room(bev, r);
        }
        else if (type == "leave") {
            if (!msg.contains("room") || !msg["room"].is_string()) { send_err(bev, "missing 'room' for leave"); continue; }
            std::string r = msg["room"].get<std::string>();
            if (r.empty()) { send_err(bev, "room id empty"); continue; }
            leave_room(bev, r);
        }
        else if (type == "msg") {
            if (!msg.contains("room") || !msg["room"].is_string()) { send_err(bev, "missing 'room' for msg"); continue; }
            if (!msg.contains("body") || !msg["body"].is_string()) { send_err(bev, "missing 'body' for msg", msg["room"]); continue; }
            std::string r = msg["room"].get<std::string>();
            if (!in_room(bev, r)) { send_err(bev, "not in room", r); continue; }

            json u;
            { std::lock_guard<std::mutex> lk(g_mtx); u = user_snapshot_unsafe(bev); }

            json out = { {"type","msg"}, {"room", r}, {"body", msg["body"]}, {"user", u} };
            room_broadcast(r, out, bev);
        }
        else if (type == "pm") {
            // 校验
            if (!msg.contains("to")   || !msg["to"].is_string())   { send_err(bev, "pm missing 'to'");   continue; }
            if (!msg.contains("body") || !msg["body"].is_string()) { send_err(bev, "pm missing 'body'"); continue; }
            const std::string to   = msg["to"].get<std::string>();
            const std::string body = msg["body"].get<std::string>();

            // 查找目标连接
            bufferevent* tgt = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                auto it = g_uid2bev.find(to);
                if (it != g_uid2bev.end()) tgt = it->second;
            }
            if (!tgt) { send_err(bev, "user offline"); continue; }

            // 发送方信息快照
            json from_u;
            { std::lock_guard<std::mutex> lk(g_mtx); from_u = user_snapshot_unsafe(bev); }

            // 组包
            json out = {
                {"type","pm"},
                {"from", from_u},    // {"id": "...", "name": "..."}
                {"body", body}
            };

            // 跨线程安全写：incref 防止并发 free（send_json 内部有背压/断开策略）
            bufferevent_incref(tgt);
            send_json(tgt, out);
            bufferevent_decref(tgt);

            // 可选：回执给自己
            // send_json(bev, {{"type","pm-ack"}, {"to", to}});
        }
        else {
            send_err(bev, "unknown type: " + type);
        }
    }
}

// 写回调：outbuf ≤ WRITE_RESUME 时触发 → 从队列继续刷
static void write_cb(bufferevent* bev, void*){
    if (outbuf_len(bev) > WRITE_RESUME) return;

    std::deque<std::string> tmp;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_sendq.find(bev);
        if (it == g_sendq.end() || it->second.empty()) return;
        tmp.swap(it->second);
    }
    for (auto& s : tmp) {
        if (outbuf_len(bev) > WRITE_BLOCK) {
            std::lock_guard<std::mutex> lk(g_mtx);
            auto& q = g_sendq[bev];
            q.emplace_front(std::move(s));                  // 当前这一条回塞
            while (!tmp.empty()) {                          // 剩余也回塞
                q.emplace_front(std::move(tmp.back()));
                tmp.pop_back();
            }
            return;
        }
        bufferevent_write(bev, s.data(), s.size());
    }
}

// ---------------- 连接事件回调：EOF / 错误 / 超时 时清理订阅并释放 ----------------
void event_cb(bufferevent* bev, short what, void*) {
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        const char* why = (what & BEV_EVENT_TIMEOUT) ? "timeout" :
                          (what & BEV_EVENT_ERROR)   ? "error"   : "eof";
        close_client_and_cleanup(bev, why);
    }
}

// ========== 多线程：Worker 线程与分发机制 ==========

struct Worker {
    event_base* base = nullptr;
    event* notify_ev = nullptr;
    evutil_socket_t notify_recv = -1; // worker 线程读这一端
    evutil_socket_t notify_send = -1; // 主线程写这一端
    std::thread thr;
};

// 所有 worker
std::vector<Worker> g_workers;
std::atomic<uint32_t> g_rr{0}; // 轮询分发

static void enable_tcp_keepalive(evutil_socket_t fd){
    int yes=1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
#ifdef __linux__
    int idle=60, intvl=10, cnt=3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl,sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,  sizeof(cnt));
#endif
}


// 在 worker 线程里：收到一个 fd，就把它包成 bufferevent 并注册回调
void on_new_client_fd(Worker* w, evutil_socket_t cfd) {
    // 使 socket 非阻塞（稳妥起见，虽然 bufferevent 会处理，但这里再设置一遍）
    evutil_make_socket_nonblocking(cfd);
    enable_tcp_keepalive(cfd);
    // 关键：BEV_OPT_THREADSAFE，允许跨线程安全写（广播时很有用）
    auto bev = bufferevent_socket_new(w->base, cfd,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    if (!bev) {
        ::close(cfd);
        return;
    }

    bufferevent_setcb(bev, read_cb, write_cb, event_cb, nullptr);
    // 写低水位：当 outbuf ≤ WRITE_RESUME 时触发 write_cb（继续刷队列）
    bufferevent_setwatermark(bev, EV_WRITE, WRITE_RESUME, 0);

    // 写/读超时：长期刷不掉算慢客户端
    timeval rto{30,0}, wto{30,0};
    bufferevent_set_timeouts(bev, nullptr, &wto);

    bufferevent_enable(bev, EV_READ | EV_WRITE);

    // 创建默认用户（guest-xxxx）并计数
    User u;
    u.id = gen_uid_hex();
    std::string tail = (u.id.size() > 4) ? u.id.substr(u.id.size()-4) : u.id;
    u.name = "guest-" + tail;

    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_users.emplace(bev, u);          
        g_uid2bev[u.id] = bev;            
        g_clients.push_back(bev);
        total = g_clients.size();
    }
    send_json(bev, {
    {"type","login"},
    {"user", { {"id", u.id}, {"name", u.name} }}
    });
    std::cout << "[worker@" << (void*)w->base << "] New client, total: " << total << '\n';
}

// worker 的“通知事件”回调：从 notify_recv 读取若干个fd
static void notify_cb(evutil_socket_t fd, short, void* arg) {
    Worker* w = static_cast<Worker*>(arg);
    while (true) {
        evutil_socket_t cfd;
        ssize_t n = recv(fd, &cfd, sizeof(cfd), 0);
        if (n == (ssize_t)sizeof(cfd)) {
            on_new_client_fd(w, cfd);
        } else {
            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // 没有更多数据
            }
            break;
        }
    }
}

static void worker_thread_main(Worker* w) {
    w->base = event_base_new();

    // 建立可读事件，监听 notify_recv
    w->notify_ev = event_new(w->base, w->notify_recv, EV_READ | EV_PERSIST, notify_cb, w);
    event_add(w->notify_ev, nullptr);

    // 跑事件循环
    event_base_dispatch(w->base);

    event_free(w->notify_ev);
    event_base_free(w->base);
}

// 创建 N 个 worker：每个 worker 用 socketpair 跟主线程通信
void start_workers(int n) {
    g_workers.resize(n);
    for (int i = 0; i < n; ++i) {
        evutil_socket_t fds[2];
        if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
            perror("socketpair");
            exit(1);
        }
        // worker 读 fds[0]，主线程写 fds[1]
        evutil_make_socket_nonblocking(fds[0]);
        evutil_make_socket_nonblocking(fds[1]);
        g_workers[i].notify_recv = fds[0];
        g_workers[i].notify_send = fds[1];

        g_workers[i].thr = std::thread(worker_thread_main, &g_workers[i]);
    }
}

// 将新accept的fd分发给某个worker（轮询）
void dispatch_to_worker(evutil_socket_t cfd) {
    uint32_t idx = g_rr.fetch_add(1);
    Worker& w = g_workers[idx % g_workers.size()];

    // 直接把 fd 整数写过去（同一进程，fd 表是共享的）
    ssize_t n = send(w.notify_send, &cfd, sizeof(cfd), 0);
    if (n != (ssize_t)sizeof(cfd)) {
        // 分发失败就关闭连接，避免泄漏
        ::close(cfd);
    }
}

// ---------------- 监听回调：主线程只负责分发，不创建 bufferevent ----------------
void listener_cb(evconnlistener*, evutil_socket_t fd, sockaddr*, int, void*) {
    evutil_make_socket_nonblocking(fd);
    dispatch_to_worker(fd);
}

// ---------------- main：启动线程与监听 ----------------
int main() {
    // 1) 开启 libevent 线程支持
    evthread_use_pthreads();

    // 2) 启动 workers（默认：min(8, 硬件线程数)；也可固定为你机器核数）
    int hw = (int)std::thread::hardware_concurrency();
    if (hw <= 0) hw = 4;
    int NUM_WORKERS = std::min(8, hw);
    start_workers(NUM_WORKERS);

    // 3) 主线程仅用于 accept
    event_base* base = event_base_new();

    sockaddr_in sin{};
    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port        = htons(PORT);

    evconnlistener* listener =
        evconnlistener_new_bind(base, listener_cb, nullptr,
                                LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
                                -1, reinterpret_cast<sockaddr*>(&sin),
                                sizeof(sin));
    if (!listener) { std::cerr << "Listener error\n"; return 1; }

    std::cout << "libevent chat server (rooms + user) on " << PORT
              << " with " << NUM_WORKERS << " workers\n";

    event_base_dispatch(base);

    // （通常不会到这里，省略回收；生产中应添加优雅退出逻辑）
    evconnlistener_free(listener);
    event_base_free(base);
    return 0;
}
