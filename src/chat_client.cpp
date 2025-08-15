// chat_client.cpp — rooms + nickname + PM
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>              // ← 用于解析 /pm 命令

#include <nlohmann/json.hpp>
using json = nlohmann::json;

constexpr int PORT = 12345;                 // 与服务器一致
constexpr char SERVER_IP[] = "127.0.0.1";
constexpr size_t BUFFER_SIZE = 1024;

// ========== 自身用户信息（从服务器 login / nick 回执学习） ==========
static std::string g_my_id;
static std::string g_my_name;

// ---------- 小工具 ----------
static bool send_json_line(int sock, const json& j) {
    std::string out = j.dump();
    out.push_back('\n');
    ssize_t n = send(sock, out.data(), out.size(), 0);
    return n == static_cast<ssize_t>(out.size());
}

static inline std::string trim(const std::string& s) {
    size_t l = 0, r = s.size();
    while (l < r && std::isspace(static_cast<unsigned char>(s[l]))) ++l;
    while (r > l && std::isspace(static_cast<unsigned char>(s[r-1]))) --r;
    return s.substr(l, r-l);
}

static inline std::string short_id(const std::string& id) {
    return id.size() > 4 ? id.substr(id.size()-4) : id;
}

static std::string user_tag_from(const json& msg) {
    // 期望形如: "user": {"id":"...", "name":"..."}
    std::string name = "?", id = "?";
    if (msg.contains("user") && msg["user"].is_object()) {
        const auto& u = msg["user"];
        if (u.contains("name") && u["name"].is_string()) name = u["name"].get<std::string>();
        if (u.contains("id")   && u["id"].is_string())   id   = u["id"].get<std::string>();
    }
    std::string tail = short_id(id);
    std::string mine = (!g_my_id.empty() && id == g_my_id) ? " (you)" : "";
    return name + "#" + tail + mine;
}

static bool valid_nick_local(const std::string& s) {
    if (s.empty() || s.size() > 20) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c=='_' || c=='-')) return false;
    return true;
}

// ---------- 读取线程：一行一 JSON ----------
void receive_messages(int sock)
{
    std::string buf;
    char chunk[BUFFER_SIZE];

    while (true) {
        ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) break;                // 断连
        buf.append(chunk, n);

        // 可能一次收到多行；逐行处理
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);

            try {
                json msg = json::parse(line);
                const std::string type = msg.value("type", "");

                if (type == "login") {
                    // 连接成功后的首次回执：拿到自己的 uid/name
                    if (msg.contains("user") && msg["user"].is_object()) {
                        const auto& u = msg["user"];
                        if (u.contains("id") && u["id"].is_string())   g_my_id   = u["id"].get<std::string>();
                        if (u.contains("name") && u["name"].is_string()) g_my_name = u["name"].get<std::string>();
                        std::cout << "[login] you are " << g_my_name
                                  << " (" << g_my_id << ", #" << short_id(g_my_id) << ")\n";
                    } else {
                        std::cout << "[login] " << line << '\n';
                    }
                }
                else if (type == "pm") {
                    // 服务器私聊下行: {"type":"pm","from":{"id","name"},"body":...}
                    std::string from_name = "?";
                    std::string from_id   = "?";
                    if (msg.contains("from") && msg["from"].is_object()) {
                        const auto& f = msg["from"];
                        if (f.contains("name") && f["name"].is_string()) from_name = f["name"].get<std::string>();
                        if (f.contains("id")   && f["id"].is_string())   from_id   = f["id"].get<std::string>();
                    }
                    std::cout << "[PM from " << from_name << " #" << short_id(from_id) << "] "
                              << msg.value("body","") << '\n';
                }
                else if (type == "msg") {
                    std::cout << "[" << msg.value("room","?") << "] "
                              << user_tag_from(msg) << ": "
                              << msg.value("body","") << '\n';
                }
                else if (type == "sys") {
                    const std::string info = msg.value("info", "");
                    // 若是 nick ok，更新本地 my_id/my_name
                    if (info == "nick ok" && msg.contains("user") && msg["user"].is_object()) {
                        const auto& u = msg["user"];
                        if (u.contains("id") && u["id"].is_string())     g_my_id   = u["id"].get<std::string>();
                        if (u.contains("name") && u["name"].is_string()) g_my_name = u["name"].get<std::string>();
                    }
                    std::cout << "[sys]"
                              << "[" << msg.value("room","-") << "] "
                              << info;
                    if (msg.contains("user")) std::cout << " - " << user_tag_from(msg);
                    if (msg.contains("members")) std::cout << " (members=" << msg["members"].get<int>() << ")";
                    std::cout << '\n';
                }
                else if (type == "error") {
                    std::cerr << "[Error]"
                              << (msg.contains("room") ? ("[" + msg["room"].get<std::string>() + "] ") : " ")
                              << msg.value("info","") << '\n';
                }
                else {
                    std::cout << "[未知类型] " << line << '\n';
                }
            } catch (const std::exception& e) {
                std::cerr << "[解析失败] " << e.what() << '\n';
            }
        }
    }
}

