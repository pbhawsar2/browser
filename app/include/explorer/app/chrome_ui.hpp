#pragma once
// Explorer — Compact Obsidian WebUI Chrome (44px bezel / 38px auto-hiding tab bar / 38px find bar)
// + Unified 4-Tab Floating Control Overlay (Power | Vault | Downloads | Apps).
// Zero-DOM-rebuild telemetry & progress updates, pixel-aligned SVG controls, zero text overlap.

#include <cstdint>
#include <string>
#include <string_view>

namespace explorer::app {

inline std::string base64Encode(std::string_view in) {
    static constexpr char kTab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                     (static_cast<uint8_t>(in[i + 1]) << 8) |
                     static_cast<uint8_t>(in[i + 2]);
        out.push_back(kTab[(v >> 18) & 0x3F]);
        out.push_back(kTab[(v >> 12) & 0x3F]);
        out.push_back(kTab[(v >> 6) & 0x3F]);
        out.push_back(kTab[v & 0x3F]);
        i += 3;
    }
    if (i < in.size()) {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
        out.push_back(kTab[(v >> 18) & 0x3F]);
        out.push_back(kTab[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < in.size() ? kTab[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

inline std::string makeHtmlDataUrl(std::string_view html) {
    return "data:text/html;charset=utf-8;base64," + base64Encode(html);
}

inline std::string topChromeHtmlDataUrl() {
    static const char* kHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src https: http: data:;">
<title>Explorer Chrome</title>
<style>
  :root {
    --bg-canvas: #080C14;
    --bg-bezel: #0C121E;
    --bg-card: #131C2C;
    --bg-card-hover: #1A2B45;
    --bg-card-active: #0B111E;
    --bg-url: #111827;
    --bg-url-focus: #142038;
    --border: #334155;
    --border-subtle: rgba(148, 163, 184, 0.16);
    --cyan: #38BDF8;
    --emerald: #34D399;
    --coral: #F87171;
    --text: #FFFFFF;
    --text-dim: #E2E8F0;
    --muted: #94A3B8;
  }
  html[data-theme="light"] {
    --bg-canvas: #F1F5F9;
    --bg-bezel: #F8FAFC;
    --bg-card: #FFFFFF;
    --bg-card-hover: #E0F2FE;
    --bg-card-active: #BAE6FD;
    --bg-url: #FFFFFF;
    --bg-url-focus: #FFFFFF;
    --border: #CBD5E1;
    --border-subtle: rgba(15, 23, 42, 0.12);
    --cyan: #0284C7;
    --emerald: #059669;
    --coral: #DC2626;
    --text: #0F172A;
    --text-dim: #1E293B;
    --muted: #64748B;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; user-select: none; }
  html, body {
    width: 100%; height: 100%; overflow: hidden;
    background: var(--bg-canvas); color: var(--text);
    font-family: "Adwaita Sans", Inter, system-ui, -apple-system, sans-serif;
    text-rendering: geometricPrecision;
    -webkit-font-smoothing: antialiased;
  }

  /* ---- Top 44px Compact Bezel ---- */
  .top-bezel {
    position: relative;
    height: 44px;
    background: var(--bg-bezel);
    border-bottom: 1px solid var(--border-subtle);
    padding: 0 10px;
    display: flex;
    align-items: center;
    gap: 6px;
    -webkit-app-region: drag;
  }
  .no-drag { -webkit-app-region: no-drag; }
  .nav-group, .right-group { display: flex; align-items: center; gap: 5px; flex-shrink: 0; }

  button.card-btn {
    position: relative;
    width: 36px; height: 34px; border-radius: 7px;
    background: var(--bg-card); border: 1px solid var(--border); color: var(--text-dim);
    display: inline-flex; align-items: center; justify-content: center;
    cursor: pointer; outline: none; flex-shrink: 0;
    transition: background-color 100ms ease, border-color 100ms ease, color 100ms ease;
    -webkit-app-region: no-drag;
  }
  button.card-btn svg {
    width: 16px; height: 16px; stroke: currentColor; stroke-width: 2.2;
    stroke-linecap: round; stroke-linejoin: round; fill: none; display: block;
  }
  button.card-btn:hover:not(:disabled) {
    background: var(--bg-card-hover); border-color: rgba(56, 189, 248, 0.72); color: #FFFFFF;
  }
  button.card-btn:active:not(:disabled) {
    background: var(--bg-card-active); border-color: var(--cyan); color: #7DD3FC;
  }
  button.card-btn:disabled {
    background: #0F1623; border-color: rgba(51, 65, 85, 0.55); color: rgba(148, 163, 184, 0.38); cursor: default;
  }
  button.card-btn.win-close:hover { background: #36151E; border-color: var(--coral); color: #FECACA; }
  button.card-btn.win-close:active { background: #471522; border-color: var(--coral); color: #FFFFFF; }

  .badge-dot {
    position: absolute; top: 5px; right: 5px; width: 7px; height: 7px;
    border-radius: 50%; background: var(--cyan); display: none;
  }
  .badge-dot.show { display: block; }

  /* Centered clamped 34px URL bar */
  .url-wrap {
    flex: 1 1 200px; min-width: 180px;
    display: flex; justify-content: center; align-items: center;
    padding: 0 4px; -webkit-app-region: drag;
  }
  .url-box {
    width: 100%; max-width: 880px; height: 34px;
    background: var(--bg-url); border: 1px solid #3B4C66; border-radius: 9px;
    display: flex; align-items: center; padding: 0 8px 0 10px; gap: 7px;
    overflow: hidden;
    transition: border-color 110ms ease, background-color 110ms ease;
    -webkit-app-region: no-drag;
  }
  .url-box:focus-within { background: var(--bg-url-focus); border-color: rgba(56, 189, 248, 0.85); }
  .url-icon {
    width: 15px; height: 15px; color: var(--muted); flex-shrink: 0;
    display: inline-flex; align-items: center; justify-content: center;
    cursor: pointer;
  }
  .url-icon.secure { color: var(--emerald); }
  .url-icon svg {
    width: 15px; height: 15px; stroke: currentColor; stroke-width: 2.1;
    stroke-linecap: round; stroke-linejoin: round; fill: none; display: block;
  }
  #url-input {
    flex: 1; min-width: 60px; height: 100%; background: transparent; border: none; outline: none;
    color: var(--text); font-size: 14.5px; font-weight: 600; letter-spacing: 0.01em; line-height: 1; caret-color: var(--cyan); user-select: text;
  }
  #url-input::placeholder { color: var(--muted); font-weight: 500; }

  .mini-pill {
    height: 24px; padding: 0 9px; border-radius: 999px; flex-shrink: 0; cursor: pointer;
    font-size: 11.5px; font-weight: 600; line-height: 1; white-space: nowrap;
    font-variant-numeric: tabular-nums;
    border: 1px solid rgba(56, 189, 248, 0.34); background: rgba(56, 189, 248, 0.14); color: #7DD3FC;
    display: inline-flex; align-items: center; justify-content: center; gap: 4px;
  }
  html[data-theme="light"] .mini-pill { color: #0369A1; border-color: rgba(2,132,199,0.4); background: rgba(2,132,199,0.12); }
  .mini-pill.off { border-color: rgba(148,163,184,0.28); background: rgba(148,163,184,0.10); color: var(--muted); }
  .mini-pill.key { border-color: rgba(52,211,153,0.42); background: rgba(52,211,153,0.14); color: #6EE7B7; display: none; }
  html[data-theme="light"] .mini-pill.key { color: #047857; }
  .mini-pill.key.show { display: inline-flex; }
  .mini-pill.media { border-color: rgba(248,113,113,0.52); background: rgba(248,113,113,0.16); color: #FCA5A5; display: none; }
  html[data-theme="light"] .mini-pill.media { color: #B91C1C; }
  .mini-pill.media.show { display: inline-flex; }

  .zoom-pill {
    height: 26px; border-radius: 999px; flex-shrink: 0;
    border: 1px solid rgba(148, 163, 184, 0.36); background: rgba(19, 28, 44, 0.96);
    display: inline-flex; align-items: center; overflow: hidden;
    font-variant-numeric: tabular-nums;
  }
  .zoom-pill button {
    height: 100%; border: none; background: transparent; color: #E2E8F0;
    font-size: 12.5px; font-weight: 700; line-height: 1; cursor: pointer;
    padding: 0 7px; display: inline-flex; align-items: center; justify-content: center;
    transition: background-color 100ms ease, color 100ms ease;
  }
  .zoom-pill button:hover { background: rgba(56, 189, 248, 0.24); color: #FFFFFF; }
  .zoom-pill button:active { background: rgba(56, 189, 248, 0.38); color: #7DD3FC; }
  .zoom-pill #btn-zoom-reset {
    padding: 0 7px; color: #7DD3FC; font-size: 12px; font-weight: 700;
    border-left: 1px solid rgba(148, 163, 184, 0.22);
    border-right: 1px solid rgba(148, 163, 184, 0.22);
  }

  button.power-pill {
    height: 34px; border-radius: 8px; padding: 0 11px; flex-shrink: 0;
    font-size: 12px; font-weight: 600; line-height: 1; font-variant-numeric: tabular-nums;
    cursor: pointer; outline: none; display: inline-flex; align-items: center; gap: 6px; white-space: nowrap;
    transition: background-color 110ms ease, border-color 110ms ease, color 110ms ease;
    -webkit-app-region: no-drag;
  }
  button.power-pill .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; flex-shrink: 0; }
  button.power-pill.zero-fan { background: rgba(52, 211, 153, 0.16); border: 1px solid rgba(52, 211, 153, 0.52); color: #6EE7B7; }
  button.power-pill.zero-fan:hover { background: rgba(52, 211, 153, 0.26); color: #D1FAE5; }
  button.power-pill.ac-turbo { background: rgba(56, 189, 248, 0.16); border: 1px solid rgba(56, 189, 248, 0.52); color: #7DD3FC; }
  button.power-pill.ac-turbo:hover { background: rgba(56, 189, 248, 0.26); color: #E0F2FE; }

  .sep { width: 1px; height: 20px; background: rgba(148, 163, 184, 0.22); margin: 0 1px; flex-shrink: 0; }

  .progress-bar {
    position: absolute; left: 0; bottom: 0; height: 2px; width: 100%;
    background: linear-gradient(90deg, #38BDF8, #7DD3FC);
    transform-origin: left center; transform: scaleX(0); will-change: transform, opacity;
    opacity: 0; transition: transform 110ms linear, opacity 160ms ease; pointer-events: none;
  }
  .progress-bar.active { opacity: 1; }

  /* ---- Auto-Hiding 38px Horizontal Tab Bar ---- */
  .tab-bar {
    height: 38px; background: #090E19; border-bottom: 1px solid rgba(148, 163, 184, 0.14);
    padding: 0 10px; display: none; align-items: center; gap: 6px; overflow-x: auto; scrollbar-width: none;
  }
  .tab-bar.visible { display: flex; }
  .tab-bar::-webkit-scrollbar { display: none; }

  .tab-pill {
    height: 30px; min-width: 118px; max-width: 232px; flex: 0 1 190px;
    background: #111827; border: 1px solid rgba(148, 163, 184, 0.18); border-radius: 8px;
    padding: 0 5px 0 10px; display: flex; align-items: center; gap: 7px; cursor: pointer;
    overflow: hidden;
    transition: background-color 100ms ease, border-color 100ms ease;
  }
  .tab-pill:hover { background: #162238; border-color: rgba(148, 163, 184, 0.34); }
  .tab-pill.active { background: #18273F; border-color: rgba(56, 189, 248, 0.65); }
  .tab-fav { width: 15px; height: 15px; border-radius: 3px; flex-shrink: 0; object-fit: contain; display: block; }
  .tab-dot { width: 7px; height: 7px; border-radius: 50%; background: #38BDF8; flex-shrink: 0; }
  .tab-title {
    flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
    font-size: 12.5px; font-weight: 500; line-height: 1.2; color: #CBD5E1;
  }
  .tab-pill.active .tab-title { color: #FFFFFF; font-weight: 600; }
  .tab-close {
    width: 22px; height: 22px; border: none; background: transparent; border-radius: 6px;
    color: #94A3B8; display: inline-flex; align-items: center; justify-content: center;
    cursor: pointer; flex-shrink: 0; padding: 0;
  }
  .tab-close svg { width: 12px; height: 12px; stroke: currentColor; stroke-width: 2.3; stroke-linecap: round; fill: none; display: block; }
  .tab-close:hover { background: rgba(248, 113, 113, 0.30); color: #FEE2E2; }

  /* ---- Collapsible 38px Find Bar ---- */
  .find-bar {
    height: 38px; background: #0E1524; border-bottom: 1px solid rgba(148, 163, 184, 0.14);
    padding: 0 10px; display: none; align-items: center; gap: 6px;
  }
  .find-bar.visible { display: flex; }
  #find-input {
    width: 300px; height: 28px; background: #131C2E; border: 1px solid #3B4C66; border-radius: 7px;
    padding: 0 10px; color: #FFFFFF; font-size: 13px; font-weight: 500; line-height: 1; outline: none; user-select: text;
  }
  #find-input:focus { border-color: var(--cyan); }
  button.find-btn {
    width: 28px; height: 28px; border-radius: 6px; background: var(--bg-card); border: 1px solid var(--border);
    color: #F1F5F9; cursor: pointer; display: inline-flex; align-items: center; justify-content: center; flex-shrink: 0;
  }
  button.find-btn svg { width: 14px; height: 14px; stroke: currentColor; stroke-width: 2.2; stroke-linecap: round; stroke-linejoin: round; fill: none; display: block; }
  button.find-btn:hover { background: var(--bg-card-hover); border-color: var(--cyan); color: #FFF; }
  button.find-btn.close:hover { background: #36151E; border-color: var(--coral); color: #FECACA; }
  html[data-theme="light"], html[data-theme="light"] body {
    background: #F8FAFC; color: #0F172A; border-color: rgba(15, 23, 42, 0.22);
  }
  html[data-theme="light"] .card, html[data-theme="light"] .list-item {
    background: #FFFFFF; border-color: #CBD5E1; color: #0F172A;
  }
  html[data-theme="light"] .lbl, html[data-theme="light"] .toggle-row, html[data-theme="light"] .item-name, html[data-theme="light"] .val {
    color: #0F172A;
  }
  html[data-theme="light"] .item-sub, html[data-theme="light"] .card-title { color: #475569; }
  html[data-theme="light"] .val.cyan, html[data-theme="light"] .item-stat { color: #0284C7; }
  html[data-theme="light"] .val.ok { color: #059669; }
  html[data-theme="light"] .tab { color: #475569; }
  html[data-theme="light"] .tab.active { background: #E0F2FE; border-color: #0284C7; color: #0F172A; }
  html[data-theme="light"] input.field { background: #F1F5F9; border-color: #CBD5E1; color: #0F172A; }
  html[data-theme="light"] .btn { background: rgba(2,132,199,0.12); border-color: rgba(2,132,199,0.45); color: #0369A1; }
  html[data-theme="light"] .btn.danger { background: rgba(220,38,38,0.12); border-color: rgba(220,38,38,0.45); color: #B91C1C; }
</style>
</head>
<body>
  <div class="top-bezel" id="top-bezel">
    <div class="nav-group no-drag">
      <button class="card-btn" id="btn-back" title="Back (Alt+Left)">
        <svg viewBox="0 0 24 24"><polyline points="15 18 9 12 15 6"/></svg>
      </button>
      <button class="card-btn" id="btn-fwd" title="Forward (Alt+Right)">
        <svg viewBox="0 0 24 24"><polyline points="9 18 15 12 9 6"/></svg>
      </button>
      <button class="card-btn" id="btn-reload" title="Reload (Ctrl+R)">
        <svg id="icon-reload" viewBox="0 0 24 24"><path d="M21 12a9 9 0 1 1-2.64-6.36"/><polyline points="21 3 21 9 15 9"/></svg>
      </button>
      <button class="card-btn" id="btn-newtab" title="New Tab (Ctrl+T)">
        <svg viewBox="0 0 24 24"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>
      </button>
    </div>

    <div class="url-wrap">
      <div class="url-box">
        <span class="url-icon" id="url-icon" title="Site Permissions, Cookies &amp; App Settings">
          <svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="7"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>
        </span>
        <input id="url-input" type="text" spellcheck="false" autocomplete="off" placeholder="Search Google or enter URL...">
        <button class="mini-pill media" id="btn-media-active" title="Active Microphone/Camera Access (click to manage permissions)">Mic</button>
        <button class="mini-pill key" id="btn-key" title="Saved Encrypted Passwords">Key</button>
        <div class="zoom-pill" title="Page Zoom (Ctrl - / Ctrl 0 / Ctrl +)">
          <button type="button" id="btn-zoom-out" title="Zoom Out -10% (Ctrl -)">−</button>
          <button type="button" id="btn-zoom-reset" title="Reset Zoom to 100% (Ctrl 0)">100%</button>
          <button type="button" id="btn-zoom-in" title="Zoom In +10% (Ctrl +)">+</button>
        </div>
        <button class="mini-pill" id="btn-shield" title="Brave Adblock Shield (click to manage)">Shield</button>
      </div>
    </div>

    <div class="right-group no-drag">
      <button class="card-btn" id="btn-downloads" title="Downloads">
        <svg viewBox="0 0 24 24"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
        <span class="badge-dot" id="dl-dot"></span>
      </button>
      <button class="card-btn" id="btn-apps" title="Apps, Passwords & Site Data">
        <svg viewBox="0 0 24 24"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>
      </button>
      <button class="power-pill ac-turbo" id="btn-hud" title="Hardware & Power Supervisor">
        <span class="dot"></span>
        <span id="hud-text">AC Turbo</span>
      </button>
      <div class="sep"></div>
      <button class="card-btn" id="btn-min" title="Minimize">
        <svg viewBox="0 0 24 24"><line x1="6" y1="15" x2="18" y2="15"/></svg>
      </button>
      <button class="card-btn" id="btn-max" title="Maximize / Restore">
        <svg viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2"/></svg>
      </button>
      <button class="card-btn" id="btn-full" title="Fullscreen (F11)">
        <svg viewBox="0 0 24 24"><polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/><polyline points="21 15 21 21 15 21"/><polyline points="3 9 3 3 9 3"/></svg>
      </button>
      <button class="card-btn win-close" id="btn-close" title="Close (Ctrl+Q)">
        <svg viewBox="0 0 24 24"><line x1="7" y1="7" x2="17" y2="17"/><line x1="17" y1="7" x2="7" y2="17"/></svg>
      </button>
    </div>

    <div class="progress-bar" id="progress-bar"></div>
  </div>

  <div class="tab-bar" id="tab-bar"></div>

  <div class="find-bar" id="find-bar">
    <input id="find-input" type="text" placeholder="Find in page..." autocomplete="off">
    <button class="find-btn" id="find-prev" title="Previous (Shift+Enter)">
      <svg viewBox="0 0 24 24"><polyline points="18 15 12 9 6 15"/></svg>
    </button>
    <button class="find-btn" id="find-next" title="Next (Enter)">
      <svg viewBox="0 0 24 24"><polyline points="6 9 12 15 18 9"/></svg>
    </button>
    <button class="find-btn close" id="find-close" title="Close (Esc)">
      <svg viewBox="0 0 24 24"><line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/></svg>
    </button>
  </div>

<script>
  const send = (cmd, arg = '') => {
    if (window.explorerHost && typeof window.explorerHost.post === 'function') {
      window.explorerHost.post(cmd, String(arg));
    }
  };

  const urlInput = document.getElementById('url-input');
  const findInput = document.getElementById('find-input');
  const tabBar = document.getElementById('tab-bar');
  const findBar = document.getElementById('find-bar');
  const progressBar = document.getElementById('progress-bar');

  let lastTabsSig = '';
  let lastLoading = null;
  let lastIsHttps = null;

  document.getElementById('btn-back').onclick = () => send('back');
  document.getElementById('btn-fwd').onclick = () => send('forward');
  document.getElementById('btn-reload').onclick = () => send('reload_or_stop');
  document.getElementById('btn-newtab').onclick = () => send('new_tab');
  document.getElementById('btn-hud').onclick = () => send('open_panel', 'power');
  document.getElementById('btn-shield').onclick = () => send('open_panel', 'shield');
  document.getElementById('btn-key').onclick = () => send('open_panel', 'vault');
  document.getElementById('url-icon').onclick = () => send('open_panel', 'apps');
  document.getElementById('btn-media-active').onclick = () => send('open_panel', 'apps');
  document.getElementById('btn-zoom-out').onclick = () => send('zoom_out');
  document.getElementById('btn-zoom-reset').onclick = () => send('zoom_reset');
  document.getElementById('btn-zoom-in').onclick = () => send('zoom_in');
  document.getElementById('btn-downloads').onclick = () => send('open_panel', 'downloads');
  document.getElementById('btn-apps').onclick = () => send('open_panel', 'apps');
  document.getElementById('btn-min').onclick = () => send('win_min');
  document.getElementById('btn-max').onclick = () => send('win_max');
  document.getElementById('btn-full').onclick = () => send('win_full');
  document.getElementById('btn-close').onclick = () => send('win_close');

  document.getElementById('top-bezel').ondblclick = (e) => {
    if (e.target.closest('button, input, .url-box')) return;
    send('win_max');
  };

  urlInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); send('navigate', urlInput.value); urlInput.blur(); }
    else if (e.key === 'Escape') { e.preventDefault(); urlInput.blur(); send('focus_content'); }
  });
  urlInput.addEventListener('focus', () => urlInput.select());

  findInput.addEventListener('input', () => send('find_input', findInput.value));
  findInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); send(e.shiftKey ? 'find_prev' : 'find_next', findInput.value); }
    else if (e.key === 'Escape') { e.preventDefault(); send('find_close'); }
  });
  document.getElementById('find-prev').onclick = () => send('find_prev', findInput.value);
  document.getElementById('find-next').onclick = () => send('find_next', findInput.value);
  document.getElementById('find-close').onclick = () => send('find_close');

  window.__focusOmnibar = () => { urlInput.focus(); urlInput.select(); };
  window.__focusFind = () => { findInput.focus(); findInput.select(); };

  window.__updateTelemetry = (batteryMode, pillText, adblockOn, blocked) => {
    const sh = document.getElementById('btn-shield');
    sh.className = 'mini-pill' + (adblockOn ? '' : ' off');
    sh.textContent = adblockOn ? ('Shield' + (blocked > 0 ? ' ' + blocked : '')) : 'Shield Off';
    const hudBtn = document.getElementById('btn-hud');
    hudBtn.className = 'power-pill ' + (batteryMode ? 'zero-fan' : 'ac-turbo');
    document.getElementById('hud-text').textContent = pillText || 'AC Turbo';
  };

  window.__updateProgress = (loading, progress) => {
    if (loading && progress > 0 && progress < 1.0) {
      progressBar.style.transform = 'scaleX(' + Math.max(0.08, Math.min(1, progress)) + ')';
      progressBar.classList.add('active');
    } else {
      progressBar.classList.remove('active');
      progressBar.style.transform = 'scaleX(0)';
    }
  };

  window.__updateChrome = (s) => {
    document.documentElement.setAttribute('data-theme', s.darkTheme === false ? 'light' : 'dark');
    document.getElementById('btn-back').disabled = !s.canBack;
    document.getElementById('btn-fwd').disabled = !s.canFwd;

    if (lastLoading !== !!s.loading) {
      lastLoading = !!s.loading;
      const rIcon = document.getElementById('icon-reload');
      rIcon.innerHTML = lastLoading
        ? '<line x1="7" y1="7" x2="17" y2="17"/><line x1="17" y1="7" x2="7" y2="17"/>'
        : '<path d="M21 12a9 9 0 1 1-2.64-6.36"/><polyline points="21 3 21 9 15 9"/>';
    }

    if (document.activeElement !== urlInput) urlInput.value = s.url || '';
    const isHttps = (s.url || '').startsWith('https://');
    if (lastIsHttps !== isHttps) {
      lastIsHttps = isHttps;
      const uIcon = document.getElementById('url-icon');
      uIcon.className = 'url-icon' + (isHttps ? ' secure' : '');
      uIcon.innerHTML = isHttps
        ? '<svg viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></svg>'
        : '<svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="7"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>';
    }

    window.__updateTelemetry(s.batteryMode, s.pillText, s.adblockOn, s.blocked);

    const mediaBtn = document.getElementById('btn-media-active');
    if (mediaBtn) {
      mediaBtn.className = 'mini-pill media' + (s.mediaAccess ? ' show' : '');
      mediaBtn.textContent = s.mediaAccess || 'Mic';
    }

    const keyBtn = document.getElementById('btn-key');
    keyBtn.className = 'mini-pill key' + ((s.siteCreds > 0 || s.hasPendingLogin) ? ' show' : '');
    keyBtn.textContent = s.hasPendingLogin ? 'Save Login' : ('Key (' + s.siteCreds + ')');

    if (s.zoomPct) {
      document.getElementById('btn-zoom-reset').textContent = s.zoomPct + '%';
    }

    document.getElementById('dl-dot').className = 'badge-dot' + (s.activeDownloads > 0 ? ' show' : '');
    window.__updateProgress(s.loading, s.progress);

    const showTabs = Array.isArray(s.tabs) && s.tabs.length > 1;
    tabBar.className = 'tab-bar' + (showTabs ? ' visible' : '');
    const tabsSig = showTabs ? JSON.stringify(s.tabs) : '';
    if (tabsSig !== lastTabsSig) {
      lastTabsSig = tabsSig;
      if (!showTabs) {
        tabBar.innerHTML = '';
      } else {
        const existing = new Map();
        for (const el of tabBar.children) {
          if (el.dataset && el.dataset.tabId) existing.set(el.dataset.tabId, el);
        }
        const activeIds = new Set(s.tabs.map(t => String(t.id)));
        for (const [id, el] of existing) {
          if (!activeIds.has(id)) el.remove();
        }
        for (let i = 0; i < s.tabs.length; ++i) {
          const t = s.tabs[i];
          const idStr = String(t.id);
          let pill = existing.get(idStr);
          const wantFav = (t.favicon && (t.favicon.startsWith('https://') || t.favicon.startsWith('data:image/'))) ? t.favicon : '';
          if (!pill) {
            pill = document.createElement('div');
            pill.dataset.tabId = idStr;
            pill.dataset.fav = '';
            pill.onmousedown = (ev) => { if (ev.button === 1) { ev.preventDefault(); send('close_tab', t.id); } };
            pill.onclick = (ev) => { if (!ev.target.closest('.tab-close')) send('activate_tab', t.id); };
            const iconSlot = document.createElement('span');
            iconSlot.className = 'tab-dot';
            pill.appendChild(iconSlot);
            const title = document.createElement('span');
            title.className = 'tab-title';
            pill.appendChild(title);
            const close = document.createElement('button');
            close.className = 'tab-close'; close.type = 'button'; close.title = 'Close Tab';
            close.innerHTML = '<svg viewBox="0 0 24 24"><line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/></svg>';
            close.onclick = (ev) => { ev.stopPropagation(); send('close_tab', t.id); };
            pill.appendChild(close);
          }
          pill.className = 'tab-pill' + (t.active ? ' active' : '');
          if (pill.dataset.fav !== wantFav) {
            pill.dataset.fav = wantFav;
            const first = pill.firstElementChild;
            if (wantFav) {
              const img = document.createElement('img');
              img.className = 'tab-fav'; img.src = wantFav; img.onerror = () => { img.style.display = 'none'; };
              if (first) pill.replaceChild(img, first);
            } else {
              const dot = document.createElement('span');
              dot.className = 'tab-dot';
              if (first) pill.replaceChild(dot, first);
            }
          }
          if (pill.firstElementChild && pill.firstElementChild.classList.contains('tab-dot')) {
            pill.firstElementChild.style.opacity = t.active ? '1' : '0.4';
          }
          const titleEl = pill.querySelector('.tab-title');
          if (titleEl && titleEl.textContent !== (t.title || 'New Tab')) {
            titleEl.textContent = t.title || 'New Tab';
          }
          if (tabBar.children[i] !== pill) {
            tabBar.insertBefore(pill, tabBar.children[i] || null);
          }
        }
      }
    }
    findBar.className = 'find-bar' + (s.findVisible ? ' visible' : '');
  };
</script>
</body>
</html>)HTML";
    static const std::string kCachedUrl = makeHtmlDataUrl(kHtml);
    return kCachedUrl;
}

inline std::string powerHudHtmlDataUrl() {
    static const char* kHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline';">
<title>Explorer Control Panel</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; user-select: none; }
  ::-webkit-scrollbar { width: 7px; height: 7px; background: #0E1524; }
  ::-webkit-scrollbar-track { background: #0E1524; }
  ::-webkit-scrollbar-thumb { background: rgba(56, 189, 248, 0.36); border-radius: 999px; border: 1.5px solid #0E1524; }
  ::-webkit-scrollbar-thumb:hover { background: rgba(56, 189, 248, 0.72); }
  ::-webkit-scrollbar-button, ::-webkit-scrollbar-corner { display: none; width: 0; height: 0; }
  html, body {
    width: 100%; height: 100%; overflow: hidden;
    background: #0E1524; color: #F8FAFC;
    font-family: "Adwaita Sans", Inter, system-ui, -apple-system, sans-serif;
    text-rendering: optimizeLegibility; -webkit-font-smoothing: antialiased;
    border: 1px solid rgba(148, 163, 184, 0.28); border-radius: 12px;
    display: flex; flex-direction: column;
  }
  .top {
    padding: 10px 12px; display: flex; align-items: center; gap: 6px;
    border-bottom: 1px solid rgba(148, 163, 184, 0.14); flex-shrink: 0;
  }
  .tabs { display: flex; flex: 1; min-width: 0; gap: 4px; align-items: center; }
  .tab {
    flex: 1 0 auto; white-space: nowrap;
    background: transparent; border: 1px solid transparent; border-radius: 7px;
    color: #94A3B8; font-size: 12.5px; font-weight: 600; line-height: 1.2;
    padding: 6px 9px; cursor: pointer; text-align: center;
    transition: background-color 110ms ease, color 110ms ease, border-color 110ms ease;
  }
  .tab:hover { color: #F8FAFC; background: #131C2E; }
  .tab.active { background: #18273F; border-color: rgba(56,189,248,0.52); color: #FFFFFF; }
  .close-btn {
    width: 28px; height: 28px; border-radius: 7px; flex-shrink: 0;
    background: #131C2E; border: 1px solid #334155; color: #CBD5E1;
    cursor: pointer; display: inline-flex; align-items: center; justify-content: center; padding: 0;
  }
  .close-btn svg { width: 14px; height: 14px; stroke: currentColor; stroke-width: 2.2; stroke-linecap: round; fill: none; display: block; }
  .close-btn:hover { background: #36151E; border-color: #F87171; color: #FECACA; }

  .body {
    flex: 1; min-height: 0; overflow-y: auto; padding: 10px 12px;
    display: flex; flex-direction: column; gap: 9px;
  }
  .pane { display: none; flex-direction: column; gap: 9px; }
  .pane.active { display: flex; }

  .card {
    background: #131C2E; border: 1px solid rgba(148, 163, 184, 0.16);
    border-radius: 9px; padding: 10px 12px; display: flex; flex-direction: column; gap: 7px;
  }
  .card-title {
    font-size: 11.5px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em;
    color: #94A3B8; line-height: 1.25; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .row { display: flex; justify-content: space-between; align-items: center; font-size: 13px; gap: 10px; min-height: 22px; }
  .lbl { color: #CBD5E1; font-weight: 500; flex-shrink: 0; white-space: nowrap; }
  .val {
    font-family: ui-monospace, "JetBrains Mono", monospace; font-size: 12.5px; font-weight: 600;
    font-variant-numeric: tabular-nums; color: #FFFFFF;
    min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; text-align: right;
  }
  .val.ok { color: #6EE7B7; } .val.warn { color: #FBBF24; } .val.cyan { color: #7DD3FC; }

  .toggle-row {
    display: flex; justify-content: space-between; align-items: center; gap: 12px;
    min-height: 26px; font-size: 13px; font-weight: 500; color: #F1F5F9; cursor: pointer;
  }
  .toggle-row span { flex: 1; min-width: 0; line-height: 1.3; }
  .sw {
    width: 38px; height: 21px; border-radius: 999px; background: #334155;
    position: relative; transition: background 130ms ease; border: none; cursor: pointer; flex-shrink: 0;
  }
  .sw::after {
    content: ''; position: absolute; top: 2.5px; left: 3px; width: 16px; height: 16px;
    border-radius: 50%; background: #FFFFFF; transition: transform 130ms ease;
  }
  .sw.on { background: #38BDF8; }
  .sw.on::after { transform: translateX(17px); }
  .sw.on.green { background: #34D399; }

  .btn {
    height: 34px; border-radius: 8px; padding: 0 12px;
    background: rgba(56, 189, 248, 0.15); border: 1px solid rgba(56, 189, 248, 0.44);
    color: #E0F2FE; font-size: 13px; font-weight: 600; line-height: 1;
    cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    display: inline-flex; align-items: center; justify-content: center;
  }
  .btn:hover { background: rgba(56, 189, 248, 0.26); border-color: #38BDF8; color: #FFFFFF; }
  .btn.danger { background: rgba(248,113,113,0.14); border-color: rgba(248,113,113,0.45); color: #FECACA; }
  .btn.danger:hover { background: rgba(248,113,113,0.28); border-color: #F87171; color: #FFFFFF; }
  .btn.sm { height: 30px; padding: 0 10px; font-size: 12.5px; border-radius: 6px; }
  .btn-row { display: flex; gap: 6px; width: 100%; align-items: center; }
  .btn-row .btn { flex: 1; min-width: 0; }

  input.field {
    width: 100%; height: 36px; background: #0B111E; border: 1px solid #334155; border-radius: 7px;
    padding: 0 12px; color: #FFFFFF; font-size: 14px; font-weight: 500; line-height: 1.2; outline: none; user-select: text;
  }
  input.field::placeholder { color: #94A3B8; font-weight: 400; }
  input.field:focus { border-color: #38BDF8; background: #0F172A; }

  .list-item {
    background: #0D1524; border: 1px solid rgba(148,163,184,0.18); border-radius: 8px;
    padding: 10px 12px; display: flex; flex-direction: column; gap: 6px; min-width: 0;
  }
  .item-head { display: flex; justify-content: space-between; align-items: center; gap: 8px; min-width: 0; }
  .item-name {
    flex: 1; min-width: 0; font-size: 14px; font-weight: 600; line-height: 1.3; color: #FFFFFF;
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  .item-sub {
    width: 100%; min-width: 0; font-size: 13px; line-height: 1.3; color: #CBD5E1;
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  .item-stat {
    flex-shrink: 0; white-space: nowrap; font-family: ui-monospace, "JetBrains Mono", monospace;
    font-size: 13px; font-weight: 600; line-height: 1.3; color: #7DD3FC;
  }
  .empty { font-size: 13px; color: #94A3B8; text-align: center; padding: 12px 0; }
  .status-msg { font-size: 12.5px; line-height: 1.35; color: #6EE7B7; min-height: 16px; word-break: break-word; }
  html[data-theme="light"], html[data-theme="light"] body {
    background: #F8FAFC; color: #0F172A; border-color: rgba(15, 23, 42, 0.22);
  }
  html[data-theme="light"] .card, html[data-theme="light"] .list-item {
    background: #FFFFFF; border-color: #CBD5E1; color: #0F172A;
  }
  html[data-theme="light"] .lbl, html[data-theme="light"] .toggle-row, html[data-theme="light"] .item-name, html[data-theme="light"] .val {
    color: #0F172A;
  }
  html[data-theme="light"] .item-sub, html[data-theme="light"] .card-title { color: #475569; }
  html[data-theme="light"] .val.cyan, html[data-theme="light"] .item-stat { color: #0284C7; }
  html[data-theme="light"] .val.ok { color: #059669; }
  html[data-theme="light"] .tab { color: #475569; }
  html[data-theme="light"] .tab.active { background: #E0F2FE; border-color: #0284C7; color: #0F172A; }
  html[data-theme="light"] input.field { background: #F1F5F9; border-color: #CBD5E1; color: #0F172A; }
  html[data-theme="light"] .btn { background: rgba(2,132,199,0.12); border-color: rgba(2,132,199,0.45); color: #0369A1; }
  html[data-theme="light"] .btn.danger { background: rgba(220,38,38,0.12); border-color: rgba(220,38,38,0.45); color: #B91C1C; }
</style>
</head>
<body>
  <div class="top">
    <div class="tabs">
      <button class="tab active" id="tab-power" type="button" onclick="switchTab('power')">Power</button>
      <button class="tab" id="tab-shield" type="button" onclick="switchTab('shield')">Shield</button>
      <button class="tab" id="tab-vault" type="button" onclick="switchTab('vault')">Vault</button>
      <button class="tab" id="tab-downloads" type="button" onclick="switchTab('downloads')">Downloads</button>
      <button class="tab" id="tab-apps" type="button" onclick="switchTab('apps')">Apps</button>
    </div>
    <button class="close-btn" id="btn-hud-close" type="button" onclick="send('close_hud')" title="Close (Esc)">
      <svg viewBox="0 0 24 24"><line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/></svg>
    </button>
  </div>

  <div class="body">
    <!-- TAB 1: HARDWARE & POWER SUPERVISOR -->
    <div class="pane active" id="pane-power">
      <div class="card">
        <div class="card-title">Ryzen 7 5800H Hardware Telemetry</div>
        <div class="row"><span class="lbl">CPU / Package</span><span class="val" id="v-cpu">—</span></div>
        <div class="row"><span class="lbl">AMD Cezanne iGPU</span><span class="val cyan" id="v-igpu">—</span></div>
        <div class="row"><span class="lbl">VA-API VCN</span><span class="val ok" id="v-va">—</span></div>
        <div class="row"><span class="lbl">RTX 3050 Ti dGPU</span><span class="val ok" id="v-dgpu">—</span></div>
        <div class="row"><span class="lbl">Cooling Fans</span><span class="val" id="v-fan">—</span></div>
        <div class="row"><span class="lbl">Power / Battery</span><span class="val cyan" id="v-power">—</span></div>
      </div>

      <div class="card">
        <div class="card-title">Power, Display &amp; Zoom Governor</div>
        <div class="row">
          <span class="lbl">Page Zoom</span>
          <div style="display:flex; align-items:center; gap:6px;">
            <button class="btn sm" type="button" style="flex:0 0 30px; padding:0;" onclick="send('zoom_out')" title="Zoom Out (Ctrl -)">−</button>
            <span class="val cyan" id="v-zoom" style="min-width:46px; text-align:center;">100%</span>
            <button class="btn sm" type="button" style="flex:0 0 30px; padding:0;" onclick="send('zoom_in')" title="Zoom In (Ctrl +)">+</button>
            <button class="btn sm" type="button" style="flex:0 0 auto;" onclick="send('zoom_reset')" title="Reset Zoom (Ctrl 0)">Reset</button>
          </div>
        </div>
        <div class="row">
          <span class="lbl">Theme Appearance</span>
          <button class="btn sm" id="btn-theme-mode" type="button" onclick="send('cycle_theme')" title="Cycle System Auto / Dark / Light Theme">System (Dark)</button>
        </div>
        <div class="toggle-row" onclick="send('toggle_turbo')">
          <span>AC Turbo Performance Hold (PPD)</span>
          <button class="sw" id="sw-turbo" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_cinema60')">
          <span>Cinema 60 Hz Display Lock</span>
          <button class="sw" id="sw-cinema" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_autohide')">
          <span>Auto-Hide Top Bar on Scroll</span>
          <button class="sw" id="sw-autohide" type="button"></button>
        </div>
      </div>
      <button class="btn" type="button" onclick="send('open_devtools')">Open Developer Tools (F12)</button>
    </div>

    <!-- TAB 2: BRAVE ADBLOCK SHIELD -->
    <div class="pane" id="pane-shield">
      <div class="card">
        <div class="card-title">Brave Adblock Shield (adblock-rust)</div>
        <div class="toggle-row" onclick="send('toggle_adblock')">
          <span>Block Ads &amp; Trackers</span>
          <button class="sw green" id="sw-adblock" type="button"></button>
        </div>
        <div class="row"><span class="lbl">Active Filter Rules</span><span class="val cyan" id="v-shield-rules">0</span></div>
        <div class="row"><span class="lbl">Blocked Requests</span><span class="val ok" id="v-shield-blocked">0</span></div>
        <div class="row"><span class="lbl">Privacy Headers</span><span class="val ok">Sec-GPC: 1 · DNT: 1</span></div>
        <div class="row"><span class="lbl">WebRTC IP Policy</span><span class="val cyan">LAN Leak Protected</span></div>
        <button class="btn" id="btn-upd-filters" type="button" onclick="send('update_filters')">Update EasyList &amp; Privacy Filters</button>
      </div>
    </div>

    <!-- TAB 3: ENCRYPTED PASSWORD VAULT (AES-256-GCM) -->
    <div class="pane" id="pane-vault">
      <div class="card" id="pending-box" style="display:none; border-color: rgba(52,211,153,0.5);">
        <div class="card-title" style="color:#6EE7B7;">Captured Login Ready</div>
        <div class="item-sub" id="pending-text"></div>
        <div class="btn-row">
          <button class="btn sm" type="button" onclick="send('vault_accept_pending')">Save Encrypted</button>
          <button class="btn sm danger" type="button" onclick="send('vault_dismiss_pending')">Dismiss</button>
        </div>
      </div>

      <div class="card" id="auth-box" style="display:none; border-color: rgba(56,189,248,0.55);">
        <div class="card-title" style="color:#7DD3FC;">Linux Login / Root Authentication Required</div>
        <div class="item-sub" id="auth-target-text">Verify OS password to reveal credential</div>
        <input class="field" id="auth-sys-pass" type="password" placeholder="Enter Linux login or root password">
        <div class="btn-row">
          <button class="btn sm" type="button" onclick="submitVaultAuth()">Unlock &amp; Show</button>
          <button class="btn sm danger" type="button" onclick="cancelVaultAuth()">Cancel</button>
        </div>
      </div>

      <div class="card">
        <div class="card-title">Save / Update Password (AES-256-GCM)</div>
        <input class="field" id="v-origin" placeholder="Site origin (e.g. https://github.com)">
        <input class="field" id="v-user" placeholder="Username or email">
        <input class="field" id="v-pass" type="password" placeholder="Password">
        <button class="btn" type="button" onclick="saveCred()">Save / Update Credential</button>
      </div>

      <div class="card">
        <div class="card-title">Saved Logins</div>
        <div id="vault-list" style="display:flex; flex-direction:column; gap:6px;"></div>
      </div>
    </div>

    <!-- TAB 3: DOWNLOAD MANAGER -->
    <div class="pane" id="pane-downloads">
      <div class="btn-row">
        <button class="btn sm" type="button" onclick="send('dl_open_dir')">Open Downloads Folder</button>
        <button class="btn sm danger" type="button" onclick="send('dl_clear')">Clear Finished</button>
      </div>
      <div id="dl-list" style="display:flex; flex-direction:column; gap:6px;"></div>
    </div>

    <!-- TAB 5: SITE PERMISSIONS, COOKIES/CACHE & WEB APPS (PWA) -->
    <div class="pane" id="pane-apps">
      <div class="card">
        <div class="card-title" id="perm-origin-title">Site Permissions &amp; Access</div>
        <div class="toggle-row" onclick="send('toggle_perm', 'microphone')">
          <span>Microphone Access</span>
          <button class="sw green" id="sw-perm-mic" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'camera')">
          <span>Camera Access</span>
          <button class="sw green" id="sw-perm-camera" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'location')">
          <span>Location (Geolocation)</span>
          <button class="sw green" id="sw-perm-location" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'notifications')">
          <span>Notifications</span>
          <button class="sw green" id="sw-perm-notifications" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'clipboard')">
          <span>Clipboard Read &amp; Write</span>
          <button class="sw green" id="sw-perm-clipboard" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'screen_capture')">
          <span>Screen &amp; Window Share</span>
          <button class="sw green" id="sw-perm-screen" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'storage')">
          <span>Persistent Storage &amp; Files</span>
          <button class="sw green" id="sw-perm-storage" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'sound')">
          <span>Site Audio / Sound</span>
          <button class="sw green" id="sw-perm-sound" type="button"></button>
        </div>
        <div class="toggle-row" onclick="send('toggle_perm', 'popups')">
          <span>Popups &amp; New Tabs</span>
          <button class="sw green" id="sw-perm-popups" type="button"></button>
        </div>
        <button class="btn sm" id="btn-perm-reset" type="button" onclick="send('reset_site_perms')">Reset Current Site Permissions</button>
      </div>

      <div class="card">
        <div class="card-title">Privacy, Cookies &amp; Cache (No History Stored)</div>
        <div class="toggle-row" onclick="send('toggle_cookies')">
          <span>Persistent Cookies &amp; Sign-Ins</span>
          <button class="sw green" id="sw-cookies" type="button"></button>
        </div>
        <button class="btn sm" type="button" onclick="send('clear_site_data')">Clear Current Site Cookies &amp; Storage</button>
        <button class="btn sm" type="button" onclick="send('clear_http_cache')">Clear HTTP Disk &amp; Memory Cache</button>
        <button class="btn sm danger" type="button" onclick="send('clear_all_data')">Clear All Cookies, Cache &amp; Site Data</button>
      </div>

      <div class="card">
        <div class="card-title">Install Current Site as Standalone App</div>
        <input class="field" id="app-name" placeholder="App Name (e.g. Gemini Work)">
        <button class="btn" type="button" onclick="installApp()">Install as Standalone Desktop App</button>
      </div>

      <div class="card">
        <div class="card-title">Installed Web Apps (Isolated Data)</div>
        <div id="apps-list" style="display:flex; flex-direction:column; gap:6px;"></div>
      </div>
    </div>

    <div class="status-msg" id="status-msg"></div>
  </div>

<script>
  const send = (cmd, arg = '') => {
    if (window.explorerHost && typeof window.explorerHost.post === 'function') {
      window.explorerHost.post(cmd, String(arg));
    }
  };

  let lastCredsSig = '';
  let lastDownloadsSig = '';
  let lastAppsSig = '';
  let lastAutoOrigin = '';
  let lastAutoTitle = '';
  let vaultEditing = false;
  let pendingAuthTarget = null;
  const revealedPasswords = new Map();
  let revealHideTimer = null;

  function credKey(origin, username) {
    return origin + '\n' + username;
  }

  function clearRevealedPasswords() {
    if (revealedPasswords.size > 0) {
      revealedPasswords.clear();
      lastCredsSig = '';
    }
    if (revealHideTimer) {
      clearTimeout(revealHideTimer);
      revealHideTimer = null;
    }
  }

  function promptVaultAuth(origin, username) {
    pendingAuthTarget = { origin, username };
    document.getElementById('auth-target-text').textContent = 'Reveal password for ' + username + ' @ ' + origin;
    const inp = document.getElementById('auth-sys-pass');
    inp.value = '';
    document.getElementById('auth-box').style.display = 'flex';
    inp.focus();
  }

  function cancelVaultAuth() {
    pendingAuthTarget = null;
    document.getElementById('auth-sys-pass').value = '';
    document.getElementById('auth-box').style.display = 'none';
  }

  function submitVaultAuth() {
    if (!pendingAuthTarget) return;
    const sysPass = document.getElementById('auth-sys-pass').value;
    if (!sysPass) return;
    document.getElementById('auth-sys-pass').value = '';
    document.getElementById('auth-box').style.display = 'none';
    send('vault_reveal', JSON.stringify({
      origin: pendingAuthTarget.origin,
      username: pendingAuthTarget.username,
      authPassword: sysPass
    }));
    pendingAuthTarget = null;
  }

  document.getElementById('auth-sys-pass').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); submitVaultAuth(); }
    else if (e.key === 'Escape') { e.preventDefault(); cancelVaultAuth(); }
  });

  window.__onVaultRevealResult = (ok, origin, username, password, msg) => {
    document.getElementById('status-msg').textContent = msg || '';
    if (ok && origin && username) {
      revealedPasswords.set(credKey(origin, username), password);
      if (revealHideTimer) clearTimeout(revealHideTimer);
      revealHideTimer = setTimeout(() => {
        clearRevealedPasswords();
        if (window.__lastHudData) window.__updateHud(window.__lastHudData);
      }, 30000);
      lastCredsSig = '';
      if (window.__lastHudData) window.__updateHud(window.__lastHudData);
    }
  };

  function switchTab(name, notify = true) {
    for (const t of ['power', 'shield', 'vault', 'downloads', 'apps']) {
      document.getElementById('tab-' + t).className = 'tab' + (t === name ? ' active' : '');
      document.getElementById('pane-' + t).className = 'pane' + (t === name ? ' active' : '');
    }
    if (notify) {
      document.getElementById('status-msg').textContent = '';
      send('hud_tab_select', name);
    }
  }
  window.__selectTab = (name) => switchTab(name, false);

  function saveCred() {
    const o = document.getElementById('v-origin').value.trim();
    const u = document.getElementById('v-user').value.trim();
    const p = document.getElementById('v-pass').value;
    if (!o || !u || !p) return;
    send('vault_save', JSON.stringify({origin: o, username: u, password: p}));
    document.getElementById('v-pass').value = '';
    vaultEditing = false;
  }

  function installApp() {
    const n = document.getElementById('app-name').value.trim();
    send('webapp_install', n);
  }

  window.__updateHud = (h) => {
    window.__lastHudData = h;
    document.documentElement.setAttribute('data-theme', h.darkTheme === false ? 'light' : 'dark');
    document.getElementById('sw-adblock').className = 'sw green' + (h.adblockOn ? ' on' : '');
    document.getElementById('sw-autohide').className = 'sw' + (h.autohide ? ' on' : '');
    document.getElementById('sw-turbo').className = 'sw' + (!h.batteryMode ? ' on' : '');
    document.getElementById('sw-cinema').className = 'sw' + (h.cinema60 ? ' on' : '');
    const themeBtn = document.getElementById('btn-theme-mode');
    if (themeBtn && h.themeLabel) themeBtn.textContent = h.themeLabel;

    const pTitle = document.getElementById('perm-origin-title');
    if (pTitle) {
      pTitle.textContent = h.currentOrigin
        ? ('Site Permissions · ' + h.currentOrigin)
        : 'Site Permissions (Default)';
    }
    const sp = h.sitePerms || {};
    const setPermSw = (id, val) => {
      const el = document.getElementById(id);
      if (el) el.className = 'sw green' + (val !== false ? ' on' : '');
    };
    setPermSw('sw-perm-mic', sp.mic);
    setPermSw('sw-perm-camera', sp.camera);
    setPermSw('sw-perm-location', sp.location);
    setPermSw('sw-perm-notifications', sp.notifications);
    setPermSw('sw-perm-clipboard', sp.clipboard);
    setPermSw('sw-perm-screen', sp.screen);
    setPermSw('sw-perm-storage', sp.storage);
    setPermSw('sw-perm-sound', sp.sound);
    setPermSw('sw-perm-popups', sp.popups);
    setPermSw('sw-cookies', h.saveCookies !== false);

    document.getElementById('v-shield-rules').textContent = (h.rules || 0) + ' rules';
    document.getElementById('v-shield-blocked').textContent = (h.blocked || 0) + ' blocked';
    document.getElementById('v-cpu').textContent = h.cpu || '—';
    document.getElementById('v-igpu').textContent = h.igpu || '—';
    document.getElementById('v-va').textContent = h.va || '—';
    const dgpuEl = document.getElementById('v-dgpu');
    dgpuEl.textContent = h.dgpu || '—';
    dgpuEl.className = 'val ' + ((h.dgpu || '').startsWith('suspended') ? 'ok' : 'warn');
    document.getElementById('v-fan').textContent = h.fan || '—';
    document.getElementById('v-power').textContent = h.power || '—';
    if (h.zoomPct) {
      const zEl = document.getElementById('v-zoom');
      if (zEl) zEl.textContent = h.zoomPct + '%';
    }
    document.getElementById('status-msg').textContent = h.statusMsg || '';

    const origField = document.getElementById('v-origin');
    const userField = document.getElementById('v-user');
    const passField = document.getElementById('v-pass');
    const formFocused = (document.activeElement === origField ||
                         document.activeElement === userField ||
                         document.activeElement === passField);
    if (!vaultEditing && !formFocused && h.currentOrigin &&
        (origField.value === '' || origField.value === lastAutoOrigin)) {
      origField.value = h.currentOrigin;
      lastAutoOrigin = h.currentOrigin;
    }
    const appField = document.getElementById('app-name');
    if (document.activeElement !== appField &&
        (appField.value === '' || appField.value === lastAutoTitle)) {
      appField.value = h.currentTitle || '';
      lastAutoTitle = h.currentTitle || '';
    }

    const pBox = document.getElementById('pending-box');
    if (h.pendingOrigin && h.pendingUser) {
      pBox.style.display = 'flex';
      document.getElementById('pending-text').textContent = h.pendingUser + ' @ ' + h.pendingOrigin;
    } else {
      pBox.style.display = 'none';
    }

    // Vault list (DOM rebuilt only when credentials or currentOrigin change)
    const credsSig = JSON.stringify({c: h.creds || [], o: h.currentOrigin || ''});
    if (credsSig !== lastCredsSig) {
      lastCredsSig = credsSig;
      const vList = document.getElementById('vault-list');
      vList.innerHTML = '';
      if (!h.creds || h.creds.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty'; empty.textContent = 'No saved passwords yet';
        vList.appendChild(empty);
      } else {
        for (const c of h.creds) {
          const el = document.createElement('div');
          el.className = 'list-item';
          const nm = document.createElement('div'); nm.className = 'item-name'; nm.textContent = c.username;
          const sb = document.createElement('div'); sb.className = 'item-sub'; sb.textContent = c.origin;
          const key = credKey(c.origin, c.username);
          const isRevealed = revealedPasswords.has(key);
          const pwLine = document.createElement('div');
          pwLine.className = 'item-stat';
          pwLine.style.color = isRevealed ? '#6EE7B7' : '#64748B';
          pwLine.textContent = isRevealed ? ('Password: ' + revealedPasswords.get(key)) : 'Password: ••••••••••••';
          el.appendChild(nm); el.appendChild(sb); el.appendChild(pwLine);
          const row = document.createElement('div'); row.className = 'btn-row';
          if (h.currentOrigin && c.origin === h.currentOrigin) {
            const bf = document.createElement('button'); bf.className = 'btn sm'; bf.type = 'button'; bf.textContent = 'Autofill';
            bf.onclick = () => send('vault_fill', JSON.stringify({origin: c.origin, username: c.username}));
            row.appendChild(bf);
          }
          const bs = document.createElement('button'); bs.className = 'btn sm'; bs.type = 'button';
          bs.textContent = isRevealed ? 'Hide' : 'Show';
          bs.onclick = () => {
            if (revealedPasswords.has(key)) {
              revealedPasswords.delete(key);
              lastCredsSig = '';
              window.__updateHud(h);
            } else {
              promptVaultAuth(c.origin, c.username);
            }
          };
          const be = document.createElement('button'); be.className = 'btn sm'; be.type = 'button'; be.textContent = 'Edit';
          be.onclick = () => {
            vaultEditing = true;
            document.getElementById('v-origin').value = c.origin;
            document.getElementById('v-user').value = c.username;
            document.getElementById('v-pass').focus();
          };
          const bd = document.createElement('button'); bd.className = 'btn sm danger'; bd.type = 'button'; bd.textContent = 'Delete';
          bd.onclick = () => {
            revealedPasswords.delete(key);
            send('vault_delete', JSON.stringify({origin: c.origin, username: c.username}));
          };
          row.appendChild(bs); row.appendChild(be); row.appendChild(bd);
          el.appendChild(row);
          vList.appendChild(el);
        }
      }
    }

    // Downloads list (DOM rebuilt only when download state/progress changes)
    const dlSig = JSON.stringify(h.downloads || []);
    if (dlSig !== lastDownloadsSig) {
      lastDownloadsSig = dlSig;
      const dList = document.getElementById('dl-list');
      dList.innerHTML = '';
      if (!h.downloads || h.downloads.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty'; empty.textContent = 'No downloads in this session';
        dList.appendChild(empty);
      } else {
        for (const d of h.downloads) {
          const el = document.createElement('div');
          el.className = 'list-item';
          const head = document.createElement('div'); head.className = 'item-head';
          const nm = document.createElement('span'); nm.className = 'item-name'; nm.textContent = d.name;
          const st = document.createElement('span'); st.className = 'item-stat';
          st.textContent = d.inProgress ? (d.percent + '%') : (d.complete ? 'Completed' : 'Canceled');
          head.appendChild(nm); head.appendChild(st); el.appendChild(head);
          const row = document.createElement('div'); row.className = 'btn-row';
          if (d.inProgress) {
            const bc = document.createElement('button'); bc.className = 'btn sm danger'; bc.type = 'button'; bc.textContent = 'Cancel';
            bc.onclick = () => send('dl_cancel', String(d.id));
            row.appendChild(bc);
          } else if (d.complete) {
            const bo = document.createElement('button'); bo.className = 'btn sm'; bo.type = 'button'; bo.textContent = 'Open File';
            bo.onclick = () => send('dl_open', d.path);
            row.appendChild(bo);
          }
          if (row.childElementCount > 0) el.appendChild(row);
          dList.appendChild(el);
        }
      }
    }

    // Installed WebApps list (DOM rebuilt only when installed apps change)
    const appsSig = JSON.stringify(h.webapps || []);
    if (appsSig !== lastAppsSig) {
      lastAppsSig = appsSig;
      const aList = document.getElementById('apps-list');
      aList.innerHTML = '';
      if (!h.webapps || h.webapps.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty'; empty.textContent = 'No Web Apps installed yet';
        aList.appendChild(empty);
      } else {
        for (const a of h.webapps) {
          const el = document.createElement('div');
          el.className = 'list-item';
          const nm = document.createElement('div'); nm.className = 'item-name'; nm.textContent = a.name;
          const sb = document.createElement('div'); sb.className = 'item-sub'; sb.textContent = a.url;
          el.appendChild(nm); el.appendChild(sb);
          const row = document.createElement('div'); row.className = 'btn-row';
          const bl = document.createElement('button'); bl.className = 'btn sm'; bl.type = 'button'; bl.textContent = 'Launch App';
          bl.onclick = () => send('webapp_launch', a.id);
          const bu = document.createElement('button'); bu.className = 'btn sm danger'; bu.type = 'button'; bu.textContent = 'Uninstall';
          bu.onclick = () => send('webapp_uninstall', a.id);
          row.appendChild(bl); row.appendChild(bu);
          el.appendChild(row);
          aList.appendChild(el);
        }
      }
    }
  };
</script>
</body>
</html>)HTML";
    static const std::string kCachedUrl = makeHtmlDataUrl(kHtml);
    return kCachedUrl;
}

}  // namespace explorer::app
