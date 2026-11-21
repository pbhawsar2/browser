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

- **Keep SOLID Modules Strictly Separated:**
  - `hal/` owns all Linux hardware, DRM/VA-API, Optimus dGPU isolation, power governor, display refresh, process throttling, and telemetry logic — zero UI or CEF coupling.
  - `policy/` owns pure deterministic state machines (`PowerPolicyEngine`, `MediaCodecPolicy`, `PidThrottlePlanner`, `navigation_policy`).
  - `shield/` + `shield-brave/` own Brave `adblock-rust` filtering and async list updates.
  - `vault/` owns AES-256-GCM encrypted credential storage.
  - `webapp/` owns standalone chromeless Linux WebApp installation.
  - `engine/` owns CEF Alloy client hooks and hardware-driven Chromium/V8 flags.
  - `app/` owns the Frameless Obsidian WebUI chrome, multi-tab window lifecycle, downloads, find bar, and Control Overlay.
  - Every hardware/engine optimization **must** remain cleanly bypassable via `EXPLORER_DISABLE_TUNING=1`.
- **UI & Theme Invariants:**
  - Top bezel stays compact (`44px`) with `36x34px` rounded card buttons and pixel-aligned SVG icons.
  - Multi-tab UI uses the auto-hiding top horizontal `.tab-bar` (`38px`, `visible` only when `tabs > 1`).
  - Default page zoom is `1.00` (`100%`, `kDefaultPageZoomFactor`) with `10%` increment/decrement steps, and **Google** is the default search engine.
  - Zero browsing history stored; persistent cookies & explicit clear-site-data controls.

---

## 3. Stability First: 100% Rigorous Verification

The primary objective is maintaining rock-solid app stability, zero-overlap UI polish, high Speedometer/rendering performance, and hardware decode reliability on Wayland / Linux.

Before presenting any changes for commit approval:
1. **Build & Unit Test Integrity**:
   - Ensure `shield-brave` (`cargo build --release --manifest-path shield-brave/Cargo.toml`) and the C++20 CMake project (`cmake --build build` + `ctest --test-dir build --output-on-failure`) compile cleanly with zero warnings and 100% passing unit tests.
2. **Desktop & Shell Integration**:
   - Validate `.desktop` files (`desktop-file-validate`).
   - Ensure install scripts remain turnkey and industrial-grade (standalone binaries via `install -p -m 755`, **zero fragile symlinks**).
3. **Clean Tree**:
   - Ensure no temporary files (`*.swp`, `*.tmp_rot`, ad-hoc build artifacts) pollute the working tree.

---

## 4. Communication Standards

- Be concise and provide direct file links (`file:///...`) to modified files.
- Summarize changes before requesting approval to commit.
- Never assume implicit permission for VCS mutations.
