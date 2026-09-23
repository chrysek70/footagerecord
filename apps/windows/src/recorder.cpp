#include "recorder.h"
#include <d3d11_4.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <thread>
#include <utility>

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
constexpr int64_t FrameTicks = 10'000'000 / 30;  // 30 fps in 100 ns units

int64_t NowTicks() {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return counter.QuadPart / frequency.QuadPart * 10'000'000 +
           counter.QuadPart % frequency.QuadPart * 10'000'000 / frequency.QuadPart;
}

com_ptr<IMFMediaType> VideoType(GUID subtype, PixelSize size) {
    com_ptr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(type.put()));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    check_hresult(type->SetGUID(MF_MT_SUBTYPE, subtype));
    check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    check_hresult(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, size.width, size.height));
    check_hresult(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, 30, 1));
    check_hresult(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    check_hresult(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    check_hresult(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    return type;
}
}  // namespace

Recorder::~Recorder() { CloseCapture(); }

void Recorder::Start(GraphicsCaptureItem const& item, std::filesystem::path const& file, bool showCursor) {
    file_ = file;
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, d3d_.put(), nullptr, context_.put()));
    // Capture callbacks, the encoder and the finishing thread share this device.
    d3d_.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    video_ = d3d_.as<ID3D11VideoDevice>();
    videoContext_ = context_.as<ID3D11VideoContext>();
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(d3d_.as<IDXGIDevice>().get(), inspectable.put()));
    device_ = inspectable.as<IDirect3DDevice>();

    auto source = item.Size();
    size_ = FitForH264(source.Width, source.Height);
    if (size_.width == 0) throw hresult_error(E_FAIL, L"The selected source has no usable picture. Choose another window or screen.");

    UINT token = 0;
    check_hresult(MFCreateDXGIDeviceManager(&token, manager_.put()));
    check_hresult(manager_->ResetDevice(d3d_.get(), token));
    com_ptr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(attributes.put(), 3));
    check_hresult(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager_.get()));
    check_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    check_hresult(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    check_hresult(MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.get(), writer_.put()));
    auto output = VideoType(MFVideoFormat_H264, size_);
    auto bitrate = std::clamp<long long>(2LL * size_.width * size_.height, 1'500'000, 24'000'000);
    check_hresult(output->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate)));
    check_hresult(writer_->AddStream(output.get(), &stream_));
    check_hresult(writer_->SetInputMediaType(stream_, VideoType(MFVideoFormat_NV12, size_).get(), nullptr));
    check_hresult(writer_->BeginWriting());

    item_ = item;
    poolSize_ = source;
    pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize_);
    auto weak = weak_from_this();
    arrived_ = pool_.FrameArrived([weak](auto const& sender, auto const&) {
        auto self = weak.lock();
        if (!self) return;
        try { self->OnFrame(sender); }
        catch (hresult_error const& error) { self->Fail(error.message().c_str()); }
        catch (...) { self->Fail(L"Recording failed."); }
    });
    closed_ = item_.Closed([weak](auto const&, auto const&) {
        if (auto self = weak.lock()) self->Post(RecorderSourceClosed);
    });
    session_ = pool_.CreateCaptureSession(item_);
    session_.IsCursorCaptureEnabled(showCursor);
    session_.StartCapture();
}

void Recorder::OnFrame(Direct3D11CaptureFramePool const& sender) {
    std::scoped_lock lock(mutex_);
    if (stopping_) return;
    auto frame = sender.TryGetNextFrame();
    if (!frame) return;
    auto content = frame.ContentSize();
    int64_t captured = frame.SystemRelativeTime().count();
    if (start_ < 0) {
        start_ = captured;
        clockOffset_ = NowTicks() - captured;
    }
    int64_t time = captured - start_;
    // At most 30 frames per second. The capture API only sends frames when something changes.
    if (content.Width > 0 && content.Height > 0 && (last_ < 0 || time - last_ >= FrameTicks * 9 / 10)) {
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        Encode(texture.get(), content, time);
        last_ = time;
        if (!started_) {
            started_ = true;
            Post(RecorderStarted);
        }
    }
    frame.Close();
    if (content.Width > 0 && content.Height > 0 &&
        (content.Width != poolSize_.Width || content.Height != poolSize_.Height)) {
        poolSize_ = content;
        sender.Recreate(device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize_);
    }
}

