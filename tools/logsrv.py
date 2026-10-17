#!/usr/bin/env python3
"""Live console log: the title sends each log line as a UDP datagram to port 9023.
Prints with timestamps and appends to build/console.log (readable while it runs).
  python3 tools/logsrv.py            # run before launching the title
"""
import os, socket, sys, time

os.makedirs("build", exist_ok=True)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 9023))
print("listening on UDP 9023", flush=True)
with open("build/console.log", "a", buffering=1) as out:
    while True:
        data, addr = sock.recvfrom(4096)
        line = f"{time.strftime('%H:%M:%S')} {addr[0]} {data.decode('utf-8', 'replace').rstrip()}"
        print(line, flush=True)
        out.write(line + "\n")
