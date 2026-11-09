#include "audio/audio_outputs.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "audio/audio_device.hpp"
#include "audio/audio_ring.hpp"
#include "io/video_io.hpp"

using nlohmann::json;

namespace mosaic {

struct AudioOutputs::Source {
    std::shared_ptr<AudioRing> ring = std::make_shared<AudioRing>();
    std::unique_ptr<NdiAudioInput> rx;
};

// What an output's device thread reads (settings) and writes (meters), without locks.
struct AudioOutputs::Live {
    std::shared_ptr<AudioRing> ring;
    std::atomic<float> gain{1.f};
    std::atomic<double> delaySeconds{0};
    std::atomic<int> pair{0};
    std::atomic<float> peakL{0}, peakR{0};
    std::atomic<double> errorMs{0};
    std::atomic<uint64_t> jumps{0}, missing{0};
    AudioPlayhead head;         // device thread only
    std::vector<float> l, r;    // device thread only

    void render(float* out, int frames, int channels, double rate, double heardAt) {
        l.resize(static_cast<size_t>(frames));
        r.resize(static_cast<size_t>(frames));
        head.fill(*ring, rate, heardAt, delaySeconds.load(), frames, l.data(), r.data());
        float g = gain.load(), pl = 0, pr = 0;
        std::fill(out, out + static_cast<size_t>(frames) * channels, 0.f);
        int p = pair.load();
        for (int i = 0; i < frames; ++i) {
            float a = l[i] * g, b = r[i] * g;
            pl = std::max(pl, std::fabs(a));
            pr = std::max(pr, std::fabs(b));
            float* f = out + static_cast<size_t>(i) * channels;
            if (p * 2 + 1 < channels) f[p * 2] = a, f[p * 2 + 1] = b;
            else if (channels == 1) f[0] = 0.5f * (a + b);
        }
        // Meters hold the loudest since the last status read.
        peakL = std::max(peakL.load(), pl);
        peakR = std::max(peakR.load(), pr);
        errorMs = head.errorMs();
        jumps = head.jumps();
        missing = head.missing();
    }
};

struct AudioOutputs::Output {
    AudioOutputConfig cfg;
    std::shared_ptr<Source> source;
    std::shared_ptr<Live> live = std::make_shared<Live>();
    std::unique_ptr<AudioDeviceStream> stream;
};

AudioOutputs::AudioOutputs(NdiSystem* ndi) : ndi_(ndi) {}

AudioOutputs::~AudioOutputs() {
    std::lock_guard l(m_);
    outputs_.clear();  // streams stop (their threads join) before the sources go
    sources_.clear();
}

static double effectiveDelayMs(const AudioOutputConfig& c, double wallDelayMs) {
    return std::clamp(c.matchWall ? wallDelayMs + c.delayMs : c.delayMs, 0.0, 5000.0);
}

void AudioOutputs::configure(const std::vector<AudioOutputConfig>& outputs, double wallDelayMs) {
    std::lock_guard lock(m_);
    wallDelayMs_ = wallDelayMs;
    std::map<std::string, const AudioOutputConfig*> wanted;
    for (auto& c : outputs)
        if (c.enabled && !c.source.empty()) wanted[c.id] = &c;

    // Gone, or now on another device or source: stop (a new stream starts below).
    for (auto it = outputs_.begin(); it != outputs_.end();) {
        auto w = wanted.find(it->first);
        if (w == wanted.end() || w->second->device != it->second->cfg.device || w->second->source != it->second->cfg.source) it = outputs_.erase(it);
        else ++it;
    }
    for (auto& [id, c] : wanted) {
        auto& o = outputs_[id];
        if (!o) {
            o = std::make_unique<Output>();
            auto& src = sources_[c->source];
            if (!src) {
                src = std::make_shared<Source>();
                if (ndi_) src->rx = ndi_->createAudioReceiver(c->source, src->ring);
            }
            o->source = src;
            o->live->ring = src->ring;
        }
        o->cfg = *c;
        o->live->gain = c->mute ? 0.f : static_cast<float>(std::pow(10.0, c->volumeDb / 20.0));
        o->live->delaySeconds = effectiveDelayMs(*c, wallDelayMs) / 1000.0;
        o->live->pair = c->channelPair;
        if (!o->stream) {  // after the settings, so it never plays a moment undelayed
            auto live = o->live;
            o->stream = openAudioDevice(c->device, [live](float* out, int frames, int channels, double rate, double heardAt) {
                live->render(out, frames, channels, rate, heardAt);
            });
        }
    }
    // Sources no output plays any more: disconnect.
    for (auto it = sources_.begin(); it != sources_.end();) {
        bool used = std::any_of(outputs_.begin(), outputs_.end(), [&](auto& o) { return o.second->source == it->second; });
        it = used ? std::next(it) : sources_.erase(it);
    }
}

json AudioOutputs::status() const {
    std::lock_guard lock(m_);
    auto db = [](float peak) { return peak > 1e-5f ? std::round(200.0 * std::log10(peak)) / 10.0 : -100.0; };
    json list = json::array();
    for (auto& [id, o] : outputs_) {
        auto& live = *o->live;
        const auto& src = *o->source;
        int ch = o->stream->channels();
        std::string error = o->stream->error();
        if (error.empty() && ch && o->cfg.channelPair * 2 + 1 >= ch && ch != 1)
            error = "the device has " + std::to_string(ch) + " channels: no channel pair " + std::to_string(o->cfg.channelPair * 2 + 1) + "-" +
                    std::to_string(o->cfg.channelPair * 2 + 2);
        double since = steadySeconds() - src.ring->lastArrival();
        list.push_back({{"id", id},
                        {"device", o->stream->deviceName()},
                        {"channels", ch},
                        {"rate", o->stream->rate()},
                        {"error", error},
                        {"sourceConnected", src.rx != nullptr && src.rx->connected()},
                        {"sourceChannels", src.rx ? src.rx->channels() : 0},
                        {"sourceError", !ndi_ ? "NDI isn't available" : !src.rx ? "cannot connect to the source" : since > 1 ? "no audio from the source" : ""},
                        {"levelDb", {db(live.peakL.exchange(0)), db(live.peakR.exchange(0))}},
                        {"delayMs", effectiveDelayMs(o->cfg, wallDelayMs_)},
                        {"offByMs", live.errorMs.load()},
                        {"jumps", live.jumps.load()},
                        {"silentMs", live.missing.load() * 1000.0 / std::max(1.0, o->stream->rate())}});
    }
    return {{"wallDelayMs", wallDelayMs_}, {"outputs", list}};
}

}  // namespace mosaic
