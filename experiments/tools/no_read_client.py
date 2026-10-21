#!/usr/bin/env python3
"""只发不收的测试客户端（AI 编写的测试工具，不属于实验程序）。

连接 echo 服务器后不停地发送数据，但从不读取回显，
用来制造"客户端接收窗口为 0 → 服务器发送缓冲区写满"的场景。

用法：python3 experiments/tools/no_read_client.py [host] [port]
"""
import socket
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9877

chunk = b"x" * 65536
total = 0

with socket.create_connection((host, port)) as s:
    print(f"已连接，本地端口 = {s.getsockname()[1]}")
    s.settimeout(1.0)
    last = time.monotonic()
    while True:
        try:
            total += s.send(chunk)
        except socket.timeout:
            print(f"发送卡住了：1 秒内一个字节都发不出去，累计已发送 {total / 1048576:.1f} MiB")
            continue
        now = time.monotonic()
        if now - last >= 0.5:
            print(f"累计已发送 {total / 1048576:.1f} MiB")
            last = now
