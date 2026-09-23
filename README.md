# Footage Record

Choose a window or screen. Record. Get an MP4.

Native desktop implementation has started. The macOS development preview records a chosen window or screen to MP4 through ScreenCaptureKit; window recordings have been verified end to end on this Mac. Whole-screen recording stalled after a few frames because the capture queue depth was set to 3; that setting has been removed, and a real screen recording in the app still needs a retest. Microphone recording is still untested. The Windows app records video and sound and awaits its first test on a real PC.

## Run the Mac preview

Requires macOS 15+ and Xcode/Swift 6.2+ to build. The built app does not require Xcode.

```sh
./scripts/build-macos.sh
open 'dist/Footage Record.app'
```

Choose Window or Screen, move the pointer over what you want to record, and click macOS's **Share This Window** button (Apple's picker wording can't be changed). Recording starts right away after the countdown; press Esc or Cancel to stop the countdown. Turn off "Start recording as soon as I choose" in Settings to press Record yourself instead. The main window hides during recording. Stop from the menu-bar icon, Command-period, or the red recording indicator's **Stop Recording This Window** on the recorded window. Recordings save to `~/Movies/Footage Record` by default.

Settings can change the folder, countdown, cursor and video size: Small (up to 720p), Balanced (up to 1080p), Sharp (up to 4K) or Original (the source's exact pixels). Sources are never scaled up. Anything larger than 4096×2304, such as an 8K display at Original, is saved as HEVC instead of H.264.

This is a local, ad-hoc-signed development build. It is not notarized for public distribution. No account, network service, or administrator installation is needed by the app. OS capture/microphone consent still applies.

## Verify

```sh
swift test --package-path apps/macos
./scripts/build-capture-fixture.sh
```

The second command builds a separate synthetic window with animation and an optional generated tone for recording checks. It does not use the microphone.

## Windows

[First working version and build instructions](apps/windows/README.md): pick a window or screen, record with app/computer sound and optional microphone, stop from the tray, get an H.264/AAC MP4. Built and tested by GitHub Actions; not yet run on a real Windows PC.

## About

Initial audience: everyday users on their own computers. Desktop first; macOS and Windows. Business model remains open. Confirmed initial experience: local and account-free.
