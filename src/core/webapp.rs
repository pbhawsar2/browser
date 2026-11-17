//! Installable Web App (PWA) metadata extractor, high-res squircle icon
//! renderer, XDG `.desktop` installer, and legacy Velox WebApp compatibility.

use gtk4::cairo;
use gtk4::gdk;
use gtk4::gio;
use gtk4::glib;
use gtk4::prelude::*;
use soup::prelude::*;
use soup::{Message, Session};
use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use webkit6::prelude::*;
use webkit6::WebView;

pub const WEBAPP_PREFIX: &str = "io.explorer.WebApp.";
pub const LEGACY_WEBAPP_PREFIX: &str = "io.velox.WebApp.";
const ICON_SIZES: &[i32] = &[48, 64, 128, 256, 512];

#[derive(Debug, Clone)]
pub struct WebAppMetadata {
    pub name: String,
    pub url: String,
    pub theme_color: Option<String>,
    pub icon_urls: Vec<String>,
}

#[derive(Debug, Clone)]
pub enum PreparedIcon {
    Svg(Vec<u8>),
    SquirclePngs(Vec<(i32, Vec<u8>)>),
}

impl PreparedIcon {
    pub fn preview_texture(&self) -> Option<gdk::Texture> {
        match self {
            PreparedIcon::Svg(bytes) => {
                let stream = gio::MemoryInputStream::from_bytes(&glib::Bytes::from(bytes));
                let pb = gtk4::gdk_pixbuf::Pixbuf::from_stream_at_scale(
                    &stream,
                    128,
                    128,
                    true,
                    gio::Cancellable::NONE,
                )
                .ok()?;
                Some(pixbuf_to_texture(&pb))
            }
            PreparedIcon::SquirclePngs(list) => list
                .iter()
                .rev()
                .find_map(|(_, bytes)| gdk::Texture::from_bytes(&glib::Bytes::from(bytes)).ok()),
        }
    }
}

#[derive(Debug, Clone)]
#[allow(dead_code)]
pub struct InstalledWebApp {
    pub desktop_id: String,
    pub name: String,
    pub url: String,
    pub desktop_path: PathBuf,
    pub preview_texture: Option<gdk::Texture>,
}

const EXTRACT_WEBAPP_META_JS: &str = r#"(() => {
  const clean = (s) => (s || '').replace(/\s+/g, ' ').trim();
  const abs = (h, base) => { try { return new URL(h, base).href; } catch { return ''; } };
  const parseSizes = (s) => {
    let best = 0;
    for (const part of String(s || '').split(/\s+/)) {
      const m = part.match(/^(\d+)x(\d+)$/i);
      if (m) best = Math.max(best, Math.min(Number(m[1]), Number(m[2])));
    }
    return best;
  };

  let appName = '';
  const ogSite = document.querySelector('meta[property="og:site_name"][content]');
  const appMeta = document.querySelector('meta[name="application-name"][content]');
  if (ogSite && clean(ogSite.content)) appName = clean(ogSite.content);
  else if (appMeta && clean(appMeta.content)) appName = clean(appMeta.content);
  else appName = clean(document.title).split(/\s+[-|—·]\s+/)[0] || location.hostname;

  const theme = document.querySelector('meta[name="theme-color"][content]')?.content || '';

  const cands = [];
  const push = (href, score) => {
    const u = abs(href, location.href);
    if (u && (u.startsWith('http:') || u.startsWith('https:') || u.startsWith('data:image/'))) {
      cands.push([score, u]);
    }
  };

  for (const el of document.querySelectorAll('link[rel~="apple-touch-icon"], link[rel~="apple-touch-icon-precomposed"], link[rel~="icon"], link[rel="shortcut icon"]')) {
    const rel = (el.getAttribute('rel') || '').toLowerCase();
    const sz = parseSizes(el.getAttribute('sizes'));
    const bonus = rel.includes('apple-touch') ? 400 : (rel.includes('fluid') ? 350 : 100);
    const svgBonus = (el.href || '').toLowerCase().includes('.svg') ? 600 : 0;
    push(el.getAttribute('href'), bonus + svgBonus + sz);
  }
  const tile = document.querySelector('meta[name="msapplication-TileImage"][content]');
  if (tile) push(tile.content, 240);

  const origin = location.origin;
  if (origin && origin.startsWith('http')) {
    push(origin + '/apple-touch-icon.png', 180);
    push(origin + '/favicon.ico', 50);
  }

  cands.sort((a, b) => b[0] - a[0]);
  const seen = new Set();
  const icons = [];
  for (const [, u] of cands) {
    if (!seen.has(u)) { seen.add(u); icons.push(u); }
    if (icons.length >= 6) break;
  }

  const manifestHref = document.querySelector('link[rel~="manifest"][href]')?.getAttribute('href') || '';
  return JSON.stringify({
    name: appName.slice(0, 64),
    url: location.href,
    theme: theme,
    manifest: manifestHref ? abs(manifestHref, location.href) : '',
    icons: icons
  });
})();"#;

