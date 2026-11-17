//! Slide-down Find-in-Page bar bound cleanly to active `WebView`.

use gtk4::gdk;
use gtk4::prelude::*;
use gtk4::{Box as GtkBox, Button, Entry, Label, Orientation, Revealer, RevealerTransitionType};
use std::cell::RefCell;
use std::rc::Rc;
use webkit6::prelude::*;
use webkit6::{FindOptions, WebView};

#[derive(Clone)]
pub struct FindBarUi {
    pub revealer: Revealer,
    entry: Entry,
    count_label: Label,
    active_view: Rc<RefCell<Option<WebView>>>,
}

impl FindBarUi {
    pub fn new() -> Self {
        let revealer = Revealer::builder()
            .transition_type(RevealerTransitionType::SlideDown)
            .transition_duration(120)
            .reveal_child(false)
            .build();

        let bar = GtkBox::new(Orientation::Horizontal, 6);
        bar.add_css_class("find-bar");

        let entry = Entry::builder()
            .placeholder_text("Find on page...")
            .width_chars(24)
            .css_classes(["find-entry"])
            .build();
        let count_label = Label::builder()
            .label("")
            .css_classes(["find-count"])
            .build();
        let prev_btn = Button::builder()
            .icon_name("go-up-symbolic")
            .tooltip_text("Previous match")
            .has_frame(false)
            .build();
        prev_btn.add_css_class("nav-btn");
        let next_btn = Button::builder()
            .icon_name("go-down-symbolic")
            .tooltip_text("Next match")
            .has_frame(false)
            .build();
        next_btn.add_css_class("nav-btn");
        let close_btn = Button::builder()
            .icon_name("window-close-symbolic")
            .tooltip_text("Close find bar (Esc)")
            .has_frame(false)
            .build();
        close_btn.add_css_class("nav-btn");

        bar.append(&entry);
        bar.append(&count_label);
        bar.append(&prev_btn);
        bar.append(&next_btn);
        bar.append(&close_btn);
        revealer.set_child(Some(&bar));

        let active_view: Rc<RefCell<Option<WebView>>> = Rc::new(RefCell::new(None));

        let ui = Self {
            revealer,
            entry,
            count_label,
            active_view,
        };

        {
            let u = ui.clone();
            ui.entry.connect_changed(move |_| u.search_current());
        }
        {
            let u = ui.clone();
            ui.entry.connect_activate(move |_| u.find_next());
        }
        {
            let u = ui.clone();
            next_btn.connect_clicked(move |_| u.find_next());
        }
        {
            let u = ui.clone();
            prev_btn.connect_clicked(move |_| u.find_prev());
        }
        {
            let u = ui.clone();
            close_btn.connect_clicked(move |_| u.close());
        }

        let key_ctrl = gtk4::EventControllerKey::new();
        {
            let u = ui.clone();
            key_ctrl.connect_key_pressed(move |_, key, _, _| {
                if key == gdk::Key::Escape {
                    u.close();
                    return gtk4::glib::Propagation::Stop;
                }
                gtk4::glib::Propagation::Proceed
            });
        }
        ui.entry.add_controller(key_ctrl);

        ui
    }

    pub fn set_active_view(&self, view: Option<&WebView>) {
        *self.active_view.borrow_mut() = view.cloned();
        if self.revealer.reveals_child() {
            self.search_current();
        }
    }

    pub fn open(&self) {
        self.revealer.set_reveal_child(true);
        self.entry.grab_focus();
        self.entry.select_region(0, -1);
        self.search_current();
    }

    pub fn close(&self) {
        self.revealer.set_reveal_child(false);
        self.count_label.set_text("");
        if let Some(wv) = self.active_view.borrow().as_ref()
            && let Some(fc) = wv.find_controller()
        {
            fc.search_finish();
        }
    }

    fn search_current(&self) {
        let query = self.entry.text().to_string();
        let Some(wv) = self.active_view.borrow().clone() else {
            return;
        };
        let Some(fc) = wv.find_controller() else {
            return;
        };
        if query.is_empty() {
            fc.search_finish();
            self.count_label.set_text("");
            return;
        }
        let flags = (FindOptions::CASE_INSENSITIVE | FindOptions::WRAP_AROUND).bits();
        fc.count_matches(&query, flags, 999);
        fc.search(&query, flags, 999);
    }

    fn find_next(&self) {
        if let Some(wv) = self.active_view.borrow().as_ref()
            && let Some(fc) = wv.find_controller()
        {
            fc.search_next();
        }
    }

    fn find_prev(&self) {
        if let Some(wv) = self.active_view.borrow().as_ref()
            && let Some(fc) = wv.find_controller()
        {
            fc.search_previous();
        }
    }

    pub fn attach_webview_signals(&self, wv: &WebView) {
        if let Some(fc) = wv.find_controller() {
            let lbl = self.count_label.clone();
            let active = self.active_view.clone();
            let target = wv.clone();
            fc.connect_counted_matches(move |_, n| {
                if active.borrow().as_ref() == Some(&target) {
                    lbl.set_text(&format!("{n} found"));
                }
            });
        }
    }
}
