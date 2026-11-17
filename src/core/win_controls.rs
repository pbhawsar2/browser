//! Crisp Cairo-drawn window controls (`36x34 px`) matching Photos and
//! video-player companion styling (#131C2C pill, #38BDF8 Cyan accent, #F87171 Coral close)
//! with bold `2.2px` vector strokes balanced to Adwaita symbolic icons.

use gtk4::cairo;
use gtk4::prelude::*;
use gtk4::{Align, Button, DrawingArea};
use std::cell::Cell;
use std::rc::Rc;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum WinCtrlKind {
    Minimize,
    Maximize,
    Fullscreen,
    Close,
}

#[derive(Clone)]
pub struct WinCtrlButton {
    button: Button,
    area: DrawingArea,
    active: Rc<Cell<bool>>,
}

impl WinCtrlButton {
    pub fn widget(&self) -> &Button {
        &self.button
    }

    pub fn set_active(&self, active: bool) {
        if self.active.get() != active {
            self.active.set(active);
            self.area.queue_draw();
        }
    }
}

pub fn make_win_ctrl_button<F>(kind: WinCtrlKind, tooltip: &str, on_click: F) -> WinCtrlButton
where
    F: Fn() + 'static,
{
    let btn = Button::builder()
        .has_frame(false)
        .focus_on_click(false)
        .tooltip_text(tooltip)
        .valign(Align::Center)
        .width_request(36)
        .height_request(34)
        .build();
    btn.add_css_class("win-ctrl");
    if kind == WinCtrlKind::Close {
        btn.add_css_class("win-close");
    }

    let area = DrawingArea::new();
    area.set_content_width(36);
    area.set_content_height(34);
    btn.set_child(Some(&area));

    let hovered = Rc::new(Cell::new(false));
    let pressed = Rc::new(Cell::new(false));
    let active = Rc::new(Cell::new(false));

    let motion = gtk4::EventControllerMotion::new();
    {
        let h = hovered.clone();
        let a = area.clone();
        motion.connect_enter(move |_, _, _| {
            h.set(true);
            a.queue_draw();
        });
    }
    {
        let h = hovered.clone();
        let p = pressed.clone();
        let a = area.clone();
        motion.connect_leave(move |_| {
            h.set(false);
            p.set(false);
            a.queue_draw();
        });
    }
    btn.add_controller(motion);

    let press_ctrl = gtk4::GestureClick::new();
    {
        let p = pressed.clone();
        let a = area.clone();
        press_ctrl.connect_pressed(move |_, _, _, _| {
            p.set(true);
            a.queue_draw();
        });
    }
    {
        let p = pressed.clone();
        let a = area.clone();
        press_ctrl.connect_released(move |_, _, _, _| {
            p.set(false);
            a.queue_draw();
        });
    }
    btn.add_controller(press_ctrl);

    {
        let h = hovered.clone();
        let p = pressed.clone();
        let act = active.clone();
        area.set_draw_func(move |_, cr, w, h_px| {
            draw_control(cr, w as f64, h_px as f64, kind, h.get(), p.get(), act.get());
        });
    }

    btn.connect_clicked(move |_| on_click());

    WinCtrlButton {
        button: btn,
        area,
        active,
    }
}

