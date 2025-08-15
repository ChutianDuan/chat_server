# 架构总览

一个基于 **libevent** 的最小但兼具生产可用性的聊天服务器，采用 **1 个接入线程（Acceptor）+ N 个 Worker** 的线程模型。接入线程只负责接受 TCP 连接并通过 UNIX `socketpair` **分发文件描述符（fd）** 到各个 Worker。每个 Worker 拥有独立的 `event_base`，并负责其名下连接的所有 I/O 与回调。

核心特性：

* **线程模型**：接入线程仅 `accept`，N 个 Worker 各自运行事件循环
* **分帧协议**：以换行 `\n` 分隔的一行一条 JSON（line-delimited JSON）
* **背压控制**：基于 per-connection 队列 + 阈值 + 低水位续写
* **房间与私信**：房间成员管理、用户注册表、跨线程安全广播与私信（依赖线程安全 bufferevent）

---

## 1. 线程模型与启动

### 1.1 启动顺序

1. **开启线程支持**

   * 在创建任何 `event_base` 之前调用 `evthread_use_pthreads()`。
2. **启动 Worker**

   * 为每个 Worker 创建一个 UNIX `socketpair`（**推荐 `SOCK_DGRAM`**），记录 `notify_recv`（Worker 读端）与 `notify_send`（Acceptor 写端），启动线程：

     * 创建 `event_base`
     * 在 `notify_recv` 上注册**持久可读事件**（`notify_ev`）
     * 进入 `event_base_dispatch()`
3. **启动接入线程（Acceptor）**

   * 创建接入 `event_base`
   * 使用 `evconnlistener_new_bind()` 监听 TCP
   * 运行 `event_base_dispatch()`

### 1.2 FD 分发路径（Acceptor → Worker）

* 在接入线程的 `listener_cb(...)`：

  * `evutil_make_socket_nonblocking(fd)`
  * 调用 `dispatch_to_worker(fd)`：按轮询选择一个 Worker，并通过其 `notify_send` 使用 `send()` 发送**整数 fd**。

**为何推荐 `SOCK_DGRAM`？** 数据报语义提供**天然报文边界**，每次 `send()` 对应一次完整的 `recv()`，对 4/8 字节的 fd 载荷而言，不存在短读/“半包”。

若保留 `SOCK_STREAM`，则必须在 `notify_cb` 中累积字节，直到达到 `sizeof(fd)` 再消费一个 fd。

---

## 2. Worker 生命周期与连接初始化

当 Worker 上的 `notify_cb` 被触发：

1. `recv(notify_recv, &cfd, sizeof(cfd), 0)` → 取到一个 fd
2. `on_new_client_fd(worker, cfd)`：

   * `evutil_make_socket_nonblocking(cfd)`
   * `bufferevent_socket_new(worker->base, cfd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE)`
   * `bufferevent_setcb(bev, read_cb, write_cb, event_cb, nullptr)`
   * `bufferevent_setwatermark(bev, EV_WRITE, WRITE_RESUME, 0)`（低水位触发继续刷队列）
   * **超时**：当前仅设置**写超时** `bufferevent_set_timeouts(bev, nullptr, &wto)`；如需识别慢读可加读超时
   * 初始化 `User {id, name}`（如 `guest-xxxx`），在**加锁**条件下写入全局表
   * 给客户端发送一条 `login` JSON

---

## 3. 消息分帧与业务协议

**传输分帧**：*一行一条 JSON*，每条消息以 `\n` 结尾。

* 读侧用 `evbuffer_search_eol(in, ..., EVBUFFER_EOL_LF)` 查找 `\n` 并提取完整一行
* 解析侧对该行调用 `json::parse(...)`

**业务操作**（现有）：`nick`、`join`、`leave`、`msg`、`pm`

* 校验失败返回兼容的 `error` 对象
* 可选（渐进升级）：为所有消息增加 Envelope（`ver/ts/role/type/req_id/ok/code/data/meta`），并**为每个请求返回 ACK（role:"resp"）**

