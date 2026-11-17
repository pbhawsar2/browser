//! URL normalization, Google default search routing, and omnibar display helpers.

pub const HOME_URL: &str = "explorer://start";
pub const GOOGLE_SEARCH_URL: &str = "https://www.google.com/search?q=";

/// Converts omnibar user input into an executable navigation URL.
/// Uses Google as the sole default search engine for non-URL queries.
pub fn resolve_input_to_url(raw: &str) -> String {
    let trimmed = raw.trim();
    if trimmed.is_empty() || trimmed == HOME_URL || trimmed == "about:newtab" {
        return HOME_URL.to_string();
    }

    let lower = trimmed.to_ascii_lowercase();
    if lower.starts_with("http://")
        || lower.starts_with("https://")
        || lower.starts_with("file://")
        || lower.starts_with("data:")
        || lower.starts_with("about:")
        || lower.starts_with("view-source:")
        || lower.starts_with("explorer://")
    {
        return trimmed.to_string();
    }

    if !trimmed.chars().any(char::is_whitespace) {
        let host_part = trimmed.split(['/', '?', '#']).next().unwrap_or("");
        if host_part == "localhost"
            || host_part.starts_with("localhost:")
            || is_ipv4_host(host_part)
            || host_part.starts_with('[')
        {
            return format!("http://{trimmed}");
        }
        if looks_like_domain(host_part) {
            return format!("https://{trimmed}");
        }
    }

    format!("{GOOGLE_SEARCH_URL}{}", encode_query_component(trimmed))
}

fn is_ipv4_host(host_port: &str) -> bool {
    let host = host_port.split(':').next().unwrap_or(host_port);
    let mut count = 0;
    for part in host.split('.') {
        if part.parse::<u8>().is_err() {
            return false;
        }
        count += 1;
    }
    count == 4
}

fn looks_like_domain(host_port: &str) -> bool {
    let host = host_port.split(':').next().unwrap_or(host_port);
    if host.is_empty() || host.starts_with('.') || host.ends_with('.') || !host.contains('.') {
        return false;
    }
    let Some((_, tld)) = host.rsplit_once('.') else {
        return false;
    };
    if tld.len() < 2 || !tld.chars().all(|c| c.is_ascii_alphabetic()) {
        return false;
    }
    host.split('.').all(|seg| {
        !seg.is_empty()
            && seg
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '-')
    })
}

pub fn encode_query_component(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for &b in s.as_bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char);
            }
            b' ' => out.push('+'),
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

pub fn is_internal_url(uri: &str) -> bool {
    uri == HOME_URL || uri == "about:blank" || uri.starts_with("explorer://")
}

/// Returns empty string for internal Start Page so placeholder stays clean.
pub fn display_url_for_bar(uri: &str) -> String {
    if is_internal_url(uri) {
        String::new()
    } else {
        uri.to_string()
    }
}

pub fn friendly_title(title: Option<&str>, uri: Option<&str>) -> String {
    if let Some(u) = uri
        && is_internal_url(u)
    {
        return "New Tab".to_string();
    }
    if let Some(t) = title.map(str::trim)
        && !t.is_empty()
    {
        return t.to_string();
    }
    if let Some(u) = uri {
        if let Some(after_scheme) = u.split("://").nth(1) {
            let host = after_scheme.split('/').next().unwrap_or(u);
            if !host.is_empty() {
                return host.to_string();
            }
        }
        return u.to_string();
    }
    "New Tab".to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolves_urls_and_defaults_to_google_search() {
        assert_eq!(resolve_input_to_url("amazon.in"), "https://amazon.in");
        assert_eq!(
            resolve_input_to_url("flipkart.com/search?q=laptop"),
            "https://flipkart.com/search?q=laptop"
        );
        assert_eq!(
            resolve_input_to_url("localhost:3000/test"),
            "http://localhost:3000/test"
        );
        assert_eq!(
            resolve_input_to_url("fedora 45 wayland"),
            "https://www.google.com/search?q=fedora+45+wayland"
        );
        assert_eq!(
            resolve_input_to_url("c++ 2.0"),
            "https://www.google.com/search?q=c%2B%2B+2.0"
        );
        assert_eq!(resolve_input_to_url(""), HOME_URL);
        assert!(!resolve_input_to_url("news today").contains("duckduckgo"));
    }
}
