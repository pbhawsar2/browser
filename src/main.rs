//! Explorer browser entrypoint.
//!
//! Initializes mimalloc and validated environment optimizations BEFORE
//! `gtk::init()` / WebKit initialization, handles CLI/Single-Instance D-Bus
//! activation, and launches `core::window`.

mod core;
mod tuning;

use gtk4::gdk;
use gtk4::gio;
use gtk4::glib;
use gtk4::prelude::*;
use libadwaita as adw;
use std::path::PathBuf;

#[global_allocator]
static GLOBAL: mimalloc::MiMalloc = mimalloc::MiMalloc;

const APP_CSS: &str = include_str!("core/style.css");

struct CliArgs {
    app_url: Option<String>,
    app_id: Option<String>,
    urls: Vec<String>,
}

fn parse_cli(args: &[String]) -> CliArgs {
    let mut parsed = CliArgs {
        app_url: None,
        app_id: None,
        urls: Vec::new(),
    };
    let mut it = args.iter().skip(1);
    while let Some(arg) = it.next() {
        if let Some(u) = arg.strip_prefix("--app=") {
            parsed.app_url = Some(u.to_string());
        } else if arg == "--app" {
            if let Some(u) = it.next() {
                parsed.app_url = Some(u.clone());
            }
        } else if let Some(id) = arg.strip_prefix("--app-id=") {
            parsed.app_id = Some(id.to_string());
        } else if arg == "--app-id" {
            if let Some(id) = it.next() {
                parsed.app_id = Some(id.clone());
            }
        } else if !arg.starts_with('-') {
            parsed.urls.push(arg.clone());
        }
    }
    parsed
}

fn main() -> glib::ExitCode {
    let on_battery = tuning::is_on_battery_boot();
    tuning::EngineTuning::prepare_process_env(on_battery);

    let argv: Vec<String> = std::env::args().collect();
    let cli = parse_cli(&argv);
    let app_id = std::env::var("EXPLORER_APP_ID")
        .or_else(|_| std::env::var("VELOX_APP_ID"))
        .ok()
        .filter(|s| !s.is_empty())
        .or_else(|| cli.app_id.clone())
        .unwrap_or_else(|| "io.explorer.Browser".to_string());

    let profile_dir = std::env::var_os("EXPLORER_PROFILE_DIR")
        .or_else(|| std::env::var_os("VELOX_PROFILE_DIR"))
        .map(PathBuf::from)
        .unwrap_or_else(|| glib::user_data_dir().join("explorer"));

    let cache_dir = std::env::var_os("EXPLORER_CACHE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| glib::user_cache_dir().join("explorer"));

    let hardware_accel = std::env::var_os("EXPLORER_SW_RENDER").is_none();
    let is_app_mode = cli.app_url.is_some();

    let flags = if std::env::var_os("EXPLORER_NON_UNIQUE").is_some() {
        gio::ApplicationFlags::NON_UNIQUE
    } else if !is_app_mode {
        gio::ApplicationFlags::HANDLES_COMMAND_LINE
    } else {
        gio::ApplicationFlags::FLAGS_NONE
    };

    let app = adw::Application::builder()
        .application_id(&app_id)
        .flags(flags)
        .build();

    let base_cfg = core::window::LaunchConfig {
        initial_urls: cli.urls,
        app_mode_url: cli.app_url,
        app_id: cli.app_id,
        profile_dir,
        cache_dir,
        hardware_accel,
    };

    app.connect_startup(move |application| {
        adw::StyleManager::default().set_color_scheme(adw::ColorScheme::ForceDark);

        if let Some(display) = gdk::Display::default() {
            let theme = gtk4::IconTheme::for_display(&display);
            theme.add_search_path(glib::user_data_dir().join("icons"));

            let css = gtk4::CssProvider::new();
            css.load_from_string(APP_CSS);
            gtk4::style_context_add_provider_for_display(
                &display,
                &css,
                gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
            );
        }

        let open_action = gio::SimpleAction::new("open-url", Some(glib::VariantTy::STRING));
        open_action.connect_activate(|_, param| {
            if let Some(url) = param.and_then(|v| v.str()) {
                let _ = core::window::open_urls_if_running(&[url.to_string()]);
            }
        });
        application.add_action(&open_action);

        let install_action = gio::SimpleAction::new("install-app", None);
        install_action.connect_activate(|_, _| core::window::trigger_install_webapp_if_running());
        application.add_action(&install_action);

        let snap_action = gio::SimpleAction::new("snapshot", Some(glib::VariantTy::STRING));
        snap_action.connect_activate(|_, param| {
            if let Some(path) = param.and_then(|v| v.str()) {
                core::window::capture_snapshot_if_running(path);
            }
        });
        application.add_action(&snap_action);

        let quit_action = gio::SimpleAction::new("quit", None);
        let app_q = application.clone();
        quit_action.connect_activate(move |_, _| {
            let wins = app_q.windows();
            if wins.is_empty() {
                app_q.quit();
            } else {
                for w in wins {
                    w.close();
                }
            }
        });
        application.add_action(&quit_action);
        application.set_accels_for_action("app.quit", &["<Primary>q"]);
    });

    if !is_app_mode && std::env::var_os("EXPLORER_NON_UNIQUE").is_none() {
        let cfg_cmd = base_cfg.clone();
        app.connect_command_line(move |application, cmdline| {
            let remote_args: Vec<String> = cmdline
                .arguments()
                .iter()
                .map(|os| os.to_string_lossy().into_owned())
                .collect();
            let parsed = parse_cli(&remote_args);
            if !parsed.urls.is_empty() && core::window::open_urls_if_running(&parsed.urls) {
                return glib::ExitCode::SUCCESS;
            }
            if let Some(win) = application.active_window() {
                win.present();
                return glib::ExitCode::SUCCESS;
            }
            let mut launch = cfg_cmd.clone();
            launch.initial_urls = parsed.urls;
            core::window::build_and_present(application, launch);
            glib::ExitCode::SUCCESS
        });
        app.run()
    } else {
        let cfg_act = base_cfg;
        app.connect_activate(move |application| {
            if let Some(win) = application.active_window() {
                win.present();
                return;
            }
            core::window::build_and_present(application, cfg_act.clone());
        });
        app.run_with_args::<&str>(&[])
    }
}