pub fn applications_dir() -> PathBuf {
    glib::user_data_dir().join("applications")
}

pub fn icons_root_dir() -> PathBuf {
    glib::user_data_dir().join("icons/hicolor")
}

/// Asynchronously inspects page metadata & downloads/prepares crisp high-res icons.
pub fn extract_and_prepare_webapp(
    web_view: &WebView,
    on_ready: impl FnOnce(WebAppMetadata, PreparedIcon) + 'static,
) {
    extract_and_prepare_webapp_boxed(web_view, Box::new(on_ready));
}

pub fn extract_and_prepare_webapp_boxed(
    web_view: &WebView,
    on_ready: Box<dyn FnOnce(WebAppMetadata, PreparedIcon) + 'static>,
) {
    let fallback_title = web_view
        .title()
        .map(|s| s.to_string())
        .unwrap_or_default();
    let fallback_url = web_view.uri().map(|s| s.to_string()).unwrap_or_default();

    web_view.evaluate_javascript(
        EXTRACT_WEBAPP_META_JS,
        None,
        None,
        gio::Cancellable::NONE,
        move |res| {
            let raw = res
                .ok()
                .map(|v| v.to_str().to_string())
                .unwrap_or_default();
            let meta = parse_js_meta(&raw, &fallback_title, &fallback_url);
            let urls = meta.icon_urls.clone();
            let session = Session::new();
            try_next_icon(session, meta, urls, 0, Some(on_ready));
        },
    );
}

fn try_next_icon(
    session: Session,
    meta: WebAppMetadata,
    urls: Vec<String>,
    idx: usize,
    on_ready: Option<Box<dyn FnOnce(WebAppMetadata, PreparedIcon) + 'static>>,
) {
    let Some(cb) = on_ready else {
        return;
    };
    if idx >= urls.len() {
        let svg = generate_monogram_svg(&meta.name, &meta.url, meta.theme_color.as_deref());
        cb(meta, PreparedIcon::Svg(svg));
        return;
    }

    let url = urls[idx].clone();
    if let Some(rest) = url.strip_prefix("data:") {
        if let Some(bytes) = decode_data_uri(rest)
            && let Some(prep) = build_prepared_from_bytes(&bytes)
        {
            cb(meta, prep);
            return;
        }
        try_next_icon(session, meta, urls, idx + 1, Some(cb));
        return;
    }

    let Ok(msg) = Message::new("GET", &url) else {
        try_next_icon(session, meta, urls, idx + 1, Some(cb));
        return;
    };
    if let Some(hdrs) = msg.request_headers() {
        hdrs.append("User-Agent", crate::tuning::EXPLORER_USER_AGENT);
    }

    let session_c = session.clone();
    session.send_and_read_async(
        &msg,
        glib::Priority::DEFAULT,
        gio::Cancellable::NONE,
        move |res| {
            if let Ok(bytes) = res
                && let Some(prep) = build_prepared_from_bytes(&bytes)
            {
                cb(meta, prep);
                return;
            }
            try_next_icon(session_c, meta, urls, idx + 1, Some(cb));
        },
    );
}