void Recorder::CreateProcessor(UINT width, UINT height) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
    description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    description.InputFrameRate = {30, 1};
    description.InputWidth = width;
    description.InputHeight = height;
    description.OutputFrameRate = {30, 1};
    description.OutputWidth = size_.width;
    description.OutputHeight = size_.height;
    description.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    enumerator_ = nullptr;
    processor_ = nullptr;
    check_hresult(video_->CreateVideoProcessorEnumerator(&description, enumerator_.put()));
    check_hresult(video_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()));
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
    input.RGB_Range = 0;  // full-range desktop RGB
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
    output.YCbCr_Matrix = 1;  // BT.709
    output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &input);
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.get(), &output);
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr.Y = 16.0f / 255.0f;
    black.YCbCr.Cb = 0.5f;
    black.YCbCr.Cr = 0.5f;
    black.YCbCr.A = 1.0f;
    videoContext_->VideoProcessorSetOutputBackgroundColor(processor_.get(), TRUE, &black);
}

void Recorder::Encode(ID3D11Texture2D* frame, SizeInt32 content, int64_t time) {
    D3D11_TEXTURE2D_DESC frameDescription{};
    frame->GetDesc(&frameDescription);
    D3D11_TEXTURE2D_DESC copyDescription{};
    if (copy_) copy_->GetDesc(&copyDescription);
    // Copy out of the capture pool so its frame can be reused at once. Rebuild views when the source resizes.
    if (!copy_ || copyDescription.Width != frameDescription.Width || copyDescription.Height != frameDescription.Height) {
        copyDescription = frameDescription;
        copyDescription.MipLevels = 1;
        copyDescription.ArraySize = 1;
        copyDescription.SampleDesc = {1, 0};
        copyDescription.Usage = D3D11_USAGE_DEFAULT;
        copyDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        copyDescription.CPUAccessFlags = 0;
        copyDescription.MiscFlags = 0;
        input_ = nullptr;
        copy_ = nullptr;
        check_hresult(d3d_->CreateTexture2D(&copyDescription, nullptr, copy_.put()));
        CreateProcessor(frameDescription.Width, frameDescription.Height);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputView{};
        inputView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        check_hresult(video_->CreateVideoProcessorInputView(copy_.get(), enumerator_.get(), &inputView, input_.put()));
    }
    context_->CopyResource(copy_.get(), frame);

    // A new NV12 texture per sample: the encoder may still be reading earlier ones.
    D3D11_TEXTURE2D_DESC outputDescription{};
    outputDescription.Width = size_.width;
    outputDescription.Height = size_.height;
    outputDescription.MipLevels = 1;
    outputDescription.ArraySize = 1;
    outputDescription.Format = DXGI_FORMAT_NV12;
    outputDescription.SampleDesc = {1, 0};
    outputDescription.Usage = D3D11_USAGE_DEFAULT;
    outputDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
    com_ptr<ID3D11Texture2D> output;
    check_hresult(d3d_->CreateTexture2D(&outputDescription, nullptr, output.put()));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputView{};
    outputView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    com_ptr<ID3D11VideoProcessorOutputView> target;
    check_hresult(video_->CreateVideoProcessorOutputView(output.get(), enumerator_.get(), &outputView, target.put()));

    // Fit the picture into the fixed video size, keeping its shape, with black around it.
    LONG width = std::min<LONG>(content.Width, frameDescription.Width);
    LONG height = std::min<LONG>(content.Height, frameDescription.Height);
    double scale = std::min(static_cast<double>(size_.width) / width, static_cast<double>(size_.height) / height);
    LONG fittedWidth = std::lround(width * scale), fittedHeight = std::lround(height * scale);
    RECT source{0, 0, width, height};
    RECT destination{(size_.width - fittedWidth) / 2, (size_.height - fittedHeight) / 2, 0, 0};
    destination.right = destination.left + fittedWidth;
    destination.bottom = destination.top + fittedHeight;
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &source);
    videoContext_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &destination);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_.get();
    check_hresult(videoContext_->VideoProcessorBlt(processor_.get(), target.get(), 0, 1, &stream));
    WriteSample(output.get(), time);
    lastOutput_ = output;
}

