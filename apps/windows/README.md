# Footage Record for Windows

First working version: choose a window or screen in the Windows picker, count down (Esc cancels), record, stop from the tray icon or the window, and get an MP4 in `Videos\Footage Record`.

- Capture: Windows.Graphics.Capture. Frames are converted and scaled on the GPU (D3D11 video processor) and encoded with the Media Foundation hardware H.264 encoder at up to 30 fps.
- Size: the source's own pixels, never scaled up. Sources larger than 4096×2304 are scaled down to fit H.264.
- Sound (optional, remembered between runs): "Sound from what you record" captures only the chosen window's app (process loopback, including its child processes) or all computer sound for a screen, or when the window's app can't be identified from its title. Microphone is off by default. Both are captured at 48 kHz, placed on one timeline by their timestamps (silence fills gaps) and encoded as AAC 192 kbps.
- Files: written to a hidden-named temp file, checked for a real duration, then renamed; never overwrites.
- Not yet: settings for video size and countdown, global hotkey, styled UI, installer and signing.

The executable requests `asInvoker`, statically links the MSVC runtime and needs no installer or administrator rights.

## Build

Use Visual Studio 2022 or newer with Desktop development with C++, CMake, and a recent Windows 11 SDK (including C++/WinRT).

```powershell
cmake -S apps/windows -B build/windows -A x64
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release
& build/windows/Release/FootageRecord.exe
```

GitHub Actions builds and tests it on every push; download `Windows-development` from the run's artifacts.
