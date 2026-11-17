//! Main browser window coordinator: unified top bezel, sleek horizontal top
//! tab bar (zero left panel), pre-warmed spare tab pool, +5% default DPI zoom,
//! shortcuts, fullscreen video, crash-safe session restore, and UI snapshotter.

use super::apps_popover::AppsMenuUi;
use super::downloads::DownloadManagerUi;
use super::find_bar::FindBarUi;
use super::navigation::{self, HOME_URL};
use super::pages;
use super::power_popover::PowerHudUi;
use super::webapp_dialog;
use super::win_controls::{self, WinCtrlButton, WinCtrlKind};
use crate::tuning::{self, EngineTuning, PowerSupervisor};
use gtk4::cairo;
use gtk4::gdk;
use gtk4::gio;
use gtk4::glib;
use gtk4::graphene;
use gtk4::gsk;
use gtk4::prelude::*;
use gtk4::{
    Align, Box as GtkBox, Button, Entry, Label, Orientation, Overlay, ProgressBar, Revealer,
    RevealerTransitionType, ScrolledWindow, Stack, WidgetPaintable, WindowHandle,
};
use libadwaita as adw;
use adw::prelude::*;
use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Duration;
use webkit6::prelude::*;
use webkit6::{
    CacheModel, CookiePersistentStorage, LoadEvent, NetworkSession, Settings, SnapshotOptions,
    SnapshotRegion, UserContentManager, WebContext, WebView,
};

const DEFAULT_PAGE_ZOOM: f64 = 1.05;

fn configured_default_zoom() -> f64 {
    std::env::var("EXPLORER_DEFAULT_ZOOM")
        .ok()
        .and_then(|v| v.parse::<f64>().ok())
        .filter(|&z| (0.5..=2.5).contains(&z))
        .unwrap_or(DEFAULT_PAGE_ZOOM)
}

#[derive(Clone)]
pub struct LaunchConfig {
    pub initial_urls: Vec<String>,
    pub app_mode_url: Option<String>,
    pub app_id: Option<String>,
    pub profile_dir: PathBuf,
    pub cache_dir: PathBuf,
    pub hardware_accel: bool,
}

#[derive(Clone)]
struct TabRecord {
    id: u64,
    view: WebView,
    pill_box: GtkBox,
    title_lbl: Label,
    current_url: Rc<RefCell<String>>,
    failed_url: Rc<RefCell<Option<String>>>,
}

#[derive(Clone)]
struct BrowserState {
    window: adw::ApplicationWindow,
    is_app_mode: bool,
    profile_dir: PathBuf,
    hardware_accel: bool,
    web_context: WebContext,
    network_session: NetworkSession,
    ucm: UserContentManager,
    supervisor: PowerSupervisor,
    url_entry: Entry,
    app_title_lbl: Label,
    back_btn: Button,
    fwd_btn: Button,
    reload_btn: Button,
    apps_menu: AppsMenuUi,
    progress_bar: ProgressBar,
    tab_bar_wrap: GtkBox,
    tab_strip: GtkBox,
    stack: Stack,
    find_bar: FindBarUi,
    power_hud: PowerHudUi,
    tabs: Rc<RefCell<Vec<TabRecord>>>,
    active_tab: Rc<Cell<Option<u64>>>,
    next_id: Rc<Cell<u64>>,
    spare_tab: Rc<RefCell<Option<WebView>>>,
    spare_scheduled: Rc<Cell<bool>>,
    html_fullscreen: Rc<Cell<bool>>,
    media_pages: Rc<RefCell<HashSet<u64>>>,
    session_save_timer: Rc<RefCell<Option<glib::SourceId>>>,
    closing: Rc<Cell<bool>>,
}

thread_local! {
    static ACTIVE_CONTROLLER: RefCell<Option<BrowserState>> = const { RefCell::new(None) };
}

pub fn open_urls_if_running(urls: &[String]) -> bool {
    ACTIVE_CONTROLLER.with(|c| {
        let Some(st) = c.borrow().clone() else {
            return false;
        };
        for (i, u) in urls.iter().enumerate() {
            st.open_new_tab(Some(u), i == urls.len() - 1);
        }
        st.window.present();
        true
    })
}

pub fn trigger_install_webapp_if_running() {
    ACTIVE_CONTROLLER.with(|c| {
        if let Some(st) = c.borrow().clone()
            && !st.is_app_mode
            && let Some(wv) = st.active_webview()
        {
            let apps_c = st.apps_menu.clone();
            webapp_dialog::show_install_webapp_dialog(&st.window, &wv, move || {
                apps_c.refresh();
            });
        }
    });
}

/// Captures a composite PNG of the live GTK chrome + active WebKit viewport.
pub fn capture_snapshot_if_running(out_path: &str) {
    let st = ACTIVE_CONTROLLER.with(|c| c.borrow().clone());
    let Some(st) = st else {
        return;
    };
    let win = st.window.clone();
    let stack = st.stack.clone();
    let dest = PathBuf::from(out_path);
    let w = win.width().max(1);
    let h = win.height().max(1);

    // Paint live GTK widgets (top bezel, tabs, URL bar, power pill, window controls)
    let paintable = WidgetPaintable::new(Some(&win));
    let snap = gtk4::Snapshot::new();
    paintable.snapshot(&snap, w as f64, h as f64);
    let Some(node) = snap.to_node() else {
        return;
    };
    let renderer = win
        .renderer()
        .unwrap_or_else(|| gsk::CairoRenderer::new().upcast());
    let ui_tex = renderer.render_texture(&node, Some(&graphene::Rect::new(0.0, 0.0, w as f32, h as f32)));

    let stack_pos = stack
        .compute_point(&win, &graphene::Point::zero())
        .unwrap_or_else(|| graphene::Point::new(0.0, 44.0));
    let stack_x = stack_pos.x() as f64;
    let stack_y = stack_pos.y() as f64;

    if let Some(wv) = st.active_webview() {
        wv.snapshot(
            SnapshotRegion::Visible,
            SnapshotOptions::NONE,
            gio::Cancellable::NONE,
            move |res| {
                let web_tex = res.ok();
                let _ = write_composite_png(&dest, w, h, &ui_tex, web_tex.as_ref(), stack_x, stack_y);
            },
        );
    } else {
        let _ = write_composite_png(&dest, w, h, &ui_tex, None, stack_x, stack_y);
    }
}

