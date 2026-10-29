// Video I/O interfaces. The engine only talks to these; NDI is one
// implementation, compiled in when the NDI SDK is available.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mosaic {

// One rendered output frame, pointing straight at GPU readback memory.
struct VideoFrameView {
    const uint8_t* data = nullptr;  // BGRA, top row first
    int stride = 0;                 // bytes per row
    int width = 0, height = 0;
    double fps = 60.0;
    uint64_t frameId = 0;           // engine frame the pixels were rendered in
    int64_t timecode = 0;           // 100 ns units; identical for every output of a frame
};

class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual std::string kind() const = 0;
    // Submits a frame. If retainsUntilNext() is true the memory must stay
    // valid until the next send() or flush() returns (NDI async semantics).
    virtual void send(const VideoFrameView& frame) = 0;
    virtual bool retainsUntilNext() const { return false; }
    // Blocks until the sink no longer references any submitted memory.
    virtual void flush() {}
    // Number of receivers connected, or -1 if unknown.
    virtual int connections() const { return -1; }
};

// Measures throughput without sending anywhere (used when NDI is unavailable).
std::unique_ptr<VideoSink> createNullSink();

// A received NDI frame. Valid until NdiReceiver::release().
struct InputFrame {
    const uint8_t* data = nullptr;  // BGRA/BGRX
    int width = 0, height = 0, stride = 0;
    uint64_t sequence = 0;
    void* opaque = nullptr;
};

class NdiReceiver {
public:
    virtual ~NdiReceiver() = default;
    // Render thread: takes the newest frame received since the last call.
    // Older frames are discarded by the receiver (bounded, latest-wins).
    virtual bool acquireLatest(InputFrame& out) = 0;
    virtual void release(InputFrame& frame) = 0;
    virtual bool connected() const = 0;      // a frame arrived in the last second
    virtual double inputFps() const = 0;
    virtual uint64_t framesReceived() const = 0;
};

class NdiSystem {
public:
    virtual ~NdiSystem() = default;
    virtual std::string version() const = 0;
    virtual std::vector<std::string> discoveredSources() = 0;
    virtual std::unique_ptr<VideoSink> createSender(const std::string& streamName) = 0;
    virtual std::unique_ptr<NdiReceiver> createReceiver(const std::string& sourceName) = 0;
};

// Returns nullptr (with a reason) when built without the SDK or the runtime is missing.
std::unique_ptr<NdiSystem> createNdiSystem(std::string& whyNot);

}  // namespace mosaic
