#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
简单/可扩展的聊天服务器压测脚本（不使用 argparse）。
协议：行分隔 JSON；与给定 C++ 服务器完全兼容。
依赖：仅 Python 3 标准库（asyncio, json, time, random, statistics）
注意：已针对 Python 3.8 语法编写（不使用 | 联合类型语法）。
"""

import asyncio
import json
import random
import string
import time
from typing import Optional

# ==============================
# 配置区（修改这里，无需命令行参数）
# ==============================
HOST         = "127.0.0.1"
PORT         = 12345

SCENARIO     = "basic"   # 可选: "basic" | "broadcast" | "multiroom" | "slow" | "churn" | "soak"
ROOM         = "bench"
CLIENTS      = 200           # broadcast/slow/soak 使用的客户端总数
SENDERS      = 1             # 发送者数量（broadcast/multiroom）
MESSAGES     = 2000          # 每个发送者发送的消息数
TARGET_MPS   = 1000          # 目标全局发送速率（Messages Per Second），用于限速
MSG_SIZE     = 64            # 消息体字节数（填充随机字符）
JOIN_TIMEOUT = 10.0          # 入房/登录等待超时（秒）
TEST_DURATION= 30.0          # churn/soak 持续时间（秒）

# multiroom 专用
ROOMS        = 10            # 房间数
USERS_PER_R  = 50            # 每房间用户数（multiroom）

# slow 专用
SLOW_RATIO   = 0.02          # 慢客户端比例（2%）
SLOW_PAUSE   = 2.0           # 慢客户端“读停顿”秒数（模拟停读）

# ==============================
# 内部实现
# ==============================

def now_ms() -> int:
    return int(time.time() * 1000)

def rand_body(size: int) -> str:
    # 让 body 是一个 JSON 字符串（字符串内容仍是 JSON），服务器只看作普通字符串
    payload = {
        "mid": ''.join(random.choices(string.ascii_letters + string.digits, k=12)),
        "t0":  now_ms(),
        "pad": ''.join(random.choices(string.ascii_letters + string.digits, k=max(0, size-32)))
    }
    return json.dumps(payload, separators=(",", ":"))

def pct(values, p):
    if not values:
        return None
    s = sorted(values)
    k = (len(s)-1) * (p/100.0)
    f = int(k)
    c = min(f+1, len(s)-1)
    if f == c:
        return float(s[f])
    return float(s[f] + (s[c]-s[f])*(k-f))

class ChatClient:
    def __init__(self, name: str, room: str, lat_sink=None, slow: bool=False):
        self.name = name
        self.room = room
        self.lat_sink = lat_sink  # list.append
        self.slow = slow

        self.reader = None    # type: Optional[asyncio.StreamReader]
        self.writer = None    # type: Optional[asyncio.StreamWriter]
        self.uid: Optional[str] = None
        self.logged_in = asyncio.Event()
        self.joined = asyncio.Event()
        self.recv_task: Optional[asyncio.Task] = None

        self.recv_msgs = 0
        self.errors = 0

    async def connect(self, host, port):
        self.reader, self.writer = await asyncio.open_connection(host, port)
        # 先等待 login（同步读取）
        await self._wait_for_login()

        # ⚠️ 关键：先开接收协程，才能读到后续的 "sys/joined"
        self.recv_task = asyncio.create_task(self._recv_loop())

        # 再发 nick / join
        await self.send_json({"type": "nick", "name": self.name})
        await self.send_json({"type": "join", "room": self.room})

        # 等待 joined（有接收协程在读了，才能真正等到）
        try:
            await asyncio.wait_for(self.joined.wait(), timeout=JOIN_TIMEOUT)
        except asyncio.TimeoutError:
            # 某些实现可能不给自己回“joined”，不强依赖
            pass

    async def close(self):
        try:
            if self.writer:
                self.writer.close()
                await self.writer.wait_closed()
        except Exception:
            pass
        if self.recv_task:
            self.recv_task.cancel()
            try:
                await self.recv_task
            except Exception:
                pass

    async def _wait_for_login(self):
        # 简单在连接初期拉一条 login
        start = time.time()
        while time.time() - start < JOIN_TIMEOUT:
            line = await self.reader.readline()
            if not line:
                break
            try:
                msg = json.loads(line.decode("utf-8", errors="ignore"))
            except Exception:
                continue
            if msg.get("type") == "login" and isinstance(msg.get("user"), dict):
                self.uid = msg["user"].get("id")
                self.logged_in.set()
                return
        raise RuntimeError("login timeout")

    async def send_json(self, obj: dict):
        if not self.writer:
            return
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        self.writer.write(data)
        await self.writer.drain()

    async def send_chat(self, body: str):
        await self.send_json({"type": "msg", "room": self.room, "body": body})

    async def _recv_loop(self):
        while True:
            try:
                line = await self.reader.readline()
                if not line:
                    return
                if self.slow:
                    # 模拟“慢读”：周期性停顿读取
                    await asyncio.sleep(SLOW_PAUSE)
                try:
                    msg = json.loads(line.decode("utf-8", errors="ignore"))
                except Exception:
                    self.errors += 1
                    continue

                t1 = now_ms()
                tp = msg.get("type")

                if tp == "sys":
                    info = msg.get("info", "")
                    if info in ("joined", "already in room"):
                        # 任何"sys"提示房间状态变化都可视为已加入成功
                        self.joined.set()

                elif tp == "msg":
                    # 从 body 中读出 t0
                    body = msg.get("body")
                    # body 是字符串；尝试当 JSON 解
                    try:
                        inner = json.loads(body)
                        t0 = inner.get("t0")
                        if isinstance(t0, int) and self.lat_sink:
                            self.lat_sink.append(t1 - t0)
                    except Exception:
                        pass
                    self.recv_msgs += 1

                elif tp == "pm":
                    # 本脚本不统计 PM
                    pass

                elif tp == "error":
                    self.errors += 1

            except asyncio.CancelledError:
                return
            except Exception:
                self.errors += 1
                return


# 辅助：等待一定比例的客户端完成入房
async def wait_joins(clients, fraction=0.90, timeout=5.0):
    deadline = time.time() + timeout
    need = max(1, int(len(clients) * fraction))
    while time.time() < deadline:
        joined = sum(1 for c in clients if c.joined.is_set())
        if joined >= need:
            return True
        await asyncio.sleep(0.05)
    return False


# ========== 场景 ==========
async def scenario_basic():
    """两人互通验通路：alice 说，bob 收。"""
    lats = []
    alice = ChatClient("alice", ROOM, lat_sink=None)
    bob   = ChatClient("bob",   ROOM, lat_sink=lats)
    await asyncio.gather(alice.connect(HOST, PORT), bob.connect(HOST, PORT))

    # 发送一条
    await alice.send_chat(rand_body(48))
    await asyncio.sleep(1.0)

    ok = (len(lats) >= 1)
    print(f"[basic] bob recv: {len(lats)}  RESULT:", "PASS" if ok else "FAIL")
    await asyncio.gather(alice.close(), bob.close())


async def scenario_broadcast():
    """1 发送者，N-1 接收者，测吞吐与延迟分布。"""
    latencies = []

    # 创建所有客户端
    clients = [ChatClient(f"user{i}", ROOM, lat_sink=latencies) for i in range(CLIENTS)]
    await asyncio.gather(*[c.connect(HOST, PORT) for c in clients])

    # 等待至少 90% 客户端真正入房，再开始发
    ok = await wait_joins(clients, fraction=0.90, timeout=JOIN_TIMEOUT)
    if not ok:
        print("[broadcast] WARN: not enough clients joined before sending")
    await asyncio.sleep(0.5)  # 稳定一下成员列表

    senders = clients[:max(1, min(SENDERS, CLIENTS))]
    receivers = clients  # 全体都接收（服务器广播除了自己）

    # 发消息（限速）
    total_msgs = MESSAGES * len(senders)
    interval = 1.0 / max(1, TARGET_MPS)  # 秒/条
    sent = 0

    async def sender_task(cli: ChatClient, count: int):
        nonlocal sent
        for _ in range(count):
            body = rand_body(MSG_SIZE)
            await cli.send_chat(body)
            sent += 1
            await asyncio.sleep(interval)

    per_sender = MESSAGES
    await asyncio.gather(*[sender_task(s, per_sender) for s in senders])

    # 给接收端一点处理时间
    await asyncio.sleep(2.0)

    # 统计
    recv_total = sum(c.recv_msgs for c in receivers)
    err_total  = sum(c.errors for c in receivers)

    if latencies:
        p50 = pct(latencies, 50); p95 = pct(latencies, 95); p99 = pct(latencies, 99)
        print(f"[broadcast] clients={CLIENTS} senders={len(senders)} room='{ROOM}'")
        print(f"[broadcast] sent={sent}  recv_total={recv_total}  errors={err_total}")
        print(f"[broadcast] lat(ms): p50={p50:.2f}  p95={p95:.2f}  p99={p99:.2f}  samples={len(latencies)}")
    else:
        print(f"[broadcast] clients={CLIENTS} senders={len(senders)} room='{ROOM}'")
        print(f"[broadcast] sent={sent}  recv_total={recv_total}  errors={err_total}")
        print("[broadcast] no latency samples (大概率是没成功入房或过早开始发送)")

    await asyncio.gather(*[c.close() for c in clients])


async def scenario_multiroom():
    """R 个房间，每房间 U 人，多发送者随机房间发。"""
    latencies = []
    # 生成房间名
    rooms = [f"{ROOM}-{i}" for i in range(ROOMS)]
    clients = []
    for i in range(ROOMS):
        for j in range(USERS_PER_R):
            name = f"u{i}_{j}"
            c = ChatClient(name, rooms[i], lat_sink=latencies)
            clients.append(c)

    await asyncio.gather(*[c.connect(HOST, PORT) for c in clients])
    await wait_joins(clients, fraction=0.90, timeout=JOIN_TIMEOUT)
    await asyncio.sleep(0.5)

    # 选择发送者（每个房间一个发送者，或受 SENDERS 限制）
    per_room_senders = max(1, SENDERS // max(1, ROOMS))
    senders = []
    for i in range(ROOMS):
        base = i * USERS_PER_R
        senders.extend(clients[base : base + per_room_senders])
    if not senders:
        senders = clients[:1]

    interval = 1.0 / max(1, TARGET_MPS)
    sent = 0

    async def sender_task(cli: ChatClient, count: int):
        nonlocal sent
        for _ in range(count):
            await cli.send_chat(rand_body(MSG_SIZE))
            sent += 1
            await asyncio.sleep(interval)

    await asyncio.gather(*[sender_task(s, MESSAGES) for s in senders])
    await asyncio.sleep(2.0)

    recv_total = sum(c.recv_msgs for c in clients)
    err_total  = sum(c.errors for c in clients)

    print(f"[multiroom] rooms={ROOMS} users/room={USERS_PER_R} senders={len(senders)}")
    print(f"[multiroom] sent={sent} recv_total={recv_total} errors={err_total}")
    if latencies:
        from math import isfinite
        p50 = pct(latencies,50); p95 = pct(latencies,95); p99 = pct(latencies,99)
        print(f"[multiroom] lat(ms): p50={p50:.2f} p95={p95:.2f} p99={p99:.2f} samples={len(latencies)}")
    else:
        print("[multiroom] no latency samples")

    await asyncio.gather(*[c.close() for c in clients])


async def scenario_slow():
    """注入慢客户端（停读），观察是否只影响慢的那批。"""
    latencies = []
    n_slow = max(1, int(CLIENTS * SLOW_RATIO))

    clients = []
    # 前 n_slow 个慢
    for i in range(CLIENTS):
        slow = (i < n_slow)
        c = ChatClient(f"user{i}", ROOM, lat_sink=latencies, slow=slow)
        clients.append(c)
    await asyncio.gather(*[c.connect(HOST, PORT) for c in clients])
    await wait_joins(clients, fraction=0.90, timeout=JOIN_TIMEOUT)

    sender = clients[-1]
    interval = 1.0 / max(1, TARGET_MPS)

    async def sender_task():
        for _ in range(MESSAGES):
            await sender.send_chat(rand_body(MSG_SIZE))
            await asyncio.sleep(interval)

    await sender_task()
    await asyncio.sleep(SLOW_PAUSE + 2.0)  # 等慢读完成

    recv_fast = sum(c.recv_msgs for c in clients[n_slow:])
    recv_slow = sum(c.recv_msgs for c in clients[:n_slow])

    print(f"[slow] clients={CLIENTS} slow={n_slow} pause={SLOW_PAUSE}s  sent={MESSAGES}")
    print(f"[slow] recv_fast={recv_fast}  recv_slow={recv_slow} (越小越说明被回压/断流保护)")
    if latencies:
        p50 = pct(latencies,50); p95 = pct(latencies,95); p99 = pct(latencies,99)
        print(f"[slow] lat(ms): p50={p50:.2f} p95={p95:.2f} p99={p99:.2f} samples={len(latencies)}")

    await asyncio.gather(*[c.close() for c in clients])


async def scenario_churn():
    """高频上下线（连接抖动），同时少量消息。"""
    end_time = time.time() + TEST_DURATION
    total_connect = 0
    total_close = 0
    latencies = []

    async def one_cycle(idx: int):
        nonlocal total_connect, total_close
        c = ChatClient(f"c{idx}", ROOM, lat_sink=latencies)
        await c.connect(HOST, PORT)
        total_connect += 1
        # 发送少量消息
        for _ in range(5):
            await c.send_chat(rand_body(48))
            await asyncio.sleep(0.01)
        await c.close()
        total_close += 1

    idx = 0
    tasks = set()
    while time.time() < end_time:
        # 每个 tick 启两个连接生命周期
        for _ in range(2):
            tasks.add(asyncio.create_task(one_cycle(idx)))
            idx += 1
        # 回收已完成
        done, pending = await asyncio.wait(tasks, timeout=0.05, return_when=asyncio.FIRST_COMPLETED)
        tasks = set(pending)

    # 等所有收尾
    if tasks:
        await asyncio.wait(tasks)

    print(f"[churn] duration={TEST_DURATION}s connect={total_connect} close={total_close} lat_samples={len(latencies)}")
    if latencies:
        p50 = pct(latencies,50); p95 = pct(latencies,95); p99 = pct(latencies,99)
        print(f"[churn] lat(ms): p50={p50:.2f} p95={p95:.2f} p99={p99:.2f}")


async def scenario_soak():
    """中低负载长跑（观察 RSS/句柄/错误累计）。"""
    latencies = []
    clients = [ChatClient(f"user{i}", ROOM, lat_sink=latencies) for i in range(CLIENTS)]
    await asyncio.gather(*[c.connect(HOST, PORT) for c in clients])
    await wait_joins(clients, fraction=0.90, timeout=JOIN_TIMEOUT)

    end_time = time.time() + TEST_DURATION
    interval = 1.0 / max(1, TARGET_MPS)

    sent = 0
    async def sender_round():
        nonlocal sent
        # 随机挑选 1% 客户端发消息
        picks = random.sample(clients, max(1, CLIENTS // 100))
        for s in picks:
            await s.send_chat(rand_body(MSG_SIZE))
            sent += 1

    while time.time() < end_time:
        await sender_round()
        await asyncio.sleep(interval)

    await asyncio.sleep(2.0)
    recv_total = sum(c.recv_msgs for c in clients)
    err_total  = sum(c.errors for c in clients)

    print(f"[soak] duration={TEST_DURATION}s clients={CLIENTS} sent={sent} recv_total={recv_total} errors={err_total}")
    if latencies:
        p50 = pct(latencies,50); p95 = pct(latencies,95); p99 = pct(latencies,99)
        print(f"[soak] lat(ms): p50={p50:.2f} p95={p95:.2f} p99={p99:.2f} samples={len(latencies)}")

    await asyncio.gather(*[c.close() for c in clients])


# ========== 主入口 ==========
async def main():
    print(f"== chat tester == {HOST}:{PORT} scenario={SCENARIO}")
    if SCENARIO == "basic":
        await scenario_basic()
    elif SCENARIO == "broadcast":
        await scenario_broadcast()
    elif SCENARIO == "multiroom":
        await scenario_multiroom()
    elif SCENARIO == "slow":
        await scenario_slow()
    elif SCENARIO == "churn":
        await scenario_churn()
    elif SCENARIO == "soak":
        await scenario_soak()
    else:
        print("Unknown SCENARIO；请在文件顶部配置 SCENARIO 为: basic | broadcast | multiroom | slow | churn | soak")

if __name__ == "__main__":
    asyncio.run(main())
