Folia is a lightweight, local notes app with nested pages, LaTeX math and code blocks, whose whole window layout is a Lua file you can redesign without recompiling.

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
