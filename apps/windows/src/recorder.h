#pragma once
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include "recording.h"

// Posted to the UI window: wParam = recording generation, lParam = RecorderEvent.
constexpr UINT RecorderMessage = WM_APP + 1;
enum RecorderEvent : LPARAM { RecorderStarted = 1, RecorderSourceClosed, RecorderFailed, RecorderFinished };

// Records one window or display into an H.264 MP4 file. Capture callbacks never touch
// windows; they post RecorderMessage, and the UI reads results after Finished or Failed.
class Recorder : public std::enable_shared_from_this<Recorder> {
public:
    Recorder(HWND notify, WPARAM generation) : notify_(notify), generation_(generation) {}
    ~Recorder();
    void Start(winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item,
               std::filesystem::path const& file, bool showCursor);
    // Ends capture and finishes the file on a worker thread, then posts Finished or Failed.
    void Stop();
    double Duration() const;
    std::wstring Error() const;

private:
    void OnFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& pool);
    void Encode(ID3D11Texture2D* frame, winrt::Windows::Graphics::SizeInt32 content, int64_t time);
    void CreateProcessor(UINT width, UINT height);
    void WriteSample(ID3D11Texture2D* nv12, int64_t time);
    void Finish();
    void CloseCapture() noexcept;
    void Fail(std::wstring const& message);
    void Post(RecorderEvent event) const { PostMessageW(notify_, RecorderMessage, generation_, event); }

    HWND notify_;
    WPARAM generation_;
    mutable std::mutex mutex_;
    bool stopping_ = false;
    bool started_ = false;
    int64_t start_ = -1;       // capture clock time of the first frame
    int64_t last_ = -1;        // media time of the last written frame
    int64_t clockOffset_ = 0;  // QPC time minus capture clock time
    int64_t stopTicks_ = 0;
    double duration_ = 0;
    std::wstring error_;
    std::filesystem::path file_;
    PixelSize size_;

    winrt::com_ptr<ID3D11Device> d3d_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11VideoDevice> video_;
    winrt::com_ptr<ID3D11VideoContext> videoContext_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor> processor_;
    winrt::com_ptr<ID3D11Texture2D> copy_;
    winrt::com_ptr<ID3D11VideoProcessorInputView> input_;
    winrt::com_ptr<ID3D11Texture2D> lastOutput_;
    winrt::com_ptr<IMFDXGIDeviceManager> manager_;
    winrt::com_ptr<IMFSinkWriter> writer_;
    DWORD stream_ = 0;

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize_{};
    winrt::event_token arrived_{}, closed_{};
};
