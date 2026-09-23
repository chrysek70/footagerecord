#pragma once
#include <windows.h>
#include <unknwn.h>
#include <audioclient.h>
#include <winrt/base.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

constexpr uint32_t AudioRate = 48000;

struct AudioOptions {
    bool source = false;    // sound from what is recorded
    DWORD processId = 0;    // the recorded window's app; 0 records all computer sound
    bool microphone = false;
};

// Captures app or computer sound and/or the microphone as 48 kHz stereo and mixes them on one
// timeline. Packets are placed by their timestamps, so stretches with nothing playing become silence.
class AudioMixer {
public:
    using Deliver = std::function<void(int16_t const* frames, size_t count, int64_t firstFrame)>;
    ~AudioMixer();
    // Opens the devices. Throws hresult_error with a message for the user.
    void Open(AudioOptions const& options);
    bool Enabled() const { return !inputs_.empty(); }
    void Start(Deliver deliver);
    // Timeline zero as a QPC time in 100 ns units: the moment of the first video frame.
    void SetZero(int64_t ticks) { zero_ = ticks; }
    // Stops capture and delivers everything up to `endTicks`, padding with silence.
    void Stop(int64_t endTicks);

private:
    struct Input {
        winrt::com_ptr<IAudioClient> client;
        winrt::com_ptr<IAudioCaptureClient> capture;
        winrt::handle event;
        int64_t next = -1;
        bool failed = false;
    };
    static Input Prepare(winrt::com_ptr<IAudioClient> const& client, DWORD flags);
    void Pump();
    void Drain(Input& input);
    void Mix(float const* samples, uint32_t frames, int64_t position);
    void Flush(int64_t target, bool final);
    int64_t FrameAt(int64_t ticks) const;

    std::vector<Input> inputs_;
    std::vector<float> pending_;  // interleaved stereo from frame `flushed_` onward
    int64_t flushed_ = 0;
    std::atomic<int64_t> zero_{-1};
    std::atomic<bool> stop_{false};
    Deliver deliver_;
    std::thread thread_;
};
