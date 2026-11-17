//! Live hardware telemetry pill and Power/Optimization HUD popover.

use super::webapp;
use crate::tuning::PowerSupervisor;
use gtk4::prelude::*;
use gtk4::{
    Align, Box as GtkBox, Button, Label, MenuButton, Orientation, Popover, Separator, Switch,
};
use std::cell::Cell;
use std::rc::Rc;
use webkit6::WebContext;

#[derive(Clone)]
pub struct PowerHudUi {
    pub pill_button: MenuButton,
    pill_label: Label,
    cpu_lbl: Label,
    igpu_lbl: Label,
    dgpu_lbl: Label,
    va_lbl: Label,
    webapps_box: GtkBox,
    updating_ui: Rc<Cell<bool>>,
}

impl PowerHudUi {
    pub fn new(
        supervisor: PowerSupervisor,
        web_context: WebContext,
        on_policy_changed: impl Fn() + 'static,
    ) -> Self {
        let pill_label = Label::builder()
            .label(supervisor.pill_text())
            .single_line_mode(true)
            .build();
        let pill_button = MenuButton::builder()
            .child(&pill_label)
            .tooltip_text("Hardware power, fan & telemetry")
            .focus_on_click(false)
            .build();
        pill_button.add_css_class("power-pill");

        let root = GtkBox::new(Orientation::Vertical, 10);
        root.set_size_request(320, -1);

        let title = Label::builder()
            .label("Explorer Power & Hardware HUD")
            .xalign(0.0)
            .css_classes(["hud-title"])
            .build();
        root.append(&title);

        let card = GtkBox::new(Orientation::Vertical, 5);
        card.add_css_class("hud-card");
        let cpu_lbl = Label::builder().xalign(0.0).css_classes(["hud-mono"]).build();
        let igpu_lbl = Label::builder().xalign(0.0).css_classes(["hud-mono"]).build();
        let dgpu_lbl = Label::builder().xalign(0.0).css_classes(["hud-mono"]).build();
        let va_lbl = Label::builder()
            .label(format!(
                "VA-API ({}): {}",
                supervisor.va_caps().render_node,
                supervisor.va_caps().summary()
            ))
            .xalign(0.0)
            .css_classes(["hud-mono"])
            .build();
        card.append(&cpu_lbl);
        card.append(&igpu_lbl);
        card.append(&dgpu_lbl);
        card.append(&va_lbl);
        root.append(&card);

        let updating_ui = Rc::new(Cell::new(false));
        let policy_cb = Rc::new(on_policy_changed);

        // Toggle switches
        let (hw_row, hw_sw) = make_switch_row("Block CPU AV1 (Force HW VP9)", supervisor.hw_only_video());
        {
            let sup = supervisor.clone();
            let wc = web_context.clone();
            let guard = updating_ui.clone();
            let cb = policy_cb.clone();
            hw_sw.connect_state_set(move |_, active| {
                if !guard.get() {
                    sup.set_hw_only_video(active, &wc);
                    cb();
                }
                gtk4::glib::Propagation::Proceed
            });
        }
        root.append(&hw_row);

        let (cin_row, cin_sw) = make_switch_row("Cinema 60Hz Display Lock", supervisor.cinema_60hz());
        {
            let sup = supervisor.clone();
            let guard = updating_ui.clone();
            let cb = policy_cb.clone();
            cin_sw.connect_state_set(move |_, active| {
                if !guard.get() {
                    sup.set_cinema_60hz(active);
                    cb();
                }
                gtk4::glib::Propagation::Proceed
            });
        }
        root.append(&cin_row);

        let (turbo_row, turbo_sw) =
            make_switch_row("AC Turbo Performance Hold", !supervisor.is_battery_mode());
        {
            let sup = supervisor.clone();
            let guard = updating_ui.clone();
            let cb = policy_cb.clone();
            turbo_sw.connect_state_set(move |_, active| {
                if !guard.get() {
                    sup.set_ac_turbo_override(active);
                    cb();
                }
                gtk4::glib::Propagation::Proceed
            });
        }
        root.append(&turbo_row);

        root.append(&Separator::new(Orientation::Horizontal));

        let apps_hdr = Label::builder()
            .label("Installed Web Apps")
            .xalign(0.0)
            .css_classes(["hud-mono"])
            .build();
        root.append(&apps_hdr);

        let webapps_box = GtkBox::new(Orientation::Vertical, 4);
        root.append(&webapps_box);

        let popover = Popover::new();
        popover.add_css_class("hud-popover");
        popover.set_child(Some(&root));
        pill_button.set_popover(Some(&popover));

        let hud = Self {
            pill_button,
            pill_label,
            cpu_lbl,
            igpu_lbl,
            dgpu_lbl,
            va_lbl,
            webapps_box,
            updating_ui,
        };

        {
            let hud_c = hud.clone();
            let sup_c = supervisor.clone();
            let hw_c = hw_sw.clone();
            let cin_c = cin_sw.clone();
            let tur_c = turbo_sw.clone();
            popover.connect_show(move |_| {
                hud_c.sync(&sup_c, &hw_c, &cin_c, &tur_c);
                hud_c.refresh_webapps_list();
            });
        }

        hud.sync(&supervisor, &hw_sw, &cin_sw, &turbo_sw);
        hud
    }

