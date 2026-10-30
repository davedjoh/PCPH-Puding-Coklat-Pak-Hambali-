import asyncio
import hmac
import json
import os
import time
import unicodedata
from urllib.parse import parse_qs, urlparse

import httpx
import websockets

API_URL = os.getenv("API_URL", "https://openagentic.id/api/v1/chat/completions")
API_KEY = os.environ["OPENAGENTIC_KEY"]
RELAY_TOKEN = os.environ["RELAY_TOKEN"]
MODEL = os.getenv("MODEL", "deepseek-v4.1-flash")
HOST = os.getenv("HOST", "127.0.0.1")
PORT = int(os.getenv("PORT", "8765"))
MAX_TOKENS = int(os.getenv("MAX_TOKENS", "150"))
MAX_HISTORY = int(os.getenv("MAX_HISTORY", "20"))
SYSTEM_PROMPT = os.getenv(
    "SYSTEM_PROMPT",
    "You are a friendly AI companion shown on a small screen. Answer in at most two short sentences.",
)
HISTORY_FILE = os.getenv("HISTORY_FILE", "history.json")

# One shared client: the HTTPS connection to the API stays open between prompts.
client = httpx.AsyncClient(
    timeout=httpx.Timeout(60.0, connect=10.0),
    limits=httpx.Limits(max_keepalive_connections=5, keepalive_expiry=120),
)

# ---------- history (per device, persisted to a small JSON file) ----------
try:
    with open(HISTORY_FILE, "r", encoding="utf-8") as f:
        histories: dict[str, list[dict]] = json.load(f)
except (FileNotFoundError, ValueError):
    histories = {}


def save_histories() -> None:
    tmp = HISTORY_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(histories, f, ensure_ascii=False)
    os.replace(tmp, HISTORY_FILE)


# ---------- text sanitizing for the ASCII-only display font ----------
REPLACEMENTS = {
    "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
    "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ",
}


def to_ascii(text: str) -> str:
    for src, dst in REPLACEMENTS.items():
        text = text.replace(src, dst)
    return unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")


# ---------- LLM streaming ----------
async def stream_deltas(messages: list[dict]):
    """Yield reply text fragments from the LLM API as they arrive."""
    body = {
        "model": MODEL,
        "messages": messages,
        "stream": True,
        "max_tokens": MAX_TOKENS,
    }
    headers = {"Authorization": f"Bearer {API_KEY}"}

    async with client.stream("POST", API_URL, json=body, headers=headers) as r:
        if r.status_code != 200:
            detail = (await r.aread())[:120].decode("utf-8", "replace")
            raise RuntimeError(f"API {r.status_code}: {detail}")

        ctype = r.headers.get("content-type", "")
        if "text/event-stream" in ctype:
            async for line in r.aiter_lines():
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    delta = json.loads(data)["choices"][0]["delta"].get("content")
                except (ValueError, KeyError, IndexError):
                    continue
                if delta:
                    yield delta
        else:
            # Non-streaming fallback: take the JSON object, ignore any trailing protocol text.
            raw = await r.aread()
            start, end = raw.find(b"{"), raw.rfind(b"}")
            if start < 0 or end < start:
                raise RuntimeError("API returned no JSON")
            obj = json.loads(raw[start:end + 1])
            content = obj["choices"][0]["message"]["content"]
            if content:
                yield content

# ---------- WebSocket handler ----------
async def handler(ws):
    query = parse_qs(urlparse(ws.request.path).query)
    token = query.get("token", [""])[0]
    device = query.get("device", ["default"])[0][:64]

    if not hmac.compare_digest(token, RELAY_TOKEN):
        await ws.close(code=4401, reason="unauthorized")
        return

    print(f"[{device}] connected")
    try:
        async for raw in ws:
            if not isinstance(raw, str) or not raw:
                continue
            kind, text = raw[0], raw[1:].strip()

            if kind == "R":
                histories[device] = []
                save_histories()
                await ws.send("D")
                continue

            if kind != "P" or not text:
                await ws.send("D")
                continue

            history = histories.setdefault(device, [])
            history.append({"role": "user", "content": text})
            messages = [{"role": "system", "content": SYSTEM_PROMPT}] + history[-MAX_HISTORY:]

            reply = ""
            t0 = time.perf_counter()
            first = True
            try:
                async for delta in stream_deltas(messages):
                    chunk = to_ascii(delta)
                    if not chunk:
                        continue
                    if first:
                        print(f"[{device}] first token after {time.perf_counter() - t0:.2f}s")
                        first = False
                    reply += chunk
                    await ws.send("C" + chunk)
            except Exception as exc:  # network error, API error, parse error
                print(f"[{device}] error: {exc}")
                history.pop()  # drop the unanswered user message
                await ws.send("E" + to_ascii(str(exc))[:80])
                continue

            if not reply:
                history.pop()
                await ws.send("ENo reply")
                continue

            history.append({"role": "assistant", "content": reply})
            del history[:-MAX_HISTORY]
            save_histories()
            print(f"[{device}] done in {time.perf_counter() - t0:.2f}s")
            await ws.send("D")
    except websockets.ConnectionClosed:
        pass
    finally:
        print(f"[{device}] disconnected")


async def main():
    async with websockets.serve(handler, HOST, PORT, ping_interval=20, ping_timeout=20):
        print(f"relay listening on {HOST}:{PORT}")
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
