use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Discovers an optional custom PGO/ThinLTO WebKitGTK prefix before falling back to
/// system pkg-config, keeping future Fedora / GTK / WebKit upgrades seamless.
fn discover_webkit_prefix() -> Option<PathBuf> {
    let candidates = [
        env::var_os("EXPLORER_WEBKIT_DIR").map(PathBuf::from),
        Some(PathBuf::from("/opt/explorer-webkit")),
        Some(PathBuf::from("/opt/velox-webkit")),
    ];
    for cand in candidates.into_iter().flatten() {
        if cand.join("lib64/pkgconfig/webkitgtk-6.0.pc").exists() {
            return Some(cand);
        }
    }
    None
}

fn pkg_config_output(args: &[&str], pkg_config_path: Option<&Path>) -> Vec<String> {
    let mut cmd = Command::new("pkg-config");
    cmd.args(args);
    if let Some(pc_dir) = pkg_config_path {
        cmd.env("PKG_CONFIG_PATH", pc_dir);
    }
    let out = cmd.output().expect("pkg-config must be installed");
    assert!(
        out.status.success(),
        "pkg-config {:?} failed: {}",
        args,
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .map(ToOwned::to_owned)
        .collect()
}

fn main() {
    println!("cargo:rerun-if-changed=webext/explorer-webext.c");
    println!("cargo:rerun-if-changed=src/core/style.css");
    println!("cargo:rerun-if-changed=assets/icons/io.explorer.Browser.svg");
    println!("cargo:rerun-if-env-changed=EXPLORER_WEBKIT_DIR");

    let custom_prefix = discover_webkit_prefix();
    let custom_lib64 = custom_prefix.as_ref().map(|p| p.join("lib64"));
    let custom_pc = custom_lib64.as_ref().map(|p| p.join("pkgconfig"));
    if let Some(ref lib_dir) = custom_lib64 {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    }

    // Direct VA-API + DRM linking for background hardware profile probing without spawning child processes.
    println!("cargo:rustc-link-lib=va");
    println!("cargo:rustc-link-lib=va-drm");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR missing"));
    let profile = env::var("PROFILE").unwrap_or_else(|_| "debug".into());

    let target_profile_dir = out_dir
        .ancestors()
        .nth(3)
        .map(Path::to_path_buf)
        .unwrap_or_else(|| out_dir.clone());
    let ext_out_dir = target_profile_dir.join("explorer-webext");
    std::fs::create_dir_all(&ext_out_dir).expect("create explorer-webext dir");
    let so_path = ext_out_dir.join("libexplorer_webext.so");

    let cflags = pkg_config_output(
        &["--cflags", "webkitgtk-web-process-extension-6.0", "gstreamer-1.0"],
        custom_pc.as_deref(),
    );
    let libs = pkg_config_output(
        &["--libs", "webkitgtk-web-process-extension-6.0", "gstreamer-1.0"],
        custom_pc.as_deref(),
    );

    let opt_flag = if profile == "release" { "-O3" } else { "-O2" };
    let mut cmd = Command::new("gcc");
    cmd.args(["-shared", "-fPIC", opt_flag, "-march=native", "-Wall", "-Wextra"])
        .arg("webext/explorer-webext.c")
        .arg("-o")
        .arg(&so_path);
    for flag in cflags {
        cmd.arg(flag);
    }
    for lib in libs {
        if let Some(dir) = lib.strip_prefix("-L")
            && dir.starts_with("/opt/")
        {
            cmd.arg(format!("-Wl,-rpath,{dir}"));
        }
        cmd.arg(lib);
    }

    let status = cmd.status().expect("failed to invoke gcc for webext");
    assert!(status.success(), "failed to compile webext/explorer-webext.c");

    println!("cargo:rustc-env=EXPLORER_WEBEXT_DIR={}", ext_out_dir.display());
}