    pub fn sync_pill_only(&self, supervisor: &PowerSupervisor) {
        self.pill_label.set_text(&supervisor.pill_text());
        if supervisor.is_battery_mode() {
            self.pill_button.remove_css_class("ac-turbo");
            self.pill_button.add_css_class("zero-fan");
        } else {
            self.pill_button.remove_css_class("zero-fan");
            self.pill_button.add_css_class("ac-turbo");
        }
    }

    fn sync(
        &self,
        supervisor: &PowerSupervisor,
        hw_sw: &Switch,
        cin_sw: &Switch,
        turbo_sw: &Switch,
    ) {
        self.sync_pill_only(supervisor);
        let t = supervisor.telemetry();

        let temp_txt = t
            .cpu_temp_c
            .map(|c| format!("{c:.0}C"))
            .unwrap_or_else(|| "--".into());
        let pwr_txt = t
            .pkg_watts
            .map(|w| format!("{w:.1}W"))
            .unwrap_or_else(|| "--".into());
        self.cpu_lbl
            .set_text(&format!("CPU: {:.0}% · {temp_txt} · {pwr_txt}", t.cpu_percent));

        let g_busy = t.igpu_busy.map(|b| format!("{b}%")).unwrap_or_else(|| "--".into());
        let g_mhz = t.igpu_mhz.map(|m| format!("{m}MHz")).unwrap_or_else(|| "--".into());
        let g_temp = t.igpu_temp_c.map(|c| format!("{c:.0}C")).unwrap_or_else(|| "--".into());
        self.igpu_lbl
            .set_text(&format!("iGPU: {g_busy} · {g_mhz} · {g_temp}"));

        let fan = t.fan_rpm.map(|r| format!("{r} RPM")).unwrap_or_else(|| "--".into());
        let dgpu = t.dgpu_state.as_deref().unwrap_or("absent");
        self.dgpu_lbl
            .set_text(&format!("RTX 3050 Ti: {dgpu} · Fan: {fan}"));

        self.va_lbl.set_text(&format!(
            "VA-API ({}): {} · {}{}{}",
            supervisor.va_caps().render_node,
            supervisor.va_caps().summary(),
            if supervisor.on_battery_raw() { "Battery" } else { "AC" },
            if supervisor.ac_turbo_override() { " (Turbo Override)" } else { "" },
            ""
        ));

        self.updating_ui.set(true);
        hw_sw.set_active(supervisor.hw_only_video());
        cin_sw.set_active(supervisor.cinema_60hz());
        turbo_sw.set_active(!supervisor.is_battery_mode());
        self.updating_ui.set(false);
    }

    pub fn refresh_webapps_list(&self) {
        while let Some(child) = self.webapps_box.first_child() {
            self.webapps_box.remove(&child);
        }
        let apps = webapp::list_installed_webapps();
        if apps.is_empty() {
            let empty = Label::builder()
                .label("None installed (use menu to Install as App)")
                .xalign(0.0)
                .css_classes(["hud-mono"])
                .build();
            self.webapps_box.append(&empty);
            return;
        }
        for app in apps {
            let row = GtkBox::new(Orientation::Horizontal, 6);
            let icon_img = gtk4::Image::builder().pixel_size(20).build();
            if let Some(ref tex) = app.preview_texture {
                icon_img.set_paintable(Some(tex));
            } else {
                icon_img.set_icon_name(Some("application-x-executable-symbolic"));
            }
            let name_btn = Button::builder()
                .label(&app.name)
                .has_frame(false)
                .hexpand(true)
                .build();
            if let Some(child) = name_btn.child().and_downcast::<Label>() {
                child.set_xalign(0.0);
                child.set_ellipsize(gtk4::pango::EllipsizeMode::End);
            }
            let app_launch = app.clone();
            name_btn.connect_clicked(move |_| webapp::launch_webapp(&app_launch));

            let del_btn = Button::builder()
                .icon_name("user-trash-symbolic")
                .tooltip_text("Uninstall Web App")
                .has_frame(false)
                .build();
            del_btn.add_css_class("tab-close");
            let list_box = self.webapps_box.clone();
            let row_rm = row.clone();
            let app_del = app.clone();
            del_btn.connect_clicked(move |_| {
                if webapp::uninstall_webapp(&app_del).is_ok() {
                    list_box.remove(&row_rm);
                }
            });

            row.append(&icon_img);
            row.append(&name_btn);
            row.append(&del_btn);
            self.webapps_box.append(&row);
        }
    }
}

fn make_switch_row(label_text: &str, active: bool) -> (GtkBox, Switch) {
    let row = GtkBox::new(Orientation::Horizontal, 8);
    let lbl = Label::builder()
        .label(label_text)
        .xalign(0.0)
        .hexpand(true)
        .valign(Align::Center)
        .build();
    let sw = Switch::builder()
        .active(active)
        .valign(Align::Center)
        .build();
    row.append(&lbl);
    row.append(&sw);
    (row, sw)
}
