#!/usr/bin/env python3
"""I1 heartbeat 수동 테스트용 모의 서버. 요청 1건을 받고 종료한다."""
import argparse
import socket
import time


def read_request(conn):
    data = b""
    conn.settimeout(5)
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk

    header_part, _, rest = data.partition(b"\r\n\r\n")
    headers_text = header_part.decode("utf-8", errors="replace")
    lines = headers_text.split("\r\n")
    request_line = lines[0] if lines else ""
    headers = lines[1:]

    content_length = 0
    for h in headers:
        if h.lower().startswith("content-length:"):
            content_length = int(h.split(":", 1)[1].strip())

    body = rest
    while len(body) < content_length:
        chunk = conn.recv(4096)
        if not chunk:
            break
        body += chunk

    return request_line, headers, body.decode("utf-8", errors="replace")


def send_response(conn, status_line, body, content_type="application/json"):
    resp = (
        f"HTTP/1.1 {status_line}\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
        f"{body}"
    )
    conn.sendall(resp.encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description="I1 heartbeat mock server (단건 요청 처리 후 종료)")
    parser.add_argument("--port", type=int, default=3000)
    parser.add_argument("--mode", choices=["200", "403", "noresponse"], required=True)
    parser.add_argument(
        "--hold-seconds", type=float, default=10.0,
        help="noresponse 모드에서 응답을 보내지 않고 연결을 들고 있는 시간(초)",
    )
    args = parser.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"[mock_server] mode={args.mode} port={args.port} 에서 요청 1건을 기다립니다...", flush=True)

    conn, addr = srv.accept()
    print(f"[mock_server] 연결 수락: {addr}", flush=True)

    try:
        request_line, headers, body = read_request(conn)
        print(f"[mock_server] request_line: {request_line}")
        print("[mock_server] headers:")
        for h in headers:
            print(f"  {h}")
        print(f"[mock_server] body: {body}")

        if args.mode == "200":
            send_response(conn, "200 OK", '{"config_version":0}')
        elif args.mode == "403":
            send_response(conn, "403 Forbidden", "forbidden", content_type="text/plain")
        elif args.mode == "noresponse":
            print(f"[mock_server] 응답을 보내지 않고 {args.hold_seconds}초 대기합니다...", flush=True)
            time.sleep(args.hold_seconds)
    finally:
        conn.close()
        srv.close()
        print("[mock_server] 종료", flush=True)


if __name__ == "__main__":
    main()
