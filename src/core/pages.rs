//! Built-in Start Page (`explorer://start`) and clean network error templates.

/// Renders the obsidian Start Page aligned with Photos/video-player aesthetics
/// and powered by Google Search.
pub fn start_page_html(mode_badge: &str, va_summary: &str) -> String {
    let badge_esc = html_escape(mode_badge);
    let va_esc = html_escape(va_summary);
    format!(
        r##"<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Explorer</title>
<style>
  :root {{
    color-scheme: dark;
    --bg: #080C14;
    --card: #0E1524;
    --card-hover: #162238;
    --border: rgba(148, 163, 184, 0.14);
    --cyan: #38BDF8;
    --text: #F8FAFC;
    --muted: #94A3B8;
  }}
  * {{ box-sizing: border-box; }}
  html, body {{
    margin: 0; padding: 0; height: 100%;
    background: radial-gradient(ellipse at 50% 24%, #101D33 0%, #080C14 62%, #05080E 100%);
    color: var(--text);
    font-family: "Adwaita Sans", "Inter", system-ui, -apple-system, sans-serif;
    -webkit-font-smoothing: antialiased;
  }}
  .wrap {{
    min-height: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 32px 24px 48px;
    gap: 22px;
  }}
  .badge {{
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 6px 14px;
    border-radius: 999px;
    background: rgba(56, 189, 248, 0.11);
    border: 1px solid rgba(56, 189, 248, 0.28);
    color: #CBD5E1;
    font-size: 12.5px;
    font-weight: 600;
    letter-spacing: 0.2px;
  }}
  .brand {{
    display: flex;
    align-items: center;
    gap: 14px;
  }}
  .brand h1 {{
    margin: 0;
    font-size: 40px;
    font-weight: 800;
    letter-spacing: -0.6px;
    color: #F8FAFC;
  }}
  form.search {{
    width: min(640px, 92vw);
    position: relative;
  }}
  form.search input {{
    width: 100%;
    height: 48px;
    padding: 0 20px 0 46px;
    border-radius: 13px;
    border: 1px solid rgba(148, 163, 184, 0.20);
    background: #0F172A;
    color: #F8FAFC;
    font-size: 15.5px;
    outline: none;
    transition: border-color 140ms ease, box-shadow 140ms ease, background-color 140ms ease;
  }}
  form.search input:focus {{
    border-color: var(--cyan);
    background: #131E34;
    box-shadow: 0 0 0 3px rgba(56, 189, 248, 0.18);
  }}
  form.search svg.lens {{
    position: absolute;
    left: 16px;
    top: 50%;
    transform: translateY(-50%);
    width: 18px; height: 18px;
    stroke: #94A3B8;
    pointer-events: none;
  }}
  .tiles {{
    display: grid;
    grid-template-columns: repeat(6, minmax(112px, 134px));
    gap: 12px;
    max-width: 900px;
  }}
  @media (max-width: 780px) {{
    .tiles {{ grid-template-columns: repeat(3, minmax(110px, 1fr)); }}
  }}
  .tile {{
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 14px 15px;
    border-radius: 12px;
    background: var(--card);
    border: 1px solid var(--border);
    text-decoration: none;
    color: var(--text);
    transition: transform 110ms ease, background-color 110ms ease, border-color 110ms ease;
  }}
  .tile:hover {{
    background: var(--card-hover);
    border-color: rgba(56, 189, 248, 0.42);
    transform: translateY(-1px);
  }}
  .tile b {{ font-size: 14.5px; font-weight: 700; color: #F1F5F9; }}
  .tile span {{ font-size: 12px; color: var(--muted); }}
  .foot {{
    margin-top: 6px;
    font-family: ui-monospace, "JetBrains Mono", monospace;
    font-size: 12px;
    color: #64748B;
  }}
</style>
</head>
<body>
  <main class="wrap">
    <div class="badge">{badge_esc}</div>
    <div class="brand">
      <svg width="46" height="46" viewBox="0 0 128 128" aria-hidden="true">
        <defs>
          <radialGradient id="g" cx="38%" cy="34%" r="65%">
            <stop offset="0%" stop-color="#38BDF8"/>
            <stop offset="55%" stop-color="#0284C7"/>
            <stop offset="100%" stop-color="#0F172A"/>
          </radialGradient>
          <linearGradient id="ring" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#FDE047"/>
            <stop offset="50%" stop-color="#FB923C"/>
            <stop offset="100%" stop-color="#F43F5E"/>
          </linearGradient>
        </defs>
        <circle cx="64" cy="64" r="52" fill="url(#g)"/>
        <path d="M98 28 C114 48, 112 86, 86 105 C62 122, 26 112, 16 84 C26 98, 52 104, 72 90 C92 76, 98 48, 98 28 Z" fill="url(#ring)" opacity="0.94"/>
        <polygon points="64,34 73,55 94,64 73,73 64,94 55,73 34,64 55,55" fill="#F8FAFC"/>
      </svg>
      <h1>Explorer</h1>
    </div>
    <form class="search" onsubmit="event.preventDefault(); const v = this.q.value.trim(); if (!v) return; if (/^https?:\/\//i.test(v)) location.href=v; else if (!/\s/.test(v) && /\.[a-z]{{2,}}($|\/|\?)/i.test(v)) location.href='https://'+v; else location.href='https://www.google.com/search?q='+encodeURIComponent(v);">
      <svg class="lens" viewBox="0 0 24 24" fill="none" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="7"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>
      <input name="q" type="text" placeholder="Search with Google or enter URL..." autofocus autocomplete="off">
    </form>
    <div class="tiles">
      <a class="tile" href="https://www.google.com/"><b>Google</b><span>Search &amp; Web</span></a>
      <a class="tile" href="https://www.youtube.com/"><b>YouTube</b><span>VP9 VA-API HW</span></a>
      <a class="tile" href="https://github.com/"><b>GitHub</b><span>Code &amp; Repos</span></a>
      <a class="tile" href="https://en.wikipedia.org/"><b>Wikipedia</b><span>Reference</span></a>
      <a class="tile" href="https://www.amazon.in/"><b>Amazon</b><span>Shopping</span></a>
      <a class="tile" href="https://www.flipkart.com/"><b>Flipkart</b><span>Shopping</span></a>
    </div>
    <div class="foot">Zero-Copy DMA-BUF · {va_esc}</div>
  </main>
</body>
</html>"##
    )
}

pub fn error_page_html(failed_url: &str, reason: &str) -> String {
    let url_esc = html_escape(failed_url);
    let msg_esc = html_escape(reason);
    format!(
        r##"<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Unable to load page</title>
<style>
  html, body {{
    margin: 0; height: 100%; background: #080C14; color: #F1F5F9;
    font-family: "Adwaita Sans", system-ui, sans-serif;
    display: flex; align-items: center; justify-content: center;
  }}
  .card {{
    width: min(520px, 90vw);
    background: #0E1524;
    border: 1px solid rgba(148, 163, 184, 0.16);
    border-radius: 14px;
    padding: 28px;
  }}
  h2 {{ margin: 0 0 8px; font-size: 20px; }}
  p {{ margin: 0 0 18px; color: #94A3B8; font-size: 14px; line-height: 1.5; word-break: break-word; }}
  button {{
    background: #0284C7; color: #fff; border: none; border-radius: 8px;
    padding: 10px 18px; font-size: 14px; font-weight: 600; cursor: pointer;
  }}
  button:hover {{ background: #0EA5E9; }}
</style>
</head>
<body>
  <div class="card">
    <h2>Page could not be loaded</h2>
    <p>{url_esc}<br><br>{msg_esc}</p>
    <button onclick="location.reload()">Try Again</button>
  </div>
</body>
</html>"##
    )
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}