fn write_composite_png(
    dest: &Path,
    w: i32,
    h: i32,
    ui_tex: &gdk::Texture,
    web_tex: Option<&gdk::Texture>,
    web_x: f64,
    web_y: f64,
) -> Result<(), String> {
    let mut ui_buf = vec![0u8; (ui_tex.width() as usize) * 4 * (ui_tex.height() as usize)];
    ui_tex.download(&mut ui_buf, (ui_tex.width() as usize) * 4);
    let ui_surf = cairo::ImageSurface::create_for_data(
        ui_buf,
        cairo::Format::ARgb32,
        ui_tex.width(),
        ui_tex.height(),
        ui_tex.width() * 4,
    )
    .map_err(|e| format!("ui surface: {e}"))?;

    let out_surf = cairo::ImageSurface::create(cairo::Format::ARgb32, w, h)
        .map_err(|e| format!("out surface: {e}"))?;
    {
        let cr = cairo::Context::new(&out_surf).map_err(|e| format!("ctx: {e}"))?;
        let _ = cr.set_source_surface(&ui_surf, 0.0, 0.0);
        let _ = cr.paint();

        if let Some(wt) = web_tex {
            let mut w_buf = vec![0u8; (wt.width() as usize) * 4 * (wt.height() as usize)];
            wt.download(&mut w_buf, (wt.width() as usize) * 4);
            let w_surf = cairo::ImageSurface::create_for_data(
                w_buf,
                cairo::Format::ARgb32,
                wt.width(),
                wt.height(),
                wt.width() * 4,
            )
            .map_err(|e| format!("web surface: {e}"))?;
            let _ = cr.set_source_surface(&w_surf, web_x, web_y);
            let _ = cr.paint();
        }
    }

    let stride = out_surf.stride() as usize;
    let data = out_surf.take_data().map_err(|e| format!("take_data: {e}"))?;
    let mem = gdk::MemoryTexture::new(
        w,
        h,
        gdk::MemoryFormat::B8g8r8a8Premultiplied,
        &glib::Bytes::from(&*data),
        stride,
    );
    if let Some(parent) = dest.parent() {
        let _ = fs::create_dir_all(parent);
    }
    fs::write(dest, mem.save_to_png_bytes().as_ref()).map_err(|e| format!("write png: {e}"))
}

