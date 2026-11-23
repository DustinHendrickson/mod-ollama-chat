import contextlib
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import httpx

from bridge import chat_messages
from subscription import (
    PLAN_SCOPE, RequestError, authorization, available_models, complete,
    read_session, response_text, sign_in, token_fields,
)

TEST_MODEL = "example-model"


def event(kind, **fields):
    return "data: " + json.dumps({"type": kind, **fields})


class ChatGPTBridgeTests(unittest.TestCase):
    def test_translates_only_supported_fields_and_uses_plan_token(self):
        incoming = {
            "model": TEST_MODEL,
            "messages": [{"role": "system", "content": "Be brief"},
                         {"role": "user", "content": "Hello"}],
            "temperature": 0.8,
            "max_tokens": 100,
        }
        response = httpx.Response(200, text="\n".join([
            event("response.output_text.delta", delta="Hi"),
            event("response.completed"),
        ]))
        with patch("subscription.authorization", return_value={"Authorization": "Bearer test"}) as auth, \
                patch("subscription.httpx.stream", return_value=contextlib.nullcontext(response)) as request:
            self.assertEqual(complete("state", TEST_MODEL, chat_messages(incoming, TEST_MODEL)), "Hi")
        auth.assert_called_once_with("state")
        request.assert_called_once_with(
            "POST", "https://api.openai.com/v1/responses",
            headers={"Authorization": "Bearer test"}, timeout=40,
            json={"model": TEST_MODEL, "input": [{"role": "developer", "content": "Be brief"},
                                          {"role": "user", "content": "Hello"}],
                  "stream": True, "store": False},
        )

    def test_rejects_a_model_other_than_the_server_selection(self):
        with self.assertRaisesRegex(RequestError, "unexpected_model"):
            chat_messages({"model": "other", "messages": []}, TEST_MODEL)

    def test_lists_only_selectable_model_slugs(self):
        response = httpx.Response(200, json={"models": [
            {"slug": "listed-model", "visibility": "list"},
            {"slug": "hidden-model", "visibility": "hide"},
            {"slug": "invalid model", "visibility": "list"},
        ]})
        with patch("subscription.authorization", return_value={}), \
                patch("subscription.httpx.get", return_value=response):
            self.assertEqual(available_models("state"), ["listed-model"])

    def test_declining_plan_usage_preserves_registration_without_access_tokens(self):
        with tempfile.TemporaryDirectory() as state:
            directory = Path(state)
            (directory / "host.json").write_text('{"id": "urn:uuid:test"}', encoding="utf-8")
            callback = {"client_id": "oaiapp_test", "code": "code"}
            tokens = {"id_token": "test-id-token", "scope": "openid profile"}

            def claims(*args, **kwargs):
                return {"sub": "test-subject", "nonce": authorize.call_args.args[1]["nonce"]}

            with patch("subscription.session_lock", side_effect=contextlib.nullcontext), \
                    patch("subscription.await_callback", return_value=callback) as authorize, \
                    patch("subscription.token_request", return_value=tokens), \
                    patch("subscription.jwt.PyJWKClient") as jwks, \
                    patch("subscription.jwt.decode", side_effect=claims):
                jwks.return_value.get_signing_key_from_jwt.return_value = SimpleNamespace(key="key")
                with self.assertRaisesRegex(RequestError, "chatgpt_plan_permission_required"):
                    sign_in(directory, 1455)
                self.assertEqual(read_session(directory), {
                    "client_id": "oaiapp_test", "subject": "test-subject",
                    "id_token": "test-id-token", "scope": "openid profile",
                })
                with self.assertRaisesRegex(RequestError, "chatgpt_plan_permission_required"):
                    authorization(directory)
                with self.assertRaisesRegex(RequestError, "chatgpt_plan_permission_required"):
                    sign_in(directory, 1455)
                self.assertEqual(authorize.call_args.args[1]["client_id"], "oaiapp_test")
                self.assertEqual(authorize.call_args.args[1]["prompt"], "consent")

    def test_stream_limit_discards_partial_reply(self):
        with self.assertRaises(RequestError) as raised:
            response_text([
                event("response.output_text.delta", delta="partial"),
                event("response.failed", response={"error": {"code": "subscription_sharing_usage_limit_exceeded"}}),
            ], "req_test")
        self.assertEqual(raised.exception.status, 429)
        self.assertEqual(raised.exception.request_id, "req_test")

    def test_unfinished_stream_is_not_served(self):
        with self.assertRaisesRegex(RequestError, "incomplete_reply"):
            response_text([event("response.output_text.delta", delta="partial"), "data: [DONE]"])

    def test_requires_plan_scope_even_with_identity(self):
        with self.assertRaisesRegex(RequestError, "chatgpt_plan_permission_required"):
            token_fields({"scope": "openid profile email"}, {})

    def test_rotating_refresh_token_and_retained_scope(self):
        fields = token_fields(
            {"access_token": "new", "refresh_token": "rotated", "token_type": "Bearer", "expires_in": 3600},
            {"scope": PLAN_SCOPE, "refresh_token": "old"},
        )
        self.assertEqual(fields["refresh_token"], "rotated")
        self.assertEqual(fields["scope"], PLAN_SCOPE)


if __name__ == "__main__":
    unittest.main()
