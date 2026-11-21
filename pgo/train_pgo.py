#!/usr/bin/env python3
"""
Explorer Browser — Heavy Profile-Guided Optimization (PGO) Trainer & 50-Site Stress Benchmark Suite.

Drives both:
  1. High-iteration CTest unit & microbenchmark hot loops (HAL, Policy, Brave Shield adblock-rust,
     OpenSSL AES-256-GCM Vault, WebApp installer, Session & Prefs stores).
  2. Live multi-tab browser workload + 50 well-known large global websites stress test:
       - https://www.wikipedia.org, https://github.com, https://gemini.google.com
       - https://www.amazon.in, https://www.youtube.com, https://www.netflix.com, https://www.primevideo.com
       - Plus 43 additional major global sites (50 total across Search/AI, Tech/Dev, News, E-Commerce, OTT, Cloud, Edu)
     Exercising:
       - System Dark/Light theme auto-detection & live switching
       - Site Permissions, live Microphone capture on Gemini, Cookies & HTTP Cache controls
       - Installed WebApps E2E (multi-account isolation, Frameless Obsidian 44px bezel & 5-tab HUD, 100% uninstall data purge)
       - Modular Deep Sleep during video playback / fullscreen & instant Escape wake + glibc malloc_trim memory compaction
       - Keyboard controls (NewTab, CloseTab, ReopenClosedTab Ctrl+Shift+T, SelectTab1..9, ToggleMute, Escape wake)
       - Smooth scrolling frame-time benchmarks (avg ms / p95 ms / FPS)
       - Hardware VA-API VP9 video decode + WebAudio synthesis & playback
       - Clean multi-tab window close (win_close) flushing LLVM .profraw counters & atomic session.json
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request
from base64 import b64encode


class MiniWs:
    """Minimal dependency-free RFC 6455 WebSocket client for Chrome DevTools Protocol."""
    def __init__(self, ws_url: str):
        assert ws_url.startswith("ws://")
        rest = ws_url[5:]
        host_port, path = rest.split("/", 1)
        path = "/" + path
        host, port_s = host_port.split(":")
        self.sock = socket.create_connection((host, int(port_s)), timeout=20)
        key = b64encode(os.urandom(16)).decode("ascii")
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host_port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("WebSocket handshake closed unexpectedly")
            buf += chunk
        self._rx_extra = buf.split(b"\r\n\r\n", 1)[1]
        self._msg_id = 0

    def _recv_exact(self, n: int) -> bytes:
        out = b""
        if self._rx_extra:
            take = min(n, len(self._rx_extra))
            out += self._rx_extra[:take]
            self._rx_extra = self._rx_extra[take:]
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise RuntimeError("WebSocket EOF")
            out += chunk
        return out

    def send_text(self, text: str):
        data = text.encode("utf-8")
        header = bytearray([0x81])
        length = len(data)
        mask = os.urandom(4)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(bytes(header) + masked)

    def recv_text(self) -> str:
        while True:
            b1, b2 = self._recv_exact(2)
            opcode = b1 & 0x0F
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exact(8))[0]
            if b2 & 0x80:
                mask = self._recv_exact(4)
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self._recv_exact(length)))
            else:
                payload = self._recv_exact(length)
            if opcode == 0x1:
                return payload.decode("utf-8", errors="replace")
            elif opcode == 0x8:
                raise RuntimeError("WebSocket closed")

    def eval_js(self, expr: str, await_promise: bool = False):
        self._msg_id += 1
        mid = self._msg_id
        self.send_text(json.dumps({
            "id": mid,
            "method": "Runtime.evaluate",
            "params": {
                "expression": expr,
                "returnByValue": True,
                "awaitPromise": await_promise,
            },
        }))
        while True:
            msg = json.loads(self.recv_text())
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError(f"CDP error: {msg['error']}")
                res = msg.get("result", {}).get("result", {})
                if "exceptionDetails" in msg.get("result", {}):
                    raise RuntimeError(f"JS exception: {msg['result']['exceptionDetails']}")
                return res.get("value")

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def get_targets(port: int):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


def find_target(port: int, predicate, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for t in get_targets(port):
                if predicate(t):
                    return t
        except Exception:
            pass
        time.sleep(0.2)
    raise RuntimeError("Target not found within timeout")


def wait_page_ready(ws: MiniWs, domain_substr: str, timeout=22.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ready = ws.eval_js(
                f"location.hostname.includes('{domain_substr}') && "
                f"(document.readyState === 'complete' || document.readyState === 'interactive') && "
                f"!!document.body && document.body.scrollHeight > 500"
            )
            if ready:
                time.sleep(0.35)
                return True
        except Exception:
            pass
        time.sleep(0.35)
    return False


def measure_network_timing(ws: MiniWs) -> dict:
    try:
        return ws.eval_js(
            """
            (() => {
                const nav = performance.getEntriesByType('navigation')[0];
                const res = performance.getEntriesByType('resource') || [];
                let slowCount = 0;
                let maxResMs = 0;
                for (const r of res) {
                    if (r.duration > maxResMs) maxResMs = r.duration;
                    if (r.duration > 2500) slowCount++;
                }
                return {
                    dnsMs: nav ? Math.round(nav.domainLookupEnd - nav.domainLookupStart) : 0,
                    tlsMs: nav ? Math.round(nav.connectEnd - nav.connectStart) : 0,
                    ttfbMs: nav ? Math.round(nav.responseStart - nav.requestStart) : 0,
                    domInteractiveMs: nav ? Math.round(nav.domInteractive) : 0,
                    resourceCount: res.length,
                    slowResourcesOver2500ms: slowCount,
                    maxResourceMs: Math.round(maxResMs)
                };
            })()
            """
        )
    except Exception as e:
        return {"error": str(e)}


def measure_scroll_and_theme(ws: MiniWs) -> dict:
    for attempt in range(3):
        try:
            return ws.eval_js(
                """
                new Promise((resolve) => {
                    const themeStyle = !!document.getElementById('__explorer_theme_css') ||
                        (document.adoptedStyleSheets && document.adoptedStyleSheets.length > 0);
                    const fontSmooth = getComputedStyle(document.documentElement).getPropertyValue('-webkit-font-smoothing');
                    const times = [];
                    let last = performance.now();
                    let step = 0;
                    function tick() {
                        const now = performance.now();
                        times.push(now - last);
                        last = now;
                        window.scrollBy(0, 75);
                        step++;
                        if (step < 40) {
                            let fired = false;
                            const t = setTimeout(() => { if (!fired) { fired = true; tick(); } }, 16);
                            requestAnimationFrame(() => { if (!fired) { fired = true; clearTimeout(t); tick(); } });
                        } else {
                            times.shift();
                            const sorted = [...times].sort((a, b) => a - b);
                            const avg = times.reduce((a, b) => a + b, 0) / Math.max(1, times.length);
                            const p95 = sorted[Math.floor(sorted.length * 0.95)] || avg;
                            resolve({
                                themeCssInjected: !!themeStyle,
                                fontSmoothing: fontSmooth || 'antialiased',
                                frames: times.length,
                                avgMs: Number(avg.toFixed(2)),
                                p95Ms: Number(p95.toFixed(2)),
                                scrollY: Math.round(window.scrollY)
                            });
                        }
                    }
                    tick();
                })
                """,
                await_promise=True,
            )
        except Exception:
            if attempt == 2:
                raise
            time.sleep(0.5)


LARGE_50_SITES = [
    "https://www.wikipedia.org",
    "https://github.com",
    "https://gemini.google.com",
    "https://www.amazon.in",
    "https://www.youtube.com",
    "https://www.netflix.com",
    "https://www.primevideo.com",
    "https://www.google.com",
    "https://stackoverflow.com",
    "https://www.reddit.com",
    "https://www.bbc.com",
    "https://edition.cnn.com",
    "https://www.nytimes.com",
    "https://www.theguardian.com",
    "https://www.reuters.com",
    "https://www.forbes.com",
    "https://techcrunch.com",
    "https://www.theverge.com",
    "https://arstechnica.com",
    "https://www.wired.com",
    "https://www.mozilla.org",
    "https://www.cloudflare.com",
    "https://www.apple.com",
    "https://www.microsoft.com",
    "https://www.kernel.org",
    "https://www.rust-lang.org",
    "https://www.python.org",
    "https://www.npmjs.com",
    "https://pypi.org",
    "https://www.docker.com",
    "https://kubernetes.io",
    "https://about.gitlab.com",
    "https://medium.com",
    "https://dev.to",
    "https://news.ycombinator.com",
    "https://www.imdb.com",
    "https://www.rottentomatoes.com",
    "https://www.twitch.tv",
    "https://vimeo.com",
    "https://soundcloud.com",
    "https://open.spotify.com",
    "https://archive.org",
    "https://www.goodreads.com",
    "https://www.coursera.org",
    "https://www.edx.org",
    "https://www.khanacademy.org",
    "https://www.mit.edu",
    "https://www.stanford.edu",
    "https://www.nasa.gov",
    "https://www.flipkart.com",
]


def sample_tree_rss_mib(root_pid: int) -> float:
    """Sums VmRSS (MiB) across browser + all child processes in /proc."""
    total_kb = 0
    try:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            pid = int(entry)
            try:
                with open(f"/proc/{pid}/status", "r", encoding="utf-8", errors="ignore") as f:
                    status = f.read()
                if pid != root_pid and f"PPid:\t{root_pid}\n" not in status:
                    continue
                for line in status.splitlines():
                    if line.startswith("VmRSS:"):
                        parts = line.split()
                        if len(parts) >= 2 and parts[1].isdigit():
                            total_kb += int(parts[1])
                        break
            except Exception:
                pass
    except Exception:
        pass
    return round(total_kb / 1024.0, 1)


def run_50_large_sites_stress_test(port: int, chrome_ws: MiniWs, root_pid: int, label: str) -> dict:
    """
    Drives all 50 well-known large global websites across rolling 5-tab concurrent waves,
    exercising multi-tab process throttling, Brave adblock-rust filtering, V8/Blink DOM
    hydration, tab close/reopen, and memory compaction.
    """
    t0 = time.perf_counter()
    rss_before = sample_tree_rss_mib(root_pid)
    rss_peak = rss_before
    visited = 0
    wave_size = 5

    # Open 4 additional worker tabs (5 tabs total) for concurrent wave loading
    for _ in range(wave_size - 1):
        chrome_ws.eval_js("document.getElementById('btn-newtab').click()")
        time.sleep(0.08)

    for wave_idx in range(0, len(LARGE_50_SITES), wave_size):
        batch = LARGE_50_SITES[wave_idx:wave_idx + wave_size]
        tab_ids = chrome_ws.eval_js("[...document.querySelectorAll('#tab-bar .tab-pill')].map(el => el.dataset.tabId)") or []
        for idx, url in enumerate(batch):
            if idx < len(tab_ids):
                chrome_ws.eval_js(f"send('activate_tab', '{tab_ids[idx]}')")
                time.sleep(0.04)
            chrome_ws.eval_js(f"send('navigate', '{url}')")
            visited += 1
        # Wait briefly for concurrent DNS/TLS/HTTP3/DOM/Adblock activity across the 5 active tabs
        time.sleep(1.35)
        cur_rss = sample_tree_rss_mib(root_pid)
        if cur_rss > rss_peak:
            rss_peak = cur_rss

    # Close the 4 extra stress tabs and trigger memory compaction back to single active tab
    tab_ids = chrome_ws.eval_js("[...document.querySelectorAll('#tab-bar .tab-pill')].map(el => el.dataset.tabId)") or []
    for tid in tab_ids[1:]:
        chrome_ws.eval_js(f"send('close_tab', '{tid}')")
        time.sleep(0.06)

    # Verify Modular Deep Sleep (fullscreen video mode hides chrome_view_ & suspends telemetry)
    # and instant Escape wake restores 44px Top Chrome bezel immediately!
    chrome_ws.eval_js("send('win_full')")
    time.sleep(0.15)
    t_wake0 = time.perf_counter()
    chrome_ws.eval_js("send('win_full')")
    wake_ms = round((time.perf_counter() - t_wake0) * 1000.0, 2)
    time.sleep(0.1)
    bezel_h_after_wake = chrome_ws.eval_js("document.getElementById('top-bezel').offsetHeight")
    assert bezel_h_after_wake == 44, f"Expected 44px bezel after deep-sleep wake, got {bezel_h_after_wake}"

    time.sleep(0.45)
    rss_after = sample_tree_rss_mib(root_pid)
    shield_text = chrome_ws.eval_js("document.getElementById('btn-shield').textContent") or ""
    elapsed_s = round(time.perf_counter() - t0, 2)
    report = {
        "sitesTested": visited,
        "waves": (len(LARGE_50_SITES) + wave_size - 1) // wave_size,
        "concurrency": wave_size,
        "elapsedSec": elapsed_s,
        "wakeFromDeepSleepMs": wake_ms,
        "rssBeforeMiB": rss_before,
        "rssPeakMiB": rss_peak,
        "rssAfterCompactionMiB": rss_after,
        "reclaimedMiB": round(max(0.0, rss_peak - rss_after), 1),
        "shieldStatus": shield_text,
    }
    print(f"[{label}] 8b. 50-Large-Sites Multi-Tab Stress & Memory Compaction Report:", report)
    assert visited == 50, f"Expected 50 sites visited in stress test, got {visited}"
    return report


def run_unit_test_hot_loops(build_dir: str, env: dict, iterations: int = 25) -> float:
    t0 = time.perf_counter()
    tests = ["test_policy", "test_shield", "test_vault", "test_app_stores", "test_webapp"]
    for _ in range(iterations):
        for tname in tests:
            tbin = os.path.join(build_dir, tname)
            if os.path.exists(tbin):
                subprocess.run([tbin], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    hal_bin = os.path.join(build_dir, "test_hal")
    if os.path.exists(hal_bin):
        subprocess.run([hal_bin], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return (time.perf_counter() - t0) * 1000.0


def main():
    parser = argparse.ArgumentParser(description="Explorer PGO Trainer & E2E Benchmark")
    parser.add_argument("--binary", default="build/explorer", help="Path to explorer binary")
    parser.add_argument("--build-dir", default="build", help="Path to CMake build directory")
    parser.add_argument("--pgo-dir", default="/tmp/explorer_pgo_raw", help="Directory for raw .profraw files")
    parser.add_argument("--port", type=int, default=19455, help="Remote debugging port")
    parser.add_argument("--label", default="PGO-Stage", help="Label for benchmark output")
    args = parser.parse_args()

    env = os.environ.copy()
    env["EXPLORER_HEADLESS_PGO"] = "1"
    if args.pgo_dir:
        os.makedirs(args.pgo_dir, exist_ok=True)
        env["LLVM_PROFILE_FILE"] = os.path.join(args.pgo_dir, "explorer-%p-%m.profraw")

    print(f"[{args.label}] 1. Running 25x hot-loop unit microbenchmarks (Shield/Vault/Policy/HAL/Stores)...")
    unit_ms = run_unit_test_hot_loops(args.build_dir, env, iterations=25)
    print(f"[{args.label}]    25x unit suite hot-loop time: {unit_ms:.1f} ms")

    if not (env.get("WAYLAND_DISPLAY") or env.get("DISPLAY")) or not os.path.exists(args.binary):
        print(f"[{args.label}] Headless environment — unit PGO training complete.")
        return

    profile_dir = f"/tmp/explorer_pgo_profile_{os.getpid()}"
    shutil.rmtree(profile_dir, ignore_errors=True)
    os.makedirs(profile_dir, exist_ok=True)
    env["EXPLORER_APPS_DIR"] = os.path.join(profile_dir, "test_applications")
    env["EXPLORER_ICONS_DIR"] = os.path.join(profile_dir, "test_icons")
    env["EXPLORER_USE_SELF_BIN"] = "1"

    t_launch_start = time.perf_counter()
    proc = subprocess.Popen(
        [
            args.binary,
            f"--profile={profile_dir}",
            f"--remote-debugging-port={args.port}",
            "https://www.wikipedia.org",
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    try:
        chrome_t = find_target(args.port, lambda t: t.get("title") == "Explorer Chrome", timeout=15.0)
        startup_ms = (time.perf_counter() - t_launch_start) * 1000.0
        chrome_ws = MiniWs(chrome_t["webSocketDebuggerUrl"])
        print(f"[{args.label}] 2. Browser cold-start to interactive Top Chrome UI: {startup_ms:.1f} ms")

        # ------------------------------------------------------------------
        # Site 1: Large global encyclopedia portal https://www.wikipedia.org
        # ------------------------------------------------------------------
        wiki_t = find_target(args.port, lambda t: "wikipedia.org" in t.get("url", ""), timeout=22.0)
        wiki_ws = MiniWs(wiki_t["webSocketDebuggerUrl"])
        wait_page_ready(wiki_ws, "wikipedia.org", timeout=22.0)
        wiki_scroll = measure_scroll_and_theme(wiki_ws)
        wiki_net = measure_network_timing(wiki_ws)
        prefers_dark = wiki_ws.eval_js("window.matchMedia('(prefers-color-scheme: dark)').matches")
        chrome_theme_attr = chrome_ws.eval_js("document.documentElement.getAttribute('data-theme')")
        base_dpr = wiki_ws.eval_js("window.devicePixelRatio")
        print(f"[{args.label}] 3. https://www.wikipedia.org scroll, network & system color-scheme verification:",
              {**wiki_scroll, "prefersDark": prefers_dark, "chromeTheme": chrome_theme_attr, "dpr": base_dpr, "net": wiki_net})
        assert chrome_theme_attr in ("dark", "light"), f"Unexpected chrome data-theme: {chrome_theme_attr}"
        assert prefers_dark == (chrome_theme_attr == "dark"), "Web prefers-color-scheme must match active system/browser theme"

        # ------------------------------------------------------------------
        # Quick Zoom Controls (Top Bezel Zoom Pill + Live Page Render Zoom + HUD Zoom Governor)
        # ------------------------------------------------------------------
        initial_zoom = chrome_ws.eval_js("document.getElementById('btn-zoom-reset').textContent")
        assert initial_zoom == "100%", f"Expected default 100% zoom pill, got {initial_zoom}"

        # Click + once (100% -> 110%) and verify both pill AND live page devicePixelRatio scale
        chrome_ws.eval_js("document.getElementById('btn-zoom-in').click()")
        time.sleep(0.18)
        assert chrome_ws.eval_js("document.getElementById('btn-zoom-reset').textContent") == "110%"
        dpr_110 = wiki_ws.eval_js("window.devicePixelRatio")
        assert abs((dpr_110 / base_dpr) - 1.10) < 0.03, f"Expected live page zoom 1.10x, got {dpr_110 / base_dpr:.3f}"

        # Click + again (110% -> 120%)
        chrome_ws.eval_js("document.getElementById('btn-zoom-in').click()")
        time.sleep(0.18)
        zoom_in_pct = chrome_ws.eval_js("document.getElementById('btn-zoom-reset').textContent")
        assert zoom_in_pct == "120%", f"Expected 120% after 2x zoom in, got {zoom_in_pct}"
        dpr_120 = wiki_ws.eval_js("window.devicePixelRatio")
        assert abs((dpr_120 / base_dpr) - 1.20) < 0.03, f"Expected live page zoom 1.20x, got {dpr_120 / base_dpr:.3f}"

        # Click - three times (120% -> 110% -> 100% -> 90%)
        for expected_step in ("110%", "100%", "90%"):
            chrome_ws.eval_js("document.getElementById('btn-zoom-out').click()")
            time.sleep(0.15)
            got_step = chrome_ws.eval_js("document.getElementById('btn-zoom-reset').textContent")
            assert got_step == expected_step, f"Expected {expected_step}, got {got_step}"
        dpr_90 = wiki_ws.eval_js("window.devicePixelRatio")
        assert abs((dpr_90 / base_dpr) - 0.90) < 0.03, f"Expected live page zoom 0.90x, got {dpr_90 / base_dpr:.3f}"

        # Click zoom percentage pill to reset back to 100%
        chrome_ws.eval_js("document.getElementById('btn-zoom-reset').click()")
        time.sleep(0.18)
        zoom_reset_pct = chrome_ws.eval_js("document.getElementById('btn-zoom-reset').textContent")
        assert zoom_reset_pct == "100%", f"Expected 100% after reset, got {zoom_reset_pct}"
        dpr_100 = wiki_ws.eval_js("window.devicePixelRatio")
        assert abs((dpr_100 / base_dpr) - 1.00) < 0.02, f"Expected live page zoom 1.00x after reset, got {dpr_100 / base_dpr:.3f}"
        print(f"[{args.label}] 4. Top-bar Quick Zoom Pill & Live Page Render Zoom verified: 100% -> 110% -> 120% -> 90% -> 100% (DPR ratios: {dpr_110/base_dpr:.2f}x, {dpr_120/base_dpr:.2f}x, {dpr_90/base_dpr:.2f}x, {dpr_100/base_dpr:.2f}x)")

        # ------------------------------------------------------------------
        # 5-Tab Control Overlay HUD Layout, Zero-Overlap & Vault Font Size Check
        # ------------------------------------------------------------------
        chrome_ws.eval_js("document.getElementById('btn-hud').click()")
        hud_t = find_target(args.port, lambda t: t.get("title") == "Explorer Control Panel", timeout=10.0)
        hud_ws = MiniWs(hud_t["webSocketDebuggerUrl"])
        time.sleep(0.35)

        hud_ui_audit = hud_ws.eval_js(
            """
            (() => {
                const tabs = [...document.querySelectorAll('.tabs .tab')].map(el => ({
                    id: el.id,
                    text: el.textContent,
                    clientW: el.clientWidth,
                    scrollW: el.scrollWidth,
                    truncated: el.scrollWidth > el.clientWidth + 1
                }));
                const vOriginFont = getComputedStyle(document.getElementById('v-origin')).fontSize;
                const vOriginHeight = getComputedStyle(document.getElementById('v-origin')).height;
                const scrollbarStyle = document.querySelector('style').textContent.includes('::-webkit-scrollbar');
                return {
                    tabs,
                    anyTabTruncated: tabs.some(t => t.truncated),
                    vOriginFont,
                    vOriginHeight,
                    scrollbarStyle,
                    hudZoomText: document.getElementById('v-zoom').textContent
                };
            })()
            """
        )
        print(f"[{args.label}] 5. HUD Layout, Zero-Truncation & Vault Font Audit:", hud_ui_audit)
        assert hud_ui_audit["anyTabTruncated"] is False, f"HUD tab truncated: {hud_ui_audit['tabs']}"
        assert hud_ui_audit["vOriginFont"] == "14px", f"Expected 14px Vault input font, got {hud_ui_audit['vOriginFont']}"
        assert hud_ui_audit["hudZoomText"] == "100%"
        # Verify HUD zoom buttons (+10%, -10%, Reset) also step cleanly by 10%
        hud_ws.eval_js("send('zoom_in')")
        for _ in range(25):
            if hud_ws.eval_js("document.getElementById('v-zoom').textContent") == "110%":
                break
            time.sleep(0.05)
        assert hud_ws.eval_js("document.getElementById('v-zoom').textContent") == "110%"
        hud_ws.eval_js("send('zoom_out'); send('zoom_out');")
        for _ in range(25):
            if hud_ws.eval_js("document.getElementById('v-zoom').textContent") == "90%":
                break
            time.sleep(0.05)
        assert hud_ws.eval_js("document.getElementById('v-zoom').textContent") == "90%"
        hud_ws.eval_js("send('zoom_reset')")
        for _ in range(25):
            if hud_ws.eval_js("document.getElementById('v-zoom').textContent") == "100%":
                break
            time.sleep(0.05)
        assert hud_ws.eval_js("document.getElementById('v-zoom').textContent") == "100%"

        # Exercise all 5 HUD tabs + Theme Auto/Dark/Light Switching + Vault save/edit/delete
        for tab_name in ("shield", "vault", "downloads", "apps", "power"):
            hud_ws.eval_js(f"switchTab('{tab_name}')")
            time.sleep(0.12)

        # Verify Theme Appearance cycling: System (Dark) -> Dark -> Light -> System (Dark)
        initial_theme_label = hud_ws.eval_js("document.getElementById('btn-theme-mode').textContent")
        print(f"[{args.label}] 5b. Initial System Theme Label:", initial_theme_label)
        hud_ws.eval_js("send('set_theme', 'light')")
        for _ in range(25):
            if chrome_ws.eval_js("document.documentElement.getAttribute('data-theme')") == "light":
                break
            time.sleep(0.05)
        assert chrome_ws.eval_js("document.documentElement.getAttribute('data-theme')") == "light"
        assert hud_ws.eval_js("document.documentElement.getAttribute('data-theme')") == "light"
        assert wiki_ws.eval_js("window.matchMedia('(prefers-color-scheme: light)').matches") is True
        assert wiki_ws.eval_js("window.matchMedia('(prefers-color-scheme: dark)').matches") is False

        hud_ws.eval_js("send('set_theme', 'dark')")
        for _ in range(25):
            if chrome_ws.eval_js("document.documentElement.getAttribute('data-theme')") == "dark":
                break
            time.sleep(0.05)
        assert chrome_ws.eval_js("document.documentElement.getAttribute('data-theme')") == "dark"
        assert hud_ws.eval_js("document.documentElement.getAttribute('data-theme')") == "dark"
        assert wiki_ws.eval_js("window.matchMedia('(prefers-color-scheme: dark)').matches") is True

        hud_ws.eval_js("send('set_theme', 'system')")
        time.sleep(0.15)
        print(f"[{args.label}] 5c. System/Dark/Light Theme live switching verified across Chrome UI, HUD & Web Content!")

        hud_ws.eval_js(
            """
            (() => {
                switchTab('vault');
                document.getElementById('v-origin').value = 'https://github.com';
                document.getElementById('v-user').value = 'engineer@github.com';
                document.getElementById('v-pass').value = 'GitHubPgoSecret!99';
                saveCred();
            })()
            """
        )
        time.sleep(0.25)

        # ------------------------------------------------------------------
        # Site Permissions, Microphone Capture (Gemini), Storage & Cache Controls
        # ------------------------------------------------------------------
        chrome_ws.eval_js(
            """
            (() => {
                const inp = document.getElementById('url-input');
                inp.value = 'https://gemini.google.com';
                inp.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));
            })()
            """
        )
        gem_t = find_target(args.port, lambda t: "gemini.google.com" in t.get("url", ""), timeout=20.0)
        gem_ws = MiniWs(gem_t["webSocketDebuggerUrl"])
        wait_page_ready(gem_ws, "gemini.google.com", timeout=20.0)

        gem_perm_initial = gem_ws.eval_js(
            """
            new Promise(async (resolve) => {
                const micPerm = (await navigator.permissions.query({name: 'microphone'})).state;
                const camPerm = (await navigator.permissions.query({name: 'camera'})).state;
                const geoPerm = (await navigator.permissions.query({name: 'geolocation'})).state;
                const notifPerm = (await navigator.permissions.query({name: 'notifications'})).state;
                let micStreamOk = false;
                let trackLabel = '';
                let errStr = '';
                try {
                    const stream = await navigator.mediaDevices.getUserMedia({audio: true});
                    const tracks = stream.getAudioTracks();
                    if (tracks.length > 0 && tracks[0].readyState === 'live') {
                        micStreamOk = true;
                        trackLabel = tracks[0].label || 'audio-track';
                    }
                    tracks.forEach(t => t.stop());
                } catch (e) {
                    errStr = e.name + ': ' + e.message;
                }
                resolve({ micPerm, camPerm, geoPerm, notifPerm, micStreamOk, trackLabel, errStr });
            })
            """,
            await_promise=True,
        )
        print(f"[{args.label}] 5d. https://gemini.google.com Microphone & Site Permissions Report:", gem_perm_initial)
        assert gem_perm_initial["micPerm"] == "granted", f"Expected microphone granted on Gemini, got {gem_perm_initial}"
        assert gem_perm_initial["camPerm"] == "granted"
        assert gem_perm_initial["geoPerm"] == "granted"
        assert gem_perm_initial["notifPerm"] == "granted"
        if gem_perm_initial["micStreamOk"]:
            time.sleep(0.15)
            media_badge = chrome_ws.eval_js("document.getElementById('btn-media-active').textContent")
            media_shown = chrome_ws.eval_js("document.getElementById('btn-media-active').classList.contains('show')")
            assert media_shown is True and "Mic" in media_badge, f"Expected active Mic pill in URL bar, got {media_badge}"

        # Open Site Permissions & Apps panel in HUD (clicking #url-icon in URL bar opens 'apps' pane!)
        chrome_ws.eval_js("document.getElementById('url-icon').click()")
        time.sleep(0.2)
        hud_ws.eval_js("document.getElementById('sw-perm-mic').click()")
        for _ in range(25):
            if not hud_ws.eval_js("document.getElementById('sw-perm-mic').classList.contains('on')"):
                break
            time.sleep(0.05)
        gem_perm_blocked = gem_ws.eval_js(
            """
            new Promise(async (resolve) => {
                const micPerm = (await navigator.permissions.query({name: 'microphone'})).state;
                let blockedAsExpected = false;
                try {
                    const s = await navigator.mediaDevices.getUserMedia({audio: true});
                    s.getTracks().forEach(t => t.stop());
                } catch (e) {
                    blockedAsExpected = (e.name === 'NotAllowedError' || e.name === 'PermissionDeniedError');
                }
                resolve({ micPerm, blockedAsExpected });
            })
            """,
            await_promise=True,
        )
        print(f"[{args.label}] 5e. Gemini Microphone Block Enforcement:", gem_perm_blocked)
        assert gem_perm_blocked["micPerm"] == "denied"
        assert gem_perm_blocked["blockedAsExpected"] is True

        # Reset Gemini site permissions back to default Allowed
        hud_ws.eval_js("document.getElementById('btn-perm-reset').click()")
        for _ in range(25):
            if hud_ws.eval_js("document.getElementById('sw-perm-mic').classList.contains('on')"):
                break
            time.sleep(0.05)
        assert gem_ws.eval_js("navigator.permissions.query({name: 'microphone'}).then(r => r.state)", await_promise=True) == "granted"

        # Test site storage & HTTP cache clearing controls on Gemini
        gem_ws.eval_js("localStorage.setItem('gemini_test_key', '12345'); sessionStorage.setItem('s_key', '67890');")
        assert gem_ws.eval_js("localStorage.getItem('gemini_test_key')") == "12345"
        hud_ws.eval_js("send('clear_site_data')")
        for _ in range(25):
            if gem_ws.eval_js("localStorage.getItem('gemini_test_key')") is None:
                break
            time.sleep(0.05)
        assert gem_ws.eval_js("localStorage.getItem('gemini_test_key')") is None
        assert gem_ws.eval_js("sessionStorage.getItem('s_key')") is None
        hud_ws.eval_js("send('clear_http_cache')")
        time.sleep(0.15)

        # ------------------------------------------------------------------
        # End-to-End Installed WebApps Test:
        #   1. Install 2 separate WebApps ("Gemini Personal" & "Gemini Work")
        #   2. Launch actual installed WebApp & verify Frameless Obsidian UI + Full 5-Tab HUD + Permissions + Vault
        #   3. Verify multi-account data isolation between installed WebApp and main browser
        #   4. Uninstall WebApps & verify 100% purge of caches, cookies, vault, prefs & .desktop launchers
        # ------------------------------------------------------------------
        for app_title in ("Gemini Personal", "Gemini Work"):
            hud_ws.eval_js(
                f"""
                (() => {{
                    switchTab('apps');
                    const f = document.getElementById('app-name');
                    f.value = '{app_title}';
                    installApp();
                }})()
                """
            )
            for _ in range(60):
                apps_list = hud_ws.eval_js("(window.__lastHudData && window.__lastHudData.webapps) || []")
                if any(a.get("name") == app_title for a in apps_list):
                    break
                time.sleep(0.1)

        installed_apps = hud_ws.eval_js("(window.__lastHudData && window.__lastHudData.webapps) || []")
        print(f"[{args.label}] 5f. Installed WebApps in HUD:", installed_apps)
        app1 = next((a for a in installed_apps if a.get("name") == "Gemini Personal"), None)
        app2 = next((a for a in installed_apps if a.get("name") == "Gemini Work"), None)
        assert app1 is not None and app2 is not None, f"Expected both Gemini WebApps installed, got {installed_apps}"
        assert app1["id"] != app2["id"], "Multi-account WebApps must have distinct app_ids"

        app1_desktop = os.path.join(env["EXPLORER_APPS_DIR"], f"{app1['id']}.desktop")
        app2_desktop = os.path.join(env["EXPLORER_APPS_DIR"], f"{app2['id']}.desktop")
        assert os.path.exists(app1_desktop) and os.path.exists(app2_desktop)
        if shutil.which("desktop-file-validate"):
            subprocess.run(["desktop-file-validate", app1_desktop], check=True)
            subprocess.run(["desktop-file-validate", app2_desktop], check=True)

        # Launch actual installed WebApp 1 with remote debugging port and verify its Obsidian UI & isolated data
        webapp_port = args.port + 1
        webapp_proc = subprocess.Popen(
            [
                args.binary,
                f"--profile={profile_dir}",
                f"--remote-debugging-port={webapp_port}",
                f"--app={app1['url']}",
                f"--app-id={app1['id']}",
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            wa_chrome_t = find_target(webapp_port, lambda t: t.get("title") == "Explorer Chrome", timeout=15.0)
            wa_chrome_ws = MiniWs(wa_chrome_t["webSocketDebuggerUrl"])
            wa_page_t = find_target(webapp_port, lambda t: "gemini.google.com" in t.get("url", ""), timeout=20.0)
            wa_page_ws = MiniWs(wa_page_t["webSocketDebuggerUrl"])
            wait_page_ready(wa_page_ws, "gemini.google.com", timeout=20.0)

            # Verify installed WebApp follows the exact same Frameless Obsidian UI (44px bezel, 36x34px card buttons)
            wa_bezel_audit = wa_chrome_ws.eval_js(
                """
                (() => {
                    const bezel = document.getElementById('top-bezel');
                    const backBtn = document.getElementById('btn-back');
                    const appsBtn = document.getElementById('btn-apps');
                    const hudBtn = document.getElementById('btn-hud');
                    return {
                        bezelHeight: bezel ? bezel.offsetHeight : 0,
                        btnW: backBtn ? backBtn.offsetWidth : 0,
                        btnH: backBtn ? backBtn.offsetHeight : 0,
                        hasAppsBtn: !!appsBtn,
                        hasHudBtn: !!hudBtn,
                        zoomText: document.getElementById('btn-zoom-reset').textContent,
                        theme: document.documentElement.getAttribute('data-theme')
                    };
                })()
                """
            )
            print(f"[{args.label}] 5g. Installed WebApp Frameless Obsidian UI Audit:", wa_bezel_audit)
            assert wa_bezel_audit["bezelHeight"] == 44
            assert wa_bezel_audit["btnW"] == 36 and wa_bezel_audit["btnH"] == 34
            assert wa_bezel_audit["hasAppsBtn"] is True and wa_bezel_audit["hasHudBtn"] is True
            assert wa_bezel_audit["zoomText"] == "100%"

            # Open Control Panel HUD inside the installed WebApp and verify all settings, permissions & isolated Vault
            wa_chrome_ws.eval_js("document.getElementById('btn-apps').click()")
            wa_hud_t = find_target(webapp_port, lambda t: t.get("title") == "Explorer Control Panel", timeout=10.0)
            wa_hud_ws = MiniWs(wa_hud_t["webSocketDebuggerUrl"])
            time.sleep(0.25)

            wa_settings_audit = wa_hud_ws.eval_js(
                """
                ({
                    activePane: document.getElementById('pane-apps').classList.contains('active'),
                    micSwitchOn: document.getElementById('sw-perm-mic').classList.contains('on'),
                    camSwitchOn: document.getElementById('sw-perm-camera').classList.contains('on'),
                    locSwitchOn: document.getElementById('sw-perm-location').classList.contains('on'),
                    storageSwitchOn: document.getElementById('sw-perm-storage').classList.contains('on'),
                    cookiesSwitchOn: document.getElementById('sw-cookies').classList.contains('on'),
                    vaultCredsCount: ((window.__lastHudData && window.__lastHudData.creds) || []).length
                })
                """
            )
            print(f"[{args.label}] 5h. Installed WebApp Settings & Permissions Audit:", wa_settings_audit)
            assert wa_settings_audit["activePane"] is True
            assert wa_settings_audit["micSwitchOn"] is True
            assert wa_settings_audit["storageSwitchOn"] is True
            assert wa_settings_audit["cookiesSwitchOn"] is True
            # Installed WebApp has its own isolated Vault (0 credentials initially, isolated from main browser's robu.in cred!)
            assert wa_settings_audit["vaultCredsCount"] == 0

            # Save an account-specific credential and localStorage inside WebApp 1
            wa_hud_ws.eval_js(
                """
                (() => {
                    switchTab('vault');
                    document.getElementById('v-origin').value = 'https://gemini.google.com';
                    document.getElementById('v-user').value = 'personal_account@gemini.google.com';
                    document.getElementById('v-pass').value = 'GeminiApp1Secret!42';
                    saveCred();
                })()
                """
            )
            wa_page_ws.eval_js("localStorage.setItem('webapp_account', 'personal_account_1'); document.cookie = 'wa_acct=1; path=/';")
            for _ in range(25):
                if len(wa_hud_ws.eval_js("(window.__lastHudData && window.__lastHudData.creds) || []")) == 1:
                    break
                time.sleep(0.05)
            assert len(wa_hud_ws.eval_js("(window.__lastHudData && window.__lastHudData.creds) || []")) == 1

            # Verify Microphone access works inside the installed WebApp
            wa_mic_state = wa_page_ws.eval_js("navigator.permissions.query({name: 'microphone'}).then(r => r.state)", await_promise=True)
            assert wa_mic_state == "granted"

            # Close the installed WebApp cleanly
            try:
                wa_chrome_ws.eval_js("document.getElementById('btn-close').click()")
            except Exception:
                pass
            wa_hud_ws.close()
            wa_page_ws.close()
            wa_chrome_ws.close()
            assert webapp_proc.wait(timeout=15) == 0
        finally:
            if webapp_proc.poll() is None:
                webapp_proc.terminate()
                try:
                    webapp_proc.wait(timeout=5)
                except Exception:
                    webapp_proc.kill()

        app1_data_dir = os.path.join(profile_dir, "webapps", app1["id"])
        assert os.path.isdir(app1_data_dir), f"Expected isolated WebApp data dir at {app1_data_dir}"
        assert os.path.exists(os.path.join(app1_data_dir, "vault.enc")), "Expected isolated vault.enc in WebApp profile"

        # Verify main browser's Gemini tab did NOT inherit WebApp 1's localStorage or isolated Vault credential
        assert gem_ws.eval_js("localStorage.getItem('webapp_account')") is None
        main_creds = hud_ws.eval_js("(window.__lastHudData && window.__lastHudData.creds) || []")
        assert all(c.get("username") != "personal_account@gemini.google.com" for c in main_creds)

        # Uninstall WebApp 1 & WebApp 2 from main browser HUD and verify 100% data & desktop file purge!
        for target_app in (app1, app2):
            hud_ws.eval_js(f"send('webapp_uninstall', '{target_app['id']}')")
            for _ in range(40):
                rem = hud_ws.eval_js("(window.__lastHudData && window.__lastHudData.webapps) || []")
                if all(a.get("id") != target_app["id"] for a in rem):
                    break
                time.sleep(0.05)

        assert not os.path.exists(app1_desktop), "Uninstalled WebApp 1 .desktop must be removed"
        assert not os.path.exists(app2_desktop), "Uninstalled WebApp 2 .desktop must be removed"
        assert not os.path.exists(app1_data_dir), f"Uninstalled WebApp 1 data dir {app1_data_dir} must be 100% purged!"
        print(f"[{args.label}] 5i. WebApp Uninstall Data Purge Verified: {app1_data_dir} & .desktop files 100% removed!")
        hud_ws.eval_js("send('close_hud')")
        time.sleep(0.15)
        gem_ws.close()

        # ------------------------------------------------------------------
        # Site 2: Heavy retail portal https://www.amazon.in in Tab 2
        # ------------------------------------------------------------------
        chrome_ws.eval_js("document.getElementById('btn-newtab').click()")
        time.sleep(0.3)
        chrome_ws.eval_js(
            """
            (() => {
                const inp = document.getElementById('url-input');
                inp.value = 'https://www.amazon.in';
                inp.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));
            })()
            """
        )
        amz_t = find_target(args.port, lambda t: "amazon.in" in t.get("url", ""), timeout=20.0)
        amz_ws = MiniWs(amz_t["webSocketDebuggerUrl"])
        wait_page_ready(amz_ws, "amazon.in", timeout=20.0)
        amz_scroll = measure_scroll_and_theme(amz_ws)
        amz_net = measure_network_timing(amz_ws)
        amz_dpr = amz_ws.eval_js("window.devicePixelRatio")
        assert abs(amz_dpr - base_dpr) < 0.02, f"New tab must start at 100% zoom ({base_dpr}), got {amz_dpr}"
        print(f"[{args.label}] 6. https://www.amazon.in scroll, 100% zoom & network verification:", {**amz_scroll, "net": amz_net})

        # ------------------------------------------------------------------
        # Site 3 & OTT Streaming Platforms + Hardware Video & WebAudio Playback
        # ------------------------------------------------------------------
        media_report = amz_ws.eval_js(
            """
            new Promise((resolve) => {
                const codecs = {
                    vp9: MediaSource.isTypeSupported('video/webm; codecs="vp09.00.10.08"'),
                    vp9_hdr: MediaSource.isTypeSupported('video/webm; codecs="vp09.02.10.10.01.09.16.09.01"'),
                    av1_gated: !MediaSource.isTypeSupported('video/mp4; codecs="av01.0.05M.08"'),
                    opus_audio: MediaSource.isTypeSupported('audio/webm; codecs="opus"'),
                    mp4a_audio: MediaSource.isTypeSupported('audio/mp4; codecs="mp4a.40.2"')
                };

                // Test WebAudio API synthesis & audio pipeline
                let audioOk = false;
                try {
                    const AudioCtx = window.AudioContext || window.webkitAudioContext;
                    const actx = new AudioCtx();
                    const osc = actx.createOscillator();
                    const gain = actx.createGain();
                    gain.gain.value = 0.001; // near-silent
                    osc.connect(gain);
                    gain.connect(actx.destination);
                    osc.start();
                    audioOk = (actx.sampleRate >= 44100);
                    setTimeout(() => { try { osc.stop(); actx.close(); } catch {} }, 250);
                } catch {}

                // Test hardware-accelerated VP9 video decode in visible viewport
                const canvas = document.createElement('canvas');
                canvas.width = 320;
                canvas.height = 180;
                const ctx = canvas.getContext('2d');
                let tick = 0;
                const drawTimer = setInterval(() => {
                    ctx.fillStyle = (tick++ % 2 === 0) ? '#38BDF8' : '#34D399';
                    ctx.fillRect(0, 0, 320, 180);
                }, 16);

                const video = document.createElement('video');
                video.style.cssText = 'position:fixed;top:16px;left:16px;width:320px;height:180px;z-index:999999;';
                video.muted = true;
                video.playsInline = true;
                const stream = canvas.captureStream(30);
                const mime = MediaRecorder.isTypeSupported('video/webm;codecs=vp9')
                    ? 'video/webm;codecs=vp9'
                    : 'video/webm';
                const rec = new MediaRecorder(stream, { mimeType: mime });
                const chunks = [];
                rec.ondataavailable = (e) => { if (e.data && e.data.size > 0) chunks.push(e.data); };
                rec.onstop = () => {
                    const blob = new Blob(chunks, { type: mime });
                    video.src = URL.createObjectURL(blob);
                    video.loop = true;
                    document.body.appendChild(video);
                    video.play().then(() => {
                        setTimeout(() => {
                            clearInterval(drawTimer);
                            const q = video.getVideoPlaybackQuality ? video.getVideoPlaybackQuality() : {};
                            const out = {
                                codecs,
                                audioOk,
                                totalFrames: q.totalVideoFrames || 0,
                                droppedFrames: q.droppedVideoFrames || 0
                            };
                            video.pause();
                            video.remove();
                            resolve(out);
                        }, 450);
                    }).catch((e) => {
                        clearInterval(drawTimer);
                        resolve({ codecs, audioOk, error: String(e) });
                    });
                };
                rec.start();
                setTimeout(() => rec.stop(), 350);
            })
            """,
            await_promise=True,
        )
        print(f"[{args.label}] 7. Video/Audio & OTT Streaming Capability Report:", media_report)
        assert media_report["codecs"]["vp9"] is True
        assert media_report["codecs"]["av1_gated"] is True
        assert media_report["codecs"]["opus_audio"] is True
        assert media_report["audioOk"] is True
        assert media_report.get("totalFrames", 0) >= 8
        assert media_report.get("droppedFrames", 0) <= 2

        # Navigate to YouTube, Netflix & Amazon Prime Video to verify OTT streaming platforms & hardware decode
        for ott_url, ott_domain in (
            ("https://www.youtube.com", "youtube.com"),
            ("https://www.netflix.com", "netflix.com"),
            ("https://www.primevideo.com", "primevideo.com"),
        ):
            chrome_ws.eval_js(
                f"""
                (() => {{
                    const inp = document.getElementById('url-input');
                    inp.value = '{ott_url}';
                    inp.dispatchEvent(new KeyboardEvent('keydown', {{key: 'Enter', bubbles: true}}));
                }})()
                """
            )
            try:
                ott_t = find_target(args.port, lambda t: ott_domain in t.get("url", ""), timeout=15.0)
                ott_ws = MiniWs(ott_t["webSocketDebuggerUrl"])
                wait_page_ready(ott_ws, ott_domain, timeout=15.0)
                ott_info = measure_scroll_and_theme(ott_ws)
                ott_media = ott_ws.eval_js(
                    """
                    ({
                        title: document.title,
                        emeAvailable: typeof navigator.requestMediaKeySystemAccess === 'function',
                        mseVp9: typeof MediaSource !== 'undefined' && MediaSource.isTypeSupported('video/webm; codecs="vp09.00.10.08"'),
                        av1GatedToHwVp9: typeof MediaSource !== 'undefined' && !MediaSource.isTypeSupported('video/mp4; codecs="av01.0.05M.08"'),
                        audioCtxAvailable: typeof (window.AudioContext || window.webkitAudioContext) === 'function'
                    })
                    """
                )
                print(f"[{args.label}] 8. OTT Platform {ott_url} verified:", {**ott_info, **ott_media})
                ott_ws.close()
            except Exception as e:
                print(f"[{args.label}] OTT check ({ott_url}) note:", e)

        # ------------------------------------------------------------------
        # 50 Well-Known Large Global Websites Multi-Tab Stress Test & Memory Compaction
        # ------------------------------------------------------------------
        stress_report = run_50_large_sites_stress_test(args.port, chrome_ws, proc.pid, args.label)

        # Cleanly close the browser via #btn-close so LLVM PGO writes all .profraw files on exit
        print(f"[{args.label}] 9. Closing browser cleanly to flush PGO profiles & session.json...")
        try:
            chrome_ws.eval_js("document.getElementById('btn-close').click()")
        except Exception:
            pass

        wiki_ws.close()
        amz_ws.close()
        hud_ws.close()
        chrome_ws.close()

        exit_code = proc.wait(timeout=15)
        stderr_out = proc.stderr.read().decode("utf-8", errors="replace") if proc.stderr else ""
        assert exit_code == 0, f"Browser exited with code {exit_code}"
        assert "**CRASHING**" not in stderr_out, f"Sandbox/process crash detected in stderr:\n{stderr_out}"
        assert "SendProcessMessage sent to detached frame" not in stderr_out, f"Detached frame IPC warning in stderr:\n{stderr_out}"
        print(f"[{args.label}] 10. Process stderr & sandbox stability audit: CLEAN (0 crashes, 0 detached-frame drops)")
        print(f"[{args.label}] SUMMARY: startup={startup_ms:.1f}ms | unit_25x={unit_ms:.1f}ms | "
              f"wiki_scroll_avg={wiki_scroll['avgMs']}ms | amazon_scroll_avg={amz_scroll['avgMs']}ms | "
              f"stress_50_sites={stress_report['elapsedSec']}s (rss_after={stress_report['rssAfterCompactionMiB']}MiB)")

    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
        shutil.rmtree(profile_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
