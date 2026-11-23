# Sign in with ChatGPT for mod-ollama-chat

This optional Python bridge lets a server operator authorize eligible bot
chat requests through their own ChatGPT plan. It uses the module's existing
OpenAI-compatible provider and forwards text requests through the
[documented Sign in with ChatGPT flow](https://developers.openai.com/siwc/token-sharing-open-source).

## Set up the bridge

Use Python 3.10 or newer on a POSIX host, such as Linux. Run these commands
from this directory. The bridge defaults to loopback HTTP for a worldserver
that can reach the same loopback interface.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt

umask 077
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/mod-ollama-chat/chatgpt-bridge"
mkdir -p "$state_dir"
chmod 700 "$state_dir"
if [ ! -s "$state_dir/bridge-key" ]; then
    .venv/bin/python -c 'import secrets; print(secrets.token_urlsafe(32))' > "$state_dir/bridge-key"
fi

.venv/bin/python bridge.py --state-dir "$state_dir" login
.venv/bin/python bridge.py --state-dir "$state_dir" models
```

Open the sign-in link in a browser that can reach the CLI's loopback
callback. Sign in with your own ChatGPT account and authorize plan usage.
Choose a slug returned by the `models` command and use it for both the
bridge and the module. Eligible models, usage limits, and any applicable
credits depend on the selected account and workspace.

```sh
.venv/bin/python bridge.py --state-dir "$state_dir" serve --model '<model-slug>'
```

In your local `mod_ollama_chat.conf`:

```ini
OllamaChat.Provider = openai
OllamaChat.Url = http://127.0.0.1:8765/v1/chat/completions
OllamaChat.Model = <model-slug>
OllamaChat.ApiKey = <contents of bridge-key>
OllamaChat.ApiKeyHeader =
```

`OllamaChat.ApiKey` is the local bridge key in this configuration. Reload the
module with `.ollama reload` and verify it with `.ollama test hello`. If the
worldserver runs in a separate container or network namespace, bind the
bridge to an appropriate private interface with `--listen` and use its
private address in the module URL. Keep the bridge endpoint off the public
internet.

## Credentials and player data

The state directory contains the host registration, OAuth session, and
bridge key. Keep it outside the checkout and restrict access to the account
running the bridge. Run `login` again after a session expires or consent
changes. For another account use a separate private state directory. To revoke
access, disconnect the bridge in ChatGPT's Login connections settings and
stop the bridge. See [OpenAI's account and session guidance](https://developers.openai.com/siwc/token-sharing-open-source/profiles-and-sessions).

Module prompts may include player chat, character names, and in-game context.
Tell players what is sent to OpenAI before enabling this provider. The bridge
requests `store: false` and uses the operator's authorized plan. It forwards
text messages with the selected model and buffers the upstream stream until
the response completes.

## Test without an account

```sh
.venv/bin/python -m unittest -v
```

The unit tests mock OpenAI requests and OAuth responses.
