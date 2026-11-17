//! Live hardware telemetry pill and Power/Optimization HUD popover.

use crate::tuning::PowerSupervisor;
use gtk4::prelude::*;
use gtk4::{
    Align, Box as GtkBox, Label, MenuButton, Orientation, Popover, Switch,
};
use std::cell::Cell;
use std::rc::Rc;
use webkit6::WebContext;

#[derive(Clone)]
pub struct PowerHudUi {
    pub pill_button: MenuButton,
    popover: Popover,
    pill_label: Label,
    last_battery_style: Rc<Cell<Option<bool>>>,
    cpu_lbl: Label,
    igpu_lbl: Label,
    dgpu_lbl: Label,
    va_lbl: Label,
    power_lbl: Label,
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
            .halign(Align::Center)
            .valign(Align::Center)
            .build();
        let pill_button = MenuButton::builder()
            .child(&pill_label)
            .tooltip_text("Hardware power, fan & telemetry")
            .focus_on_click(false)
            .hexpand(false)
            .build();
        pill_button.add_css_class("power-pill");

        let root = GtkBox::new(Orientation::Vertical, 10);
        root.set_size_request(336, -1);

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
        let power_lbl = Label::builder().xalign(0.0).css_classes(["hud-mono"]).build();
        card.append(&cpu_lbl);
        card.append(&igpu_lbl);
        card.append(&dgpu_lbl);
        card.append(&va_lbl);
        card.append(&power_lbl);
        root.append(&card);

        let updating_ui = Rc::new(Cell::new(false));
        let policy_cb = Rc::new(on_policy_changed);

        let controls_card = GtkBox::new(Orientation::Vertical, 8);
        controls_card.add_css_class("hud-card");

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
        controls_card.append(&hw_row);

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
        controls_card.append(&cin_row);

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
        controls_card.append(&turbo_row);
        root.append(&controls_card);

        let popover = Popover::new();
        popover.add_css_class("hud-popover");
        popover.set_child(Some(&root));
        pill_button.set_popover(Some(&popover));

        let hud = Self {
            pill_button,
            popover: popover.clone(),
            pill_label,
            last_battery_style: Rc::new(Cell::new(None)),
            cpu_lbl,
            igpu_lbl,
            dgpu_lbl,
            va_lbl,
            power_lbl,
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
            });
        }

        hud.sync(&supervisor, &hw_sw, &cin_sw, &turbo_sw);
        hud
    }

    pub fn is_popover_open(&self) -> bool {
        self.popover.is_visible()
    }

    pub fn sync_pill_only(&self, supervisor: &PowerSupervisor) {
        let text = supervisor.pill_text();
        if self.pill_label.text().as_str() != text {
            self.pill_label.set_text(&text);
        }
        let batt = supervisor.is_battery_mode();
        if self.last_battery_style.get() != Some(batt) {
            self.last_battery_style.set(Some(batt));
            if batt {
                self.pill_button.remove_css_class("ac-turbo");
                self.pill_button.add_css_class("zero-fan");
            } else {
                self.pill_button.remove_css_class("zero-fan");
                self.pill_button.add_css_class("ac-turbo");
            }
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

        let mut cpu_parts = vec![format!("{:.0}%", t.cpu_percent)];
        if let Some(c) = t.cpu_temp_c {
            cpu_parts.push(format!("{c:.0}°C"));
        }
        if let Some(w) = t.pkg_watts {
            cpu_parts.push(format!("{w:.1}W"));
        }
        self.cpu_lbl
            .set_text(&format!("CPU: {}", cpu_parts.join(" · ")));

        let mut igpu_parts = Vec::new();
        if let Some(b) = t.igpu_busy {
            igpu_parts.push(format!("{b}%"));
        }
        if let Some(m) = t.igpu_mhz {
            igpu_parts.push(format!("{m}MHz"));
        }
        if let Some(c) = t.igpu_temp_c {
            igpu_parts.push(format!("{c:.0}°C"));
        }
        if let Some(w) = t.igpu_watts {
            igpu_parts.push(format!("{w:.1}W"));
        }
        self.igpu_lbl.set_text(&format!(
            "iGPU: {}",
            if igpu_parts.is_empty() {
                "idle".to_string()
            } else {
                igpu_parts.join(" · ")
            }
        ));

        let fan = t
            .fan_rpm
            .map(|r| format!("{r} RPM"))
            .unwrap_or_else(|| "0 RPM".into());
        let dgpu = t.dgpu_state.as_deref().unwrap_or("absent");
        self.dgpu_lbl
            .set_text(&format!("RTX 3050 Ti: {dgpu} · Fan: {fan}"));

        self.va_lbl.set_text(&format!(
            "VA-API ({}): {}",
            supervisor.va_caps().render_node,
            supervisor.va_caps().summary(),
        ));

        let power_src = if supervisor.on_battery_raw() {
            let batt_w = t
                .batt_watts
                .or(t.pkg_watts)
                .map(|w| format!(" · {w:.1}W"))
                .unwrap_or_default();
            if supervisor.ac_turbo_override() {
                format!("Battery{batt_w} (Turbo Override)")
            } else {
                format!("Battery{batt_w} · Zero-Fan")
            }
        } else {
            "AC Power · AC Turbo".to_string()
        };
        self.power_lbl.set_text(&format!("Power: {power_src}"));

        self.updating_ui.set(true);
        hw_sw.set_active(supervisor.hw_only_video());
        cin_sw.set_active(supervisor.cinema_60hz());
        turbo_sw.set_active(!supervisor.is_battery_mode());
        self.updating_ui.set(false);
    }
}

fn make_switch_row(label_text: &str, active: bool) -> (GtkBox, Switch) {
    let row = GtkBox::new(Orientation::Horizontal, 12);
    row.set_height_request(26);
    let lbl = Label::builder()
        .label(label_text)
        .xalign(0.0)
        .hexpand(true)
        .valign(Align::Center)
        .css_classes(["hud-row-label"])
        .build();
    let sw = Switch::builder()
        .active(active)
        .valign(Align::Center)
        .build();
    row.append(&lbl);
    row.append(&sw);
    (row, sw)
}
