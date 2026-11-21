//! C-ABI wrapper around Brave's battle-tested `adblock` (`adblock-rust` v0.13) engine.
//! Supports full EasyList, EasyPrivacy, uBlock Origin, and `@@` exception rule syntax
//! with lock-free read concurrency via `RwLock` for hot filter-list reloads.

use adblock::lists::{FilterSet, ParseOptions};
use adblock::request::Request;
use adblock::Engine;
use std::ffi::CStr;
use std::os::raw::c_char;
use std::sync::RwLock;

pub struct BraveShieldHandle {
    engine: RwLock<Engine>,
    rule_count: RwLock<usize>,
}

fn build_engine_from_rules(rules_text: &str) -> (Engine, usize) {
    let mut filter_set = FilterSet::new(false);
    let count = rules_text
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('!') && !l.starts_with('['))
        .count();
    filter_set.add_filter_list(rules_text.to_string(), ParseOptions::default());
    (Engine::new_with_filter_set(filter_set), count)
}

#[unsafe(no_mangle)]
pub extern "C" fn explorer_brave_shield_new(rules_utf8: *const c_char) -> *mut BraveShieldHandle {
    let rules = if rules_utf8.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(rules_utf8) }.to_str().unwrap_or("")
    };
    let (engine, count) = build_engine_from_rules(rules);
    Box::into_raw(Box::new(BraveShieldHandle {
        engine: RwLock::new(engine),
        rule_count: RwLock::new(count),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn explorer_brave_shield_reload(
    handle: *mut BraveShieldHandle,
    rules_utf8: *const c_char,
) -> usize {
    if handle.is_null() || rules_utf8.is_null() {
        return 0;
    }
    let rules = unsafe { CStr::from_ptr(rules_utf8) }.to_str().unwrap_or("");
    let (new_engine, count) = build_engine_from_rules(rules);
    let h = unsafe { &*handle };
    if let Ok(mut guard) = h.engine.write() {
        *guard = new_engine;
    }
    if let Ok(mut cguard) = h.rule_count.write() {
        *cguard = count;
    }
    count
}

#[unsafe(no_mangle)]
pub extern "C" fn explorer_brave_shield_rule_count(handle: *const BraveShieldHandle) -> usize {
    if handle.is_null() {
        return 0;
    }
    let h = unsafe { &*handle };
    h.rule_count.read().map(|g| *g).unwrap_or(0)
}

/// Evaluates a network request using Brave's `adblock::Engine::check_network_request`.
/// Returns 1 if the request must be blocked, 0 otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn explorer_brave_shield_check(
    handle: *const BraveShieldHandle,
    url_utf8: *const c_char,
    source_url_utf8: *const c_char,
    request_type_utf8: *const c_char,
) -> i32 {
    if handle.is_null() || url_utf8.is_null() {
        return 0;
    }
    let url = unsafe { CStr::from_ptr(url_utf8) }.to_str().unwrap_or("");
    let source = if source_url_utf8.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(source_url_utf8) }
            .to_str()
            .unwrap_or("")
    };
    let rtype = if request_type_utf8.is_null() {
        "other"
    } else {
        unsafe { CStr::from_ptr(request_type_utf8) }
            .to_str()
            .unwrap_or("other")
    };

    let Ok(req) = Request::new(url, source, rtype, "GET") else {
        return 0;
    };
    let h = unsafe { &*handle };
    let Ok(guard) = h.engine.read() else {
        return 0;
    };
    if guard.check_network_request(&req).should_block() {
        1
    } else {
        0
    }
}

/// Zero-allocation slice-based network check for Chromium's hot IO thread (no strlen / no String copies).
/// Returns 1 if the request must be blocked, 0 otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn explorer_brave_shield_check_slices(
    handle: *const BraveShieldHandle,
    url_ptr: *const u8,
    url_len: usize,
    source_ptr: *const u8,
    source_len: usize,
    rtype_ptr: *const u8,
    rtype_len: usize,
) -> i32 {
    if handle.is_null() || url_ptr.is_null() || url_len == 0 {
        return 0;
    }
    let url = unsafe {
        std::str::from_utf8(std::slice::from_raw_parts(url_ptr, url_len)).unwrap_or("")
    };
    let source = if source_ptr.is_null() || source_len == 0 {
        ""
    } else {
        unsafe { std::str::from_utf8(std::slice::from_raw_parts(source_ptr, source_len)).unwrap_or("") }
    };
    let rtype = if rtype_ptr.is_null() || rtype_len == 0 {
        "other"
    } else {
        unsafe { std::str::from_utf8(std::slice::from_raw_parts(rtype_ptr, rtype_len)).unwrap_or("other") }
    };

    let Ok(req) = Request::new(url, source, rtype, "GET") else {
        return 0;
    };
    let h = unsafe { &*handle };
    let Ok(guard) = h.engine.read() else {
        return 0;
    };
    if guard.check_network_request(&req).should_block() {
        1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn explorer_brave_shield_free(handle: *mut BraveShieldHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
