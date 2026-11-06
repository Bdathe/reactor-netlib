#!/usr/bin/env python3
"""先发不读、过一会儿再读的 echo 客户端（AI 编写的测试工具，不属于库代码）。

1. 一次发出 size MiB 数据，然后 sleep 若干秒、一个字节都不读
   → 客户端接收窗口变成 0，服务器 write 写不完 → 剩余数据进入输出 Buffer，关注 EPOLLOUT
2. 再把回显全部读完并校验
   → 服务器 HandleWrite 把输出 Buffer 写空 → 取消关注 EPOLLOUT
3. 优雅关闭：shutdown(SHUT_WR) → 读到 EOF → close（接收缓冲区已空，不会发 RST）

用法：python3 bench/slow_reader.py [host] [port] [size_mib=8] [sleep_sec=2]
"""
import socket
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
size = int(sys.argv[3]) * 1024 * 1024 if len(sys.argv) > 3 else 8 * 1024 * 1024
pause = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0

data = bytes((i * 7 + 3) % 251 for i in range(size))

with socket.create_connection((host, port)) as s:
    print(f"已连接，本地端口 = {s.getsockname()[1]}")

    # 发送和接收交给内核缓冲区；sendall 可能因为服务器回不出去而卡住，所以先把接收方向留着，
    # 发送放在单独的线程里，主线程先睡再读
    import threading

    def sender():
        s.sendall(data)
        print(f"已发送 {size / 1048576:.0f} MiB")

    t = threading.Thread(target=sender)
    t.start()

    print(f"暂停 {pause:.1f} 秒，不读取回显……")
    time.sleep(pause)

    print("开始读取回显")
    received = bytearray()
    while len(received) < size:
        chunk = s.recv(1 << 20)
        if not chunk:
            break
        received += chunk
    t.join()

    ok = bytes(received) == data
    print(f"收到 {len(received) / 1048576:.1f} MiB，校验{'通过' if ok else '失败'}")

    s.shutdown(socket.SHUT_WR)
    eof = s.recv(1)
    print(f"半关闭后读到 EOF：{eof == b''}")
    sys.exit(0 if ok and eof == b"" else 1)
