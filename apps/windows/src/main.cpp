// Original capture feasibility harness. It does not encode or save video yet.
#include <windows.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <shobjidl_core.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

constexpr UINT CaptureEnded = WM_APP + 1;
constexpr int ChooseID = 100;
constexpr int StopID = 101;

// Frame callbacks never touch HWNDs. The UI samples atomics at 4 Hz.
class CaptureCheck : public std::enable_shared_from_this<CaptureCheck> {
public:
    std::atomic<uint64_t> frames{0};
    std::atomic<int> width{0}, height{0};
    std::atomic<bool> failed{false};

    void Start(GraphicsCaptureItem const& item, HWND window, WPARAM generation) {
        com_ptr<ID3D11Device> native;
        com_ptr<ID3D11DeviceContext> context;
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            native.put(), nullptr, context.put()));
        auto dxgi = native.as<IDXGIDevice>();
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        device_ = inspectable.as<IDirect3DDevice>();
        item_ = item;
        size_ = item.Size();
        width = size_.Width;
        height = size_.Height;
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
        auto weak = weak_from_this();
        arrived_ = pool_.FrameArrived([weak, window, generation](auto const& sender, auto const&) {
            if (auto self = weak.lock()) {
                try { self->OnFrame(sender); }
                catch (...) {
                    self->failed = true;
                    PostMessageW(window, CaptureEnded, generation, 0);
                }
            }
        });
        closed_ = item_.Closed([window, generation](auto const&, auto const&) {
            PostMessageW(window, CaptureEnded, generation, 0);
        });
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(true);
        session_.StartCapture();
    }

    void Stop() noexcept {
        Direct3D11CaptureFramePool oldPool{nullptr};
        GraphicsCaptureSession oldSession{nullptr};
        GraphicsCaptureItem oldItem{nullptr};
        {
            std::scoped_lock lock(mutex_);
            stopped_ = true;
            oldPool = std::exchange(pool_, nullptr);
            oldSession = std::exchange(session_, nullptr);
            oldItem = std::exchange(item_, nullptr);
        }
        // Never hold the callback mutex while closing the frame pool.
        try { if (oldItem) oldItem.Closed(closed_); } catch (...) { }
        try { if (oldPool) oldPool.FrameArrived(arrived_); } catch (...) { }
        try { if (oldSession) oldSession.Close(); } catch (...) { }
        try { if (oldPool) oldPool.Close(); } catch (...) { }
    }

    ~CaptureCheck() { Stop(); }

private:
    void OnFrame(Direct3D11CaptureFramePool const& sender) {
        std::scoped_lock lock(mutex_);
        if (stopped_) return;
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;
        auto nextSize = frame.ContentSize();
        // Access the actual D3D surface to prove usable frame delivery, not just callbacks.
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        ++frames;
        width = nextSize.Width;
        height = nextSize.Height;
        frame.Close();
        texture = nullptr;
        access = nullptr;
        if (nextSize.Width > 0 && nextSize.Height > 0 &&
            (nextSize.Width != size_.Width || nextSize.Height != size_.Height)) {
            size_ = nextSize;
            sender.Recreate(device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
        }
    }

    std::mutex mutex_;
    bool stopped_ = false;
    SizeInt32 size_{};
    IDirect3DDevice device_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    event_token arrived_{}, closed_{};
};

struct WindowState {
    HWND window{}, choose{}, stop{}, status{};
    HFONT font{};
    bool choosing = false;
    WPARAM generation = 0;
    std::shared_ptr<CaptureCheck> capture;
    std::wstring source;

    void StopCapture() {
        if (capture) capture->Stop();
        capture.reset();
        EnableWindow(choose, TRUE);
        EnableWindow(stop, FALSE);
    }
};

// Shared state outlives an outstanding picker operation if the window closes.
std::shared_ptr<WindowState> state;