pub fn build_and_present(app: &adw::Application, cfg: LaunchConfig) {
    let is_app_mode = cfg.app_mode_url.is_some();
    let on_battery = tuning::is_on_battery_boot();
    let igpu_node = tuning::detect_igpu_render_node();
    let va_caps = tuning::probe_va_capabilities(&igpu_node);
    let supervisor = PowerSupervisor::new(on_battery, va_caps);
    supervisor.apply_timer_slack();

    let data_dir = cfg.profile_dir.join("data");
    let cookies_path = cfg.profile_dir.join("cookies.sqlite");
    let filters_dir = cfg.cache_dir.join("filters");
    let _ = fs::create_dir_all(&data_dir);
    let _ = fs::create_dir_all(&cfg.cache_dir);

    let network_session = NetworkSession::new(
        Some(&data_dir.to_string_lossy()),
        Some(&cfg.cache_dir.to_string_lossy()),
    );
    if let Some(cm) = network_session.cookie_manager() {
        cm.set_persistent_storage(&cookies_path.to_string_lossy(), CookiePersistentStorage::Sqlite);
    }
    EngineTuning::prewarm_network(&network_session);

    let web_context = WebContext::default().unwrap_or_else(WebContext::new);
    web_context.set_cache_model(CacheModel::WebBrowser);
    web_context.set_spell_checking_enabled(std::env::var_os("EXPLORER_NO_SPELLCHECK").is_none());
    if let Some(ext_dir) = find_webext_dir() {
        web_context.set_web_process_extensions_directory(&ext_dir.to_string_lossy());
    }
    supervisor.push_extension_policy(&web_context);

    let ucm = EngineTuning::build_content_manager(
        &filters_dir,
        supervisor.hw_only_video(),
        supervisor.is_battery_mode(),
    );

    let default_title = if is_app_mode { "Explorer App" } else { "Explorer" };
    let window = adw::ApplicationWindow::builder()
        .application(app)
        .title(default_title)
        .default_width(1360)
        .default_height(840)
        .build();
    window.add_css_class("browser-root");

    if let Some(ref custom_id) = cfg.app_id {
        window.set_icon_name(Some(custom_id));
    } else {
        window.set_icon_name(Some("io.explorer.Browser"));
    }

    // Top Bezel controls (zero left-sidebar toggle button — all uniform 34x32 pills)
    let back_btn = make_nav_btn("go-previous-symbolic", "Back (Alt+Left)");
    let fwd_btn = make_nav_btn("go-next-symbolic", "Forward (Alt+Right)");
    let reload_btn = make_nav_btn("view-refresh-symbolic", "Reload (Ctrl+R)");
    back_btn.set_sensitive(false);
    fwd_btn.set_sensitive(false);

    let url_entry = Entry::builder()
        .placeholder_text("Search with Google or enter website name")
        .primary_icon_name("system-search-symbolic")
        .css_classes(["url-bar"])
        .hexpand(true)
        .valign(Align::Center)
        .visible(!is_app_mode)
        .build();

    let app_title_lbl = Label::builder()
        .label("")
        .ellipsize(gtk4::pango::EllipsizeMode::End)
        .css_classes(["app-header-title"])
        .hexpand(true)
        .valign(Align::Center)
        .visible(is_app_mode)
        .build();

    let dl_ui = DownloadManagerUi::new();
    dl_ui.button.set_valign(Align::Center);
    {
        let dl_c = dl_ui.clone();
        network_session.connect_download_started(move |_, dl| dl_c.track_download(dl));
    }

    let apps_menu = AppsMenuUi::new(is_app_mode);

    let top_new_tab_btn = Button::builder()
        .icon_name("list-add-symbolic")
        .tooltip_text("New Tab (Ctrl+T)")
        .has_frame(false)
        .focus_on_click(false)
        .valign(Align::Center)
        .visible(!is_app_mode)
        .build();
    top_new_tab_btn.add_css_class("nav-btn");

    let min_ctrl = {
        let w = window.clone();
        win_controls::make_win_ctrl_button(WinCtrlKind::Minimize, "Minimize", move || w.minimize())
    };
    let max_ctrl = {
        let w = window.clone();
        win_controls::make_win_ctrl_button(WinCtrlKind::Maximize, "Maximize / Restore", move || {
            if w.is_maximized() {
                w.unmaximize();
            } else {
                w.maximize();
            }
        })
    };
    let fs_ctrl = {
        let w = window.clone();
        win_controls::make_win_ctrl_button(WinCtrlKind::Fullscreen, "Toggle Fullscreen (F11)", move || {
            if w.is_fullscreen() {
                w.unfullscreen();
            } else {
                w.fullscreen();
            }
        })
    };
    let close_ctrl = {
        let w = window.clone();
        win_controls::make_win_ctrl_button(WinCtrlKind::Close, "Close window", move || w.close())
    };

    sync_win_ctrl_states(&window, &max_ctrl, &fs_ctrl);

    let tabs: Rc<RefCell<Vec<TabRecord>>> = Rc::new(RefCell::new(Vec::new()));
    let media_pages: Rc<RefCell<HashSet<u64>>> = Rc::new(RefCell::new(HashSet::new()));
    let active_tab: Rc<Cell<Option<u64>>> = Rc::new(Cell::new(None));

    let power_hud = {
        let sup = supervisor.clone();
        let tabs_p = tabs.clone();
        PowerHudUi::new(supervisor.clone(), web_context.clone(), move || {
            let js = tuning::content_blocker::policy_update_js(
                sup.hw_only_video(),
                sup.is_battery_mode(),
            );
            for rec in tabs_p.borrow().iter() {
                rec.view.evaluate_javascript(&js, None, None, gio::Cancellable::NONE, |_| {});
            }
        })
    };
    power_hud.pill_button.set_valign(Align::Center);

    // Unified horizontal top bezel (navigation + wide omnibar + actions + window controls)
    let header_hbox = GtkBox::new(Orientation::Horizontal, 5);
    header_hbox.add_css_class("top-bezel-inner");
    header_hbox.set_valign(Align::Center);

    header_hbox.append(&back_btn);
    header_hbox.append(&fwd_btn);
    header_hbox.append(&reload_btn);

    if is_app_mode {
        header_hbox.append(&app_title_lbl);
    } else {
        let clamp = adw::Clamp::builder()
            .maximum_size(880)
            .tightening_threshold(480)
            .hexpand(true)
            .valign(Align::Center)
            .child(&url_entry)
            .build();
        clamp.set_margin_start(6);
        clamp.set_margin_end(6);
        header_hbox.append(&clamp);
    }

    header_hbox.append(&dl_ui.button);
    header_hbox.append(&power_hud.pill_button);
    header_hbox.append(&top_new_tab_btn);
    header_hbox.append(&apps_menu.button);

    // Clean visual gap between page/browser actions and window controls.
    min_ctrl.widget().set_margin_start(4);
    header_hbox.append(min_ctrl.widget());
    header_hbox.append(max_ctrl.widget());
    header_hbox.append(fs_ctrl.widget());
    header_hbox.append(close_ctrl.widget());

    let handle = WindowHandle::new();
    handle.add_css_class("top-bezel");
    handle.set_child(Some(&header_hbox));

    let progress_bar = ProgressBar::builder()
        .css_classes(["load-progress"])
        .valign(Align::End)
        .can_target(false)
        .visible(false)
        .build();

    let header_overlay = Overlay::new();
    header_overlay.set_child(Some(&handle));
    header_overlay.add_overlay(&progress_bar);

    let header_revealer = Revealer::builder()
        .transition_type(RevealerTransitionType::SlideDown)
        .transition_duration(120)
        .reveal_child(true)
        .child(&header_overlay)
        .build();

    // Sleek Horizontal Top Tab Bar (NO left panel; strictly hidden when <= 1 tab)
    let tab_strip = GtkBox::new(Orientation::Horizontal, 6);
    tab_strip.add_css_class("tab-strip");
    let tab_scroll = ScrolledWindow::builder()
        .hscrollbar_policy(gtk4::PolicyType::Automatic)
        .vscrollbar_policy(gtk4::PolicyType::Never)
        .hexpand(true)
        .child(&tab_strip)
        .build();
    let tab_bar_wrap = GtkBox::new(Orientation::Horizontal, 0);
    tab_bar_wrap.add_css_class("tab-bar");
    tab_bar_wrap.set_visible(false);
    tab_bar_wrap.append(&tab_scroll);

    let find_bar = FindBarUi::new();
    let stack = Stack::builder()
        .hexpand(true)
        .vexpand(true)
        .build();

    let root_box = GtkBox::new(Orientation::Vertical, 0);
    root_box.append(&header_revealer);
    root_box.append(&tab_bar_wrap);
    root_box.append(&find_bar.revealer);
    root_box.append(&stack);
    window.set_content(Some(&root_box));

    let state = BrowserState {
        window: window.clone(),
        is_app_mode,
        profile_dir: cfg.profile_dir.clone(),
        hardware_accel: cfg.hardware_accel,
        web_context,
        network_session,
        ucm,
        supervisor,
        url_entry,
        app_title_lbl,
        back_btn,
        fwd_btn,
        reload_btn,
        apps_menu,
        progress_bar,
        tab_bar_wrap,
        tab_strip,
        stack,
        find_bar,
        power_hud,
        tabs,
        active_tab,
        next_id: Rc::new(Cell::new(1)),
        spare_tab: Rc::new(RefCell::new(None)),
        spare_scheduled: Rc::new(Cell::new(false)),
        html_fullscreen: Rc::new(Cell::new(false)),
        media_pages,
        session_save_timer: Rc::new(RefCell::new(None)),
        closing: Rc::new(Cell::new(false)),
    };

    wire_header_controls(&state, &top_new_tab_btn);
    wire_shortcuts(&state);
    wire_power_and_lifecycle(&state, &header_revealer, &max_ctrl, &fs_ctrl);

    if let Some(ref app_url) = cfg.app_mode_url {
        let resolved = navigation::resolve_input_to_url(app_url);
        state.open_new_tab(Some(&resolved), true);
    } else if !cfg.initial_urls.is_empty() {
        for (i, u) in cfg.initial_urls.iter().enumerate() {
            let resolved = navigation::resolve_input_to_url(u);
            state.open_new_tab(Some(&resolved), i == 0);
        }
    } else if let Some((saved_urls, saved_active)) = load_session(&state.profile_dir) {
        let count = saved_urls.len();
        for (i, u) in saved_urls.into_iter().enumerate() {
            let active = i == saved_active.min(count.saturating_sub(1));
            state.open_new_tab(Some(&u), active);
        }
    } else {
        state.open_new_tab(None, true);
    }

    if !is_app_mode && std::env::var_os("EXPLORER_NO_SESSION").is_none() {
        let _ = fs::write(state.profile_dir.join("running.lock"), format!("{}\n", std::process::id()));
    }

    if !is_app_mode {
        ACTIVE_CONTROLLER.with(|c| *c.borrow_mut() = Some(state.clone()));
        let st_map = state.clone();
        state.window.connect_map(move |_| st_map.schedule_spare_warmup(150));
    }

    window.present();

    // Sync D-Bus refresh rate & PPD after initial window map so cold launch stays instant.
    let sup_init = state.supervisor.clone();
    let closing_init = state.closing.clone();
    glib::idle_add_local_once(move || {
        if !closing_init.get() {
            sup_init.sync_system_power();
        }
    });
}