void Recorder::WriteSample(ID3D11Texture2D* nv12, int64_t time) {
    com_ptr<IMFMediaBuffer> buffer;
    check_hresult(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, buffer.put()));
    DWORD length = 0;
    check_hresult(buffer.as<IMF2DBuffer>()->GetContiguousLength(&length));
    check_hresult(buffer->SetCurrentLength(length));
    com_ptr<IMFSample> sample;
    check_hresult(MFCreateSample(sample.put()));
    check_hresult(sample->AddBuffer(buffer.get()));
    check_hresult(sample->SetSampleTime(time));
    check_hresult(sample->SetSampleDuration(FrameTicks));
    check_hresult(writer_->WriteSample(stream_, sample.get()));
}

void Recorder::Stop() {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        stopTicks_ = NowTicks();
    }
    CloseCapture();
    std::thread([self = shared_from_this()] { self->Finish(); }).detach();
}

void Recorder::Finish() {
    init_apartment(apartment_type::multi_threaded);
    std::wstring failure;
    try {
        std::scoped_lock lock(mutex_);
        if (!lastOutput_) throw hresult_error(E_FAIL, L"No video frames were recorded. Try choosing the source again.");
        // Hold the last picture until Stop was pressed, so a still screen doesn't end the video early.
        int64_t end = stopTicks_ - clockOffset_ - start_;
        if (end > last_ + FrameTicks) WriteSample(lastOutput_.get(), end);
        check_hresult(writer_->Finalize());
        writer_ = nullptr;
        // Only a file with a readable, non-empty duration counts as saved.
        com_ptr<IMFSourceReader> reader;
        check_hresult(MFCreateSourceReaderFromURL(file_.c_str(), nullptr, reader.put()));
        PROPVARIANT value;
        PropVariantInit(&value);
        check_hresult(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &value));
        duration_ = value.uhVal.QuadPart / 1e7;
        PropVariantClear(&value);
        if (duration_ <= 0) throw hresult_error(E_FAIL, L"No video frames were recorded. Try choosing the source again.");
    } catch (hresult_error const& error) {
        failure = L"The recording couldn’t be saved: " + std::wstring(error.message());
    }
    if (failure.empty()) {
        Post(RecorderFinished);
    } else {
        Fail(failure);
    }
}

void Recorder::Fail(std::wstring const& message) {
    {
        std::scoped_lock lock(mutex_);
        if (error_.empty()) error_ = message;
    }
    Post(RecorderFailed);
}

double Recorder::Duration() const {
    std::scoped_lock lock(mutex_);
    return duration_;
}

std::wstring Recorder::Error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

void Recorder::CloseCapture() noexcept {
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    GraphicsCaptureItem item{nullptr};
    {
        std::scoped_lock lock(mutex_);
        pool = std::exchange(pool_, nullptr);
        session = std::exchange(session_, nullptr);
        item = std::exchange(item_, nullptr);
    }
    // Never hold the frame mutex while closing the pool: its callback takes that mutex.
    try { if (item) item.Closed(closed_); } catch (...) { }
    try { if (pool) pool.FrameArrived(arrived_); } catch (...) { }
    try { if (session) session.Close(); } catch (...) { }
    try { if (pool) pool.Close(); } catch (...) { }
}