fn draw_control(
    cr: &cairo::Context,
    w: f64,
    h: f64,
    kind: WinCtrlKind,
    hovered: bool,
    pressed: bool,
    active: bool,
) {
    cr.set_antialias(cairo::Antialias::Best);

    // Match Photos / video-player pill geometry & color ramp cleanly.
    let (bg, border, fg) = if kind == WinCtrlKind::Close {
        if pressed {
            ((0.26, 0.08, 0.12), (0.973, 0.443, 0.443), (1.0, 1.0, 1.0))
        } else if hovered {
            ((0.20, 0.08, 0.11), (0.973, 0.443, 0.443), (0.996, 0.780, 0.780))
        } else {
            ((0.075, 0.110, 0.173), (0.200, 0.255, 0.345), (0.886, 0.910, 0.941))
        }
    } else if pressed {
        ((0.043, 0.067, 0.118), (0.220, 0.741, 0.973), (0.973, 0.980, 0.988))
    } else if hovered || active {
        ((0.095, 0.153, 0.247), (0.220, 0.690, 0.950), (0.973, 0.980, 0.988))
    } else {
        ((0.075, 0.110, 0.173), (0.200, 0.255, 0.345), (0.886, 0.910, 0.941))
    };

    rounded_rect(cr, 0.5, 0.5, (w - 1.0).max(1.0), (h - 1.0).max(1.0), 7.0);
    cr.set_source_rgb(bg.0, bg.1, bg.2);
    let _ = cr.fill_preserve();
    cr.set_source_rgb(border.0, border.1, border.2);
    cr.set_line_width(1.0);
    let _ = cr.stroke();

    // Bold stroke matching `+` and symbolic icons (`2.2px` cleanly proportioned).
    cr.set_source_rgb(fg.0, fg.1, fg.2);
    cr.set_line_width(2.2);
    cr.set_line_cap(cairo::LineCap::Round);
    cr.set_line_join(cairo::LineJoin::Round);

    let cx = w * 0.5;
    let cy = h * 0.5;
    let unit = 5.6;

    match kind {
        WinCtrlKind::Minimize => {
            let line_y = (cy + 3.0).round() + 0.5;
            cr.move_to(cx - unit, line_y);
            cr.line_to(cx + unit, line_y);
            let _ = cr.stroke();
        }
        WinCtrlKind::Maximize => {
            if active {
                let s = 9.4;
                let off = 2.0;
                let bx = cx - s * 0.5 + off;
                let by = cy - s * 0.5 - off;
                cr.move_to(bx + 2.0, by);
                cr.line_to(bx + s, by);
                cr.line_to(bx + s, by + s - 2.0);
                let _ = cr.stroke();

                let fx = cx - s * 0.5 - off;
                let fy = cy - s * 0.5 + off;
                rounded_rect(cr, fx, fy, s, s, 1.8);
                cr.set_source_rgb(bg.0, bg.1, bg.2);
                let _ = cr.fill_preserve();
                cr.set_source_rgb(fg.0, fg.1, fg.2);
                let _ = cr.stroke();
            } else {
                let s = 11.2;
                rounded_rect(cr, cx - s * 0.5, cy - s * 0.5, s, s, 2.0);
                let _ = cr.stroke();
            }
        }
        WinCtrlKind::Fullscreen => {
            let r = 6.0;
            let arm = 3.8;
            if active {
                let ir = 2.1;
                for (sx, sy) in [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)] {
                    cr.move_to(cx + sx * r, cy + sy * ir);
                    cr.line_to(cx + sx * ir, cy + sy * ir);
                    cr.line_to(cx + sx * ir, cy + sy * r);
                }
            } else {
                for (sx, sy) in [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)] {
                    cr.move_to(cx + sx * r, cy + sy * (r - arm));
                    cr.line_to(cx + sx * r, cy + sy * r);
                    cr.line_to(cx + sx * (r - arm), cy + sy * r);
                }
            }
            let _ = cr.stroke();
        }
        WinCtrlKind::Close => {
            let r = 4.9;
            cr.move_to(cx - r, cy - r);
            cr.line_to(cx + r, cy + r);
            cr.move_to(cx + r, cy - r);
            cr.line_to(cx - r, cy + r);
            let _ = cr.stroke();
        }
    }
}

fn rounded_rect(cr: &cairo::Context, x: f64, y: f64, w: f64, h: f64, r: f64) {
    let d = std::f64::consts::FRAC_PI_2;
    cr.new_sub_path();
    cr.arc(x + w - r, y + r, r, -d, 0.0);
    cr.arc(x + w - r, y + h - r, r, 0.0, d);
    cr.arc(x + r, y + h - r, r, d, 2.0 * d);
    cr.arc(x + r, y + r, r, 2.0 * d, 3.0 * d);
    cr.close_path();
}