fire_and_forget Choose(std::shared_ptr<WindowState> owner) {
    apartment_context ui;
    std::wstring errorMessage;
    owner->choosing = true;
    EnableWindow(owner->choose, FALSE);
    try {
        GraphicsCapturePicker picker;
        picker.as<IInitializeWithWindow>()->Initialize(owner->window);
        auto item = co_await picker.PickSingleItemAsync();
        co_await ui;
        if (!IsWindow(owner->window)) co_return;
        if (item) {
            auto capture = std::make_shared<CaptureCheck>();
            capture->Start(item, owner->window, ++owner->generation);
            owner->source = item.DisplayName().c_str();
            owner->capture = std::move(capture);
            EnableWindow(owner->stop, TRUE);
        } else {
            SetWindowTextW(owner->status, L"Selection cancelled. Nothing was captured.");
        }
    } catch (hresult_error const& error) {
        errorMessage = error.message().c_str();
    }
    co_await ui;
    if (!errorMessage.empty() && IsWindow(owner->window)) SetWindowTextW(owner->status, errorMessage.c_str());
    owner->choosing = false;
    if (IsWindow(owner->window)) EnableWindow(owner->choose, owner->capture ? FALSE : TRUE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == ChooseID && !state->choosing && !state->capture) Choose(state);
        if (LOWORD(wParam) == StopID) {
            state->StopCapture();
            SetWindowTextW(state->status, L"Capture stopped. No file was written.");
        }
        return 0;
    case WM_TIMER:
        if (state->capture) {
            auto const& capture = state->capture;
            auto text = state->source + L"\r\n" + std::to_wstring(capture->width.load()) + L" × " +
                std::to_wstring(capture->height.load()) + L" pixels\r\n" +
                std::to_wstring(capture->frames.load()) + L" frames received";
            SetWindowTextW(state->status, text.c_str());
        }
        return 0;
    case CaptureEnded: {
        if (wParam != state->generation || !state->capture) return 0;
        bool failed = state->capture && state->capture->failed;
        state->StopCapture();
        SetWindowTextW(state->status, failed ? L"Capture failed. Try selecting another source." : L"The selected source closed.");
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, 1);
        state->StopCapture();
        if (state->font) DeleteObject(state->font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    init_apartment(apartment_type::single_threaded);
    if (!GraphicsCaptureSession::IsSupported()) {
        MessageBoxW(nullptr, L"Screen capture is unavailable on this computer.", L"Footage Record", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    WNDCLASSW klass{};
    klass.hInstance = instance;
    klass.lpfnWndProc = WindowProc;
    klass.lpszClassName = L"FootageRecordCaptureCheck";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&klass);
    state = std::make_shared<WindowState>();
    state->window = CreateWindowExW(0, klass.lpszClassName, L"Footage Record — Windows capture check",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 540, 330, nullptr, nullptr, instance, nullptr);
    if (!state->window) return 1;
    auto child = [&](LPCWSTR type, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) {
        return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
            state->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    auto heading = child(L"STATIC", L"Native window and screen capture", 0, 24, 20, 470, 24, 0);
    auto description = child(L"STATIC", L"Engineering preview. No audio or video file is saved yet.", 0, 24, 54, 480, 38, 0);
    state->choose = child(L"BUTTON", L"Choose window or screen…", WS_TABSTOP | BS_PUSHBUTTON, 24, 100, 260, 36, ChooseID);
    state->stop = child(L"BUTTON", L"Stop capture", WS_TABSTOP | BS_PUSHBUTTON, 300, 100, 170, 36, StopID);
    state->status = child(L"STATIC", L"Choose a source to check frame delivery.", 0, 24, 162, 475, 105, 0);
    state->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (auto control : {heading, description, state->choose, state->stop, state->status})
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
    EnableWindow(state->stop, FALSE);
    SetTimer(state->window, 1, 250, nullptr);
    ShowWindow(state->window, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(state->window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    state.reset();
    return 0;
}