impl BrowserState {
    fn open_new_tab(&self, initial_url: Option<&str>, switch_to: bool) -> u64 {
        let id = self.next_id.get();
        self.next_id.set(id + 1);

        let target_url = initial_url
            .map(navigation::resolve_input_to_url)
            .unwrap_or_else(|| HOME_URL.to_string());

        let reuse_spare = if !self.is_app_mode {
            self.spare_tab.borrow_mut().take()
        } else {
            None
        };

        let (view, already_home) = match reuse_spare {
            Some(wv) => (wv, true),
            None => (self.create_configured_webview(None), false),
        };

        let title_lbl = Label::builder()
            .label(if navigation::is_internal_url(&target_url) {
                "New Tab"
            } else {
                &target_url
            })
            .xalign(0.0)
            .hexpand(true)
            .ellipsize(gtk4::pango::EllipsizeMode::End)
            .css_classes(["tab-title"])
            .build();

        let select_btn = Button::builder()
            .has_frame(false)
            .focus_on_click(false)
            .hexpand(true)
            .css_classes(["tab-select-btn"])
            .child(&title_lbl)
            .build();

        let close_btn = Button::builder()
            .icon_name("window-close-symbolic")
            .tooltip_text("Close tab (Ctrl+W)")
            .has_frame(false)
            .focus_on_click(false)
            .valign(Align::Center)
            .build();
        close_btn.add_css_class("tab-close");

        let pill_box = GtkBox::new(Orientation::Horizontal, 4);
        pill_box.add_css_class("tab-pill");
        pill_box.set_width_request(190);
        pill_box.set_valign(Align::Center);
        pill_box.append(&select_btn);
        pill_box.append(&close_btn);

        self.tab_strip.append(&pill_box);
        self.stack.add_named(&view, Some(&format!("tab-{id}")));

        let rec = TabRecord {
            id,
            view: view.clone(),
            pill_box,
            title_lbl,
            current_url: Rc::new(RefCell::new(target_url.clone())),
            failed_url: Rc::new(RefCell::new(None)),
        };

        {
            let st = self.clone();
            select_btn.connect_clicked(move |_| st.activate_tab(id));
        }
        {
            let st = self.clone();
            close_btn.connect_clicked(move |_| st.close_tab(id));
        }

        self.attach_tab_signals(&rec);
        self.tabs.borrow_mut().push(rec);
        self.sync_tab_bar_visibility();

        if target_url != HOME_URL {
            view.load_uri(&target_url);
        } else if !already_home {
            self.load_start_page_into(&view);
        }

        if switch_to || self.active_tab.get().is_none() {
            self.activate_tab(id);
        }
        if !self.is_app_mode {
            self.schedule_spare_warmup(180);
            self.schedule_session_save();
        }
        id
    }

    fn sync_tab_bar_visibility(&self) {
        let show = !self.is_app_mode && !self.html_fullscreen.get() && self.tabs.borrow().len() > 1;
        self.tab_bar_wrap.set_visible(show);
    }

    fn create_configured_webview(&self, related: Option<&WebView>) -> WebView {
        let settings = Settings::new();
        EngineTuning::tune_settings(&settings, self.hardware_accel);

        let mut builder = WebView::builder()
            .web_context(&self.web_context)
            .network_session(&self.network_session)
            .user_content_manager(&self.ucm)
            .settings(&settings);
        if let Some(rel) = related {
            builder = builder.related_view(rel);
        }
        let wv = builder.build();
        wv.set_zoom_level(configured_default_zoom());

        EngineTuning::attach_webview(&wv, &self.url_entry, &self.network_session);
        self.find_bar.attach_webview_signals(&wv);

        let sup = self.supervisor.clone();
        let st_msg = self.clone();
        wv.connect_user_message_received(move |_, msg| {
            if msg.name().as_deref() == Some("explorer.process-pid")
                && let Some(params) = msg.parameters()
                && let Some((page_id, pid)) = params.get::<(u64, i32)>()
            {
                sup.register_page_pid(page_id, pid);
                st_msg.reconcile_pids();
                return true;
            }
            false
        });

        wv
    }

