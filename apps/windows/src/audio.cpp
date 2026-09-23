#include "audio.h"
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace winrt;

namespace {
constexpr REFERENCE_TIME BufferTicks = 2'000'000;         // 200 ms WASAPI buffer
constexpr int64_t LatencyFrames = AudioRate * 15 / 100;   // deliver 150 ms behind live
constexpr int64_t SnapFrames = AudioRate / 500;           // 2 ms: treat as continuous
constexpr int64_t FlushFrames = 1024;

int64_t NowTicks() {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return counter.QuadPart / frequency.QuadPart * 10'000'000 +
           counter.QuadPart % frequency.QuadPart * 10'000'000 / frequency.QuadPart;
}

WAVEFORMATEX FloatStereo() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = AudioRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

com_ptr<IAudioClient> DefaultDevice(EDataFlow flow) {
    auto enumerator = create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    com_ptr<IMMDevice> device;
    check_hresult(enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.put()));
    com_ptr<IAudioClient> client;
    check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
    return client;
}

// Receives the result of ActivateAudioInterfaceAsync, which completes on a worker thread.
struct Activation : implements<Activation, IActivateAudioInterfaceCompletionHandler> {
    handle done{check_pointer(CreateEventW(nullptr, TRUE, FALSE, nullptr))};
    HRESULT result = E_FAIL;
    com_ptr<IAudioClient> client;

    HRESULT __stdcall ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) noexcept override {
        com_ptr<::IUnknown> unknown;
        HRESULT activated = E_FAIL;
        result = operation->GetActivateResult(&activated, unknown.put());
        if (SUCCEEDED(result)) result = activated;
        if (SUCCEEDED(result)) client = unknown.try_as<IAudioClient>();
        if (SUCCEEDED(result) && !client) result = E_NOINTERFACE;
        SetEvent(done.get());
        return S_OK;
    }
};

// Sound from one app and the processes it started (a browser plays audio from a child process).
com_ptr<IAudioClient> AppLoopback(DWORD processId) {
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = processId;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT value{};
    value.vt = VT_BLOB;
    value.blob.cbSize = sizeof(parameters);
    value.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
    auto handler = make_self<Activation>();
    com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
    check_hresult(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &value, handler.get(), operation.put()));
    if (WaitForSingleObject(handler->done.get(), 5000) != WAIT_OBJECT_0) throw hresult_error(E_FAIL, L"App audio didn’t start.");
    check_hresult(handler->result);
    return handler->client;
}
}  // namespace

AudioMixer::~AudioMixer() {
    stop_ = true;
    if (!thread_.joinable()) return;
    // The last reference can be released on the pump thread itself; it can't wait for itself.
    if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
    else thread_.join();
}

AudioMixer::Input AudioMixer::Prepare(com_ptr<IAudioClient> const& client, DWORD flags) {
    auto format = FloatStereo();
    // AUTOCONVERTPCM lets Windows resample every device to 48 kHz stereo float for us.
    check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, BufferTicks, 0, &format, nullptr));
    Input input;
    input.event.attach(check_pointer(CreateEventW(nullptr, FALSE, FALSE, nullptr)));
    check_hresult(client->SetEventHandle(input.event.get()));
    check_hresult(client->GetService(__uuidof(IAudioCaptureClient), input.capture.put_void()));
    input.client = client;
    return input;
}

