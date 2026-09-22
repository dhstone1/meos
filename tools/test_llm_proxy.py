#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llm_proxy 的宿主机自测：不碰真网络，也不烧 DeepSeek 配额。

起一个本地假 DeepSeek（http://127.0.0.1），把 llm_proxy.API_URL 指过去，
把「内核 POST 过来的问题 -> 代理 -> 假 API -> 控制台可读 ASCII」整条链路
在宿主机上重放一遍。跑法：

    python tools/test_llm_proxy.py
"""

import json
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import llm_proxy

REAL_API = llm_proxy.API_URL  # 测完要还回去


def start_server(handler, cfg):
    srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    srv.cfg = cfg
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


class FakeDeepSeek(BaseHTTPRequestHandler):
    """只认 POST /chat/completions：校验请求体、回一份带中文+Markdown 的回答。"""

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        req = json.loads(self.rfile.read(length).decode("utf-8"))
        assert req["model"].startswith("fake-"), req["model"]
        assert req["messages"][-1]["role"] == "user"
        assert self.headers.get("Authorization") in ("Bearer test-key", "Bearer client-key")
        reasoning = req["messages"][-1]["content"].startswith("r:")
        answer = ("## 你好 world **hello**\n"
                  "this is a fairly long line that should be wrapped by the "
                  "proxy because the meos console is only fifty columns wide\n")
        if reasoning:
            # 模拟 deepseek-reasoner 只回思考过程
            msg = {"role": "assistant",
                   "reasoning_content": (answer + "\nfinal answer").strip()}
        else:
            msg = {"role": "assistant", "content": answer}
        body = json.dumps({"choices": [{"message": msg}]}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Always400(BaseHTTPRequestHandler):
    """假 API 的故障模式：一律 400，看代理怎么透传。"""

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        body = b'{"error": "quota exceeded"}'
        self.send_response(400)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    fake, fake_port = start_server(FakeDeepSeek,
                                   {"model": "fake-chat", "api_key": "test-key",
                                    "timeout": 5.0})
    try:
        llm_proxy.API_URL = "http://127.0.0.1:%d/chat/completions" % fake_port

        # --- 1. ask_deepseek：转换质量 ---
        ans = llm_proxy.ask_deepseek("hello from kernel", "fake-chat",
                                     "test-key", 5.0)
        assert ans and all(ord(c) < 128 for c in ans), "not pure ASCII"
        assert "**" not in ans and "#" not in ans, "markdown leaked"
        assert all(len(line) <= llm_proxy.CONSOLE_COLS
                   for line in ans.split("\n")), "line too wide"
        assert "world hello" in ans, "text mangled"
        print("[1/5] ask_deepseek: ascii + 折行 + 去 markdown OK")

        # --- 2. reasoning_content 回退：空 content 时用思考过程顶上 ---
        ans2 = llm_proxy.ask_deepseek("r: think", "fake-reasoner",
                                      "test-key", 5.0)
        assert "[reasoning]" in ans2, "reasoning fallback missing"
        print("[2/5] deepseek-reasoner 空 content 回退 OK")

        # --- 3. 代理 HTTP 端点：POST /ask + GET / ---
        proxy, port = start_server(llm_proxy.Handler,
                                   {"model": "fake-chat", "api_key": "test-key",
                                    "timeout": 5.0})
        req = urllib.request.Request(
            "http://127.0.0.1:%d/ask" % port,
            data=b"hello from kernel", method="POST",
            headers={"X-API-Key": "test-key"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("ascii")
        assert resp.status == 200 and "world hello" in body
        # HTTP/1.0 + Connection: close：内核就靠这个拿到 FIN
        assert resp.headers.get("Connection") == "close", resp.headers
        print("[3/5] 代理 POST /ask 端点 OK")

        # --- 3b. 请求带 X-API-Key 头：代理优先用它，不回退宿主配置 ---
        req = urllib.request.Request(
            "http://127.0.0.1:%d/ask" % port,
            data=b"header key path", method="POST",
            headers={"X-API-Key": "client-key"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("ascii")
        assert resp.status == 200 and "world hello" in body
        print("[3b/5] X-API-Key 头优先于宿主配置 OK")

        with urllib.request.urlopen("http://127.0.0.1:%d/" % port,
                                    timeout=5) as resp:
            ok = resp.read().decode("ascii")
        assert "meos-llm-proxy ok" in ok and "key=per-request" in ok, ok
        print("[4/5] 代理 GET / 探活 OK")

        # --- 4. 头里没带 key 时明确 503，别让内核干等 ---
        req = urllib.request.Request(
            "http://127.0.0.1:%d/ask" % port,
            data=b"hi", method="POST")
        proxy.cfg["api_key"] = ""
        try:
            urllib.request.urlopen(req, timeout=5)
            raise SystemExit("缺 key 竟然成功了")
        except urllib.error.HTTPError as exc:
            assert exc.code == 503, exc.code
        print("[5/5] 缺 DEEPSEEK_API_KEY 时 503 OK")

        print("\n全部通过。")
        return 0
    finally:
        llm_proxy.API_URL = REAL_API


if __name__ == "__main__":
    sys.exit(main())
