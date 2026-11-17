//! Download popover UI and `webkit6::Download` lifecycle handler.

use gtk4::glib;
use gtk4::prelude::*;
use gtk4::{Align, Box as GtkBox, Button, Label, MenuButton, Orientation, Popover, ProgressBar};
use std::path::PathBuf;
use webkit6::Download;

#[derive(Clone)]
pub struct DownloadManagerUi {
    pub button: MenuButton,
    list: GtkBox,
    empty_label: Label,
}

impl DownloadManagerUi {
    pub fn new() -> Self {
        let button = MenuButton::builder()
            .icon_name("folder-download-symbolic")
            .tooltip_text("Downloads")
            .has_frame(false)
            .focus_on_click(false)
            .visible(false)
            .build();
        button.add_css_class("nav-menu");

        let pop_box = GtkBox::new(Orientation::Vertical, 8);
        pop_box.set_size_request(290, -1);

        let hdr = Label::builder()
            .label("Downloads")
            .xalign(0.0)
            .css_classes(["hud-title"])
            .build();
        pop_box.append(&hdr);

        let empty_label = Label::builder()
            .label("No recent downloads")
            .xalign(0.0)
            .css_classes(["hud-mono"])
            .build();
        pop_box.append(&empty_label);

        let list = GtkBox::new(Orientation::Vertical, 6);
        pop_box.append(&list);

        let popover = Popover::new();
        popover.add_css_class("hud-popover");
        popover.set_child(Some(&pop_box));
        button.set_popover(Some(&popover));

        Self {
            button,
            list,
            empty_label,
        }
    }

    pub fn track_download(&self, dl: &Download) {
        self.button.set_visible(true);
        self.empty_label.set_visible(false);

        let downloads_dir = std::env::var_os("XDG_DOWNLOAD_DIR")
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .or_else(|| glib::user_special_dir(glib::UserDirectory::Downloads))
            .unwrap_or_else(|| {
                PathBuf::from(std::env::var("HOME").unwrap_or_default()).join("Downloads")
            });
        let _ = std::fs::create_dir_all(&downloads_dir);

        let row = GtkBox::new(Orientation::Vertical, 4);
        row.add_css_class("hud-card");

        let top = GtkBox::new(Orientation::Horizontal, 6);
        let name_lbl = Label::builder()
            .label("Starting download...")
            .xalign(0.0)
            .hexpand(true)
            .ellipsize(gtk4::pango::EllipsizeMode::Middle)
            .build();
        let status_lbl = Label::builder()
            .label("0%")
            .css_classes(["hud-mono"])
            .valign(Align::Center)
            .build();
        let cancel_btn = Button::builder()
            .icon_name("window-close-symbolic")
            .has_frame(false)
            .tooltip_text("Cancel")
            .build();
        cancel_btn.add_css_class("tab-close");

        top.append(&name_lbl);
        top.append(&status_lbl);
        top.append(&cancel_btn);

        let progress = ProgressBar::new();
        row.append(&top);
        row.append(&progress);
        self.list.prepend(&row);

        let name_dest = name_lbl.clone();
        dl.connect_decide_destination(move |d, suggested| {
            let safe_name = PathBuf::from(suggested)
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .filter(|n| !n.is_empty())
                .unwrap_or_else(|| "download.bin".to_string());
            let target = unique_destination(&downloads_dir, &safe_name);
            name_dest.set_text(
                target
                    .file_name()
                    .and_then(|n| n.to_str())
                    .unwrap_or(&safe_name),
            );
            d.set_destination(&target.to_string_lossy());
            true
        });

        {
            let p = progress.clone();
            let s = status_lbl.clone();
            dl.connect_notify_local(Some("estimated-progress"), move |d, _| {
                let frac = d.estimated_progress().clamp(0.0, 1.0);
                p.set_fraction(frac);
                s.set_text(&format!("{:.0}%", frac * 100.0));
            });
        }

        {
            let d = dl.clone();
            cancel_btn.connect_clicked(move |_| d.cancel());
        }

        {
            let p = progress.clone();
            let s = status_lbl.clone();
            let c = cancel_btn.clone();
            dl.connect_finished(move |_| {
                p.set_fraction(1.0);
                s.set_text("Done");
                c.set_visible(false);
            });
        }

        {
            let s = status_lbl;
            let c = cancel_btn;
            dl.connect_failed(move |_, err| {
                s.set_text("Failed");
                s.set_tooltip_text(Some(&err.message()));
                c.set_visible(false);
            });
        }
    }
}

fn unique_destination(dir: &PathBuf, filename: &str) -> PathBuf {
    let candidate = dir.join(filename);
    if !candidate.exists() {
        return candidate;
    }
    let path = PathBuf::from(filename);
    let stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("download");
    let ext = path.extension().and_then(|s| s.to_str());
    for idx in 1..1000 {
        let numbered = match ext {
            Some(e) if !e.is_empty() => format!("{stem} ({idx}).{e}"),
            _ => format!("{stem} ({idx})"),
        };
        let p = dir.join(numbered);
        if !p.exists() {
            return p;
        }
    }
    candidate
}