fn build_prepared_from_bytes(bytes: &glib::Bytes) -> Option<PreparedIcon> {
    let slice = bytes.as_ref();
    if slice.len() < 32 {
        return None;
    }
    if is_svg_bytes(slice) {
        return Some(PreparedIcon::Svg(slice.to_vec()));
    }
    let tex = gdk::Texture::from_bytes(bytes).ok()?;
    if tex.width() < 24 || tex.height() < 24 {
        return None;
    }
    let mut pngs = Vec::with_capacity(ICON_SIZES.len());
    for &sz in ICON_SIZES {
        let png = render_squircle_png(&tex, sz).ok()?;
        pngs.push((sz, png));
    }
    Some(PreparedIcon::SquirclePngs(pngs))
}

fn render_squircle_png(tex: &gdk::Texture, size: i32) -> Result<Vec<u8>, String> {
    let w = tex.width();
    let h = tex.height();
    let stride = (w as usize) * 4;
    let mut bgra = vec![0u8; stride * (h as usize)];
    tex.download(&mut bgra, stride);

    let src_surf = cairo::ImageSurface::create_for_data(
        bgra,
        cairo::Format::ARgb32,
        w,
        h,
        stride as i32,
    )
    .map_err(|e| format!("cairo src surface failed: {e}"))?;

    let dst_surf = cairo::ImageSurface::create(cairo::Format::ARgb32, size, size)
        .map_err(|e| format!("cairo dst surface failed: {e}"))?;
    {
        let cr = cairo::Context::new(&dst_surf).map_err(|e| format!("cairo ctx: {e}"))?;
        let s = size as f64;
        let pad = s * 0.04;
        let inner = s - pad * 2.0;
        let r = inner * 0.23;

        // Clean squircle dark obsidian card backdrop matching Explorer icons.
        rounded_rect_path(&cr, pad, pad, inner, inner, r);
        cr.set_source_rgba(0.055, 0.082, 0.141, 1.0);
        let _ = cr.fill_preserve();
        cr.clip();

        let icon_pad = inner * 0.10;
        let target = inner - icon_pad * 2.0;
        let scale = (target / w as f64).min(target / h as f64);
        let draw_w = w as f64 * scale;
        let draw_h = h as f64 * scale;
        let dx = pad + (inner - draw_w) * 0.5;
        let dy = pad + (inner - draw_h) * 0.5;

        cr.translate(dx, dy);
        cr.scale(scale, scale);
        let _ = cr.set_source_surface(&src_surf, 0.0, 0.0);
        cr.source().set_filter(cairo::Filter::Good);
        let _ = cr.paint();
    }

    let dst_stride = dst_surf.stride() as usize;
    let raw = dst_surf
        .take_data()
        .map_err(|e| format!("cairo surface data failed: {e}"))?;
    let mem = gdk::MemoryTexture::new(
        size,
        size,
        gdk::MemoryFormat::B8g8r8a8Premultiplied,
        &glib::Bytes::from(&*raw),
        dst_stride,
    );
    Ok(mem.save_to_png_bytes().to_vec())
}

fn pixbuf_to_texture(pb: &gtk4::gdk_pixbuf::Pixbuf) -> gdk::Texture {
    let fmt = if pb.has_alpha() {
        gdk::MemoryFormat::R8g8b8a8
    } else {
        gdk::MemoryFormat::R8g8b8
    };
    gdk::MemoryTexture::new(
        pb.width(),
        pb.height(),
        fmt,
        &pb.read_pixel_bytes(),
        pb.rowstride() as usize,
    )
    .upcast()
}

