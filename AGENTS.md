# WinRDP Release & Versioning Rules

## Mandatory Commit & GitHub Release Workflow
For every completed code, packaging, or user-facing change in this repository, you MUST finish the task by:

1. **Choose the next Semantic Version (`MAJOR.MINOR.PATCH`) based on the scope of the plan/change:**
   - **Major (`X.0.0`, e.g., `1.x.x` → `2.0.0`):** Breaking architecture/config/CLI changes or major milestones.
   - **Minor (`X.Y.0`, e.g., `1.0.0` → `1.1.0`):** Large plans, new user-facing features, or substantial capability additions (reset `PATCH` to `0`).
   - **Patch (`X.Y.Z`, e.g., `1.1.0` → `1.1.1` → `1.1.2`):** Small plans, bug fixes, UI/UX tweaks, packaging/CI fixes, or minor improvements.
   - Update the version consistently in `CMakeLists.txt` (`project(winrdp VERSION X.Y.Z ...)`), `src/app/application.cpp` fallback `WINRDP_VERSION`, and download commands in `README.md`.

2. **Build, Verify, and Package `.deb`:**
   - Run `./scripts/build-app.sh` and confirm all CTest unit tests pass with zero warnings/errors.
   - Build both supported release `.deb` targets + combined `dist/SHA256SUMS.txt`:
     - Ubuntu 24.04 LTS (`dist/winrdp_<X.Y.Z>_ubuntu24.04_amd64.deb`) via `./scripts/build-deb.sh --version <X.Y.Z> --distro-tag ubuntu24.04`.
     - Ubuntu 26.04 LTS & 26.10 (`dist/winrdp_<X.Y.Z>_ubuntu26.04_amd64.deb`) via `./scripts/build-deb-docker.sh --ubuntu 26.04 --version <X.Y.Z> --verify-on ubuntu:26.04,ubuntu:26.10` (verifies clean container install, `ldd`, `desktop-file-validate`, and `winrdp --version` on both `ubuntu:26.04` and `ubuntu:26.10`).
   - Verify the extracted Ubuntu 24.04 `.deb` (`dpkg-deb --info`, extract to temp dir, check `readelf -d`, `ldd`, `desktop-file-validate`, `--version`, and `DISPLAY=:1 ... --smoke-test` when a display is available).
   - Sync local desktop integration via `./scripts/install-desktop.sh` when applicable.

3. **Commit, Push, and Publish GitHub Release:**
   - Confirm `git status` excludes build outputs (`build/`, `dist/`, `third_party/freerdp3-*`) and contains no secrets or host-specific personal paths.
   - Commit changes on `main` with a clear message referencing `vX.Y.Z` and push to `origin main`.
   - Publish GitHub Release `vX.Y.Z` attaching `dist/winrdp_<X.Y.Z>_ubuntu24.04_amd64.deb`, `dist/winrdp_<X.Y.Z>_ubuntu26.04_amd64.deb`, and `dist/SHA256SUMS.txt` via `gh release create vX.Y.Z ...` using `--notes-file - <<'EOF'` (real multiline Markdown), including install commands and SHA-256 checksums.
   - Verify publication with `gh release view vX.Y.Z`.

## Exceptions
- Pure read-only questions, reviews, or unapproved draft proposals do not require a commit or release.
- If the user explicitly asks to skip releasing (e.g., WIP commit only), follow the user's explicit instruction.