void AudioMixer::Open(AudioOptions const& options) {
    if (options.source) {
        bool appAudio = false;
        if (options.processId) {
            try {
                inputs_.push_back(Prepare(AppLoopback(options.processId), AUDCLNT_STREAMFLAGS_LOOPBACK));
                appAudio = true;
            } catch (hresult_error const&) {
                // Older Windows or an app that can't be isolated: fall back to all computer sound.
            }
        }
        if (!appAudio) {
            try {
                inputs_.push_back(Prepare(DefaultDevice(eRender),
                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY));
            } catch (hresult_error const&) {
                throw hresult_error(E_FAIL, L"Computer sound isn’t available. Check that a speaker or headphones are set up, or turn off “Sound from what you record”.");
            }
        }
    }
    if (options.microphone) {
        try {
            inputs_.push_back(Prepare(DefaultDevice(eCapture), AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY));
        } catch (hresult_error const& error) {
            if (error.code() == E_ACCESSDENIED)
                throw hresult_error(E_ACCESSDENIED, L"Microphone access is off. Turn on Settings → Privacy & security → Microphone → “Let desktop apps access your microphone”, or record without it.");
            throw hresult_error(error.code(), L"No microphone is available. Connect one, or turn off Microphone.");
        }
    }
}

void AudioMixer::Start(Deliver deliver) {
    deliver_ = std::move(deliver);
    for (auto& input : inputs_) check_hresult(input.client->Start());
    thread_ = std::thread([this] { Pump(); });
}

int64_t AudioMixer::FrameAt(int64_t ticks) const {
    return (ticks - zero_) * AudioRate / 10'000'000;
}

void AudioMixer::Pump() {
    std::vector<HANDLE> events;
    for (auto& input : inputs_) events.push_back(input.event.get());
    while (!stop_) {
        // Loopback sends nothing while no sound plays, so also wake every 10 ms.
        WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, 10);
        for (auto& input : inputs_) Drain(input);
        if (zero_ >= 0) Flush(FrameAt(NowTicks()) - LatencyFrames, false);
    }
    for (auto& input : inputs_) Drain(input);
}

void AudioMixer::Drain(Input& input) {
    if (input.failed) return;
    UINT32 packet = 0;
    while (true) {
        if (FAILED(input.capture->GetNextPacketSize(&packet))) {
            input.failed = true;  // device unplugged or changed: keep recording, silence from here
            return;
        }
        if (packet == 0) return;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devicePosition = 0, qpc = 0;
        if (FAILED(input.capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpc))) {
            input.failed = true;
            return;
        }
        if (zero_ >= 0) {
            int64_t position = qpc ? FrameAt(static_cast<int64_t>(qpc))
                                   : (input.next >= 0 ? input.next : FrameAt(NowTicks()));
            // Keep continuous audio continuous; small timestamp jitter would otherwise click.
            if (input.next >= 0 && std::llabs(position - input.next) <= SnapFrames) position = input.next;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) Mix(reinterpret_cast<float const*>(data), frames, position);
            input.next = position + frames;
        }
        input.capture->ReleaseBuffer(frames);
    }
}

void AudioMixer::Mix(float const* samples, uint32_t frames, int64_t position) {
    int64_t skip = std::max<int64_t>(0, flushed_ - position);  // already delivered: too late
    if (skip >= frames || position - flushed_ > AudioRate * 10) return;
    size_t offset = static_cast<size_t>((position + skip - flushed_) * 2);
    size_t count = static_cast<size_t>((frames - skip) * 2);
    if (pending_.size() < offset + count) pending_.resize(offset + count, 0.0f);
    for (size_t i = 0; i < count; ++i) pending_[offset + i] += samples[skip * 2 + i];
}

void AudioMixer::Flush(int64_t target, bool final) {
    int64_t count = target - flushed_;
    if (count <= 0 || (!final && count < FlushFrames)) return;
    std::vector<int16_t> out(static_cast<size_t>(count * 2));
    for (size_t i = 0; i < out.size(); ++i) {
        float value = i < pending_.size() ? pending_[i] : 0.0f;
        out[i] = static_cast<int16_t>(std::lrint(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    }
    deliver_(out.data(), static_cast<size_t>(count), flushed_);
    pending_.erase(pending_.begin(), pending_.begin() + std::min(pending_.size(), out.size()));
    flushed_ = target;
}

void AudioMixer::Stop(int64_t endTicks) {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    for (auto& input : inputs_) input.client->Stop();
    if (zero_ >= 0 && deliver_) Flush(FrameAt(endTicks), true);
}
