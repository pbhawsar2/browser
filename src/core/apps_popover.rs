//! Web Apps header menu: install the active page as a PWA, launch, or uninstall existing Web Apps.

use super::webapp;
use gtk4::prelude::*;
use gtk4::{Align, Box as GtkBox, Button, Image, Label, MenuButton, Orientation, Popover};
use std::cell::RefCell;
use std::rc::Rc;

#[derive(Clone)]
pub struct AppsMenuUi {
    pub button: MenuButton,
    install_btn: Button,
    webapps_box: GtkBox,
    popover: Popover,
    on_install: Rc<RefCell<Option<Box<dyn Fn()>>>>,
}

impl AppsMenuUi {
    pub fn new(is_app_mode: bool) -> Self {
        let button = MenuButton::builder()
            .icon_name("view-grid-symbolic")
            .tooltip_text("Web Apps")
            .has_frame(false)
            .focus_on_click(false)
            .valign(Align::Center)
            .visible(!is_app_mode)
            .build();
        button.add_css_class("nav-menu");

        let root = GtkBox::new(Orientation::Vertical, 10);
        root.set_size_request(310, -1);

        let title = Label::builder()
            .label("Web Apps")
            .xalign(0.0)
            .css_classes(["hud-title"])
            .build();
        root.append(&title);

        let install_content = GtkBox::new(Orientation::Horizontal, 8);
        let install_icon = Image::builder()
            .icon_name("list-add-symbolic")
            .pixel_size(16)
            .build();
        let install_lbl = Label::builder()
            .label("Install Current Page as App")
            .xalign(0.0)
            .hexpand(true)
            .build();
        install_content.append(&install_icon);
        install_content.append(&install_lbl);

        let install_btn = Button::builder()
            .child(&install_content)
            .has_frame(false)
            .sensitive(false)
            .css_classes(["apps-install-btn"])
            .build();
        root.append(&install_btn);

        let installed_hdr = Label::builder()
            .label("Installed Apps")
            .xalign(0.0)
            .css_classes(["hud-mono"])
            .build();
        root.append(&installed_hdr);

        let webapps_box = GtkBox::new(Orientation::Vertical, 6);
        webapps_box.add_css_class("hud-card");
        root.append(&webapps_box);

        let popover = Popover::new();
        popover.add_css_class("hud-popover");
        popover.set_child(Some(&root));
        button.set_popover(Some(&popover));

        let on_install: Rc<RefCell<Option<Box<dyn Fn()>>>> = Rc::new(RefCell::new(None));
        let ui = Self {
            button,
            install_btn,
            webapps_box,
            popover,
            on_install,
        };

        {
            let pop = ui.popover.clone();
            let cb_ref = ui.on_install.clone();
            ui.install_btn.connect_clicked(move |_| {
                pop.popdown();
                if let Some(ref cb) = *cb_ref.borrow() {
                    cb();
                }
            });
        }
        {
            let ui_c = ui.clone();
            ui.popover.connect_show(move |_| {
                ui_c.refresh();
            });
        }

        ui.refresh();
        ui
    }

    pub fn set_on_install(&self, cb: impl Fn() + 'static) {
        *self.on_install.borrow_mut() = Some(Box::new(cb));
    }

    pub fn set_can_install(&self, can_install: bool) {
        self.install_btn.set_sensitive(can_install);
    }

    pub fn refresh(&self) {
        while let Some(child) = self.webapps_box.first_child() {
            self.webapps_box.remove(&child);
        }
        let apps = webapp::list_installed_webapps();
        if apps.is_empty() {
            let empty = Label::builder()
                .label("No web apps installed")
                .xalign(0.0)
                .css_classes(["hud-mono"])
                .build();
            self.webapps_box.append(&empty);
            return;
        }
        for app in apps {
            let row = GtkBox::new(Orientation::Horizontal, 8);
            row.set_height_request(28);
            let icon_img = Image::builder().pixel_size(20).valign(Align::Center).build();
            if let Some(ref tex) = app.preview_texture {
                icon_img.set_paintable(Some(tex));
            } else {
                icon_img.set_icon_name(Some("application-x-executable-symbolic"));
            }

            let name_btn = Button::builder()
                .label(&app.name)
                .has_frame(false)
                .hexpand(true)
                .valign(Align::Center)
                .css_classes(["apps-item-btn"])
                .build();
            if let Some(child) = name_btn.child().and_downcast::<Label>() {
                child.set_xalign(0.0);
                child.set_ellipsize(gtk4::pango::EllipsizeMode::End);
            }
            {
                let app_launch = app.clone();
                let pop = self.popover.clone();
                name_btn.connect_clicked(move |_| {
                    pop.popdown();
                    webapp::launch_webapp(&app_launch);
                });
            }

            let del_btn = Button::builder()
                .icon_name("user-trash-symbolic")
                .tooltip_text("Uninstall Web App")
                .has_frame(false)
                .valign(Align::Center)
                .css_classes(["tab-close"])
                .build();
            {
                let ui_c = self.clone();
                let app_del = app.clone();
                del_btn.connect_clicked(move |_| {
                    if webapp::uninstall_webapp(&app_del).is_ok() {
                        ui_c.refresh();
                    }
                });
            }

            row.append(&icon_img);
            row.append(&name_btn);
            row.append(&del_btn);
            self.webapps_box.append(&row);
        }
    }
}
