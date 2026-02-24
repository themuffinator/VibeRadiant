# Auto Updater

This document explains how the in-app updater works and what to expect when you use it.

## Where to check for updates

- Use Help -> Check for VibeRadiant update for a manual check.
- Automatic checks run at startup (once per 24 hours) if enabled.
- Preferences -> Settings -> Updates lets you toggle automatic checks and "Include prerelease builds".

The updater queries GitHub Releases, selects the newest matching release, then reads
that release's `update.json` manifest:
- `Include prerelease builds` disabled: stable releases only.
- `Include prerelease builds` enabled: stable + nightly prereleases.

"Latest Build" artifacts do not include this manifest, so they are not compatible
with auto-update.

## Supported packages

The auto-updater only works with the official release packages:
- Windows: the zip package from GitHub Releases.
- Linux: the AppImage package from GitHub Releases.
- macOS: the tar.gz package from GitHub Releases (nightly + stable).

## What happens during an update

1. VibeRadiant downloads the update to a temporary folder.
2. The file is verified using the SHA-256 hash from the release manifest.
3. You are prompted to save any modified work.
4. The app closes, applies the update, and relaunches.

Automatic checks happen during startup while the splash screen is shown, then the
main window opens after the check completes.

## Windows details

- The zip is expanded into the install directory.
- The install directory must be writable. Installs under protected locations
  (for example Program Files) may require running with elevated permissions
  or moving the install to a writable path.

## Linux details

- Auto-update requires the AppImage build and only works when running the AppImage.
- The downloaded AppImage replaces the existing one, so the file location must be writable.

## macOS details

- Auto-update expects the official tar.gz package from Releases.
- The archive is extracted into the current install directory and the editor relaunches.
- The install path must be writable. App bundles installed under protected locations
  (for example `/Applications` without write permission) may require running from
  a user-writable location.

## If an update fails

- Download the latest package from the GitHub Releases page and replace the existing install.
- If updates repeatedly fail, check that your install path is writable.
