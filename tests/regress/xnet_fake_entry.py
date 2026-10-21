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
    53  with a "~dns" username: DNS over TCP, answered as below

UDP ASSOCIATE (RFC 1928 section 7) opens a relay on 127.0.0.1 that takes
datagrams only from the client address declared in the request, and answers
by the destination port:

    7   echo, from the destination
    9   nothing
    13  first a datagram from the destination's port + 1, then the echo
    19  three datagrams: "1", "2", "3"
    99  echo, then the association ends (the control connection is closed)
    53  with a "~dns" username: a DNS answer - 100.64.0.7 for names under
        .ts.net, 93.184.216.34 for the rest - from the destination

Usage: xnet_fake_entry.py <port file> <password file> <log file> [noudp]
It binds 127.0.0.1:0, writes the port to <port file>, and appends one line
per CONNECT to <log file>: "<username> <destination> <port>", and one per
datagram: "UDP <username> <destination> <port> <length>". With noudp, UDP
ASSOCIATE is refused with 0x07, as by an entry without UDP.
"""
import select
import socket
import struct
import sys
import threading
import time

NO_UDP = len(sys.argv) > 4 and sys.argv[4] == "noudp"

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


def dns_name(query):
    labels, i = [], 12
    while i < len(query) and query[i]:
        n = query[i]
        labels.append(query[i + 1:i + 1 + n].decode(errors="replace"))
        i += 1 + n
    return ".".join(labels), i + 5  # past the root label, QTYPE and QCLASS


def dns_answer(query):
    name, end = dns_name(query)
    ip = "100.64.0.7" if name.endswith(".ts.net") else "93.184.216.34"
    head = query[:2] + b"\x81\x80" + b"\x00\x01\x00\x01\x00\x00\x00\x00"
    answer = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + socket.inet_aton(ip)
    return head + query[12:end] + answer, name


def udp_header(host, port):
    try:
        return b"\x00\x00\x00\x01" + socket.inet_aton(host) + struct.pack("!H", port)
    except OSError:
        return b"\x00\x00\x00\x04" + socket.inet_pton(socket.AF_INET6, host) + struct.pack("!H", port)


def parse_udp(data):
    if len(data) < 4 or data[2] != 0:
        return None
    atyp = data[3]
    if atyp == 1 and len(data) >= 10:
        return socket.inet_ntop(socket.AF_INET, data[4:8]), struct.unpack("!H", data[8:10])[0], data[10:]
    if atyp == 4 and len(data) >= 22:
        return socket.inet_ntop(socket.AF_INET6, data[4:20]), struct.unpack("!H", data[20:22])[0], data[22:]
    return None


def associate(conn, user, client_port, log, log_lock):
    relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    relay.bind(("127.0.0.1", 0))
    conn.sendall(bytes([5, 0, 0, 1, 127, 0, 0, 1]) + struct.pack("!H", relay.getsockname()[1]))
    try:
        while True:
            ready, _, _ = select.select([conn, relay], [], [])
            if conn in ready and not conn.recv(1):
                return
            if relay not in ready:
                continue
            data, src = relay.recvfrom(70000)
            if src != ("127.0.0.1", client_port):
                with log_lock:
                    log.write(f"SPOOF {user} {src[0]} {src[1]}\n")
                    log.flush()
                continue
            parsed = parse_udp(data)
            if parsed is None:
                continue
            host, port, payload = parsed
            with log_lock:
                log.write(f"UDP {user} {host} {port} {len(payload)}\n")
                log.flush()
            if port == 53 and user.endswith("~dns"):
                answer, name = dns_answer(payload)
                with log_lock:
                    log.write(f"DNS {user} {name}\n")
                    log.flush()
                relay.sendto(udp_header(host, port) + answer, src)
            elif port == 13:
                relay.sendto(udp_header(host, port + 1) + b"stray", src)
                relay.sendto(udp_header(host, port) + payload, src)
            elif port == 19:
                for part in (b"1", b"2", b"3"):
                    relay.sendto(udp_header(host, port) + part, src)
            elif port in (7, 99):
                relay.sendto(udp_header(host, port) + payload, src)
                if port == 99:
                    return
    finally:
        relay.close()


def dns_over_tcp(conn, user, log, log_lock):
    conn.sendall(bytes([5, 0, 0, 1, 127, 0, 0, 1]) + struct.pack("!H", 4242))
    while True:
        (n,) = struct.unpack("!H", recv_exact(conn, 2))
        answer, name = dns_answer(recv_exact(conn, n))
        with log_lock:
            log.write(f"DNS-TCP {user} {name}\n")
            log.flush()
        conn.sendall(struct.pack("!H", len(answer)) + answer)


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
        if cmd == 3 and NO_UDP:
            conn.sendall(bytes([5, 7, 0, 1, 0, 0, 0, 0, 0, 0]))
            return
        if atyp == 1:
            host = socket.inet_ntop(socket.AF_INET, recv_exact(conn, 4))
        elif atyp == 4:
            host = socket.inet_ntop(socket.AF_INET6, recv_exact(conn, 16))
        else:
            host = recv_exact(conn, recv_exact(conn, 1)[0]).decode()
        (port,) = struct.unpack("!H", recv_exact(conn, 2))
        if cmd == 3:
            with log_lock:
                log.write(f"ASSOCIATE {user} {host} {port}\n")
                log.flush()
            associate(conn, user, port, log, log_lock)
            return
        if port == 53 and user.endswith("~dns"):
            with log_lock:
                log.write(f"{user} {host} {port}\n")
                log.flush()
            dns_over_tcp(conn, user, log, log_lock)
            return
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