fn rounded_rect_path(cr: &cairo::Context, x: f64, y: f64, w: f64, h: f64, r: f64) {
    let d = std::f64::consts::FRAC_PI_2;
    cr.new_sub_path();
    cr.arc(x + w - r, y + r, r, -d, 0.0);
    cr.arc(x + w - r, y + h - r, r, 0.0, d);
    cr.arc(x + r, y + h - r, r, d, 2.0 * d);
    cr.arc(x + r, y + r, r, 2.0 * d, 3.0 * d);
    cr.close_path();
}

pub fn generate_monogram_svg(app_name: &str, url: &str, theme_color: Option<&str>) -> Vec<u8> {
    let initial = app_name
        .chars()
        .find(|c| c.is_alphanumeric())
        .or_else(|| host_fallback_name(url).chars().find(|c| c.is_alphanumeric()))
        .unwrap_or('E')
        .to_ascii_uppercase();
    let accent = theme_color
        .map(str::trim)
        .filter(|s| s.starts_with('#') && (s.len() == 4 || s.len() == 7))
        .unwrap_or("#0EA5E9");

    format!(
        r##"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#0F172A"/>
      <stop offset="100%" stop-color="{accent}"/>
    </linearGradient>
  </defs>
  <rect x="6" y="6" width="116" height="116" rx="28" fill="url(#bg)"/>
  <text x="64" y="83" text-anchor="middle" font-family="Adwaita Sans, Inter, sans-serif" font-size="60" font-weight="800" fill="#F8FAFC">{initial}</text>
</svg>"##
    )
    .into_bytes()
}

pub fn sanitize_app_slug(raw_name: &str, url: &str) -> String {
    let mut base: String = raw_name
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();
    while base.contains("__") {
        base = base.replace("__", "_");
    }
    let clean = base.trim_matches('_').to_string();
    if !clean.is_empty() {
        return clean.chars().take(36).collect();
    }
    let host: String = host_fallback_name(url)
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();
    let trimmed = host.trim_matches('_');
    if trimmed.is_empty() {
        "WebApp".to_string()
    } else {
        trimmed.chars().take(36).collect()
    }
}

fn fnv1a_32(bytes: &[u8]) -> u32 {
    let mut h: u32 = 0x811C_9DC5;
    for &b in bytes {
        h ^= b as u32;
        h = h.wrapping_mul(0x0100_0193);
    }
    h
}

pub fn desktop_id_for(name: &str, url: &str) -> String {
    let slug = sanitize_app_slug(name, url);
    let hash = fnv1a_32(url.as_bytes());
    format!("{WEBAPP_PREFIX}{slug}_{hash:08x}")
}

pub fn install_webapp(
    name: &str,
    url: &str,
    icon: &PreparedIcon,
) -> Result<InstalledWebApp, String> {
    let exe = std::env::current_exe().map_err(|e| format!("current_exe failed: {e}"))?;
    install_webapp_in(name, url, icon, &exe, &applications_dir(), &icons_root_dir())
}

