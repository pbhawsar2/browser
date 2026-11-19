# Agent Guidelines & Development Protocol

This document defines mandatory operating rules for all AI agents (Antigravity, Gemini, Claude, Cursor, Copilot, etc.) working on the **Explorer Browser** repository.

---

## 1. Mandatory User Confirmation Before Git Actions

### 1.1 Do NOT Commit Without Explicit User Approval
- **Rule**: Agents must **NEVER** run `git commit` autonomously.
- **Protocol**:
  1. Complete code edits and internal verification.
  2. Display a clear summary of changes and the `git diff`.
  3. Propose a descriptive Conventional Commit message (e.g., `feat(...)`, `fix(...)`, `chore(...)`).
  4. Explicitly ask the user for confirmation before executing the commit.

### 1.2 Do NOT Push Without Explicit User Approval
- **Rule**: Agents must **NEVER** run `git push` to any remote repository without explicit user authorization.
- **Protocol**:
  1. Confirm that all commits are fully verified and approved.
  2. Inform the user of the target remote, branch, and tags to be pushed.
  3. Wait for direct user consent before running `git push`.

### 1.3 Working Tree & State Safety
- **Do NOT discard, revert, reset, or overwrite** uncommitted local changes (`git checkout --`, `git restore`, `git reset --hard`, `git clean -fd`, `git stash drop`) without explicit user permission.
- **Do NOT modify git remotes, branches, tags, or history** unless explicitly asked by the user.

---

## 2. Architecture & Upgradeability Guardrails

- **Keep Core and Tuning Separated:**
  - `src/core/` contains pure upstream GTK4 / Libadwaita / WebKitGTK UI, tab/window lifecycle, downloads, find bar, and WebApp installation. Do not couple hardware/driver hacks directly into `src/core/`.
  - `src/tuning/` contains all hardware, DRM/VA-API, power supervisor, content blocker, scroll scaling, and WebKit engine tuning.
  - Every optimization in `src/tuning/` **must** remain cleanly bypassable via `EXPLORER_DISABLE_TUNING=1`.
- **UI & Theme Invariants:**
  - Top bezel buttons and window controls stay aligned to `36x34px` (`button.nav-btn`, `menubutton.nav-menu > button`, and Cairo window controls).
  - Never reintroduce a permanently-allocating left tab sidebar (`GtkRevealer` horizontal width leak); multi-tab UI uses the auto-hiding top horizontal `.tab-bar` (`visible` only when `tabs > 1`).
  - Default page zoom is `1.05` (`DEFAULT_PAGE_ZOOM`), and **Google** is the default search engine.

---

## 3. Stability First: 100% Rigorous Verification

The primary objective is maintaining rock-solid app stability, high performance, and hardware decode reliability on Wayland / Linux (WebKitGTK 6.0, VA-API, and AMD/Optimus isolation).

Before presenting any changes for commit approval:
1. **Build Integrity**:
   - Ensure the project builds and tests cleanly without warnings or errors (`cargo test` and `cargo build --release`).
2. **Desktop & Shell Integration**:
   - If desktop entries, scripts, or assets are touched, validate them (`desktop-file-validate`).
   - Ensure install scripts remain turnkey and industrial-grade (standalone binaries via `install -p -m 755`, **zero fragile symlinks**).
3. **Subsystem & WebProcess Extension Verification**:
   - Verify both the main `explorer` binary and `libexplorer_webext.so` WebExtension build cleanly.
4. **Clean Tree**:
   - Ensure no temporary files (`*.swp`, `*.tmp_rot`, ad-hoc build artifacts) pollute the working tree.

---

## 4. Communication Standards

- Be concise and provide direct file links (`file:///...`) to modified files.
- Summarize changes before requesting approval to commit.
- Never assume implicit permission for VCS mutations.
