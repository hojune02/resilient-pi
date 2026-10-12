import socket
import time
from concurrent.futures import ThreadPoolExecutor

HOST = "110.76.76.127"
PORT = 8080
CLIENTS = 100
HOLD_SECONDS = 2

def connect_client(i):
    try:
        sock = socket.create_connection((HOST, PORT), timeout=3)
        return sock
    except OSError as exc:
        print(f"Client {i}: connection failed: {exc}")
        return None

sockets = []

try:
    with ThreadPoolExecutor(max_workers=100) as pool:
        sockets = [
            sock for sock in pool.map(connect_client, range(CLIENTS))
            if sock is not None
        ]

    print(f"Established {len(sockets)}/{CLIENTS} connections")
    print("Holding connections without sending data...")
    time.sleep(HOLD_SECONDS)

finally:
    for sock in sockets:
        sock.close()

print("All clients closed")