pub fn install_webapp_in(
    name: &str,
    url: &str,
    icon: &PreparedIcon,
    exe_path: &Path,
    apps_dir: &Path,
    icons_root: &Path,
) -> Result<InstalledWebApp, String> {
    let clean_name = if name.trim().is_empty() {
        host_fallback_name(url)
    } else {
        name.trim().to_string()
    };
    let desktop_id = desktop_id_for(&clean_name, url);

    match icon {
        PreparedIcon::Svg(svg_bytes) => {
            let dir = icons_root.join("scalable/apps");
            fs::create_dir_all(&dir).map_err(|e| format!("mkdir icon dir: {e}"))?;
            fs::write(dir.join(format!("{desktop_id}.svg")), svg_bytes)
                .map_err(|e| format!("write svg: {e}"))?;
        }
        PreparedIcon::SquirclePngs(pngs) => {
            for (sz, bytes) in pngs {
                let dir = icons_root.join(format!("{sz}x{sz}/apps"));
                fs::create_dir_all(&dir).map_err(|e| format!("mkdir {sz} icon dir: {e}"))?;
                fs::write(dir.join(format!("{desktop_id}.png")), bytes)
                    .map_err(|e| format!("write png: {e}"))?;
            }
        }
    }

    fs::create_dir_all(apps_dir).map_err(|e| format!("mkdir apps dir: {e}"))?;
    let desktop_path = apps_dir.join(format!("{desktop_id}.desktop"));
    let entry = build_desktop_entry(exe_path, &desktop_id, &clean_name, url);
    fs::write(&desktop_path, entry).map_err(|e| format!("write desktop: {e}"))?;

    if apps_dir == applications_dir() {
        refresh_xdg_caches_async(apps_dir.to_path_buf(), icons_root.to_path_buf());
    }

    Ok(InstalledWebApp {
        preview_texture: icon.preview_texture(),
        desktop_id,
        name: clean_name,
        url: url.to_string(),
        desktop_path,
    })
}

fn build_desktop_entry(exe: &Path, desktop_id: &str, name: &str, url: &str) -> String {
    let exe_arg = escape_exec_arg(&exe.to_string_lossy());
    let url_arg = escape_exec_arg(&format!("--app={url}"));
    let id_arg = escape_exec_arg(&format!("--app-id={desktop_id}"));
    let safe_name = name.replace(['\n', '\r'], " ");
    format!(
        "[Desktop Entry]\n\
         Version=1.0\n\
         Type=Application\n\
         Name={safe_name}\n\
         Comment=Installed web app via Explorer\n\
         Exec={exe_arg} {url_arg} {id_arg}\n\
         Icon={desktop_id}\n\
         Terminal=false\n\
         Categories=Network;WebBrowser;\n\
         StartupNotify=true\n\
         StartupWMClass={desktop_id}\n\
         X-Explorer-WebApp=true\n"
    )
}

pub fn escape_exec_arg(s: &str) -> String {
    let escaped_pct = s.replace('%', "%%");
    let mut out = String::with_capacity(escaped_pct.len() + 2);
    out.push('"');
    for ch in escaped_pct.chars() {
        if matches!(ch, '"' | '\\' | '$' | '`') {
            out.push('\\');
        }
        out.push(ch);
    }
    out.push('"');
    out
}

pub fn list_installed_webapps() -> Vec<InstalledWebApp> {
    list_installed_webapps_in(&applications_dir(), &icons_root_dir())
}

pub fn list_installed_webapps_in(apps_dir: &Path, icons_root: &Path) -> Vec<InstalledWebApp> {
    let Ok(entries) = fs::read_dir(apps_dir) else {
        return Vec::new();
    };
    let mut apps = Vec::new();
    for entry in entries.flatten() {
        let p = entry.path();
        let Some(fname) = p.file_name().and_then(|s| s.to_str()) else {
            continue;
        };
        if !(fname.ends_with(".desktop")
            && (fname.starts_with(WEBAPP_PREFIX) || fname.starts_with(LEGACY_WEBAPP_PREFIX)))
        {
            continue;
        }
        let Ok(txt) = fs::read_to_string(&p) else {
            continue;
        };
        if !txt.contains("X-Explorer-WebApp=true") && !txt.contains("X-Velox-WebApp=true") {
            continue;
        }

        let desktop_id = fname.trim_end_matches(".desktop").to_string();
        let mut name = desktop_id.clone();
        let mut url = String::new();
        for line in txt.lines() {
            if let Some(v) = line.strip_prefix("Name=") {
                name = v.trim().to_string();
            } else if let Some(v) = line.strip_prefix("Exec=")
                && let Some(idx) = v.find("--app=")
            {
                let rest = &v[idx + 6..];
                let end = rest.find("\" ").or_else(|| rest.find(' ')).unwrap_or(rest.len());
                url = rest[..end].trim_matches('"').replace("%%", "%");
            }
        }
        apps.push(InstalledWebApp {
            preview_texture: load_installed_icon_preview(icons_root, &desktop_id),
            desktop_id,
            name,
            url,
            desktop_path: p,
        });
    }
    apps.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    apps
}

