#pragma once
// Built-in zero-network-latency dark Start Page featuring the official Explorer icon.

#include "explorer/app/chrome_ui.hpp"
#include <string>

namespace explorer::app {

inline std::string startPageDataUrl() {
    static const char* kHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>New Tab — Explorer</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  ::-webkit-scrollbar { width: 8px; height: 8px; background: #080C14; }
  ::-webkit-scrollbar-track { background: #080C14; }
  ::-webkit-scrollbar-thumb { background: rgba(56,189,248,0.38); border-radius: 999px; border: 2px solid #080C14; }
  ::-webkit-scrollbar-thumb:hover { background: rgba(56,189,248,0.75); }
  ::-webkit-scrollbar-button, ::-webkit-scrollbar-corner { display: none; width: 0; height: 0; }
  body {
    background: radial-gradient(circle at 50% 18%, #132038 0%, #080C14 66%);
    color: #F8FAFC;
    font-family: "Adwaita Sans", Inter, system-ui, -apple-system, sans-serif;
    -webkit-font-smoothing: antialiased;
    -moz-osx-font-smoothing: grayscale;
    text-rendering: optimizeLegibility;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 24px;
    padding: 28px;
  }
  .hero {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
  }
  .logo { width: 76px; height: 76px; filter: drop-shadow(0 10px 24px rgba(56,189,248,0.22)); }
  .brand {
    font-size: 34px;
    font-weight: 700;
    letter-spacing: -0.025em;
    color: #FFFFFF;
  }
  .brand span { color: #38BDF8; }
  .badge {
    font-size: 13px;
    font-weight: 500;
    color: #CBD5E1;
    background: rgba(19, 28, 44, 0.92);
    border: 1px solid #334155;
    border-radius: 999px;
    padding: 6px 16px;
  }
  form {
    width: min(680px, 92vw);
    display: flex;
    background: #111827;
    border: 1.5px solid #334155;
    border-radius: 14px;
    padding: 6px;
    box-shadow: 0 14px 40px rgba(0,0,0,0.48);
    transition: border-color 140ms ease, background-color 140ms ease;
  }
  form:focus-within {
    background: #131E33;
    border-color: #38BDF8;
  }
  input {
    flex: 1;
    background: transparent;
    border: none;
    outline: none;
    color: #FFFFFF;
    font-size: 16px;
    font-weight: 500;
    padding: 11px 16px;
    caret-color: #38BDF8;
  }
  input::placeholder { color: #94A3B8; font-weight: 400; }
  button {
    background: #38BDF8;
    color: #080C14;
    border: none;
    border-radius: 10px;
    padding: 0 22px;
    font-weight: 700;
    font-size: 14.5px;
    cursor: pointer;
    transition: background-color 120ms ease;
  }
  button:hover { background: #7DD3FC; }
  .links {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    justify-content: center;
    max-width: 680px;
  }
  a {
    color: #F1F5F9;
    text-decoration: none;
    background: #131C2C;
    border: 1px solid #283548;
    border-radius: 10px;
    padding: 10px 18px;
    font-size: 14px;
    font-weight: 500;
    transition: border-color .14s, color .14s, background-color .14s;
  }
  a:hover { background: #18273F; border-color: #38BDF8; color: #FFFFFF; }
</style>
</head>
<body>
  <div class="hero">
    <svg class="logo" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">
      <defs>
        <radialGradient id="g" cx="38%" cy="32%" r="68%">
          <stop offset="0%" stop-color="#38BDF8"/>
          <stop offset="38%" stop-color="#0284C7"/>
          <stop offset="74%" stop-color="#1E3A8A"/>
          <stop offset="100%" stop-color="#090F24"/>
        </radialGradient>
        <linearGradient id="a" x1="12%" y1="18%" x2="88%" y2="86%">
          <stop offset="0%" stop-color="#FDE047"/>
          <stop offset="30%" stop-color="#FB923C"/>
          <stop offset="65%" stop-color="#F43F5E"/>
          <stop offset="100%" stop-color="#9333EA"/>
        </linearGradient>
      </defs>
      <circle cx="256" cy="256" r="196" fill="url(#g)"/>
      <path d="M312,62 C418,90 482,196 456,308 C430,420 322,492 206,474 C94,456 22,356 36,244 C46,164 98,96 168,66 C122,110 100,174 112,240 C128,328 210,390 298,378 C374,368 426,300 422,222 C418,154 374,94 312,62 Z" fill="url(#a)"/>
      <g transform="translate(256 244) rotate(36)">
        <circle cx="0" cy="0" r="88" fill="none" stroke="#E0F2FE" stroke-opacity="0.38" stroke-width="5"/>
        <polygon points="0,-108 -28,0 0,0" fill="#FFFFFF"/>
        <polygon points="0,-108 28,0 0,0" fill="#38BDF8"/>
        <polygon points="0,108 -28,0 0,0" fill="#1E40AF"/>
        <polygon points="0,108 28,0 0,0" fill="#0B1226"/>
        <circle cx="0" cy="0" r="15" fill="#FFFFFF"/>
        <circle cx="0" cy="0" r="7.5" fill="#0284C7"/>
      </g>
    </svg>
    <div class="brand">Explorer</div>
    <div class="badge">Blink + V8 · Brave Shield · AMD Cezanne Hardware Acceleration</div>
  </div>
  <form onsubmit="event.preventDefault(); const q=document.getElementById('q').value.trim(); if(!q) return; location.href = (!q.includes(' ') && q.includes('.')) ? (q.startsWith('http')?q:'https://'+q) : 'https://www.google.com/search?q='+encodeURIComponent(q);">
    <input id="q" type="text" placeholder="Search Google or enter a URL..." autofocus>
    <button type="submit">Search</button>
  </form>
  <div class="links">
    <a href="https://www.google.com">Google</a>
    <a href="https://www.youtube.com">YouTube</a>
    <a href="https://github.com">GitHub</a>
    <a href="https://browserbench.org/Speedometer3.1/">Speedometer 3.1</a>
    <a href="https://www.netflix.com">Netflix</a>
    <a href="https://www.amazon.in">Amazon</a>
  </div>
</body>
</html>)HTML";
    static const std::string kCachedUrl = makeHtmlDataUrl(kHtml);
    return kCachedUrl;
}

}  // namespace explorer::app