### 3.1 示例（兼容旧格式）

* 加入房间请求：

  ```json
  {"type":"join", "room":"lobby"}\n
  ```
* 房间消息广播：

  ```json
  {"type":"msg", "room":"lobby", "body":"hello"}\n
  ```
* 错误回执（旧格式）：

  ```json
  {"type":"error", "info":"missing 'room' for join"}\n
  ```

可选的 Envelope/ACK 方案见 `docs/PROTOCOL.md`（若启用）：请求携带 `role:"req"` 与 `req_id`，服务端回 `role:"resp"`/`ok`/`code`。

---

## 4. 共享状态与加锁规范

受 `g_mtx` 保护的全局容器：

* `g_rooms: unordered_map<string, Room>`，其中 `Room { id, members:set<bev*> }`
* `g_subs: unordered_map<bev*, set<string>>`（某连接订阅的房间集合）
* `g_users: unordered_map<bev*, User>` 与 `g_uid2bev: unordered_map<string, bev*>`
* `g_clients: vector<bev*>`（在线连接统计）
* `g_sendq: unordered_map<bev*, deque<string>>`（慢接收者的 per-connection 待发队列）

**加锁纪律**

* **锁内**：所有对上述容器的读写
* **I/O 锁外**：在锁内**拍快照**（收集 `bev*` 等指针到本地向量），随后**在锁外发送**，避免长时间占锁

---

## 5. 背压策略

阈值（单位：字节），基于 `evbuffer_get_length(bufferevent_get_output(bev))`：

* `WRITE_RESUME`（如 256 KiB）：低水位；触发 `write_cb` 继续刷队列
* `WRITE_BLOCK`（如 1 MiB）：若输出缓冲超过该值，新的发送改为**入队 `g_sendq[bev]`**
* `WRITE_CUTOFF`（如 4 MiB）：判定为**极慢连接** → 为保护服务主动关闭
* `BACKLOG_MAX`（如 256 条）：每连接队列上限；溢出仅丢弃本条，不立即断开

**流程**

1. 快路径：`outbuf < WRITE_BLOCK` → 直接 `bufferevent_write()`
2. 慢路径：`outbuf ≥ WRITE_BLOCK` → 入队 `g_sendq`
3. 续写：`write_cb()` 在 `outbuf ≤ WRITE_RESUME` 时从队列继续刷
4. 兜底：`outbuf > WRITE_CUTOFF` → 断开连接

---

## 6. 房间广播与私信

### 6.1 房间广播

```cpp
void room_broadcast(const std::string& room, const json& j, bufferevent* except = nullptr) {
    std::vector<bufferevent*> recipients;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_rooms.find(room);
        if (it == g_rooms.end()) return;
        recipients.reserve(it->second.members.size());
        for (auto* b : it->second.members)
            if (b != except) recipients.push_back(b);
    }
    for (auto* b : recipients) send_json(b, j); // 锁外发送
}
```

### 6.2 私信（PM）

* 在锁内查找 `tgt = g_uid2bev[to]`
* 在 `send_json(tgt, json)` 前后使用 `bufferevent_incref(tgt)` / `bufferevent_decref(tgt)` 防止并发释放
* 由于 `BEV_OPT_THREADSAFE`，跨线程写是安全的

> 说明：房间成员可能分布在不同 Worker；广播通过遍历成员的 `bev*` 逐个发送来实现；可进一步**按房间分片到同一 Worker** 以减少跨线程写与锁竞争。

---

## 7. 连接关闭与清理

在收到 `BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT` 时：

1. **锁外**：按订阅的房间逐一调用 `leave_room(bev, room, silent=false)` 广播离开
2. **锁内**：从 `g_uid2bev / g_sendq / g_subs / g_users / g_clients` 移除
3. `bufferevent_free(bev)`

`leave_room` 在房间成员为空时会删除该房间。

---

