// The media server's sound outputs (layout `audioOutputs`): for each, an audio-only NDI
// connection to its source (shared by outputs playing the same source), a sound device stream,
// and a playhead that keeps the audio the wanted delay behind its arrival (by default the
// wall's delay, so sound and picture line up).
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/layout.hpp"

namespace mosaic {

class NdiSystem;

class AudioOutputs {
public:
    explicit AudioOutputs(NdiSystem* ndi);
    ~AudioOutputs();

    // Any thread. Starts, changes and stops outputs to match; volume, mute and delay changes
    // apply without a gap. `wallDelayMs`: how long after the media server receives a frame the
    // wall shows it (the receivers' buffer, the engine's and the receivers' own delay).
    void configure(const std::vector<AudioOutputConfig>& outputs, double wallDelayMs);
    nlohmann::json status() const;

private:
    struct Live;
    struct Output;
    struct Source;
    NdiSystem* ndi_;
    mutable std::mutex m_;
    std::map<std::string, std::shared_ptr<Source>> sources_;  // by NDI name
    std::map<std::string, std::unique_ptr<Output>> outputs_;  // by output id
    double wallDelayMs_ = 0;
};

}  // namespace mosaic
