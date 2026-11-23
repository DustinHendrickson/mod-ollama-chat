"""A dedicated OpenAI Sign in with ChatGPT session for the WoW bridge."""

import base64
import hashlib
import json
import os
import re
import secrets
import tempfile
import time
import uuid
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlencode, urlsplit

import httpx
import jwt


AUTH = "https://auth.openai.com"
API = "https://api.openai.com/v1"
TOKEN_URL = AUTH + "/api/accounts/oauth/token"
APP_NAME = "mod-ollama-chat ChatGPT bridge"
PLAN_SCOPE = "chatgpt.tokens.use.direct"
SCOPES = f"openid profile email offline_access resource.invoke {PLAN_SCOPE}"
UNUSABLE_REFRESH = {
    "invalid_grant", "invalid_refresh_token", "token_expired",
    "refresh_token_expired", "refresh_token_invalidated", "refresh_token_reused",
}
STREAM_ERRORS = {
    "subscription_sharing_usage_limit_exceeded": 429,
    "subscription_sharing_usage_unavailable": 503,
    "subscription_sharing_user_unavailable": 503,
    "subscription_sharing_user_not_eligible": 403,
    "subscription_sharing_unsupported_capability": 400,
    "subscription_sharing_invalid_user": 401,
}


class RequestError(Exception):
    def __init__(self, status, code, request_id=None):
        self.status = status
        self.code = code
        self.request_id = request_id
        super().__init__(f"HTTP {status} {code}" + (f" ({request_id})" if request_id else ""))


def safe_identifier(value, default):
    return value if isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.-]{1,120}", value) else default


def error_code(body, default):
    error = body.get("error") if isinstance(body, dict) else None
    return safe_identifier(error.get("code") if isinstance(error, dict) else error, default)


def check_status(response):
    if 200 <= response.status_code < 300:
        return
    try:
        body = response.read()
        error = json.loads(body)
    except (ValueError, UnicodeError):
        error = {}
    request_id = safe_identifier(response.headers.get("x-request-id"), None)
    raise RequestError(response.status_code, error_code(error, "upstream_error"), request_id)


def write_private(path, value):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(value, handle)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


@contextmanager
def session_lock(directory):
    import fcntl

    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(directory / "session.lock", os.O_CREAT | os.O_RDWR, 0o600)
    with os.fdopen(fd, "wb") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def read_session(directory):
    path = directory / "session.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def token_request(**form):
    response = httpx.post(TOKEN_URL, data={**form, "resource": API}, timeout=15)
    check_status(response)
    return response.json()


def token_fields(tokens, previous):
    scopes = tokens.get("scope", previous.get("scope", "")).split()
    if PLAN_SCOPE not in scopes:
        raise RequestError(403, "chatgpt_plan_permission_required")
    if tokens.get("token_type", "").lower() != "bearer":
        raise RequestError(502, "invalid_token_response")
    return {
        "access_token": tokens["access_token"],
        "refresh_token": tokens.get("refresh_token") or previous["refresh_token"],
        "scope": " ".join(scopes),
        "expires_at": time.time() + int(tokens["expires_in"]),
    }