fn load_installed_icon_preview(icons_root: &Path, desktop_id: &str) -> Option<gdk::Texture> {
    for sz in [128, 256, 64, 48] {
        let png = icons_root.join(format!("{sz}x{sz}/apps/{desktop_id}.png"));
        if let Ok(bytes) = fs::read(&png)
            && let Ok(tex) = gdk::Texture::from_bytes(&glib::Bytes::from(&bytes))
        {
            return Some(tex);
        }
    }
    let svg = icons_root.join(format!("scalable/apps/{desktop_id}.svg"));
    if let Ok(bytes) = fs::read(&svg) {
        let stream = gio::MemoryInputStream::from_bytes(&glib::Bytes::from(&bytes));
        if let Ok(pb) = gtk4::gdk_pixbuf::Pixbuf::from_stream_at_scale(
            &stream,
            128,
            128,
            true,
            gio::Cancellable::NONE,
        ) {
            return Some(pixbuf_to_texture(&pb));
        }
    }
    None
}

pub fn uninstall_webapp(app: &InstalledWebApp) -> Result<(), String> {
    uninstall_webapp_from(app, &applications_dir(), &icons_root_dir())
}

pub fn uninstall_webapp_from(
    app: &InstalledWebApp,
    apps_dir: &Path,
    icons_root: &Path,
) -> Result<(), String> {
    let _ = fs::remove_file(&app.desktop_path);
    for sz in ICON_SIZES {
        let _ = fs::remove_file(icons_root.join(format!("{sz}x{sz}/apps/{}.png", app.desktop_id)));
    }
    let _ = fs::remove_file(icons_root.join(format!("scalable/apps/{}.svg", app.desktop_id)));
    if apps_dir == applications_dir() {
        refresh_xdg_caches_async(apps_dir.to_path_buf(), icons_root.to_path_buf());
    }
    Ok(())
}

pub fn launch_webapp(app: &InstalledWebApp) {
    if let Ok(exe) = std::env::current_exe() {
        let _ = std::process::Command::new(exe)
            .arg(format!("--app={}", app.url))
            .arg(format!("--app-id={}", app.desktop_id))
            .spawn();
    }
}

fn refresh_xdg_caches_async(apps_dir: PathBuf, icons_root: PathBuf) {
    std::thread::spawn(move || {
        let _ = std::process::Command::new("update-desktop-database")
            .arg(&apps_dir)
            .status();
        let _ = std::process::Command::new("gtk4-update-icon-cache")
            .args(["-q", "-t", "-f"])
            .arg(&icons_root)
            .status();
    });
}

fn parse_js_meta(raw: &str, fallback_title: &str, fallback_url: &str) -> WebAppMetadata {
    let name = extract_json_str(raw, "name")
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| {
            if fallback_title.trim().is_empty() {
                host_fallback_name(fallback_url)
            } else {
                fallback_title.trim().to_string()
            }
        });
    let url = extract_json_str(raw, "url")
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| fallback_url.to_string());
    let theme_color = extract_json_str(raw, "theme").filter(|s| !s.is_empty());
    let icon_urls = extract_json_str_array(raw, "icons");

    WebAppMetadata {
        name,
        url,
        theme_color,
        icon_urls,
    }
}

