// Video file playback for video sources: a decoder thread per file reads frames ahead into a
// small queue; the render thread takes the frame that's due. Windows decodes with Media
// Foundation (hardware, frames stay on the GPU as NV12 textures); macOS with AVFoundation
// (CPU BGRA, for development).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mosaic {

struct DecodedFrame {
    double pts = 0;  // seconds on the playback timeline (keeps counting across loops)
    int width = 0, height = 0;
    // Hardware decoding (Windows): an NV12 texture and array slice on the renderer's device.
    void* gpuTexture = nullptr;
    unsigned arraySlice = 0;
    int colorSpace = 0;  // 0 = BT.709 (SDR), 1 = BT.2020 PQ (HDR10), 2 = BT.2020 HLG
    // Otherwise: BGRA pixels in memory.
    const uint8_t* bgra = nullptr;
    int stride = 0;
    std::shared_ptr<void> hold;  // keeps the frame's memory / decoder surface alive
};

struct VideoInfo {
    int width = 0, height = 0;
    double fps = 0, durationS = 0;
    std::string codec;      // "H.264", "HEVC", "AV1", …
    bool hardware = false;  // decoded by the GPU
    bool hdr = false;       // HDR (PQ / HLG): shown converted to the wall's SDR
    int bitDepth = 8;
};

// Platform part: reads one frame at a time, on the decoder thread only.
class VideoBackend {
public:
    virtual ~VideoBackend() = default;
    // One frame, or `endOfStream`. A call may also return neither (try again). False = error.
    virtual bool read(DecodedFrame& out, bool& endOfStream, std::string& error) = 0;
    virtual bool rewind(std::string& error) = 0;
    virtual VideoInfo info() const = 0;
};

// nativeDevice: the renderer's device (ID3D11Device* on Windows) for hardware decoding, or null.
std::unique_ptr<VideoBackend> openVideoBackend(const std::string& path, void* nativeDevice, std::string& error);

class VideoFileDecoder {
public:
    virtual ~VideoFileDecoder() = default;
    // The newest decoded frame due at `t` (seconds since playback started), if a new one is due.
    // Frames it skips over are dropped (the decoder fell behind, or the clock jumped).
    virtual std::optional<DecodedFrame> frameDue(double t) = 0;
    // Back to the first frame (the caller restarts its clock).
    virtual void restart() = 0;
    virtual void setLoop(bool loop) = 0;
    virtual VideoInfo info() const = 0;
    virtual bool finished() const = 0;       // not looping, and played to the end
    virtual std::string status() const = 0;  // "ok", or what went wrong
    virtual double decodeFps() const = 0;    // frames decoded per second, recently
};

std::unique_ptr<VideoFileDecoder> openVideoFile(const std::string& path, void* nativeDevice, bool loop, std::string& error);

}  // namespace mosaic