    fn load_start_page_into(&self, wv: &WebView) {
        let html = pages::start_page_html(
            &format!(
                "{} · {}",
                if self.supervisor.is_battery_mode() {
                    "Battery Zero-Fan"
                } else {
                    "AC Turbo 144Hz"
                },
                self.supervisor.va_caps().summary()
            ),
            &self.supervisor.va_caps().summary(),
        );
        wv.load_html(&html, Some(HOME_URL));
    }

    fn schedule_spare_warmup(&self, delay_ms: u64) {
        if self.is_app_mode
            || self.closing.get()
            || self.spare_tab.borrow().is_some()
            || self.spare_scheduled.get()
        {
            return;
        }
        self.spare_scheduled.set(true);
        let st = self.clone();
        glib::timeout_add_local_once(Duration::from_millis(delay_ms), move || {
            st.spare_scheduled.set(false);
            if st.closing.get() || st.is_app_mode || st.spare_tab.borrow().is_some() {
                return;
            }
            let wv = st.create_configured_webview(None);
            st.load_start_page_into(&wv);
            *st.spare_tab.borrow_mut() = Some(wv);
        });
    }

    fn attach_tab_signals(&self, rec: &TabRecord) {
        let id = rec.id;

        {
            let st = self.clone();
            let rec_c = rec.clone();
            rec.view.connect_uri_notify(move |wv| {
                if let Some(uri) = wv.uri() {
                    let u = uri.to_string();
                    *rec_c.current_url.borrow_mut() = u;
                    if st.active_tab.get() == Some(id) {
                        st.sync_active_bar(&rec_c);
                    }
                    if !st.is_app_mode {
                        st.schedule_session_save();
                    }
                }
            });
        }

        {
            let st = self.clone();
            let rec_c = rec.clone();
            rec.view.connect_title_notify(move |wv| {
                let u = wv.uri().map(|s| s.to_string());
                let raw_t = wv.title().map(|s| s.to_string());
                let title = navigation::friendly_title(raw_t.as_deref(), u.as_deref());
                rec_c.title_lbl.set_text(&title);
                if st.active_tab.get() == Some(id) {
                    st.window.set_title(Some(&if st.is_app_mode {
                        title.clone()
                    } else {
                        format!("{title} — Explorer")
                    }));
                    st.app_title_lbl.set_text(&title);
                }
            });
        }

        {
            let st = self.clone();
            rec.view.connect_estimated_load_progress_notify(move |wv| {
                if st.active_tab.get() == Some(id) {
                    let frac = wv.estimated_load_progress();
                    st.progress_bar.set_fraction(frac);
                    st.progress_bar.set_visible(wv.is_loading() && frac < 0.99);
                }
            });
        }

        {
            let st = self.clone();
            let rec_c = rec.clone();
            rec.view.connect_load_changed(move |wv, ev| {
                if st.active_tab.get() == Some(id) {
                    st.sync_active_bar(&rec_c);
                }
                if ev == LoadEvent::Started {
                    *rec_c.failed_url.borrow_mut() = None;
                } else if ev == LoadEvent::Finished {
                    let js = tuning::content_blocker::policy_update_js(
                        st.supervisor.hw_only_video(),
                        st.supervisor.is_battery_mode(),
                    );
                    wv.evaluate_javascript(&js, None, None, gio::Cancellable::NONE, |_| {});
                }
            });
        }

        {
            let rec_c = rec.clone();
            rec.view.connect_load_failed(move |wv, _, uri, err| {
                // Benign cancellations (redirects, Stop button, new navigation) must not clobber the page.
                if err.matches(webkit6::NetworkError::Cancelled)
                    || err.matches(webkit6::PolicyError::FrameLoadInterruptedByPolicyChange)
                    || err.matches(gio::IOErrorEnum::Cancelled)
                {
                    return true;
                }
                let msg = err.message().to_string();
                if msg.eq_ignore_ascii_case("Load request cancelled") {
                    return true;
                }
                if rec_c.failed_url.borrow().as_deref() == Some(uri) {
                    return true;
                }
                *rec_c.failed_url.borrow_mut() = Some(uri.to_string());
                let html = pages::error_page_html(uri, &msg);
                wv.load_html(&html, Some(uri));
                true
            });
        }

        {
            let st = self.clone();
            rec.view.connect_is_playing_audio_notify(move |wv| {
                let pid = wv.page_id();
                {
                    let mut set = st.media_pages.borrow_mut();
                    if wv.is_playing_audio() {
                        set.insert(pid);
                    } else {
                        set.remove(&pid);
                    }
                }
                st.reconcile_pids();
            });
        }

        {
            let st = self.clone();
            rec.view.connect_create(move |wv, action| {
                let target_uri = action
                    .request()
                    .and_then(|r| r.uri())
                    .map(|u| u.to_string());
                if st.is_app_mode {
                    if let Some(ref u) = target_uri {
                        wv.load_uri(u);
                    }
                    return None;
                }
                let popup_id = st.open_new_tab(target_uri.as_deref(), true);
                st.find_tab(popup_id).map(|t| t.view.upcast::<gtk4::Widget>())
            });
        }

        {
            let st = self.clone();
            rec.view.connect_close(move |_| st.close_tab(id));
        }

        {
            let st = self.clone();
            rec.view.connect_enter_fullscreen(move |_| {
                st.html_fullscreen.set(true);
                st.sync_tab_bar_visibility();
                false
            });
        }
        {
            let st = self.clone();
            rec.view.connect_leave_fullscreen(move |_| {
                st.html_fullscreen.set(false);
                st.sync_tab_bar_visibility();
                false
            });
        }
    }

