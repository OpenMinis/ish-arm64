#!/usr/bin/env python3
"""A fake SOCKS5 entry for tests/regress/regress_xnet.c.

Speaks what MinisApp's nethub speaks to iSH (RFC 1928 CONNECT, RFC 1929
username/password) and answers CONNECT by the destination port instead of
dialling anything:

    22  success, then "SSH-2.0-fake\\r\\n" in the same segment as the reply,
        then echo
    7   success, then echo
    5   success after 1 s, then echo
    4   success after 3 s, then echo
    1   0x05 connection refused     2  0x04 host unreachable
    3   0x03 network unreachable    6  0x06 TTL expired (timeout)
    8   0x02 not allowed by ruleset 9  0x07 command not supported
    10  closes without answering

Usage: xnet_fake_entry.py <port file> <password file> <log file>
It binds 127.0.0.1:0, writes the port to <port file>, and appends one line
per CONNECT to <log file>: "<username> <destination> <port>".
"""
import socket
import struct
import sys
import threading
import time

REPLIES = {1: 0x05, 2: 0x04, 3: 0x03, 6: 0x06, 8: 0x02, 9: 0x07}
DELAYS = {5: 1.0, 4: 3.0}


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def echo(conn):
    while True:
        data = conn.recv(65536)
        if not data:
            return
        conn.sendall(data)


def serve(conn, password, log, log_lock):
    try:
        ver, nmethods = recv_exact(conn, 2)
        methods = recv_exact(conn, nmethods)
        if ver != 5 or 2 not in methods:
            conn.sendall(b"\x05\xff")
            return
        conn.sendall(b"\x05\x02")
        _, ulen = recv_exact(conn, 2)
        user = recv_exact(conn, ulen).decode()
        (plen,) = recv_exact(conn, 1)
        given = recv_exact(conn, plen).decode()
        if given != password:
            conn.sendall(b"\x01\x01")
            return
        conn.sendall(b"\x01\x00")
        ver, cmd, _, atyp = recv_exact(conn, 4)
        if atyp == 1:
            host = socket.inet_ntop(socket.AF_INET, recv_exact(conn, 4))
        elif atyp == 4:
            host = socket.inet_ntop(socket.AF_INET6, recv_exact(conn, 16))
        else:
            host = recv_exact(conn, recv_exact(conn, 1)[0]).decode()
        (port,) = struct.unpack("!H", recv_exact(conn, 2))
        with log_lock:
            log.write(f"{user} {host} {port}\n")
            log.flush()
        if port == 10:
            return
        if port in REPLIES:
            conn.sendall(bytes([5, REPLIES[port], 0, 1, 0, 0, 0, 0, 0, 0]))
            return
        time.sleep(DELAYS.get(port, 0))
        reply = bytes([5, 0, 0, 1, 127, 0, 0, 1]) + struct.pack("!H", 4242)
        if port == 22:
            reply += b"SSH-2.0-fake\r\n"
        conn.sendall(reply)
        echo(conn)
    except (EOFError, OSError):
        pass
    finally:
        conn.close()


def main():
    port_file, password_file, log_file = sys.argv[1:4]
    with open(password_file) as f:
        password = f.read().strip()
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(128)
    with open(port_file, "w") as f:
        f.write(str(listener.getsockname()[1]))
    log = open(log_file, "a")
    log_lock = threading.Lock()
    while True:
        conn, _ = listener.accept()
        threading.Thread(target=serve, args=(conn, password, log, log_lock), daemon=True).start()


if __name__ == "__main__":
    main()
