import asyncio
import json
import os
import subprocess
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer

import websockets

HOST = "0.0.0.0"
PORT = 8732
WS_PORT = 8733

API_VERSION = "2022-11-28"

# Set to your GitHub username or leave None to auto-resolve via `gh api user`.
GITHUB_USERNAME = None

# Fallback when the API does not report a quota.
DEFAULT_MONTHLY_QUOTA = 300.0

POLL_SECONDS = 900
DEBUG = os.environ.get("COPILOT_USAGE_DEBUG", "").strip().lower() in {"1", "true", "yes"}

_cache_lock = threading.Lock()
_cache_payload = {
    "remaining": None,
    "limit": None,
    "percent": None,
    "avgDaily": None,
    "resetAt": None,
    "updatedAt": None,
    "pollIntervalSec": int(POLL_SECONDS),
    "pollInSec": None,
    "nextPollAt": None,
    "error": "no data yet",
}

_last_poll_at = None

# WebSocket broadcast state
_ws_clients = set()
_ws_clients_lock = threading.Lock()
_ws_loop = None


def _apply_poll_schedule(payload, now, last_poll_at):
    poll_interval = int(POLL_SECONDS)
    if last_poll_at is None:
        last_poll_at = now
    next_poll = last_poll_at + timedelta(seconds=poll_interval)
    poll_in = int((next_poll - now).total_seconds())
    if poll_in < 0:
        poll_in = 0
    payload["pollIntervalSec"] = poll_interval
    payload["pollInSec"] = poll_in
    payload["nextPollAt"] = next_poll.strftime("%Y-%m-%dT%H:%M:%SZ")
    return payload


def _coerce_number(value):
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _value_to_float(value):
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        value = value.strip()
        if not value:
            return None
        try:
            return float(value)
        except ValueError:
            return None
    return None


def _extract_number(obj, keys):
    if not isinstance(obj, dict):
        return None
    for key in keys:
        if key in obj:
            value = _value_to_float(obj.get(key))
            if value is not None and value >= 0:
                return value
    return None


def _extract_string(obj, keys):
    if not isinstance(obj, dict):
        return None
    for key in keys:
        if key in obj:
            value = obj.get(key)
            if isinstance(value, str) and value.strip():
                return value.strip()
    return None


def _parse_date(obj, fallback_date):
    if not isinstance(obj, dict):
        return fallback_date
    raw = _extract_string(obj, ["date", "day"])
    if raw:
        try:
            return datetime.strptime(raw, "%Y-%m-%d").date()
        except ValueError:
            pass
    year = _extract_number(obj, ["year"])
    month = _extract_number(obj, ["month"])
    day = _extract_number(obj, ["day"]) or 1
    if year and month and day:
        try:
            return datetime(int(year), int(month), int(day)).date()
        except ValueError:
            pass
    return fallback_date


def _parse_row(obj, fallback_date):
    quantity = _extract_number(
        obj,
        [
            "grossQuantity",
            "quantity",
            "gross_quantity",
            "total",
            "used",
            "requests",
            "premium_requests",
            "total_requests",
            "count",
        ],
    )
    if quantity is None:
        return None
    return {
        "date": _parse_date(obj, fallback_date),
        "quantity": quantity,
        "model": _extract_string(obj, ["model", "model_name", "name"]),
        "total_monthly_quota": _extract_number(
            obj, ["total_monthly_quota", "monthly_quota", "quota"]
        ),
    }


def _collect_records(value, fallback_date, output):
    if isinstance(value, list):
        for item in value:
            _collect_records(item, fallback_date, output)
        return
    if not isinstance(value, dict):
        return

    found_nested = False
    for key in [
        "data",
        "usage",
        "items",
        "usageItems",
        "results",
        "entries",
        "days",
        "models",
    ]:
        if key in value and isinstance(value[key], (list, dict)):
            found_nested = True
            _collect_records(value[key], fallback_date, output)

    if not found_nested:
        record = _parse_row(value, fallback_date)
        if record:
            output.append(record)


def _parse_usage_payload(value, fallback_date):
    records = []
    _collect_records(value, fallback_date, records)
    return records


def _run_gh(args):
    if DEBUG:
        print("gh:", " ".join(args))
    return subprocess.run(args, check=False, capture_output=True, text=True)


def _resolve_username():
    if GITHUB_USERNAME:
        return GITHUB_USERNAME
    for key in ["GITHUB_USER", "GH_USERNAME"]:
        env_value = os.environ.get(key, "").strip()
        if env_value:
            return env_value
    output = _run_gh(["gh", "api", "user", "--jq", ".login"])
    if output.returncode == 0:
        username = output.stdout.strip()
        if username:
            return username
    return None


def _fetch_usage_period(username, year, month, day=None):
    endpoint = f"/users/{username}/settings/billing/premium_request/usage?year={year}&month={month}"
    if day is not None:
        endpoint += f"&day={day}"
    args = [
        "gh",
        "api",
        "-H",
        "Accept: application/vnd.github+json",
        "-H",
        f"X-GitHub-Api-Version: {API_VERSION}",
        endpoint,
    ]
    output = _run_gh(args)
    if output.returncode != 0:
        stderr = output.stderr.strip()
        message = stderr if stderr else "unknown gh error"
        raise RuntimeError(message)
    return json.loads(output.stdout)


