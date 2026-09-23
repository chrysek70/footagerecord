# Windows capture foundation

This is an **engineering capture check**, not the finished Windows recorder. It opens the native window/display picker, receives real D3D11 capture surfaces, counts frames, follows source size changes, and stops on source closure. It does not yet encode MP4 or capture audio.

The executable requests `asInvoker` and statically links the MSVC runtime. It has no third-party runtime or driver dependency and can be run from a user-owned folder. Clean standard-user installation and capture still need Windows testing.

## Build

Use Visual Studio 2022 or newer with Desktop development with C++, CMake, and a recent Windows 11 SDK (including C++/WinRT).

```powershell
cmake -S apps/windows -B build/windows -A x64
cmake --build build/windows --config Release
& build/windows/Release/FootageRecordCaptureCheck.exe
```

## Next vertical slice

Add Media Foundation H.264/AAC output, WASAPI endpoint and microphone capture, a common QPC media clock, bounded audio mixing, and temp-file finalization. Then reuse the Mac product flow. Do not present this harness as a downloadable consumer recorder.

The repository includes a Windows CI build job. It has not run until the project is pushed to a GitHub repository with Actions enabled. Hardware capture cannot be validated by a headless compilation job.
