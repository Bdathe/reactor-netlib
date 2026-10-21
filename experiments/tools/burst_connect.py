#!/usr/bin/env python3
"""突发连接的测试客户端（AI 编写的测试工具，不属于实验程序）。

尽可能快地同时发起 N 个连接，保持一段时间后全部关闭，
用来制造"多个连接几乎同时到达、全连接队列里同时有好几个连接"的场景。

用法：python3 experiments/tools/burst_connect.py [N] [保持秒数] [port]
"""
import socket
import sys
import time

n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
hold = float(sys.argv[2]) if len(sys.argv) > 2 else 5
port = int(sys.argv[3]) if len(sys.argv) > 3 else 9877

socks = []
for _ in range(n):
    s = socket.socket()
    s.connect(("127.0.0.1", port))
    socks.append(s)
print(f"已建立 {len(socks)} 个连接，保持 {hold} 秒")
time.sleep(hold)
for s in socks:
    s.close()
