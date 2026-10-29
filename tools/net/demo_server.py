"""A local server for the showcase's NET page: HTTP and a WebSocket chat, with nothing but the
standard library.

    python tools/net/demo_server.py [--port 8765]

HTTP
    GET  /people   a JSON list of people
    POST /echo     answers with the body it was sent, under the content type it was sent with
    GET  /slow     answers after two seconds
WebSocket
    ws://127.0.0.1:8765/chat   greets each client, echoes every message to all of them as JSON,
                               and says the time every ten seconds
"""

import argparse
import base64
import hashlib
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PEOPLE = [
    {"name": "Ada Lovelace", "role": "analyst", "year": 1843},
    {"name": "Grace Hopper", "role": "compiler writer", "year": 1952},
    {"name": "Margaret Hamilton", "role": "flight software", "year": 1969},
    {"name": "Radia Perlman", "role": "spanning tree", "year": 1985},
    {"name": "Barbara Liskov", "role": "abstraction", "year": 1987},
]
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

clients = set()
clients_lock = threading.Lock()
# Frames from the chat thread and the broadcasts must not interleave on a socket.
send_lock = threading.Lock()


def frame(payload: bytes, opcode: int = 0x1) -> bytes:
    """A server frame: never masked."""
    head = bytes([0x80 | opcode])
    if len(payload) < 126:
        head += bytes([len(payload)])
    elif len(payload) < 1 << 16:
        head += bytes([126]) + struct.pack("!H", len(payload))
    else:
        head += bytes([127]) + struct.pack("!Q", len(payload))
    return head + payload


def read_exact(connection: socket.socket, count: int) -> bytes:
    data = b""
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data += chunk
    return data


def read_frame(connection: socket.socket):
    first, second = read_exact(connection, 2)
    opcode = first & 0x0F
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", read_exact(connection, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", read_exact(connection, 8))[0]
    mask = read_exact(connection, 4) if second & 0x80 else b"\0\0\0\0"
    payload = bytearray(read_exact(connection, length))
    for index in range(length):
        payload[index] ^= mask[index % 4]
    return first & 0x80, opcode, bytes(payload)


def send_raw(connection: socket.socket, data: bytes) -> None:
    try:
        with send_lock:
            connection.sendall(data)
    except OSError:
        pass


def send(connection: socket.socket, message: dict) -> None:
    send_raw(connection, frame(json.dumps(message).encode()))


def broadcast(message: dict) -> None:
    with clients_lock:
        targets = list(clients)
    for connection in targets:
        send(connection, message)


def chat(connection: socket.socket, name: str) -> None:
    with clients_lock:
        clients.add(connection)
    send(connection, {"from": "server", "text": f"Welcome, {name}. {len(clients)} connected."})
    message = b""
    try:
        while True:
            final, opcode, payload = read_frame(connection)
            if opcode == 0x8:
                code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 1000
                send_raw(connection, frame(struct.pack("!H", code), 0x8))
                return
            if opcode == 0x9:
                send_raw(connection, frame(payload, 0xA))
                continue
            if opcode in (0x1, 0x0):
                message += payload
                if final:
                    text = message.decode("utf-8", "replace")
                    message = b""
                    if text == "/bye":
                        # The server ends the conversation: a close with a reason.
                        reason = "the server said goodbye".encode()
                        send_raw(connection, frame(struct.pack("!H", 1000) + reason, 0x8))
                        continue
                    broadcast({"from": name, "text": text, "at": time.strftime("%H:%M:%S")})
    except (ConnectionError, OSError):
        pass
    finally:
        with clients_lock:
            clients.discard(connection)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    count = 0

    def reply(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Demo-Server", "strata")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/people":
            self.reply(200, json.dumps(PEOPLE).encode(), "application/json")
        elif self.path == "/slow":
            time.sleep(2)
            self.reply(200, b"finally", "text/plain; charset=utf-8")
        elif self.path == "/chat" and self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers["Sec-WebSocket-Key"]
            accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            asked = self.headers.get("Sec-WebSocket-Protocol")
            if asked:
                self.send_header("Sec-WebSocket-Protocol", asked.split(",")[0].strip())
            self.end_headers()
            self.wfile.flush()
            Handler.count += 1
            chat(self.connection, f"guest {Handler.count}")
            self.close_connection = True
        else:
            self.reply(404, b"not found", "text/plain; charset=utf-8")

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path == "/echo":
            self.reply(200, body, self.headers.get("Content-Type", "application/octet-stream"))
        else:
            self.reply(404, b"not found", "text/plain; charset=utf-8")

    def log_message(self, format, *args) -> None:  # noqa: A002 - the base class names it
        print(f"{self.address_string()} {format % args}", flush=True)


def ticker() -> None:
    while True:
        time.sleep(10)
        broadcast({"from": "clock", "text": time.strftime("It is %H:%M:%S.")})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--port", type=int, default=8765)
    arguments = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", arguments.port), Handler)
    server.daemon_threads = True
    threading.Thread(target=ticker, daemon=True).start()
    print(f"Serving http://127.0.0.1:{arguments.port} and ws://127.0.0.1:{arguments.port}/chat", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
