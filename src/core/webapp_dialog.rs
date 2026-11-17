//! Modal dialog to customize and install the active page as an XDG Web App.

use super::webapp::{self, PreparedIcon, WebAppMetadata};
use gtk4::prelude::*;
use gtk4::{
    Align, Box as GtkBox, Button, Entry, Image, Label, Orientation, Window,
};
use webkit6::prelude::*;
use webkit6::WebView;

pub fn show_install_webapp_dialog(
    parent: &impl IsA<Window>,
    web_view: &WebView,
    on_installed: impl Fn() + 'static,
) {
    let dialog = Window::builder()
        .transient_for(parent)
        .modal(true)
        .title("Install Web App")
        .default_width(380)
        .resizable(false)
        .build();

    let box_root = GtkBox::new(Orientation::Vertical, 14);
    box_root.set_margin_top(18);
    box_root.set_margin_bottom(18);
    box_root.set_margin_start(18);
    box_root.set_margin_end(18);

    let header = GtkBox::new(Orientation::Horizontal, 14);
    let icon_img = Image::builder()
        .pixel_size(64)
        .icon_name("web-browser-symbolic")
        .css_classes(["webapp-icon-preview"])
        .build();

    let name_entry = Entry::builder()
        .placeholder_text("Loading site details...")
        .hexpand(true)
        .valign(Align::Center)
        .build();

    header.append(&icon_img);
    header.append(&name_entry);
    box_root.append(&header);

    let url_lbl = Label::builder()
        .label(web_view.uri().as_deref().unwrap_or(""))
        .xalign(0.0)
        .ellipsize(gtk4::pango::EllipsizeMode::Middle)
        .css_classes(["hud-mono"])
        .build();
    box_root.append(&url_lbl);

    let actions = GtkBox::new(Orientation::Horizontal, 8);
    actions.set_halign(Align::End);
    let cancel_btn = Button::with_label("Cancel");
    let install_btn = Button::builder()
        .label("Install App")
        .sensitive(false)
        .css_classes(["suggested-action"])
        .build();
    actions.append(&cancel_btn);
    actions.append(&install_btn);
    box_root.append(&actions);

    dialog.set_child(Some(&box_root));

    {
        let dlg = dialog.clone();
        cancel_btn.connect_clicked(move |_| dlg.close());
    }

    let prepared_state: std::rc::Rc<std::cell::RefCell<Option<(WebAppMetadata, PreparedIcon)>>> =
        std::rc::Rc::new(std::cell::RefCell::new(None));

    {
        let dlg = dialog.clone();
        let entry = name_entry.clone();
        let state = prepared_state.clone();
        let cb = std::cell::RefCell::new(Some(Box::new(on_installed) as Box<dyn Fn()>));
        install_btn.connect_clicked(move |_| {
            let guard = state.borrow();
            let Some((meta, icon)) = guard.as_ref() else {
                return;
            };
            let chosen_name = entry.text().trim().to_string();
            let final_name = if chosen_name.is_empty() {
                meta.name.as_str()
            } else {
                chosen_name.as_str()
            };
            if webapp::install_webapp(final_name, &meta.url, icon).is_ok()
                && let Some(ref notify) = *cb.borrow()
            {
                notify();
            }
            dlg.close();
        });
    }

    {
        let icon_c = icon_img;
        let entry_c = name_entry;
        let url_c = url_lbl;
        let btn_c = install_btn;
        let state_c = prepared_state;
        webapp::extract_and_prepare_webapp(web_view, move |meta, prep| {
            entry_c.set_text(&meta.name);
            url_c.set_text(&meta.url);
            if let Some(tex) = prep.preview_texture() {
                icon_c.set_paintable(Some(&tex));
            }
            *state_c.borrow_mut() = Some((meta, prep));
            btn_c.set_sensitive(true);
        });
    }

    dialog.present();
}