int main()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in svr{};
    svr.sin_family = AF_INET;
    svr.sin_port   = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &svr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&svr), sizeof(svr)) < 0) {
        perror("connect"); return 1;
    }

    std::thread receiver(receive_messages, sock);
    receiver.detach();

    std::cout << "指令：\n"
                 "  /nick <name>        设置/修改昵称 (1-20, A-Za-z0-9_-)\n"
                 "  /join <room>        加入房间并设为当前\n"
                 "  /leave <room>       离开房间\n"
                 "  /room <room>        切换当前房间（需已加入）\n"
                 "  /list               列出已加入房间与当前房间\n"
                 "  /pm <uid> <text..>  发送私聊（对方 uid 可在其 login 提示里看到）\n"
                 "  /whoami             打印自己的 uid 与昵称\n"
                 "  /quit               退出客户端\n"
                 "  直接输入文本        发送到“当前房间”\n";

    std::unordered_set<std::string> joined;  // 已加入的房间
    std::string active;                      // 当前房间

    std::string line;
    while (std::getline(std::cin, line)) {
        line = trim(line);
        if (line.empty()) continue;

        // ----- 命令解析 -----
        if (line == "/quit") break;

        if (line == "/whoami") {
            std::cout << "you are " << (g_my_name.empty()? "(unnamed)":g_my_name)
                      << " (" << (g_my_id.empty()? "?":g_my_id)
                      << ", #" << short_id(g_my_id) << ")\n";
            continue;
        }

        if (line.rfind("/nick ", 0) == 0) {
            std::string name = trim(line.substr(6));
            if (!valid_nick_local(name)) {
                std::cerr << "无效昵称：需 1-20 位且仅含 A-Za-z0-9_-。\n";
                continue;
            }
            send_json_line(sock, {{"type","nick"},{"name",name}});
            std::cout << "→ 已请求设置昵称为 [" << name << "]\n";
            continue;
        }

        if (line.rfind("/join ", 0) == 0) {
            std::string room = trim(line.substr(6));
            if (room.empty()) { std::cerr << "用法: /join <room>\n"; continue; }
            send_json_line(sock, {{"type","join"},{"room",room}});
            joined.insert(room);
            active = room;
            std::cout << "→ 已请求加入 [" << room << "], 当前房间切到 [" << active << "]\n";
            continue;
        }

        if (line.rfind("/leave ", 0) == 0) {
            std::string room = trim(line.substr(7));
            if (room.empty()) { std::cerr << "用法: /leave <room>\n"; continue; }
            send_json_line(sock, {{"type","leave"},{"room",room}});
            joined.erase(room);
            if (active == room) {
                active = joined.empty() ? "" : *joined.begin();
                std::cout << "→ 离开后当前房间: " << (active.empty()? "(无)":"["+active+"]") << '\n';
            }
            continue;
        }

        if (line.rfind("/room ", 0) == 0) {
            std::string room = trim(line.substr(6));
            if (room.empty()) { std::cerr << "用法: /room <room>\n"; continue; }
            if (!joined.count(room)) {
                std::cerr << "未加入该房间，请先 /join " << room << '\n';
            } else {
                active = room;
                std::cout << "→ 当前房间切换为 [" << active << "]\n";
            }
            continue;
        }

        if (line.rfind("/pm ", 0) == 0) {
            // 形如：/pm 2f hello world
            std::istringstream iss(line.substr(4));
            std::string to; std::string body;
            iss >> to; std::getline(iss, body);
            body = trim(body);
            if (to.empty() || body.empty()) {
                std::cerr << "用法: /pm <uid> <text..>\n"; continue;
            }
            json j = {{"type","pm"}, {"to", to}, {"body", body}};
            (void)send_json_line(sock, j);
            continue;
        }

        if (line == "/list") {
            std::cout << "已加入房间: ";
            if (joined.empty()) std::cout << "(无)\n";
            else {
                bool first = true;
                for (const auto& r : joined) {
                    if (!first) std::cout << ", ";
                    std::cout << (r == active ? ("*"+r) : r);
                    first = false;
                }
                std::cout << "   (* 为当前房间)\n";
            }
            continue;
        }

        // ----- 普通文本：发到当前房间 -----
        if (active.empty()) {
            std::cerr << "尚未选择房间。请先 /join <room> 或 /room <room>\n";
            continue;
        }
        json msg = {{"type","msg"}, {"room", active}, {"body", line}};
        (void)send_json_line(sock, msg);
    }

    close(sock);
    return 0;
}
