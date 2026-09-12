#!/usr/bin/env python3
"""JSON compacto de saude do host para a telinha ESP32 (CYD 320x240).

Nao e um exporter Prometheus -- e o inverso: le do Prometheus e serve um JSON
pequeno o bastante para um ESP32 sem PSRAM parsear sem sufoco (~600 bytes).
Chaves sao curtas de proposito; o firmware conhece o formato.

O cache existe porque a telinha pode voltar a cada poucos segundos e este host
ja vive em load ~3 com 2 cores -- sem ele, cada refresh viraria 12 queries.
"""
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LISTEN_HOST = os.environ.get("LISTEN_HOST", "0.0.0.0")
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "9881"))
PROM_URL = os.environ.get("PROM_URL", "http://prometheus:9090").rstrip("/")
SCRAPE_TIMEOUT = float(os.environ.get("SCRAPE_TIMEOUT", "8"))
CACHE_TTL = float(os.environ.get("CACHE_TTL", "10"))
# Sem taxa de rede de proposito: o node-exporter desta stack roda na propria
# netns do container (sem network_mode: host), entao node_network_* so reporta
# o eth0 dele -- nunca o trafego real do enp2s0 do host. Expor isso seria
# mostrar zero e parecer bug.
# Mountpoints na ordem em que devem aparecer na tela.
DISK_MOUNTS = [
    item.strip()
    for item in os.environ.get("DISK_MOUNTS", "/,/mnt/backup").split(",")
    if item.strip()
]
# Chip do coretemp; vazio = pega o maior hwmon disponivel.
TEMP_CHIP = os.environ.get("TEMP_CHIP", "platform_coretemp_0").strip()
SPARK_POINTS = int(os.environ.get("SPARK_POINTS", "60"))
SPARK_WINDOW = int(os.environ.get("SPARK_WINDOW", "3600"))

GIB = 1024.0 ** 3


def prom_query(expr: str) -> list[dict]:
    """Instant query. Devolve [] em qualquer falha -- a telinha lida com faltantes."""
    url = f"{PROM_URL}/api/v1/query?{urllib.parse.urlencode({'query': expr})}"
    try:
        with urllib.request.urlopen(url, timeout=SCRAPE_TIMEOUT) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError, json.JSONDecodeError):
        return []
    if payload.get("status") != "success":
        return []
    return payload.get("data", {}).get("result", [])


def scalar(expr: str, default: float | None = None) -> float | None:
    result = prom_query(expr)
    if not result:
        return default
    try:
        value = float(result[0]["value"][1])
    except (KeyError, IndexError, TypeError, ValueError):
        return default
    # NaN vira None: json.dumps emitiria NaN, que nao e JSON valido e quebra o
    # parser do ESP32.
    return default if value != value else value


def prom_range(expr: str, window: int, points: int) -> list[float]:
    end = int(time.time())
    start = end - window
    step = max(1, window // max(1, points))
    params = {"query": expr, "start": start, "end": end, "step": step}
    url = f"{PROM_URL}/api/v1/query_range?{urllib.parse.urlencode(params)}"
    try:
        with urllib.request.urlopen(url, timeout=SCRAPE_TIMEOUT) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError, json.JSONDecodeError):
        return []
    if payload.get("status") != "success":
        return []
    result = payload.get("data", {}).get("result", [])
    if not result:
        return []
    out: list[float] = []
    for _, raw in result[0].get("values", []):
        try:
            value = float(raw)
        except (TypeError, ValueError):
            continue
        if value == value:
            out.append(value)
    return out


def round1(value: float | None) -> float | None:
    return None if value is None else round(value, 1)


def collect() -> dict:
    cpu = scalar(
        'clamp_min(100 - (avg(rate(node_cpu_seconds_total{mode="idle"}[2m])) * 100), 0)'
    )
    cores = scalar('count(node_cpu_seconds_total{mode="idle"})')
    mem_total = scalar("node_memory_MemTotal_bytes")
    mem_avail = scalar("node_memory_MemAvailable_bytes")
    swap_total = scalar("node_memory_SwapTotal_bytes")
    swap_free = scalar("node_memory_SwapFree_bytes")
    uptime = scalar("node_time_seconds - node_boot_time_seconds")

    if TEMP_CHIP:
        temp = scalar(f'max(node_hwmon_temp_celsius{{chip="{TEMP_CHIP}"}})')
    else:
        temp = None
    if temp is None:
        # Fallback: maior sensor hwmon plausivel. O filtro < 120 descarta
        # sensores fantasma que reportam valores absurdos.
        temp = scalar("max(node_hwmon_temp_celsius < 120)")

    load = [
        scalar("node_load1"),
        scalar("node_load5"),
        scalar("node_load15"),
    ]

    disks = []
    for mount in DISK_MOUNTS:
        size = scalar(f'node_filesystem_size_bytes{{mountpoint="{mount}"}}')
        avail = scalar(f'node_filesystem_avail_bytes{{mountpoint="{mount}"}}')
        if size is None or avail is None or size <= 0:
            continue
        disks.append([mount, round1((size - avail) / GIB), round1(size / GIB)])

    spark_raw = prom_range(
        'clamp_min(100 - (avg(rate(node_cpu_seconds_total{mode="idle"}[5m])) * 100), 0)',
        SPARK_WINDOW,
        SPARK_POINTS,
    )
    # Inteiros 0-100: o firmware so precisa da altura da barra, e isso corta o
    # payload em ~4x frente a floats.
    spark = [max(0, min(100, int(round(v)))) for v in spark_raw][-SPARK_POINTS:]

    mem_used = None
    if mem_total is not None and mem_avail is not None:
        mem_used = mem_total - mem_avail
    swap_used = None
    if swap_total is not None and swap_free is not None:
        swap_used = swap_total - swap_free

    return {
        "ok": cpu is not None and mem_total is not None,
        "ts": int(time.time()),
        "up": int(uptime) if uptime is not None else None,
        "cpu": round1(cpu),
        "cores": int(cores) if cores else None,
        "load": [round(v, 2) if v is not None else None for v in load],
        "temp": round1(temp),
        "mem": [
            round1(mem_used / GIB) if mem_used is not None else None,
            round1(mem_total / GIB) if mem_total is not None else None,
        ],
        "swap": [
            round1(swap_used / GIB) if swap_used is not None else None,
            round1(swap_total / GIB) if swap_total is not None else None,
        ],
        "disk": disks,
        "spark": spark,
    }


class Cache:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._payload: dict | None = None
        self._at = 0.0

    def get(self) -> dict:
        with self._lock:
            now = time.monotonic()
            if self._payload is not None and (now - self._at) < CACHE_TTL:
                return self._payload
            try:
                payload = collect()
            except Exception as exc:  # nunca deixa a telinha sem resposta
                payload = {"ok": False, "ts": int(time.time()), "err": str(exc)[:120]}
            self._payload = payload
            self._at = now
            return payload


CACHE = Cache()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "tft-dashboard-api"

    def do_GET(self) -> None:  # noqa: N802
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/status", "/status.json"):
            body = json.dumps(CACHE.get(), separators=(",", ":")).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        elif path == "/healthz":
            body = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
        else:
            body = b"not found\n"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args) -> None:
        return  # silencia o log por request; a telinha bate a cada poucos segundos


def main() -> None:
    server = ThreadingHTTPServer((LISTEN_HOST, LISTEN_PORT), Handler)
    server.daemon_threads = True
    print(f"tft-dashboard-api ouvindo em {LISTEN_HOST}:{LISTEN_PORT}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
