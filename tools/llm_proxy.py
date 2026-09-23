#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MeOS 的 LLM 转发代理。

内核只会说明文 HTTP，而 DeepSeek 只提供 HTTPS——TLS 那一半放在这里做。
内核发过来的是一次极简请求：

    POST /ask HTTP/1.0
    Host: meos
    X-API-Key: <开机时输入的 key>
    Content-Length: <n>

    <用户的问题，纯文本>

代理把问题包成一次正常的 chat/completions 调用，拿回答案后**转成纯 ASCII**
再原样吐回去（内核的控制台字模只覆盖 0x20..0x7E，中文画不出来）。

MeOS 开机时会让用户在控制台输入一次 DeepSeek key，随请求用 X-API-Key
头带过来；请求头里没有就 503，代理自己不拿 key。

用法（key 由 MeOS 开机时让用户输入并随请求带来，代理自带不会配 key）：

    python tools/llm_proxy.py                # 监听 0.0.0.0:8080
    python tools/llm_proxy.py --port 9000
    python tools/llm_proxy.py --model deepseek-reasoner

内核那边的地址在 src/kernel/llm.inc 结尾的 llm_proxy_addr，
默认对着 VMware NAT 里的 192.168.220.1。
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_URL = "https://api.deepseek.com/chat/completions"

# 控制台一行能放下的字符数，用来折行，纯为好看。
# 内核终端是 800x600 / 16x24 = 50 列 x 25 行，不是老式的 80 列终端。
CONSOLE_COLS = 50

# 内核 llm_buf 是 4096 字节，减去 HTTP 头，body 留点余量
MAX_BODY = 1500

# 内核字模只有 ASCII；顺带把 Markdown 记号去掉，控制台里它们只是噪声
SYSTEM_PROMPT = (
    "You are answering inside MeOS, a small 32-bit hobby operating system "
    "with a text console that can only display plain ASCII (0x20-0x7E). "
    "Answer in English, using ASCII characters only. "
    "No markdown, no code fences, no bullet symbols, no emoji. "
    "Keep the answer under 300 words and reply with the answer alone."
)


def to_console_text(text):
    """把模型输出压成控制台能显示、也好看的样子。"""
    # 非 ASCII 一律换成 ?：内核的字模到此为止
    text = text.encode("ascii", "replace").decode("ascii")
    # Markdown 的记号在纯文本终端里只是噪声
    text = re.sub(r"^\s{0,3}#{1,6}\s*", "", text, flags=re.MULTILINE)
    text = re.sub(r"\*\*(.+?)\*\*", r"\1", text)
    text = re.sub(r"(?<!\w)[*_`]{1,3}(.+?)[*_`]{1,3}(?!\w)", r"\1", text)
    text = text.replace("|", " ")
    # 行尾空白、连续空行、连续空格都收一收
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r" *\n *", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    text = text.strip()
    # 折行，省得靠内核那边硬折
    out = []
    for line in text.split("\n"):
        while len(line) > CONSOLE_COLS:
            cut = line.rfind(" ", 0, CONSOLE_COLS) or CONSOLE_COLS
            out.append(line[:cut])
            line = line[cut:].lstrip()
        out.append(line)
    return "\n".join(out)


def ask_deepseek(question, model, api_key, timeout):
    """问一次大模型，返回控制台可显示的文本。"""
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": question},
        ],
        "stream": False,
        "max_tokens": 800,
        "temperature": 0.3,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        API_URL,
        data=data,
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + api_key,
            "Accept": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = json.loads(resp.read().decode("utf-8"))

    choices = body.get("choices") or []
    if not choices:
        raise RuntimeError("no choices in response: " + json.dumps(body)[:200])
    message = choices[0].get("message") or {}
    answer = message.get("content") or ""
    # deepseek-reasoner 偶尔只给 reasoning_content（思考过程）不给 content；
    # 内核控制台不介意看到思考过程，别让一次空回答浪费整个会话。
    if not answer:
        answer = message.get("reasoning_content") or ""
        if answer:
            answer = to_console_text(answer) + "\n[reasoning]"
    answer = to_console_text(answer)
    if not answer:
        raise RuntimeError("model returned nothing usable")
    if len(answer) > MAX_BODY:
        answer = answer[:MAX_BODY].rstrip() + "\n[truncated]"
    return answer


class Handler(BaseHTTPRequestHandler):
    """只认一个端点：POST /ask，body 是纯文本问题。"""

    protocol_version = "HTTP/1.0"      # 应答完就关连接，内核靠 FIN 判断收完了
    server_version = "meos-llm-proxy/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[proxy] " + (fmt % args) + "\n")

    def _reply(self, code, text):
        data = text.encode("ascii", "replace")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=us-ascii")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        """宿主侧探活：看一眼代理还活着、拿着什么配置，不动 DeepSeek。"""
        cfg = self.server.cfg
        self._reply(200,
                    "meos-llm-proxy ok\nmodel=%s\nkey=per-request (X-API-Key)\n"
                    % cfg["model"])

    def do_POST(self):
        if self.path.rstrip("/") != "/ask":
            self._reply(404, "no such endpoint: " + self.path)
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            self._reply(400, "bad Content-Length")
            return
        if length <= 0 or length > 8192:
            self._reply(400, "bad request size")
            return

        question = self.rfile.read(length).decode("utf-8", "replace").strip()
        if not question:
            self._reply(400, "empty question")
            return

        cfg = self.server.cfg
        # key 只来自内核：MeOS 开机时用户在控制台输入，
        # 随请求用 X-API-Key 头带过来。代理自带不配 key。
        api_key = (self.headers.get("X-API-Key") or "").strip()
        if not api_key:
            self._reply(503, "no api key: include X-API-Key header")
            return

        print("[proxy] 提问 %s（key from request）：%s" % (cfg["model"], question[:60]))
        try:
            answer = ask_deepseek(question, cfg["model"], api_key,
                                  cfg["timeout"])
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")[:300]
            print("[proxy] 接口报错 %s：%s" % (exc.code, detail))
            self._reply(502, "deepseek http %s: %s" % (exc.code, detail))
            return
        except Exception as exc:                       # noqa: BLE001
            print("[proxy] 失败：%r" % (exc,))
            self._reply(502, "proxy error: %s" % exc)
            return

        print("[proxy] 回答 %d 字节" % len(answer))
        self._reply(200, answer)


def main():
    ap = argparse.ArgumentParser(description="MeOS 的 LLM 转发代理")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--model",
                    default=os.environ.get("DEEPSEEK_MODEL", "deepseek-chat"))
    ap.add_argument("--timeout", type=float, default=50.0,
                    help="等 DeepSeek 的秒数，要比内核那边的 60 秒短")
    args = ap.parse_args()

    # key 不在这里配：MeOS 开机时让用户在控制台输入，
    # 随请求用 X-API-Key 头带过来。代理不再读 .env / DEEPSEEK_API_KEY。

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.cfg = {"model": args.model, "api_key": "", "timeout": args.timeout}

    print("MeOS LLM 代理已启动：http://%s:%d/ask   model=%s" %
          (args.host, args.port, args.model))
    print("内核里用： llm <你的问题>")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n再见。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
