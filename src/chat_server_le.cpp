#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <nlohmann/json.hpp>
#include <vector>
#include <algorithm>
#include <iostream>
#include <event2/buffer.h>

using json = nlohmann::json;
constexpr int PORT = 12345;

std::vector<bufferevent*> g_clients;          // 全局连接表

// -------- 帮助函数：发一条 JSON（自动追加 '\n'） --------
void send_json(bufferevent* bev, const json& j)
{
    std::string s = j.dump();
    s.push_back('\n');
    bufferevent_write(bev, s.data(), s.size());
}

// -------- 读回调：按行取完整 JSON --------
void read_cb(bufferevent* bev, void*)
{
    evbuffer* in = bufferevent_get_input(bev);

    while (true) {
        // 查找 '\n' 行尾
        evbuffer_ptr eol = evbuffer_search_eol(in, nullptr, nullptr, EVBUFFER_EOL_LF);
        if (eol.pos == -1) break;                     // 还没收齐一行

        size_t len = eol.pos + 1;                     // 包含 '\n'
        std::string line(len, '\0');
        evbuffer_remove(in, line.data(), len);
        line.pop_back();                              // 去掉 '\n'

        json msg;
        try {
            msg = json::parse(line);
        } catch (const std::exception& e) {
            send_json(bev, {{"type","error"}, {"info",e.what()}});
            continue;
        }

        // 目前只处理 {"type":"msg", "body":"..."}
        if (msg.value("type","") == "msg") {
            // 广播给其他客户端
            for (auto other : g_clients)
                if (other != bev) send_json(other, msg);
        }
    }
}

// -------- 连接事件回调：EOF / 错误时移除 --------
void event_cb(bufferevent* bev, short what, void*)
{
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), bev),
                        g_clients.end());
        bufferevent_free(bev);
        std::cout << "Client left, total: " << g_clients.size() << '\n';
    }
}

// -------- listener 回调：接受新连接 --------
void listener_cb(evconnlistener*, evutil_socket_t fd,
                 sockaddr*, int, void* ctx)
{
    auto base = static_cast<event_base*>(ctx);
    auto bev  = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);

    bufferevent_setcb(bev, read_cb, nullptr, event_cb, nullptr);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    g_clients.push_back(bev);
    std::cout << "New client, total: " << g_clients.size() << '\n';
}

int main()
{
    event_base* base = event_base_new();

    sockaddr_in sin{};
    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port        = htons(PORT);

    evconnlistener* listener =
        evconnlistener_new_bind(base, listener_cb, base,
                                LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
                                -1, reinterpret_cast<sockaddr*>(&sin),
                                sizeof(sin));
    if (!listener) { std::cerr << "Listener error\n"; return 1; }

    std::cout << "libevent chat server on " << PORT << '\n';
    event_base_dispatch(base);

    evconnlistener_free(listener);
    event_base_free(base);
    return 0;
}
