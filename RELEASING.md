# Versioning, Packaging, and Releases

This document describes how VibeRadiant versions are defined, how packages are built,
and how GitHub releases are assembled. It is intended for maintainers.

## Versioning

Source of truth:
- `VERSION` contains the release version in `X.Y.Z` form (for example `1.6.0`).
- Release tags must be `vX.Y.Z` and match `VERSION` (enforced by the release workflow).

Makefile variables:
- `RADIANT_VERSION_NUMBER` is read directly from `VERSION`.
- `RADIANT_VERSION_SUFFIX` is optional (for example `-rc1`).
- `RADIANT_VERSION` is `RADIANT_VERSION_NUMBER` plus the suffix.
- If a git checkout is present, `-git-<short-hash>` is appended to `RADIANT_VERSION`
  and `Q3MAP_VERSION`.
- `RADIANT_MAJOR_VERSION` and `RADIANT_MINOR_VERSION` are the 2nd and 3rd dot-separated
  parts of `RADIANT_VERSION_NUMBER` (for `1.6.0`, major is `6`, minor is `0`).

Update URLs:
- `RADIANT_GITHUB_REPO` defaults to `themuffinator/VibeRadiant`.
- The updater resolves releases through the GitHub Releases API for
  `RADIANT_GITHUB_REPO`, then downloads each release's `update.json`.
- `RADIANT_UPDATE_URL` remains the stable fallback URL:
  `https://github.com/themuffinator/VibeRadiant/releases/latest/download/update.json`.
- `RADIANT_RELEASES_URL` defaults to
  `https://github.com/themuffinator/VibeRadiant/releases/latest`.
- Both can be overridden in `Makefile.conf` or the environment to point at another channel.

## Packaging outputs

`make` writes the distributable layout to `INSTALLDIR` (default `install`):
- `install-data` populates gamepacks, docs, and `setup/data/tools/*`, and writes
  `RADIANT_MAJOR` and `RADIANT_MINOR` marker files.
- `install-dll` (Windows) copies runtime DLLs via `install-dlls*.sh`.

Windows packages:
- CI builds with `make MAKEFILE_CONF=msys2-Makefile.conf`.
- The release workflow creates `VibeRadiant-windows-x86_64.zip` from `install/*`
  after ensuring `install/settings/` exists.

Linux packages:
- `appimage.sh` copies `install/` into an AppDir, generates a desktop file and icon,
  and uses `linuxdeploy` plus the Qt plugin to build `VibeRadiant-x86_64.AppImage`.

macOS packages:
- CI builds on `macos-14` (arm64) and archives `install/` as
  `VibeRadiant-macos-arm64.tar.gz`.

Legacy Makefile release targets:
- `release-src` produces a source tarball from `git archive`.
- `release-win32` builds and produces a self-extracting 7z exe.
- `release-all` runs `git clean -xdf` and then runs both targets.

## Release workflow (GitHub Actions)

Trigger:
- Pushing a tag like `v1.6.0` runs `.github/workflows/release.yml`.

Steps:
1. Verify `VERSION` matches the tag.
2. Build Windows, Linux, and macOS artifacts.
3. Rename assets to include the version:
   - `VibeRadiant-${VERSION}-windows-x86_64.zip`
   - `VibeRadiant-${VERSION}-linux-x86_64.AppImage`
   - `VibeRadiant-${VERSION}-macos-arm64.tar.gz`
4. Generate `release/update.json` and `release/sha256sums.txt`.
5. Publish a GitHub release with all files in `release/`.

## Nightly workflow (GitHub Actions)

Trigger:
- Scheduled daily and manual dispatch via `.github/workflows/nightly.yml`.

Steps:
1. `scripts/nightly_version.py` computes `X.Y.Z.BUILD`, release tag, and whether
   there are meaningful commits since the previous nightly.
2. If changes exist (or `force=true`), CI builds Windows, Linux, and macOS artifacts using
   that computed nightly version.
3. CI generates `release/update.json`, checksums, and nightly notes via
   `scripts/nightly_release_notes.py`.
4. CI tags `vX.Y.Z.BUILD` and publishes a prerelease named `Nightly vX.Y.Z.BUILD`.

## Update manifest format

`update.json` is a JSON object with these required fields:
- `version`: string from `VERSION`
- `notes`: release notes URL (tag page)
- `published_at`: ISO 8601 UTC timestamp
- `assets`: map of platform key to asset metadata

Example (trimmed):
```json
{
  "version": "1.6.0",
  "notes": "https://github.com/themuffinator/VibeRadiant/releases/tag/v1.6.0",
  "published_at": "2026-01-01T12:00:00Z",
  "assets": {
    "windows-x86_64": {
      "url": "https://github.com/themuffinator/VibeRadiant/releases/download/v1.6.0/VibeRadiant-1.6.0-windows-x86_64.zip",
      "sha256": "....",
      "name": "VibeRadiant-1.6.0-windows-x86_64.zip",
      "type": "zip",
      "size": 12345678
    },
    "linux-x86_64": {
      "url": "https://github.com/themuffinator/VibeRadiant/releases/download/v1.6.0/VibeRadiant-1.6.0-linux-x86_64.AppImage",
      "sha256": "....",
      "name": "VibeRadiant-1.6.0-linux-x86_64.AppImage",
      "type": "appimage",
      "size": 12345678
    },
    "macos-arm64": {
      "url": "https://github.com/themuffinator/VibeRadiant/releases/download/v1.6.0/VibeRadiant-1.6.0-macos-arm64.tar.gz",
      "sha256": "....",
      "name": "VibeRadiant-1.6.0-macos-arm64.tar.gz",
      "type": "tar.gz",
      "size": 12345678
    }
  }
}
```

Platform keys are defined in `radiant/update.cpp`:
- `windows-x86_64`, `windows-x86`
- `linux-x86_64`, `linux-arm64`, `linux-unknown`
- `macos-arm64`, `macos-x86_64`, `macos-unknown`, `unknown`

If the manifest does not include the current platform key, the updater does not offer
an update.

## Release checklist

- Update `VERSION`.
- Push a tag `vX.Y.Z`.
- Confirm the release workflow publishes the assets, `update.json`, and `sha256sums.txt`.
- Verify `update.json` is reachable from the `releases/latest/download/` URL.

## Testing builds

`.github/workflows/build.yml` produces "Latest Build" artifacts on demand and can
publish a date-based tag release, but it does not generate `update.json`.