    fn activate_tab(&self, id: u64) {
        let Some(rec) = self.find_tab(id) else {
            return;
        };
        self.active_tab.set(Some(id));
        self.stack.set_visible_child_name(&format!("tab-{id}"));

        for r in self.tabs.borrow().iter() {
            if r.id == id {
                r.pill_box.add_css_class("active-tab");
            } else {
                r.pill_box.remove_css_class("active-tab");
            }
        }

        self.find_bar.set_active_view(Some(&rec.view));
        self.sync_active_bar(&rec);

        let raw_t = rec.view.title().map(|s| s.to_string());
        let uri = rec.view.uri().map(|s| s.to_string());
        let title = navigation::friendly_title(raw_t.as_deref(), uri.as_deref());
        self.window.set_title(Some(&if self.is_app_mode {
            title.clone()
        } else {
            format!("{title} — Explorer")
        }));
        self.app_title_lbl.set_text(&title);

        self.reconcile_pids();
        if !self.is_app_mode {
            self.schedule_session_save();
        }
    }

    fn sync_active_bar(&self, rec: &TabRecord) {
        let wv = &rec.view;
        self.back_btn.set_sensitive(wv.can_go_back());
        self.fwd_btn.set_sensitive(wv.can_go_forward());

        let loading = wv.is_loading();
        self.reload_btn.set_icon_name(if loading {
            "process-stop-symbolic"
        } else {
            "view-refresh-symbolic"
        });
        self.progress_bar.set_visible(loading);

        let cur = wv
            .uri()
            .map(|s| s.to_string())
            .unwrap_or_else(|| rec.current_url.borrow().clone());
        let is_http = cur.starts_with("http://") || cur.starts_with("https://");
        self.apps_menu.set_can_install(is_http);

        if !self.url_entry.has_focus() {
            let shown = navigation::display_url_for_bar(&cur);
            if self.url_entry.text().as_str() != shown {
                self.url_entry.set_text(&shown);
            }
        }
    }

    fn close_tab(&self, id: u64) {
        let (idx, removed_rec) = {
            let mut list = self.tabs.borrow_mut();
            let Some(pos) = list.iter().position(|t| t.id == id) else {
                return;
            };
            if list.len() == 1 && !self.is_app_mode {
                drop(list);
                if let Some(only) = self.find_tab(id) {
                    self.load_start_page_into(&only.view);
                }
                return;
            }
            (pos, list.remove(pos))
        };

        let page_id = removed_rec.view.page_id();
        self.supervisor.unregister_page(page_id);
        self.media_pages.borrow_mut().remove(&page_id);
        self.stack.remove(&removed_rec.view);
        self.tab_strip.remove(&removed_rec.pill_box);
        self.sync_tab_bar_visibility();

        let next_active = {
            let list = self.tabs.borrow();
            if list.is_empty() {
                None
            } else {
                Some(list[idx.min(list.len() - 1)].id)
            }
        };

        if let Some(nid) = next_active {
            if self.active_tab.get() == Some(id) {
                self.activate_tab(nid);
            }
        } else if self.is_app_mode {
            self.window.close();
            return;
        }
        if !self.is_app_mode {
            self.schedule_session_save();
        }
    }

    fn active_webview(&self) -> Option<WebView> {
        let id = self.active_tab.get()?;
        self.find_tab(id).map(|t| t.view)
    }

    fn find_tab(&self, id: u64) -> Option<TabRecord> {
        self.tabs.borrow().iter().find(|t| t.id == id).cloned()
    }

    fn navigate_active(&self, raw: &str) {
        let Some(wv) = self.active_webview() else {
            return;
        };
        let url = navigation::resolve_input_to_url(raw);
        if url == HOME_URL {
            self.load_start_page_into(&wv);
        } else {
            wv.load_uri(&url);
        }
        wv.grab_focus();
    }

    fn cycle_tab(&self, delta: isize) {
        let list = self.tabs.borrow();
        if list.len() <= 1 {
            return;
        }
        let cur_idx = self
            .active_tab
            .get()
            .and_then(|id| list.iter().position(|t| t.id == id))
            .unwrap_or(0) as isize;
        let len = list.len() as isize;
        let next = list[((cur_idx + delta).rem_euclid(len)) as usize].id;
        drop(list);
        self.activate_tab(next);
    }

    fn reconcile_pids(&self) {
        let mut active_pages = HashSet::new();
        if let Some(wv) = self.active_webview() {
            active_pages.insert(wv.page_id());
        }
        let media = self.media_pages.borrow().clone();
        self.supervisor.reconcile_tab_priorities(&active_pages, &media);
    }

    fn schedule_session_save(&self) {
        if self.is_app_mode
            || self.closing.get()
            || std::env::var_os("EXPLORER_NO_SESSION").is_some()
        {
            return;
        }
        if self.session_save_timer.borrow().is_some() {
            return;
        }
        let st = self.clone();
        let tid = glib::timeout_add_local_once(Duration::from_millis(600), move || {
            *st.session_save_timer.borrow_mut() = None;
            st.save_session_now();
        });
        *self.session_save_timer.borrow_mut() = Some(tid);
    }

    fn save_session_now(&self) {
        if self.is_app_mode || std::env::var_os("EXPLORER_NO_SESSION").is_some() {
            return;
        }
        let list = self.tabs.borrow();
        let active_id = self.active_tab.get();
        let mut active_idx = 0usize;
        let mut urls = Vec::with_capacity(list.len());
        for (i, rec) in list.iter().enumerate() {
            if Some(rec.id) == active_id {
                active_idx = i;
            }
            let u = rec
                .view
                .uri()
                .map(|s| s.to_string())
                .unwrap_or_else(|| rec.current_url.borrow().clone());
            urls.push(u);
        }
        save_session(&self.profile_dir, &urls, active_idx);
    }
}

