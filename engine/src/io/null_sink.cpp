#include "io/video_io.hpp"

namespace mosaic {

namespace {
class NullSink final : public VideoSink {
public:
    std::string kind() const override { return "null"; }
    void send(const VideoFrameView& f) override {
        // Touch one byte per row so the readback memory is actually read,
        // like an encoder would, without paying for a full CPU pass.
        uint32_t acc = 0;
        for (int y = 0; y < f.height; y += 1) acc += f.data[static_cast<size_t>(y) * f.stride];
        sink_ = acc;
    }

private:
    volatile uint32_t sink_ = 0;
};
}  // namespace

std::unique_ptr<VideoSink> createNullSink() { return std::make_unique<NullSink>(); }

}  // namespace mosaic