fn extract_json_str(raw: &str, key: &str) -> Option<String> {
    let pat = format!("\"{key}\":\"");
    let start = raw.find(&pat)? + pat.len();
    let rest = &raw[start..];
    let mut out = String::new();
    let mut escaped = false;
    for ch in rest.chars() {
        if escaped {
            out.push(match ch {
                'n' => '\n',
                't' => '\t',
                '"' => '"',
                '\\' => '\\',
                _ => ch,
            });
            escaped = false;
        } else if ch == '\\' {
            escaped = true;
        } else if ch == '"' {
            return Some(out);
        } else {
            out.push(ch);
        }
    }
    None
}

fn extract_json_str_array(raw: &str, key: &str) -> Vec<String> {
    let pat = format!("\"{key}\":[");
    let Some(start) = raw.find(&pat).map(|i| i + pat.len()) else {
        return Vec::new();
    };
    let rest = &raw[start..];
    let Some(end) = rest.find(']') else {
        return Vec::new();
    };
    let mut items = Vec::new();
    let mut seen = HashSet::new();
    for seg in rest[..end].split("\",\"") {
        let cleaned = seg.trim().trim_matches('"').replace("\\/", "/");
        if !cleaned.is_empty() && seen.insert(cleaned.clone()) {
            items.push(cleaned);
        }
    }
    items
}

fn host_fallback_name(url: &str) -> String {
    url.split("://")
        .nth(1)
        .unwrap_or(url)
        .split('/')
        .next()
        .unwrap_or("WebApp")
        .trim_start_matches("www.")
        .to_string()
}

fn is_svg_bytes(bytes: &[u8]) -> bool {
    let head = std::str::from_utf8(&bytes[..bytes.len().min(256)])
        .unwrap_or("")
        .to_ascii_lowercase();
    head.contains("<svg")
}

fn decode_data_uri(rest: &str) -> Option<glib::Bytes> {
    let (meta, payload) = rest.split_once(',')?;
    if meta.ends_with(";base64") {
        base64_decode(payload).map(glib::Bytes::from_owned)
    } else {
        Some(glib::Bytes::from_owned(payload.as_bytes().to_vec()))
    }
}

fn base64_decode(input: &str) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(input.len() * 3 / 4);
    let mut buf: u32 = 0;
    let mut bits = 0;
    for &b in input.as_bytes() {
        let val = match b {
            b'A'..=b'Z' => b - b'A',
            b'a'..=b'z' => b - b'a' + 26,
            b'0'..=b'9' => b - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' | b'\n' | b'\r' | b' ' => continue,
            _ => return None,
        };
        buf = (buf << 6) | val as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push(((buf >> bits) & 0xFF) as u8);
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn webapp_install_list_and_uninstall_roundtrip() {
        let tmp = std::env::temp_dir().join(format!("explorer_webapp_test_{}", std::process::id()));
        let apps = tmp.join("applications");
        let icons = tmp.join("icons");
        let _ = fs::remove_dir_all(&tmp);

        let svg = generate_monogram_svg("YouTube", "https://www.youtube.com/", Some("#FF0000"));
        let installed = install_webapp_in(
            "YouTube",
            "https://www.youtube.com/?v=100%20",
            &PreparedIcon::Svg(svg),
            Path::new("/home/piyush/.local/bin/explorer"),
            &apps,
            &icons,
        )
        .expect("install failed");

        assert!(installed.desktop_id.starts_with(WEBAPP_PREFIX));
        let listed = list_installed_webapps_in(&apps, &icons);
        assert_eq!(listed.len(), 1);
        assert_eq!(listed[0].name, "YouTube");
        assert_eq!(listed[0].url, "https://www.youtube.com/?v=100%20");

        uninstall_webapp_from(&listed[0], &apps, &icons).expect("uninstall failed");
        assert!(list_installed_webapps_in(&apps, &icons).is_empty());
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn sanitize_and_escape_edge_cases() {
        assert_eq!(sanitize_app_slug("YouTube Music!", "https://music.youtube.com"), "YouTube_Music");
        assert_eq!(escape_exec_arg("a%20b\"c"), "\"a%%20b\\\"c\"");
    }
}
