<p align="center">
  <img src="win/blox7/blox7.png" width="96" alt="Blox7 logo">
</p>

<h1 align="center">Blox7</h1>

<p align="center">Roblox ported back to Windows 7 through 11.</p>

<p align="center">
  <a href="https://github.com/Zephkek/Blox7/releases/latest"><img src="https://img.shields.io/badge/download-v0.1-2ea44f" alt="Download v0.1"></a>
  <img src="https://img.shields.io/badge/Windows-7%20to%2011-0078d4" alt="Windows 7 to 11">
  <img src="https://img.shields.io/badge/arch-x86--64-6e7781" alt="x86-64">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue" alt="Apache-2.0"></a>
</p>

Blox7 is a compatibility layer that runs Roblox's Android client natively on Windows, and is how Windows 7 plays
Roblox again.

<p align="center">
  <img src="docs/windows7.jpg" alt="Roblox running on Windows 7 Ultimate">
</p>

<p align="center">
  <img src="docs/bedwars.jpg" width="32%" alt="BedWars">
  <img src="docs/shooter.jpg" width="32%" alt="A tactical shooter">
  <img src="docs/lobby.jpg" width="32%" alt="A murder mystery lobby">
</p>

## Download

Grab `Blox7.exe` from [Releases](https://github.com/Zephkek/Blox7/releases/latest), run it and press Play. The first
start downloads Roblox (about 150 MB), then you sign in inside the Roblox window.

You need 64-bit Windows 7 SP1 or newer, a Vulkan or OpenGL driver, and WebView2 for signing in (version 109 on
Windows 7).

## The launcher

<p align="center">
  <img src="docs/launcher.png" alt="The Blox7 launcher with its settings open">
</p>

<p align="center">
  <img src="docs/ui-effects.png" width="32%" alt="Effects settings">
  <img src="docs/ui-device.png" width="32%" alt="Device settings">
  <img src="docs/ui-about.png" width="32%" alt="About Blox7">
</p>

The launcher looks like it shipped with Windows 7. Its settings let you pick Vulkan or OpenGL, a quality preset up
to Super HQ, a lower render scale for weak GPUs, post effects with your own shaders, fast flags and the phone Roblox
sees.

## How it works

Roblox builds its Android app for x86-64 too, so its code runs straight on a PC's processor. Blox7 gives it the rest
of Android: the same Bionic libc a phone uses, with the Linux kernel underneath swapped for a syscall layer that runs
on Win32 inside the game's process.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/how-it-works-dark.svg">
    <img src="docs/how-it-works-light.svg" alt="On a phone, libroblox.so, the Android runtime and Bionic libc call into the Linux kernel. In Blox7 the same libroblox.so and Bionic run on the Blox7 runtime and the Blox7 syscall layer, which calls Win32 and the Windows NT kernel.">
  </picture>
</p>

- Bionic's syscall stubs call functions like `sys_openat` and `sys_futex` instead of the kernel, and each one
  answers the way Linux would.
- The few raw `syscall` instructions inside Roblox are caught with hardware breakpoints and go through the same
  table.
- Graphics, input and audio skip the syscall layer: bridges hand them to the Windows GPU driver and SDL3.

## Building

Blox7 builds on Windows with Python 3, CMake, Ninja, the Android NDK r28c, llvm-mingw, the WebView2 SDK and the
AOSP bionic sources. The tools look in `D:\toolchains` unless `BLOX7_NDK`, `BLOX7_MINGW`, `BLOX7_AOSP`,
`BLOX7_DEPS_SRC` or `BLOX7_WEBVIEW2_SDK` point somewhere else.

<details>
<summary>Build commands</summary>

```bash
python win/tools/deps.py
python win/tools/build.py libc
python win/tools/build.py boot
python win/tools/build.py bridge-sources
python win/tools/build.py bridges
python win/tools/build.py webview
python win/tools/runtime-build.py
python win/tools/build.py blox7
```

</details>

That builds `win/out/release/Blox7.exe`.

## License

Apache 2.0, see [LICENSE](LICENSE). Bundled third-party code keeps its own license. The Android runtime is based
on [Mocktail](https://github.com/komaruworld/mocktail).

Blox7 is not affiliated with Roblox Corporation or Microsoft.

<p align="center">Made with love by Zeph</p>
