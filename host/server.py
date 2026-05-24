import json
import os
import subprocess
import threading
import time
import urllib.request
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer

import serial
from serial.tools import list_ports

HOST = "0.0.0.0"
PORT = 8732

# Optional USB serial output (e.g. "COM3"). Leave empty to disable.
SERIAL_PORT = os.environ.get("COPILOT_USAGE_SERIAL_PORT", "").strip()
SERIAL_BAUD = int(os.environ.get("COPILOT_USAGE_SERIAL_BAUD", "115200").strip() or 115200)
SERIAL_ECHO = os.environ.get("COPILOT_USAGE_SERIAL_ECHO", "").strip().lower() in {"1", "true", "yes"}

API_VERSION = "2022-11-28"

# Set to your GitHub username or leave None to auto-resolve via `gh api user`.
GITHUB_USERNAME = None

# Fallback when the API does not report a quota.
DEFAULT_MONTHLY_QUOTA = float(os.environ.get("COPILOT_USAGE_MONTHLY_QUOTA", "300").strip() or 300.0)

POLL_SECONDS = 900
DEBUG = os.environ.get("COPILOT_USAGE_DEBUG", "").strip().lower() in {"1", "true", "yes"}

# Claude Max usage tracking — auto-enabled when Claude Code credentials exist
CLAUDE_CREDENTIALS_PATH = os.environ.get(
    "CLAUDE_CREDENTIALS_PATH",
    os.path.expanduser("~/.claude/.credentials.json"),
)

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
    "claude": None,
}

_last_poll_at = None

# Serial output state
_serial_lock = threading.Lock()
_serial_conn = None
_serial_reconnect_delay = 1.0


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
            "discountQuantity",
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
    _write_serial(payload)


def _write_serial(payload):
    global _serial_conn
    if not SERIAL_PORT:
        return
    message = json.dumps(payload)
    data = (message + "\n").encode("utf-8")
    with _serial_lock:
        ser = _serial_conn
        if ser is None:
            return
        try:
            ser.write(data)
            ser.flush()
            if SERIAL_ECHO:
                print("serial>", message)
        except Exception:
            _serial_conn = None
            try:
                ser.close()
            except Exception:
                pass
            if DEBUG:
                print("serial write failed, connection closed")


def _close_serial():
    global _serial_conn
    with _serial_lock:
        if _serial_conn is not None:
            try:
                _serial_conn.close()
            except Exception:
                pass
            _serial_conn = None
    if DEBUG:
        print("serial disconnected")


def _ensure_serial():
    """Returns True if connected, False if not (after backoff sleep)."""
    global _serial_conn, _serial_reconnect_delay
    with _serial_lock:
        if _serial_conn is not None:
            return True
    if not SERIAL_PORT:
        return False
    try:
        new_conn = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=1)
    except Exception as exc:
        if DEBUG:
            print("serial connect failed (%s), retry in %.0fs" % (exc, _serial_reconnect_delay))
        time.sleep(_serial_reconnect_delay)
        _serial_reconnect_delay = min(_serial_reconnect_delay * 2, 30.0)
        return False
    with _serial_lock:
        _serial_conn = new_conn
    _serial_reconnect_delay = 1.0
    if DEBUG:
        print("serial connected on %s @ %d" % (SERIAL_PORT, SERIAL_BAUD))
    with _cache_lock:
        payload = dict(_cache_payload)
    payload = _apply_poll_schedule(payload, datetime.now(timezone.utc), _last_poll_at)
    _write_serial(payload)
    return True


