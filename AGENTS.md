# AI Agent Instructions (`AGENTS.md`)

These rules apply to all AI coding assistants, autonomous agents, and automated tools operating inside this repository.

## 1. Git & Remote Safety (Mandatory)

- **Do NOT auto-commit** any changes (`git commit`, `git commit --amend`) without explicit user permission.
- **Do NOT auto-push** to any remote repository (`git push`, `git push --force`, `git push --force-with-lease`) without explicit user permission.
- **Do NOT discard, revert, reset, or overwrite** uncommitted local changes (`git checkout --`, `git restore`, `git reset --hard`, `git clean -fd`, `git stash drop`) without explicit user permission.
- **Do NOT modify git remotes, branches, tags, or history** unless explicitly asked by the user.
- Before proposing a commit or push, summarize the exact files modified and wait for explicit confirmation from the user (unless the user already explicitly instructed you to commit and push in the active turn).

## 2. Architecture & Upgradeability Guardrails

- **Keep Core and Tuning Separated:**
  - `src/core/` contains pure upstream GTK4 / Libadwaita / WebKitGTK UI, tab/window lifecycle, downloads, find bar, and WebApp installation. Do not couple hardware/driver hacks directly into `src/core/`.
  - `src/tuning/` contains all hardware, DRM/VA-API, power supervisor, content blocker, scroll scaling, and WebKit engine tuning.
  - Every optimization in `src/tuning/` **must** remain cleanly bypassable via `EXPLORER_DISABLE_TUNING=1`.
- **UI & Theme Invariants:**
  - Top bezel buttons and window controls stay aligned to `36x34px` (`button.nav-btn`, `menubutton.nav-menu > button`, and Cairo window controls).
  - Never reintroduce a permanently-allocating left tab sidebar (`GtkRevealer` horizontal width leak); multi-tab UI uses the auto-hiding top horizontal `.tab-bar` (`visible` only when `tabs > 1`).
  - Default page zoom is `1.05` (`DEFAULT_PAGE_ZOOM`), and **Google** is the default search engine.

## 3. Build, Test & Verification Discipline

- Always run `cargo test` and `cargo build --release` (expecting `0` warnings) after code edits before claiming success.
- Only report file paths, symbols, commands, and benchmark/test outputs that were directly observed in the current session.
- Never touch or risk user data outside this repository (such as external `/data` drives or read-only reference projects).