def await_callback(port, parameters):
    state = parameters["state"]
    callback = {}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            parsed = urlsplit(self.path)
            fields = parse_qs(parsed.query)
            if parsed.path != "/auth/callback" or self.headers.get("Host") != f"127.0.0.1:{port}":
                self.send_error(404)
                return
            if not secrets.compare_digest(fields.get("state", [""])[0], state):
                self.send_error(400, "Invalid authorization state")
                return
            callback.update({key: values[0] for key, values in fields.items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(b"Authorization received. Return to the bridge terminal.")

        def log_message(self, *_args):
            pass

    with HTTPServer(("127.0.0.1", port), Handler) as server:
        server.timeout = 0.5
        print(AUTH + "/api/accounts/authorize?" + urlencode(parameters), flush=True)
        deadline = time.monotonic() + 600
        while not callback and time.monotonic() < deadline:
            server.handle_request()
    if not callback or "error" in callback or not callback.get("code"):
        raise RequestError(403, "chatgpt_authorization_not_completed")
    return callback


def sign_in(directory, port):
    with session_lock(directory):
        previous = read_session(directory)
        host_path = directory / "host.json"
        if not host_path.exists():
            write_private(host_path, {"id": f"urn:uuid:{uuid.uuid4()}"})
        host = json.loads(host_path.read_text(encoding="utf-8"))["id"]
    client = previous.get("client_id", "dynamic_agent_client")
    verifier = secrets.token_urlsafe(48)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
    fields = {
        "client_id": client,
        "ext_agent_host_id": host,
        "response_type": "code",
        "redirect_uri": f"http://127.0.0.1:{port}/auth/callback",
        "scope": SCOPES,
        "resource": API,
        "state": secrets.token_urlsafe(32),
        "nonce": secrets.token_urlsafe(32),
        "code_challenge_method": "S256",
        "code_challenge": challenge,
    }
    if client == "dynamic_agent_client":
        fields["agent_name_hint"] = APP_NAME
    elif PLAN_SCOPE not in previous.get("scope", "").split():
        fields["prompt"] = "consent"

    callback = await_callback(port, fields)
    issued = callback.get("client_id", client)
    if not issued.startswith("oaiapp_") or (client != "dynamic_agent_client" and issued != client):
        raise RequestError(400, "invalid_chatgpt_client_registration")

    tokens = token_request(
        grant_type="authorization_code", client_id=issued, code=callback["code"],
        code_verifier=verifier, redirect_uri=fields["redirect_uri"],
    )
    id_token = tokens["id_token"]
    jwks = jwt.PyJWKClient(AUTH + "/.well-known/jwks.json", timeout=10)
    claims = jwt.decode(
        id_token, jwks.get_signing_key_from_jwt(id_token).key, algorithms=["RS256"],
        issuer=AUTH, audience=issued, options={"require": ["exp", "sub", "nonce"]},
    )
    if not secrets.compare_digest(claims["nonce"], fields["nonce"]):
        raise RequestError(400, "invalid_chatgpt_nonce")
    if previous.get("subject", claims["sub"]) != claims["sub"]:
        raise RequestError(403, "wrong_chatgpt_account")
    session = {
        "client_id": issued,
        "subject": claims["sub"],
        "id_token": id_token,
        "scope": tokens.get("scope", ""),
    }
    plan_enabled = PLAN_SCOPE in session["scope"].split()
    if plan_enabled:
        session.update(token_fields(tokens, {}))

    with session_lock(directory):
        if read_session(directory) != previous:
            raise RequestError(409, "chatgpt_session_changed")
        write_private(directory / "session.json", session)
    if not plan_enabled:
        raise RequestError(403, "chatgpt_plan_permission_required")


def authorization(directory):
    with session_lock(directory):
        session = read_session(directory)
        if "scope" not in session and "refresh_token" not in session:
            raise RequestError(503, "chatgpt_sign_in_required")
        if PLAN_SCOPE not in session.get("scope", "").split():
            raise RequestError(403, "chatgpt_plan_permission_required")
        if "refresh_token" not in session:
            raise RequestError(503, "chatgpt_sign_in_required")
        if session["expires_at"] <= time.time() + 90:
            try:
                refreshed = token_request(
                    grant_type="refresh_token", client_id=session["client_id"],
                    refresh_token=session["refresh_token"],
                )
            except RequestError as error:
                if error.code in UNUSABLE_REFRESH:
                    for field in ("access_token", "refresh_token", "expires_at", "scope"):
                        session.pop(field, None)
                    write_private(directory / "session.json", session)
                    raise RequestError(503, "chatgpt_sign_in_required") from error
                raise
            session.update(token_fields(refreshed, session))
            write_private(directory / "session.json", session)
        return {"Authorization": f"Bearer {session['access_token']}"}


def available_models(directory):
    response = httpx.get(API + "/models", headers=authorization(directory), timeout=15)
    check_status(response)
    models = response.json().get("models", [])
    return [slug for model in models
            if model.get("visibility") == "list"
            and (slug := safe_identifier(model.get("slug"), None))]


def response_text(events, request_id=None):
    fragments = []
    for line in events:
        if not line.startswith("data:"):
            continue
        payload = line[5:].strip()
        if payload == "[DONE]":
            break
        event = json.loads(payload)
        kind = event.get("type")
        if kind == "response.output_text.delta":
            fragments.append(event["delta"])
        elif kind == "response.completed":
            answer = "".join(fragments).strip()
            if answer:
                return answer
            break
        elif kind in {"response.failed", "response.incomplete", "error"}:
            response = event.get("response", {})
            code = error_code(response, safe_identifier(event.get("code"), "incomplete_reply"))
            raise RequestError(STREAM_ERRORS.get(code, 502), code, request_id)
    raise RequestError(502, "incomplete_reply", request_id)


def complete(directory, model, messages):
    inputs = [
        {"role": "developer" if item["role"] == "system" else item["role"], "content": item["content"]}
        for item in messages
    ]
    with httpx.stream(
        "POST", API + "/responses", headers=authorization(directory), timeout=40,
        json={"model": model, "input": inputs, "store": False, "stream": True},
    ) as response:
        check_status(response)
        return response_text(response.iter_lines(), safe_identifier(response.headers.get("x-request-id"), None))
