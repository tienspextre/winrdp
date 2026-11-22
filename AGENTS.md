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
   - Run `./scripts/build-deb.sh` to produce `dist/winrdp_<X.Y.Z>_amd64.deb` and `dist/SHA256SUMS.txt`.
   - Verify the built `.deb` (`dpkg-deb --info`, extract to temp dir, check `readelf -d`, `ldd`, `desktop-file-validate`, `--version`, and `DISPLAY=:1 ... --smoke-test` when a display is available).
   - Sync local desktop integration via `./scripts/install-desktop.sh` when applicable.

3. **Commit, Push, and Publish GitHub Release:**
   - Confirm `git status` excludes build outputs (`build/`, `dist/`, `third_party/freerdp3-*`) and contains no secrets or host-specific personal paths.
   - Commit changes on `main` with a clear message referencing `vX.Y.Z` and push to `origin main`.
   - Publish GitHub Release `vX.Y.Z` attaching `dist/winrdp_<X.Y.Z>_amd64.deb` and `dist/SHA256SUMS.txt` via `gh release create vX.Y.Z ...`, including install commands and the SHA-256 checksum.
   - Verify publication with `gh release view vX.Y.Z`.

## Exceptions
- Pure read-only questions, reviews, or unapproved draft proposals do not require a commit or release.
- If the user explicitly asks to skip releasing (e.g., WIP commit only), follow the user's explicit instruction.