def _compute_summary(month_payload, day_payload, today):
    month_records = _parse_usage_payload(month_payload, today.replace(day=1))
    day_records = _parse_usage_payload(day_payload, today)

    mtd_used = sum(row["quantity"] for row in month_records)
    today_used = sum(row["quantity"] for row in day_records)

    # Per-model breakdown
    model_quantities = {}
    for row in month_records:
        name = row.get("model") or "unknown"
        model_quantities[name] = model_quantities.get(name, 0) + row["quantity"]

    models = []
    for name, qty in sorted(model_quantities.items(), key=lambda x: -x[1]):
        pct = round(qty / mtd_used * 100, 1) if mtd_used > 0 else 0
        if pct > 0:
            models.append({"model": name, "percent": pct})

    quota = None
    for row in month_records + day_records:
        candidate = row.get("total_monthly_quota")
        if candidate and candidate > 0:
            quota = candidate
    if quota is None:
        quota = DEFAULT_MONTHLY_QUOTA

    remaining = max(quota - mtd_used, 0.0)
    percent = (mtd_used / quota * 100.0) if quota > 0 else None
    days_elapsed = today.day
    avg_daily = (mtd_used / days_elapsed) if days_elapsed > 0 else None

    next_month = (today.replace(day=28) + timedelta(days=4)).replace(day=1)
    reset_at = datetime(next_month.year, next_month.month, 1).strftime("%Y-%m-%dT%H:%M:%SZ")

    return {
        "remaining": remaining,
        "limit": quota,
        "percent": percent,
        "avgDaily": avg_daily,
        "resetAt": reset_at,
        "todayUsed": today_used,
        "models": models,
    }


def _update_cache(payload):
    with _cache_lock:
        _cache_payload.update(payload)
    _broadcast_ws(payload)


def _run_gh_command():
    try:
        username = _resolve_username()
        if not username:
            raise RuntimeError("missing GitHub username")

        today = datetime.now(timezone.utc).date()
        month_payload = _fetch_usage_period(username, today.year, today.month)
        day_payload = _fetch_usage_period(username, today.year, today.month, today.day)

        extracted = _compute_summary(month_payload, day_payload, today)
        now = datetime.now(timezone.utc)
        extracted["updatedAt"] = now.strftime("%Y-%m-%dT%H:%M:%SZ")
        extracted["error"] = None
        global _last_poll_at
        _last_poll_at = now
        extracted = _apply_poll_schedule(extracted, now, _last_poll_at)
        _update_cache(extracted)
        if DEBUG:
            print("updated:", extracted)
    except Exception as exc:
        now = datetime.now(timezone.utc)
        error_payload = {
            "error": str(exc),
            "updatedAt": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        error_payload = _apply_poll_schedule(error_payload, now, _last_poll_at)
        _update_cache(error_payload)
        if DEBUG:
            print("error:", error_payload)


def _poll_loop():
    while True:
        _run_gh_command()
        time.sleep(POLL_SECONDS)


# WebSocket broadcast -------------------------------------------------

def _broadcast_ws(payload):
    with _ws_clients_lock:
        if not _ws_clients:
            return
        clients = list(_ws_clients)
        loop = _ws_loop
    if loop is None:
        return
    message = json.dumps(payload)
    asyncio.run_coroutine_threadsafe(
        _ws_send_all(clients, message), loop
    )

async def _ws_send_all(clients, message):
    await asyncio.gather(
        *(c.send(message) for c in clients),
        return_exceptions=True,
    )

async def _ws_handler(websocket):
    global _ws_loop
    if _ws_loop is None:
        _ws_loop = asyncio.get_running_loop()
    with _ws_clients_lock:
        _ws_clients.add(websocket)
    try:
        with _cache_lock:
            payload = dict(_cache_payload)
        payload = _apply_poll_schedule(payload, datetime.now(timezone.utc), _last_poll_at)
        await websocket.send(json.dumps(payload))
        async for message in websocket:
            if message == "refresh":
                if DEBUG:
                    print("refresh requested by client", websocket.remote_address)
                threading.Thread(target=_run_gh_command, daemon=True).start()
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        with _ws_clients_lock:
            _ws_clients.discard(websocket)

def _run_ws_server():
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    global _ws_loop
    _ws_loop = loop

    async def _start():
        async with websockets.serve(
            _ws_handler, HOST, WS_PORT,
            ping_interval=30,
            ping_timeout=10,
        ):
            await asyncio.Future()  # run forever

    loop.run_until_complete(_start())


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if DEBUG:
            print("request:", self.command, self.path, "from", self.client_address[0])
        if self.path != "/copilot-usage":
            self.send_response(404)
            self.end_headers()
            return

        with _cache_lock:
            payload = dict(_cache_payload)

        now = datetime.now(timezone.utc)
        payload = _apply_poll_schedule(payload, now, _last_poll_at)

        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return


def main():
    thread = threading.Thread(target=_poll_loop, daemon=True)
    thread.start()
    ws_thread = threading.Thread(target=_run_ws_server, daemon=True)
    ws_thread.start()
    print("HTTP server on http://%s:%d/copilot-usage" % (HOST, PORT))
    print("WebSocket server on ws://%s:%d" % (HOST, WS_PORT))
    server = HTTPServer((HOST, PORT), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