fn wire_header_controls(st: &BrowserState, top_new_tab_btn: &Button) {
    {
        let s = st.clone();
        st.back_btn.connect_clicked(move |_| {
            if let Some(wv) = s.active_webview() {
                wv.go_back();
            }
        });
    }
    {
        let s = st.clone();
        st.fwd_btn.connect_clicked(move |_| {
            if let Some(wv) = s.active_webview() {
                wv.go_forward();
            }
        });
    }
    {
        let s = st.clone();
        st.reload_btn.connect_clicked(move |_| {
            if let Some(wv) = s.active_webview() {
                if wv.is_loading() {
                    wv.stop_loading();
                } else {
                    wv.reload();
                }
            }
        });
    }
    {
        let s = st.clone();
        st.url_entry.connect_activate(move |entry| {
            let txt = entry.text().to_string();
            s.navigate_active(&txt);
        });
    }
    {
        let s = st.clone();
        top_new_tab_btn.connect_clicked(move |_| {
            s.open_new_tab(None, true);
            s.url_entry.grab_focus();
        });
    }
    {
        let s = st.clone();
        st.apps_menu.set_on_install(move || {
            if let Some(wv) = s.active_webview() {
                let apps_c = s.apps_menu.clone();
                webapp_dialog::show_install_webapp_dialog(&s.window, &wv, move || {
                    apps_c.refresh();
                });
            }
        });
    }
}

fn wire_shortcuts(st: &BrowserState) {
    let key_ctrl = gtk4::EventControllerKey::new();
    key_ctrl.set_propagation_phase(gtk4::PropagationPhase::Capture);
    let s = st.clone();
    key_ctrl.connect_key_pressed(move |_, key, _, mods| {
        let ctrl = mods.contains(gdk::ModifierType::CONTROL_MASK);
        let shift = mods.contains(gdk::ModifierType::SHIFT_MASK);
        let alt = mods.contains(gdk::ModifierType::ALT_MASK);

        if ctrl && !alt {
            match key {
                gdk::Key::t | gdk::Key::T if !s.is_app_mode => {
                    s.open_new_tab(None, true);
                    s.url_entry.grab_focus();
                    return glib::Propagation::Stop;
                }
                gdk::Key::w | gdk::Key::W => {
                    if let Some(id) = s.active_tab.get() {
                        s.close_tab(id);
                    }
                    return glib::Propagation::Stop;
                }
                gdk::Key::l | gdk::Key::L if !s.is_app_mode => {
                    s.url_entry.grab_focus();
                    s.url_entry.select_region(0, -1);
                    return glib::Propagation::Stop;
                }
                gdk::Key::f | gdk::Key::F => {
                    s.find_bar.open();
                    return glib::Propagation::Stop;
                }
                gdk::Key::r | gdk::Key::R => {
                    if let Some(wv) = s.active_webview() {
                        if shift { wv.reload_bypass_cache(); } else { wv.reload(); }
                    }
                    return glib::Propagation::Stop;
                }
                gdk::Key::Tab => {
                    s.cycle_tab(if shift { -1 } else { 1 });
                    return glib::Propagation::Stop;
                }
                gdk::Key::ISO_Left_Tab => {
                    s.cycle_tab(-1);
                    return glib::Propagation::Stop;
                }
                gdk::Key::plus | gdk::Key::equal => {
                    if let Some(wv) = s.active_webview() {
                        wv.set_zoom_level((wv.zoom_level() + 0.1).min(3.0));
                    }
                    return glib::Propagation::Stop;
                }
                gdk::Key::minus => {
                    if let Some(wv) = s.active_webview() {
                        wv.set_zoom_level((wv.zoom_level() - 0.1).max(0.3));
                    }
                    return glib::Propagation::Stop;
                }
                gdk::Key::_0 => {
                    if let Some(wv) = s.active_webview() {
                        wv.set_zoom_level(configured_default_zoom());
                    }
                    return glib::Propagation::Stop;
                }
                _ => {}
            }
        }

        if alt && !ctrl {
            match key {
                gdk::Key::d | gdk::Key::D if !s.is_app_mode => {
                    s.url_entry.grab_focus();
                    s.url_entry.select_region(0, -1);
                    return glib::Propagation::Stop;
                }
                gdk::Key::Left => {
                    if let Some(wv) = s.active_webview() { wv.go_back(); }
                    return glib::Propagation::Stop;
                }
                gdk::Key::Right => {
                    if let Some(wv) = s.active_webview() { wv.go_forward(); }
                    return glib::Propagation::Stop;
                }
                _ => {}
            }
        }

        match key {
            gdk::Key::F5 => {
                if let Some(wv) = s.active_webview() {
                    if shift { wv.reload_bypass_cache(); } else { wv.reload(); }
                }
                glib::Propagation::Stop
            }
            gdk::Key::F11 => {
                if s.window.is_fullscreen() { s.window.unfullscreen(); } else { s.window.fullscreen(); }
                glib::Propagation::Stop
            }
            _ => glib::Propagation::Proceed,
        }
    });
    st.window.add_controller(key_ctrl);
}

