"""Private Chat Completions endpoint for AzerothCore, backed by a ChatGPT plan."""

import argparse
import json
import os
import secrets
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import httpx
import jwt

from subscription import RequestError, available_models, complete, safe_identifier, sign_in


def chat_messages(body, model):
    if not isinstance(body, dict) or body.get("model") != model:
        raise RequestError(400, "unexpected_model")
    if body.get("stream") or body.get("tools") or body.get("functions"):
        raise RequestError(400, "only_text_chat_is_supported")
    messages = body.get("messages")
    if not isinstance(messages, list) or not messages:
        raise RequestError(400, "messages_required")
    for item in messages:
        if (not isinstance(item, dict)
                or item.get("role") not in ("user", "assistant", "system", "developer")
                or not isinstance(item.get("content"), str)):
            raise RequestError(400, "invalid_text_message")
    return messages


def serve(directory, listen, port, model):
    key = (directory / "bridge-key").read_bytes().strip()
    if len(key) < 32:
        raise ValueError("The bridge key must be at least 32 bytes.")
    call_lock = threading.Lock()
    paused_until = 0

    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(8)

        def log_message(self, *_args):
            pass

        def reply(self, status, payload):
            data = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/health":
                self.reply(200, {"ok": True})
            else:
                self.reply(404, {"error": {"code": "not_found"}})

        def do_POST(self):
            nonlocal paused_until
            try:
                if self.path != "/v1/chat/completions":
                    raise RequestError(404, "not_found")
                authorization = self.headers.get("Authorization", "").encode()
                if not secrets.compare_digest(authorization, b"Bearer " + key):
                    raise RequestError(401, "invalid_bridge_key")
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 65536:
                    raise RequestError(400, "invalid_content_length")
                messages = chat_messages(json.loads(self.rfile.read(length)), model)
                if not call_lock.acquire(blocking=False):
                    raise RequestError(503, "bridge_busy")
                try:
                    if time.monotonic() < paused_until:
                        raise RequestError(429, "subscription_usage_paused")
                    answer = complete(directory, model, messages)
                except RequestError as error:
                    if error.status == 429 and error.code != "subscription_usage_paused":
                        paused_until = time.monotonic() + 600
                    raise
                finally:
                    call_lock.release()
            except RequestError as error:
                if error.code not in ("subscription_usage_paused", "invalid_bridge_key", "bridge_busy"):
                    print(f"ChatGPT bridge request: {error}", file=sys.stderr, flush=True)
                self.reply(error.status, {"error": {"code": error.code}})
                return
            except (ValueError, TimeoutError):
                self.reply(400, {"error": {"code": "invalid_request"}})
                return
            except (httpx.HTTPError, OSError):
                self.reply(503, {"error": {"code": "subscription_unavailable"}})
                return

            self.reply(200, {
                "object": "chat.completion",
                "model": model,
                "choices": [{"index": 0, "finish_reason": "stop",
                             "message": {"role": "assistant", "content": answer}}],
            })

    with ThreadingHTTPServer((listen, port), Handler) as server:
        server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    login = commands.add_parser("login")
    login.add_argument("--port", type=int, default=1455)
    commands.add_parser("models")
    server = commands.add_parser("serve")
    server.add_argument("--model", required=True, help="model slug from the models command")
    server.add_argument("--listen", default="127.0.0.1")
    server.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    os.umask(0o077)
    directory = args.state_dir
    try:
        if args.command == "login":
            sign_in(directory, args.port)
            print("ChatGPT subscription permission saved.", flush=True)
        elif args.command == "models":
            models = available_models(directory)
            if not models:
                print("No selectable models are listed for this account.", file=sys.stderr)
                return 1
            print("\n".join(models))
        else:
            if not safe_identifier(args.model, None):
                parser.error("--model must be a model slug from the models command")
            serve(directory, args.listen, args.port, args.model)
    except (RequestError, jwt.PyJWTError, httpx.HTTPError, OSError, ValueError, KeyError) as error:
        print(str(error) if isinstance(error, RequestError) else f"ChatGPT bridge error: {type(error).__name__}",
              file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
