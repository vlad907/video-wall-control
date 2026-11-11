// No sound outputs on this platform (the engine plays audio on Windows only).
#include "audio/audio_device.hpp"

namespace mosaic {

std::vector<AudioDevice> listAudioDevices(std::string& error) {
    error = "audio outputs need Windows";
    return {};
}

namespace {
class NoDevice final : public AudioDeviceStream {
public:
    std::string error() const override { return "audio outputs need Windows"; }
    std::string deviceName() const override { return ""; }
    int channels() const override { return 0; }
    double rate() const override { return 0; }
};
}  // namespace

std::unique_ptr<AudioDeviceStream> openAudioDevice(const std::string&, AudioRender) { return std::make_unique<NoDevice>(); }

std::vector<AudioDevice> listAudioInputs(std::string& error) {
    error = "sound inputs need Windows";
    return {};
}

namespace {
class NoInput final : public AudioInputStream {
public:
    std::string error() const override { return "sound inputs need Windows"; }
    std::string deviceName() const override { return ""; }
    int channels() const override { return 0; }
};
}  // namespace

std::unique_ptr<AudioInputStream> openAudioInput(const std::string&, std::shared_ptr<AudioRing>) { return std::make_unique<NoInput>(); }

}  // namespace mosaic
