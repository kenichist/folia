Folia is a lightweight, local notes app with nested pages, LaTeX math and code blocks, whose whole window layout is a Lua file you can redesign without recompiling.

## What's new in 0.2.0

- **Images in notes.** Paste, drop or use **Edit ▸ Insert Image…**. Originals are kept in the workspace folder, and images are decoded only near the screen, within a fixed memory budget.
- **PDFs beside your notes** (macOS). Paste, drop or use **Edit ▸ Insert PDF…**, then click the card: the PDF opens as a second tab next to the note, with a divider you can drag, double-click or swap with ⇄. The Windows and Linux builds don't display PDFs yet.
- **Lighter on memory.** Cached image and PDF pixels are capped at 64 MiB, scanned PDFs no longer add memory each time they are opened, and on macOS memory freed after closing a large document goes back to the system.
- **Less work while you type and resize.** The editor no longer keeps a text layout for every line, skips a redundant clip on every paint, and the window skips style updates that change nothing.

## Downloads

| Platform | File | Notes |
|---|---|---|
| **macOS** 12+ (Apple silicon and Intel) | `Folia-*-macOS-universal.dmg` | Open the DMG and drag Folia to Applications. |
| **Windows** 10/11 (x64) | `Folia-*-windows-x64-setup.exe` | Installer. `Folia-*-windows-x64.zip` is a portable copy: unzip and run `Folia.exe`. |
| **Linux** (x86_64, glibc 2.35+) | `Folia-*-linux-x86_64.AppImage` | `chmod +x` it and run. `Folia-*-linux-x86_64.tar.gz` is the same app unpacked; run `./AppRun`. |

`SHA256SUMS.txt` lists the checksum of every file.

## First launch

These builds are not signed with a paid developer certificate yet, so the OS warns the first time:

- **macOS**: right-click Folia in Applications, choose **Open**, then **Open** again. Or run `xattr -dr com.apple.quarantine /Applications/Folia.app`.
- **Windows**: on the SmartScreen prompt choose **More info** ▸ **Run anyway**.

All packages are built and tested by the [Release workflow](https://github.com/kenichist/folia/actions/workflows/release.yml) from the tagged source, with Qt 6.8 LTS.
