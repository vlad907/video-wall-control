// The PC's sound outputs: listing them and playing to one. Windows: WASAPI shared mode
// (audio_wasapi.cpp). Elsewhere there are none (audio_none.cpp).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mosaic {

struct AudioDevice {
    std::string id;    // stable id (Windows endpoint id)
    std::string name;  // "Speakers (Focusrite USB Audio)"
    int channels = 2;
    int rate = 48000;
    bool isDefault = false;
};

std::vector<AudioDevice> listAudioDevices(std::string& error);

// Called on the device's own thread for each period: fill `frames` interleaved float frames of
// `channels` channels, which will be heard at `heardAt` (seconds, steady clock).
using AudioRender = std::function<void(float* interleaved, int frames, int channels, double rate, double heardAt)>;

// A running output. It opens the device on its own thread and keeps reopening it (after the
// device is unplugged, changed or busy), reporting why it can't play meanwhile.
class AudioDeviceStream {
public:
    virtual ~AudioDeviceStream() = default;
    virtual std::string error() const = 0;  // empty while playing
    virtual std::string deviceName() const = 0;
    virtual int channels() const = 0;       // 0 until opened
    virtual double rate() const = 0;
};

// `deviceId` empty = the Windows default output.
std::unique_ptr<AudioDeviceStream> openAudioDevice(const std::string& deviceId, AudioRender render);

}  // namespace mosaic
