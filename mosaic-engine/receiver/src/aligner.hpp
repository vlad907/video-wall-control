// Picks which frame each stream shows on the next refresh, so all displays show the same
// engine frame. Pure logic, no threads or GPU: tested by the self-test.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mosaic::rx {

struct StreamView {
    uint64_t newest = 0;  // newest frame id held; 0 = nothing yet, or not a mosaic stream
    bool synced = false;  // its frames carry engine frame ids
    bool stale = false;   // nothing arrived for a while (lost signal): don't wait for it
};

struct Choice {
    uint64_t target = 0;         // the frame every in-step stream shows
    std::vector<uint64_t> show;  // per stream: frame id to take (0 = its newest frame)
    std::vector<bool> lagging;   // per stream: too far behind to wait for; shows its own newest
};

// The newest frame id that every stream has, among streams no more than `lagFrames` behind
// the leader. Never steps backwards (`previous` is the last target). A stream more than
// `lagFrames` behind doesn't hold the others: it shows what it has and counts as late.
inline Choice chooseFrames(const std::vector<StreamView>& streams, uint64_t previous, uint64_t lagFrames) {
    Choice c;
    c.show.assign(streams.size(), 0);
    c.lagging.assign(streams.size(), false);
    uint64_t leader = 0;
    for (auto& s : streams)
        if (s.synced && !s.stale) leader = std::max(leader, s.newest);
    if (leader == 0) return c;  // nothing in step: everyone shows their newest

    uint64_t target = UINT64_MAX;
    for (size_t i = 0; i < streams.size(); ++i) {
        const auto& s = streams[i];
        if (!s.synced || s.stale || s.newest == 0) continue;
        if (s.newest + lagFrames < leader) {
            c.lagging[i] = true;
            continue;
        }
        target = std::min(target, s.newest);
    }
    c.target = std::max(target == UINT64_MAX ? leader : target, previous);
    for (size_t i = 0; i < streams.size(); ++i)
        if (streams[i].synced && !streams[i].stale && !c.lagging[i]) c.show[i] = c.target;
    return c;
}

}  // namespace mosaic::rx