## 8. 可靠性与 Keepalive

启用内核 TCP keepalive（Linux 示例）：

```cpp
static void enable_tcp_keepalive(evutil_socket_t fd){
    int yes=1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
#ifdef __linux__
    int idle=60, intvl=10, cnt=3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl,sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,  sizeof(cnt));
#endif
}
```

可选：增加应用层心跳或读超时，用于更快发现半开连接。

---

## 9. 可选增强 / 路线图

* **协议 Envelope + ACK**：为所有消息加 `ver/ts/role/type/req_id/ok/code/data/meta`，并对每个请求返回 `role:"resp"` 的 ACK；保留旧字段以兼容老客户端
* **JSON Schema 校验**：在服务端与客户端强制字段存在/类型/长度上限
* **按房间分片**：将同一房间的连接调度到同一 Worker，降低跨线程写
* **优雅下线**：停止接受新连接、通知 Worker、排空队列、刷写未发完数据、`join()` 线程
* **指标与日志**：连接数、房间数、队列深度分位点、广播时延、错误码分布
* **限流**：按 UID/房间进行速率限制，并在超限时返回 `retry_after_ms`

---

## 10. 常见陷阱（与修复）

* **notify 通道短读**：若用 `SOCK_STREAM`，必须累积到 `sizeof(fd)` 再消费；或**改用 `SOCK_DGRAM`**
* **接入线程处理业务 I/O**：接入线程应仅 `accept` 与分发，业务 I/O 必须在 Worker
* **持锁发送**：发送前先在锁内拍快照，**锁外**调用 `send_json()`
* **缺少 `BEV_OPT_THREADSAFE`**：跨线程写需要它；同时在私信时配合 `incref/decref`
* **误解背压阈值**：`WRITE_BLOCK` → 入队，`WRITE_RESUME` → 续写，`WRITE_CUTOFF` → 断开
* **仅配置写超时**：可同时配置读超时，尽快识别静默对端

---

## 11. 最小客户端指南

1. `socket` → `connect`
2. 读取首条 `login` JSON，获取 `user.id/user.name`
3. 按行发送请求：

   * `{"type":"nick","name":"Alice"}\n`
   * `{"type":"join","room":"lobby"}\n`
   * `{"type":"msg","room":"lobby","body":"hello"}\n`
   * `{"type":"pm","to":"<uid>","body":"hi"}\n`
4. 按行读取并根据 `type`（或启用 Envelope 后的 `role/type/code`）进行分发处理

---

## 12. 回调职责（速查表）

| 回调            | 触发时机                              | 责任                                                  |
| ------------- | --------------------------------- | --------------------------------------------------- |
| `listener_cb` | 接入线程收到新 TCP 连接                    | 设非阻塞、分发 fd 至某个 Worker                               |
| `notify_cb`   | Worker 的 `notify_recv` 上收到待消费的 fd | `recv()` fd；调用 `on_new_client_fd`                   |
| `read_cb`     | 连接上出现一条或多条完整 JSON 行               | 解析与路由（nick/join/leave/msg/pm），**锁内**改状态，**锁外**发送/广播 |
| `write_cb`    | 输出缓冲 ≤ 低水位（可继续写）                  | 从 `g_sendq[bev]` 刷出，若再次变慢则回塞队列                      |
| `event_cb`    | 连接出现 EOF/ERROR/TIMEOUT            | 广播离开、**锁内**清理映射、`bufferevent_free`                  |

---

## 13. 常量（默认值）

* `WRITE_RESUME` = `256 KiB`
* `WRITE_BLOCK`  = `1 MiB`
* `WRITE_CUTOFF` = `4 MiB`
* `BACKLOG_MAX`  = `256` 条/连接

以上值可根据负载与 NIC/OS 缓冲大小进行调优。

---

*本文档描述了当前仓库中实现的行为。可选特性（Envelope/ACK、JSON Schema、房间分片等）可在不破坏旧客户端的前提下渐进启用。*
