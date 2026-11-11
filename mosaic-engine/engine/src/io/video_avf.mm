// macOS video decoding with AVFoundation (AVAssetReader, BGRA in memory). For development:
// the media server runs Windows (video_mf.cpp, hardware decoding on the GPU).
#include "io/video_file.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

namespace mosaic {

namespace {

std::string fourcc(FourCharCode c) {
    switch (c) {
        case kCMVideoCodecType_H264: return "H.264";
        case kCMVideoCodecType_HEVC: return "HEVC";
        case 'av01': return "AV1";
        case kCMVideoCodecType_AppleProRes422: return "ProRes 422";
        default: break;
    }
    char s[5] = {char(c >> 24), char(c >> 16), char(c >> 8), char(c), 0};
    return s;
}

class AvfBackend final : public VideoBackend {
public:
    bool open(const std::string& path, std::string& error) {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        asset_ = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray<AVAssetTrack*>* tracks = [asset_ tracksWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
        if (tracks.count == 0) return error = "the file has no video track (or isn't a video)", false;
        track_ = tracks.firstObject;
        CGSize size = track_.naturalSize;
        info_.width = static_cast<int>(size.width);
        info_.height = static_cast<int>(size.height);
        info_.fps = track_.nominalFrameRate;
        info_.durationS = CMTimeGetSeconds(asset_.duration);
        if (track_.formatDescriptions.count)
            info_.codec = fourcc(CMFormatDescriptionGetMediaSubType((__bridge CMFormatDescriptionRef)track_.formatDescriptions.firstObject));
        info_.hardware = true;  // VideoToolbox decodes on the media engine; frames are copied out as BGRA
        return start(error);
    }

    bool read(DecodedFrame& out, bool& endOfStream, std::string& error) override {
        CMSampleBufferRef sb = [output_ copyNextSampleBuffer];
        if (!sb) {
            if (reader_.status == AVAssetReaderStatusFailed) return error = reader_.error.localizedDescription.UTF8String ?: "decoding failed", false;
            endOfStream = true;
            return true;
        }
        CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
        if (!pb) {
            CFRelease(sb);
            return true;
        }
        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        out.pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
        out.width = static_cast<int>(CVPixelBufferGetWidth(pb));
        out.height = static_cast<int>(CVPixelBufferGetHeight(pb));
        out.bgra = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb));
        out.stride = static_cast<int>(CVPixelBufferGetBytesPerRow(pb));
        out.hold = std::shared_ptr<void>(sb, [pb](void* p) {
            CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
            CFRelease(static_cast<CMSampleBufferRef>(p));
        });
        return true;
    }

    bool rewind(std::string& error) override {
        [reader_ cancelReading];
        return start(error);
    }

    VideoInfo info() const override { return info_; }

private:
    bool start(std::string& error) {
        NSError* err = nil;
        reader_ = [[AVAssetReader alloc] initWithAsset:asset_ error:&err];
        if (!reader_) return error = err.localizedDescription.UTF8String ?: "cannot read the file", false;
        output_ = [[AVAssetReaderTrackOutput alloc] initWithTrack:track_
                                                   outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
        output_.alwaysCopiesSampleData = NO;
        [reader_ addOutput:output_];
        if (![reader_ startReading]) return error = reader_.error.localizedDescription.UTF8String ?: "cannot start decoding", false;
        return true;
    }

    AVURLAsset* asset_ = nil;
    AVAssetTrack* track_ = nil;
    AVAssetReader* reader_ = nil;
    AVAssetReaderTrackOutput* output_ = nil;
    VideoInfo info_;
};

}  // namespace

std::vector<DecoderInfo> listVideoDecoders() { return {}; }  // macOS: AVFoundation picks (VideoToolbox, on the GPU where it can)

std::unique_ptr<VideoBackend> openVideoBackend(const std::string& path, void*, std::string& error) {
    auto b = std::make_unique<AvfBackend>();
    if (!b->open(path, error)) return nullptr;
    return b;
}

}  // namespace mosaic
