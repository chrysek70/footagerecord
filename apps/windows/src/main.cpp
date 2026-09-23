// Footage Record for Windows: choose a window or screen, count down, record, stop, get an MP4.
#include <windows.h>
#include <unknwn.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl_core.h>
#include <mfapi.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include "recorder.h"

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;

namespace {
enum class Phase { Ready, Choosing, Countdown, Recording, Stopping, Saved };
constexpr int PrimaryID = 100, FolderID = 101, AnotherID = 102;  // Cancel uses IDCANCEL, so Esc works
constexpr UINT TrayMessage = WM_APP + 2;
constexpr UINT_PTR CountdownTimer = 1, ClockTimer = 2, StartTimer = 3;
constexpr UINT TrayStop = 200, TrayCancel = 201, TrayShow = 202, TrayQuit = 203;

struct App {
    HWND window{}, title{}, detail{}, big{}, primary{}, cancel{}, folderButton{}, another{}, message{}, footer{};
    HFONT titleFont{}, bodyFont{}, bigFont{}, smallFont{};
    NOTIFYICONDATAW tray{};
    Phase phase = Phase::Ready;
    int countdown = 0;
    WPARAM generation = 0;
    GraphicsCaptureItem item{nullptr};
    std::shared_ptr<Recorder> recorder;
    fs::path folder, temp, saved, unfinished;
    ULONGLONG recordingSince = 0;
    double savedSeconds = 0;
    std::wstring error;
};
std::shared_ptr<App> app;

int Scale(int value) { return MulDiv(value, GetDpiForWindow(app->window), 96); }

HFONT MakeFont(int size, int weight) {
    return CreateFontW(-Scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void Layout() {
    for (auto font : {app->titleFont, app->bodyFont, app->bigFont, app->smallFont}) if (font) DeleteObject(font);
    app->titleFont = MakeFont(28, FW_SEMIBOLD);
    app->bodyFont = MakeFont(14, FW_NORMAL);
    app->bigFont = MakeFont(56, FW_LIGHT);
    app->smallFont = MakeFont(12, FW_NORMAL);
    auto place = [](HWND control, HFONT font, int x, int y, int w, int h) {
        SetWindowPos(control, nullptr, Scale(x), Scale(y), Scale(w), Scale(h), SWP_NOZORDER);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    };
    place(app->title, app->titleFont, 28, 24, 374, 80);
    place(app->detail, app->bodyFont, 28, 110, 374, 56);
    place(app->big, app->bigFont, 28, 168, 374, 76);
    place(app->primary, app->bodyFont, 28, 252, 374, 40);
    place(app->cancel, app->bodyFont, 28, 302, 180, 34);
    place(app->folderButton, app->bodyFont, 222, 302, 180, 34);
    place(app->another, app->bodyFont, 28, 346, 180, 30);
    place(app->message, app->bodyFont, 28, 388, 374, 64);
    place(app->footer, app->smallFont, 28, 462, 374, 36);
}

void SetTray(std::wstring const& tip) {
    wcsncpy_s(app->tray.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &app->tray);
}

void ShowWindowNow() {
    ShowWindow(app->window, SW_SHOW);
    ShowWindow(app->window, SW_RESTORE);
    SetForegroundWindow(app->window);
}

// Show the controls and text for the current phase.
void Render() {
    auto show = [](HWND control, bool visible) { ShowWindow(control, visible ? SW_SHOW : SW_HIDE); };
    auto text = [](HWND control, std::wstring const& value) { SetWindowTextW(control, value.c_str()); };
    auto phase = app->phase;
    show(app->big, phase == Phase::Countdown || phase == Phase::Recording);
    show(app->primary, phase == Phase::Ready || phase == Phase::Choosing || phase == Phase::Recording || phase == Phase::Saved);
    show(app->cancel, phase == Phase::Countdown);
    show(app->folderButton, phase == Phase::Saved);
    show(app->another, phase == Phase::Saved);
    EnableWindow(app->primary, phase != Phase::Choosing);
    switch (phase) {
    case Phase::Ready:
    case Phase::Choosing:
        text(app->title, L"A recording.\r\nWithout the fuss.");
        text(app->detail, phase == Phase::Choosing
            ? L"Pick the window or screen in the Windows list, then confirm. Recording starts after a 3-second countdown."
            : L"Choose what to show. We’ll save the video.");
        text(app->primary, L"Choose window or screen…");
        SetTray(L"Footage Record");
        break;
    case Phase::Countdown:
        text(app->title, L"Get ready");
        text(app->detail, L"Press Esc to cancel.");
        text(app->big, std::to_wstring(app->countdown));
        break;
    case Phase::Recording:
        text(app->title, L"Recording");
        text(app->detail, L"Stop anytime from the Footage Record tray icon.");
        text(app->big, Elapsed(app->recordingSince ? (GetTickCount64() - app->recordingSince) / 1000.0 : 0));
        text(app->primary, L"Stop recording");
        break;
    case Phase::Stopping:
        text(app->title, L"Finishing your video…");
        text(app->detail, L"Wait for the file to finish saving.");
        SetTray(L"Footage Record · finishing your video");
        break;
    case Phase::Saved: {
        std::error_code ignored;
        wchar_t size[32]{};
        StrFormatByteSizeW(static_cast<LONGLONG>(fs::file_size(app->saved, ignored)), size, 32);
        text(app->title, L"That’s a wrap.");
        text(app->detail, app->saved.filename().wstring() + L"\r\n" + Elapsed(app->savedSeconds) + L" · " + size);
        text(app->primary, L"Open video");
        SetTray(L"Footage Record");
        break;
    }
    }
    auto note = app->error;
    if (!app->unfinished.empty()) note += L"\r\nThe unfinished file was kept: " + app->unfinished.filename().wstring();
    text(app->message, note);
    show(app->message, !note.empty());
}

void Fail(std::wstring const& description) {
    KillTimer(app->window, ClockTimer);
    KillTimer(app->window, StartTimer);
    std::error_code ignored;
    if (!app->temp.empty() && fs::exists(app->temp, ignored)) app->unfinished = app->temp;
    app->recorder.reset();
    app->item = nullptr;
    app->temp.clear();
    app->error = description;
    app->phase = Phase::Ready;
    Render();
    ShowWindowNow();
}

void BeginRecording() {
    try {
        std::error_code ignored;
        fs::create_directories(app->folder, ignored);
        ULARGE_INTEGER available{};
        if (GetDiskFreeSpaceExW(app->folder.c_str(), &available, nullptr, nullptr) && available.QuadPart < 250'000'000)
            throw hresult_error(E_FAIL, L"Free up at least 250 MB or choose another disk before recording.");
        GUID id{};
        check_hresult(CoCreateGuid(&id));
        wchar_t idText[40]{};
        StringFromGUID2(id, idText, 40);
        std::wstring token = idText;
        app->temp = app->folder / (L".Footage-" + token.substr(1, token.size() - 2) + L".incomplete.mp4");
        // Hide first so a whole-screen recording never shows this window.
        ShowWindow(app->window, SW_HIDE);
        app->recorder = std::make_shared<Recorder>(app->window, ++app->generation);
        app->recorder->Start(app->item, app->temp, true);
        app->phase = Phase::Recording;
        app->recordingSince = 0;
        SetTimer(app->window, ClockTimer, 1000, nullptr);
        SetTimer(app->window, StartTimer, 15000, nullptr);
        SetTray(L"Footage Record · starting");
        Render();
    } catch (hresult_error const& error) {
        app->recorder.reset();
        std::error_code ignored;
        if (!app->temp.empty()) fs::remove(app->temp, ignored);
        app->temp.clear();
        Fail(error.message().c_str());
    }
}

void StartCountdown() {
    app->phase = Phase::Countdown;
    app->countdown = 3;
    app->error.clear();
    app->unfinished.clear();
    SetTimer(app->window, CountdownTimer, 1000, nullptr);
    Render();
    ShowWindowNow();
    SetFocus(app->cancel);
}

void CancelCountdown() {
    if (app->phase != Phase::Countdown) return;
    KillTimer(app->window, CountdownTimer);
    app->item = nullptr;
    app->phase = Phase::Ready;
    Render();
}

void StopRecording() {
    if (app->phase != Phase::Recording || !app->recorder) return;
    KillTimer(app->window, ClockTimer);
    KillTimer(app->window, StartTimer);
    app->phase = Phase::Stopping;
    Render();
    ShowWindowNow();
    app->recorder->Stop();
}

void Finished() {
    auto now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    auto target = FinalPath(app->folder, local);
    std::error_code error;
    fs::rename(app->temp, target, error);
    if (error) {
        Fail(L"The recording couldn’t be saved: " + std::wstring(error.message().begin(), error.message().end()));
        return;
    }
    app->savedSeconds = app->recorder->Duration();
    app->saved = target;
    app->recorder.reset();
    app->item = nullptr;
    app->temp.clear();
    app->phase = Phase::Saved;
    Render();
    ShowWindowNow();
}

fire_and_forget Choose() {
    auto owner = app;
    apartment_context ui;
    owner->phase = Phase::Choosing;
    owner->error.clear();
    owner->unfinished.clear();
    Render();
    GraphicsCaptureItem item{nullptr};
    std::wstring failure;
    try {
        GraphicsCapturePicker picker;
        picker.as<IInitializeWithWindow>()->Initialize(owner->window);
        item = co_await picker.PickSingleItemAsync();
    } catch (hresult_error const& error) {
        failure = L"Couldn’t open the Windows picker: " + std::wstring(error.message());
    }
    co_await ui;
    if (!IsWindow(owner->window) || owner->phase != Phase::Choosing) co_return;
    owner->error = failure;
    owner->phase = Phase::Ready;
    if (item) {
        owner->item = item;
        StartCountdown();
    } else {
        Render();
    }
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (app->phase == Phase::Recording) AppendMenuW(menu, MF_STRING, TrayStop, L"Stop recording");
    if (app->phase == Phase::Countdown) AppendMenuW(menu, MF_STRING, TrayCancel, L"Cancel countdown");
    AppendMenuW(menu, MF_STRING, TrayShow, L"Show Footage Record");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayQuit, L"Quit Footage Record");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(app->window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, app->window, nullptr);
    DestroyMenu(menu);
}

void Quit() {
    if (app->phase == Phase::Recording || app->phase == Phase::Stopping) {
        ShowWindowNow();
        MessageBoxW(app->window, L"Stop the recording and wait for it to save before quitting.",
            L"Finish your recording first", MB_OK | MB_ICONINFORMATION);
        return;
    }
    DestroyWindow(app->window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case PrimaryID:
            if (app->phase == Phase::Ready) Choose();
            else if (app->phase == Phase::Recording) StopRecording();
            else if (app->phase == Phase::Saved) ShellExecuteW(window, L"open", app->saved.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDCANCEL:
        case TrayCancel:
            CancelCountdown();
            break;
        case FolderID:
            if (auto item = ILCreateFromPathW(app->saved.c_str())) {
                SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
                ILFree(item);
            }
            break;
        case AnotherID:
            app->phase = Phase::Ready;
            Render();
            break;
        case TrayStop: StopRecording(); break;
        case TrayShow: ShowWindowNow(); break;
        case TrayQuit: Quit(); break;
        }
        return 0;
    case WM_TIMER:
        if (wParam == CountdownTimer) {
            if (--app->countdown > 0) {
                Render();
            } else {
                KillTimer(window, CountdownTimer);
                BeginRecording();
            }
        } else if (wParam == ClockTimer && app->phase == Phase::Recording) {
            Render();
            if (app->recordingSince)
                SetTray(L"Footage Record · recording " + Elapsed((GetTickCount64() - app->recordingSince) / 1000.0));
        } else if (wParam == StartTimer) {
            KillTimer(window, StartTimer);
            if (app->phase == Phase::Recording && !app->recordingSince) {
                app->error = L"The selected source isn’t producing video. Choose it again.";
                StopRecording();
            }
        }
        return 0;
    case RecorderMessage:
        if (wParam != app->generation || !app->recorder) return 0;
        switch (lParam) {
        case RecorderStarted:
            KillTimer(window, StartTimer);
            app->recordingSince = GetTickCount64();
            break;
        case RecorderSourceClosed:
            StopRecording();
            break;
        case RecorderFailed:
            if (app->phase == Phase::Recording) {
                app->error = app->recorder->Error();
                StopRecording();  // try to save what was recorded
            } else {
                Fail(app->recorder->Error());
            }
            break;
        case RecorderFinished:
            Finished();
            break;
        }
        return 0;
    case TrayMessage:
        if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
        if (reinterpret_cast<HWND>(lParam) == app->message) SetTextColor(reinterpret_cast<HDC>(wParam), RGB(176, 62, 0));
        else if (reinterpret_cast<HWND>(lParam) == app->footer) SetTextColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_GRAYTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DPICHANGED: {
        auto suggested = reinterpret_cast<RECT const*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_CLOSE:
        // Closing the window mid-recording only hides it; the tray icon keeps Stop reachable.
        if (app->phase == Phase::Recording || app->phase == Phase::Stopping) ShowWindow(window, SW_HIDE);
        else if (app->phase == Phase::Countdown) CancelCountdown();
        else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &app->tray);
        for (auto font : {app->titleFont, app->bodyFont, app->bigFont, app->smallFont}) if (font) DeleteObject(font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    init_apartment(apartment_type::single_threaded);
    if (!GraphicsCaptureSession::IsSupported()) {
        MessageBoxW(nullptr, L"Screen recording isn’t available on this computer.", L"Footage Record", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    check_hresult(MFStartup(MF_VERSION));
    app = std::make_shared<App>();
    PWSTR videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos))) {
        app->folder = fs::path(videos) / L"Footage Record";
        CoTaskMemFree(videos);
    }

    WNDCLASSW klass{};
    klass.hInstance = instance;
    klass.lpfnWndProc = WindowProc;
    klass.lpszClassName = L"FootageRecord";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&klass);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    UINT dpi = GetDpiForSystem();
    RECT frame{0, 0, MulDiv(430, dpi, 96), MulDiv(510, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    app->window = CreateWindowExW(0, klass.lpszClassName, L"Footage Record", style, CW_USEDEFAULT, CW_USEDEFAULT,
        frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, nullptr);
    if (!app->window) return 1;
    auto child = [&](LPCWSTR type, DWORD flags, int id) {
        return CreateWindowExW(0, type, L"", WS_CHILD | WS_VISIBLE | flags, 0, 0, 10, 10, app->window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    app->title = child(L"STATIC", 0, 0);
    app->detail = child(L"STATIC", 0, 0);
    app->big = child(L"STATIC", SS_CENTER, 0);
    app->primary = child(L"BUTTON", WS_TABSTOP | BS_DEFPUSHBUTTON, PrimaryID);
    app->cancel = child(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
    app->folderButton = child(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, FolderID);
    app->another = child(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, AnotherID);
    app->message = child(L"STATIC", 0, 0);
    app->footer = child(L"STATIC", 0, 0);
    SetWindowTextW(app->cancel, L"Cancel");
    SetWindowTextW(app->folderButton, L"Show in folder");
    SetWindowTextW(app->another, L"Record another");
    SetWindowTextW(app->footer, L"MP4 · original size · 30 fps · no sound yet\r\nSaved in Videos\\Footage Record. Only on this PC.");
    Layout();

    app->tray.cbSize = sizeof(app->tray);
    app->tray.hWnd = app->window;
    app->tray.uID = 1;
    app->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app->tray.uCallbackMessage = TrayMessage;
    app->tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(app->tray.szTip, L"Footage Record");
    Shell_NotifyIconW(NIM_ADD, &app->tray);

    Render();
    ShowWindow(app->window, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(app->window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    app->recorder.reset();
    app.reset();
    MFShutdown();
    return 0;
}
