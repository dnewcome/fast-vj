#!/usr/bin/env python3
"""
osc_proxy.py — UDP OSC → WebSocket bridge for fast-vj WASM
No third-party dependencies; uses only the Python standard library.

Listens for OSC packets on UDP (default port 9000) and forwards them
as binary WebSocket messages to any connected browser clients.

Usage:
    python3 tools/osc_proxy.py [--udp-port 9000] [--ws-port 9001]
"""

import argparse
import asyncio
import hashlib
import base64
import socket
import struct
import sys


# ---------------------------------------------------------------------------
# Minimal WebSocket server (RFC 6455) — stdlib only
# ---------------------------------------------------------------------------

WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _ws_handshake(key: str) -> bytes:
    accept = base64.b64encode(
        hashlib.sha1((key + WS_MAGIC).encode()).digest()
    ).decode()
    return (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n"
        "\r\n"
    ).encode()


def _ws_frame(data: bytes) -> bytes:
    """Encode a binary WebSocket frame (server→client, no masking)."""
    hdr = bytearray([0x82])  # FIN + opcode 2 (binary)
    n = len(data)
    if n <= 125:
        hdr.append(n)
    elif n <= 65535:
        hdr.append(126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(127)
        hdr += struct.pack(">Q", n)
    return bytes(hdr) + data


class WsClient:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer

    async def send(self, data: bytes):
        self.writer.write(_ws_frame(data))
        await self.writer.drain()

    def close(self):
        self.writer.close()


clients: set = set()


async def handle_ws(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    # Read HTTP upgrade request
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = await reader.read(4096)
        if not chunk:
            writer.close()
            return
        request += chunk

    # Extract Sec-WebSocket-Key
    key = None
    for line in request.decode(errors="replace").splitlines():
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
            break
    if not key:
        writer.close()
        return

    writer.write(_ws_handshake(key))
    await writer.drain()

    client = WsClient(reader, writer)
    clients.add(client)
    addr = writer.get_extra_info("peername")
    print(f"[proxy] client connected {addr} ({len(clients)} total)")

    try:
        # Drain incoming frames (we don't need client→server data)
        while True:
            header = await reader.readexactly(2)
            fin_op = header[0]
            mask_len = header[1]
            length = mask_len & 0x7F
            if length == 126:
                ext = await reader.readexactly(2)
                length = struct.unpack(">H", ext)[0]
            elif length == 127:
                ext = await reader.readexactly(8)
                length = struct.unpack(">Q", ext)[0]
            masked = bool(mask_len & 0x80)
            if masked:
                await reader.readexactly(4)   # masking key (discard)
            if length:
                await reader.readexactly(length)
            opcode = fin_op & 0x0F
            if opcode == 8:   # close
                break
    except (asyncio.IncompleteReadError, ConnectionResetError):
        pass
    finally:
        clients.discard(client)
        client.close()
        print(f"[proxy] client disconnected ({len(clients)} remaining)")


async def udp_forward(udp_port: int):
    loop = asyncio.get_event_loop()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", udp_port))
    sock.setblocking(False)
    print(f"[proxy] UDP OSC listening on port {udp_port}")
    while True:
        data = await loop.sock_recv(sock, 4096)
        if clients:
            results = await asyncio.gather(
                *[c.send(data) for c in list(clients)],
                return_exceptions=True,
            )
            for r in results:
                if isinstance(r, Exception):
                    print(f"[proxy] send error: {r}")


async def main(udp_port: int, ws_host: str, ws_port: int):
    server = await asyncio.start_server(handle_ws, ws_host, ws_port)
    print(f"[proxy] WebSocket on ws://{ws_host}:{ws_port}")
    async with server:
        await udp_forward(udp_port)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="UDP OSC → WebSocket proxy")
    parser.add_argument("--udp-port", type=int, default=9000, metavar="PORT")
    parser.add_argument("--ws-port",  type=int, default=9001, metavar="PORT")
    parser.add_argument("--ws-host",  default="localhost",    metavar="HOST")
    args = parser.parse_args()
    try:
        asyncio.run(main(args.udp_port, args.ws_host, args.ws_port))
    except KeyboardInterrupt:
        print("\n[proxy] stopped")
