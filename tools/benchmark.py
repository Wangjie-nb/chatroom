#!/usr/bin/env python3
"""
聊天室压测脚本
用法: python3 benchmark.py [并发数] [持续时间秒] [每连接每秒消息数]
示例: python3 benchmark.py 100 30 1
"""

import asyncio
import websockets
import time
import sys
import statistics
from collections import defaultdict

# 配置
SERVER = "ws://127.0.0.1:8888/ws"
NUM_CLIENTS = int(sys.argv[1]) if len(sys.argv) > 1 else 50
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 30
MSG_PER_SEC = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0

# 统计
stats = {
    "connected": 0,
    "failed": 0,
    "sent": 0,
    "received": 0,
    "latencies": [],
    "errors": defaultdict(int),
}

stop_event = None


async def client_worker(client_id):
    """单个客户端工作流"""
    global stats
    try:
        async with websockets.connect(SERVER, ping_interval=None) as ws:
            stats["connected"] += 1
            # 加入聊天室
            await ws.send(f"/join tester{client_id}")
            # 丢弃欢迎消息
            try:
                await asyncio.wait_for(ws.recv(), timeout=2)
            except:
                pass

            send_interval = 1.0 / MSG_PER_SEC if MSG_PER_SEC > 0 else 999
            last_send = time.time()

            while not stop_event.is_set():
                now = time.time()
                # 发送消息
                if MSG_PER_SEC > 0 and now - last_send >= send_interval:
                    msg = f"msg_from_{client_id}_{int(now*1000)}"
                    send_time = time.time()
                    await ws.send(msg)
                    stats["sent"] += 1
                    last_send = now

                # 接收消息（非阻塞）
                try:
                    recv = await asyncio.wait_for(ws.recv(), timeout=0.1)
                    stats["received"] += 1
                    # 计算延迟（只算自己发的消息）
                    if f"msg_from_{client_id}_" in recv:
                        try:
                            ts = int(recv.split("_")[-1].split("|")[-1])
                            latency = (time.time() - ts/1000) * 1000
                            if 0 < latency < 10000:
                                stats["latencies"].append(latency)
                        except:
                            pass
                except asyncio.TimeoutError:
                    pass
                except Exception as e:
                    stats["errors"][str(e)] += 1
                    break

    except Exception as e:
        stats["failed"] += 1
        stats["errors"][str(e)[:50]] += 1


async def main():
    global stop_event
    stop_event = asyncio.Event()

    print(f"{'='*60}")
    print(f"  聊天室压测")
    print(f"{'='*60}")
    print(f"  服务器: {SERVER}")
    print(f"  并发数: {NUM_CLIENTS}")
    print(f"  持续时间: {DURATION}秒")
    print(f"  每连接每秒消息: {MSG_PER_SEC}")
    print(f"  预期总消息/秒: {NUM_CLIENTS * MSG_PER_SEC:.0f}")
    print(f"{'='*60}")
    print()

    # 建立连接
    print("[1/3] 正在建立连接...")
    start_time = time.time()
    tasks = [asyncio.create_task(client_worker(i)) for i in range(NUM_CLIENTS)]
    await asyncio.sleep(3)  # 等连接建立
    connect_time = time.time() - start_time
    print(f"  成功连接: {stats['connected']}/{NUM_CLIENTS}")
    print(f"  连接失败: {stats['failed']}")
    print(f"  连接耗时: {connect_time:.2f}秒")
    print()

    if stats['connected'] == 0:
        print("没有连接成功，退出")
        return

    # 压测
    print(f"[2/3] 开始压测，持续 {DURATION} 秒...")
    test_start = time.time()
    sent_before = stats["sent"]
    recv_before = stats["received"]

    await asyncio.sleep(DURATION)
    stop_event.set()

    test_time = time.time() - test_start
    sent_total = stats["sent"] - sent_before
    recv_total = stats["received"] - recv_before

    # 等所有任务结束
    await asyncio.gather(*tasks, return_exceptions=True)

    # 输出结果
    print()
    print(f"[3/3] 压测结果")
    print(f"{'='*60}")
    print(f"  压测时长: {test_time:.2f}秒")
    print(f"  活跃连接: {stats['connected']}")
    print()
    print(f"  发送消息: {sent_total} 条")
    print(f"  发送速率: {sent_total/test_time:.1f} 条/秒")
    print()
    print(f"  接收消息: {recv_total} 条")
    print(f"  接收速率: {recv_total/test_time:.1f} 条/秒")
    print()
    if stats["latencies"]:
        lats = stats["latencies"]
        print(f"  消息延迟:")
        print(f"    平均: {statistics.mean(lats):.1f} ms")
        print(f"    中位数: {statistics.median(lats):.1f} ms")
        print(f"    P90: {sorted(lats)[int(len(lats)*0.9)]:.1f} ms")
        print(f"    P99: {sorted(lats)[int(len(lats)*0.99)]:.1f} ms")
        print(f"    最大: {max(lats):.1f} ms")
    print()
    if stats["errors"]:
        print(f"  错误统计:")
        for err, cnt in sorted(stats["errors"].items(), key=lambda x: -x[1])[:5]:
            print(f"    {cnt}次: {err}")
    print(f"{'='*60}")
    print()
    print("  建议:")
    print("  - 逐步增加并发数，找到服务器最大承载量")
    print("  - 观察服务器 CPU/内存: top, htop, free -h")
    print("  - 观察网络带宽: iftop, nethogs")
    print("  - 消息延迟 P99 < 100ms 为优秀, < 500ms 为良好")


if __name__ == "__main__":
    asyncio.run(main())