fn wire_power_and_lifecycle(
    st: &BrowserState,
    header_revealer: &Revealer,
    max_ctrl: &WinCtrlButton,
    fs_ctrl: &WinCtrlButton,
) {
    {
        let w = st.window.clone();
        let mc = max_ctrl.clone();
        let fc = fs_ctrl.clone();
        st.window.connect_maximized_notify(move |_| sync_win_ctrl_states(&w, &mc, &fc));
    }
    {
        let w = st.window.clone();
        let mc = max_ctrl.clone();
        let fc = fs_ctrl.clone();
        let hr = header_revealer.clone();
        let st_fs = st.clone();
        let html_fs = st.html_fullscreen.clone();
        st.window.connect_fullscreened_notify(move |win| {
            sync_win_ctrl_states(&w, &mc, &fc);
            let fs = win.is_fullscreen();
            hr.set_reveal_child(!(fs && html_fs.get()));
            st_fs.sync_tab_bar_visibility();
        });
    }

    {
        let sup = st.supervisor.clone();
        let hud = st.power_hud.clone();
        let tabs_u = st.tabs.clone();
        tuning::hardware::attach_upower_watcher(move |on_batt| {
            sup.set_on_battery(on_batt);
            hud.sync_pill_only(&sup);
            let js = tuning::content_blocker::policy_update_js(
                sup.hw_only_video(),
                sup.is_battery_mode(),
            );
            for rec in tabs_u.borrow().iter() {
                rec.view.evaluate_javascript(&js, None, None, gio::Cancellable::NONE, |_| {});
            }
        });
    }

    {
        let sup_m = st.supervisor.clone();
        tuning::hardware::attach_monitors_changed_watcher(move || {
            sup_m.invalidate_last_hz();
        });
    }

    let timer_id: Rc<RefCell<Option<glib::SourceId>>> = Rc::new(RefCell::new(None));
    schedule_telemetry_tick(&st.supervisor, &st.power_hud, &st.closing, &timer_id);

    {
        let s = st.clone();
        let tid_ref = timer_id;
        st.window.connect_close_request(move |_| {
            s.closing.set(true);
            if let Some(id) = tid_ref.borrow_mut().take() {
                id.remove();
            }
            if let Some(id) = s.session_save_timer.borrow_mut().take() {
                id.remove();
            }
            s.save_session_now();
            if !s.is_app_mode {
                ACTIVE_CONTROLLER.with(|c| *c.borrow_mut() = None);
            }
            let _ = fs::remove_file(s.profile_dir.join("running.lock"));
            *s.spare_tab.borrow_mut() = None;
            s.supervisor.release_all();
            glib::Propagation::Proceed
        });
    }
}

fn schedule_telemetry_tick(
    sup: &PowerSupervisor,
    hud: &PowerHudUi,
    closing: &Rc<Cell<bool>>,
    timer_slot: &Rc<RefCell<Option<glib::SourceId>>>,
) {
    if closing.get() {
        return;
    }
    let secs = if sup.is_battery_mode() && !hud.is_popover_open() {
        5
    } else {
        2
    };
    let sup_c = sup.clone();
    let hud_c = hud.clone();
    let closing_c = closing.clone();
    let slot_c = timer_slot.clone();
    let tid = glib::timeout_add_local_once(Duration::from_secs(secs), move || {
        *slot_c.borrow_mut() = None;
        if closing_c.get() {
            return;
        }
        sup_c.refresh_telemetry();
        hud_c.sync_pill_only(&sup_c);
        schedule_telemetry_tick(&sup_c, &hud_c, &closing_c, &slot_c);
    });
    *timer_slot.borrow_mut() = Some(tid);
}

fn sync_win_ctrl_states(win: &adw::ApplicationWindow, max_btn: &WinCtrlButton, fs_btn: &WinCtrlButton) {
    max_btn.set_active(win.is_maximized());
    fs_btn.set_active(win.is_fullscreen());
}

fn make_nav_btn(icon: &str, tooltip: &str) -> Button {
    let b = Button::builder()
        .icon_name(icon)
        .tooltip_text(tooltip)
        .has_frame(false)
        .focus_on_click(false)
        .valign(Align::Center)
        .build();
    b.add_css_class("nav-btn");
    b
}

fn find_webext_dir() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("EXPLORER_WEBEXT_DIR") {
        let dir = PathBuf::from(p);
        if dir.join("libexplorer_webext.so").exists() {
            return Some(dir);
        }
    }
    if let Ok(exe) = std::env::current_exe()
        && let Some(bin_dir) = exe.parent()
    {
        let candidates = [
            bin_dir.join("../lib/explorer"),
            bin_dir.join("explorer-webext"),
            bin_dir.join("webext"),
        ];
        for c in candidates {
            if c.join("libexplorer_webext.so").exists() {
                return Some(c);
            }
        }
    }
    if let Some(compiled) = option_env!("EXPLORER_WEBEXT_DIR") {
        let dir = PathBuf::from(compiled);
        if dir.join("libexplorer_webext.so").exists() {
            return Some(dir);
        }
    }
    None
}

fn save_session(profile_dir: &Path, urls: &[String], active_idx: usize) {
    if urls.is_empty() {
        return;
    }
    let _ = fs::create_dir_all(profile_dir);
    let escaped: Vec<String> = urls
        .iter()
        .map(|u| format!("\"{}\"", u.replace('\\', "\\\\").replace('"', "\\\"")))
        .collect();
    let payload = format!("{{\"active\":{active_idx},\"tabs\":[{}]}}\n", escaped.join(","));
    let tmp = profile_dir.join("session.json.tmp");
    let final_path = profile_dir.join("session.json");
    if fs::write(&tmp, payload).is_ok() {
        let _ = fs::rename(tmp, final_path);
    }
}

fn load_session(profile_dir: &Path) -> Option<(Vec<String>, usize)> {
    if std::env::var_os("EXPLORER_NO_SESSION").is_some() {
        return None;
    }
    let path = profile_dir.join("session.json");
    let raw = fs::read_to_string(&path).ok()?;
    let active = raw
        .split("\"active\":")
        .nth(1)
        .and_then(|s| s.split([',', '}']).next())
        .and_then(|n| n.trim().parse::<usize>().ok())
        .unwrap_or(0);

    let tabs_part = raw.split("\"tabs\":[").nth(1)?.split(']').next()?;
    let mut urls = Vec::new();
    let mut cur = String::new();
    let mut in_str = false;
    let mut esc = false;
    for ch in tabs_part.chars() {
        if !in_str {
            if ch == '"' {
                in_str = true;
                cur.clear();
            }
            continue;
        }
        if esc {
            cur.push(ch);
            esc = false;
        } else if ch == '\\' {
            esc = true;
        } else if ch == '"' {
            in_str = false;
            if !cur.is_empty() {
                urls.push(cur.clone());
            }
        } else {
            cur.push(ch);
        }
    }
    if urls.is_empty() {
        None
    } else {
        Some((urls, active))
    }
}