def _fetch_claude_usage():
    if not os.path.isfile(CLAUDE_CREDENTIALS_PATH):
        return None
    try:
        with open(CLAUDE_CREDENTIALS_PATH) as f:
            creds = json.load(f)
        token = creds.get("claudeAiOauth", {}).get("accessToken")
        if not token:
            if DEBUG:
                print("claude: no OAuth token in credentials")
            return None
        req = urllib.request.Request(
            "https://api.anthropic.com/api/oauth/usage",
            headers={
                "Authorization": "Bearer " + token,
                "anthropic-beta": "oauth-2025-04-20",
            },
        )
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        now = datetime.now(timezone.utc)
        result = {}
        for key in ("five_hour", "seven_day", "seven_day_sonnet", "seven_day_opus"):
            bucket = data.get(key)
            if bucket is not None and bucket.get("utilization") is not None:
                resets_at = bucket.get("resets_at")
                resets_in_sec = None
                if resets_at:
                    try:
                        reset_dt = datetime.fromisoformat(resets_at)
                        delta = (reset_dt - now).total_seconds()
                        resets_in_sec = max(0, int(delta))
                    except Exception:
                        pass
                result[key] = {
                    "utilization": bucket["utilization"],
                    "resetsAt": resets_at,
                    "resetsInSec": resets_in_sec,
                }
        extra = data.get("extra_usage")
        if extra is not None:
            result["extraUsage"] = {
                "isEnabled": extra.get("is_enabled"),
                "monthlyLimit": extra.get("monthly_limit"),
                "usedCredits": extra.get("used_credits"),
            }
        if DEBUG:
            print("claude:", json.dumps(result))
        return result
    except Exception as exc:
        if DEBUG:
            print("claude error:", exc)
        return {"error": str(exc)}


def _run_gh_command():
    try:
        username = _resolve_username()
        if not username:
            raise RuntimeError("missing GitHub username")

        today = datetime.now(timezone.utc).date()
        month_payload = _fetch_usage_period(username, today.year, today.month)
        day_payload = _fetch_usage_period(username, today.year, today.month, today.day)

        extracted = _compute_summary(month_payload, day_payload, today)
        extracted["claude"] = _fetch_claude_usage()
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
        error_payload["claude"] = _fetch_claude_usage()
        error_payload = _apply_poll_schedule(error_payload, now, _last_poll_at)
        _update_cache(error_payload)
        if DEBUG:
            print("error:", error_payload)


def _poll_loop():
    while True:
        _run_gh_command()
        time.sleep(POLL_SECONDS)


def _serial_read_loop():
    while True:
        if not SERIAL_PORT:
            return
        if not _ensure_serial():
            continue
        with _serial_lock:
            ser = _serial_conn
        try:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if not line:
                continue
            if line == "refresh":
                if DEBUG:
                    print("refresh requested by serial client")
                threading.Thread(target=_run_gh_command, daemon=True).start()
            elif line == "ready":
                if DEBUG:
                    print("serial client ready")
                with _cache_lock:
                    payload = dict(_cache_payload)
                payload = _apply_poll_schedule(payload, datetime.now(timezone.utc), _last_poll_at)
                _write_serial(payload)
        except serial.SerialException:
            if DEBUG:
                print("serial read failed, connection lost")
            _close_serial()
        except Exception:
            time.sleep(1)


def _choose_serial_port():
    ports = list(list_ports.comports())
    if not ports:
        return ""
    print("Available serial ports:")
    for idx, port in enumerate(ports, start=1):
        label = "%d) %s" % (idx, port.device)
        if port.description:
            label += " - %s" % port.description
        print(label)
    print("0) Disable serial output")
    while True:
        choice = input("Select serial port [1-%d or 0]: " % len(ports)).strip()
        if not choice:
            continue
        if choice == "0":
            return ""
        if choice.isdigit():
            idx = int(choice)
            if 1 <= idx <= len(ports):
                return ports[idx - 1].device
        print("Invalid selection. Try again.")


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if DEBUG:
            print("request:", self.command, self.path, "from", self.client_address[0])

        if self.path == "/refresh":
            threading.Thread(target=_run_gh_command, daemon=True).start()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            body = b'{"status":"refreshing"}'
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

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
    global SERIAL_PORT
    threading.Thread(target=_run_gh_command, daemon=True).start()
    thread = threading.Thread(target=_poll_loop, daemon=True)
    thread.start()
    if not SERIAL_PORT:
        SERIAL_PORT = _choose_serial_port()
    if SERIAL_PORT:
        print("Serial output on %s @ %d" % (SERIAL_PORT, SERIAL_BAUD))
        serial_thread = threading.Thread(target=_serial_read_loop, daemon=True)
        serial_thread.start()
    print("HTTP server on http://%s:%d/copilot-usage" % (HOST, PORT))
    server = HTTPServer((HOST, PORT), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
