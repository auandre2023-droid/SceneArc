// Segment thumbnails (offline tool, never on the playback path): one small JPEG per
// segment start, decoded in software from the nearest keyframe. CPU readback is
// allowed here; the real-time path stays GPU-only.

#include "media_tools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

namespace vp::media_tools {
namespace {
namespace fs = std::filesystem;
constexpr AVRational kMs{1, 1000};

fs::path Utf8Path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
std::string PathUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
std::string FfError(const char* prefix, int code) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, detail, sizeof detail);
    return std::string(prefix) + ": " + detail;
}

bool WriteAtomically(const fs::path& path, const std::uint8_t* data, std::size_t size) {
    fs::path temp = path;
    temp += ".tmp"; // not path.string(): that converts through the ANSI code page
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) { fs::remove(temp, ec); return false; }
    return true;
}

// 24-bit BMP fallback when the build has no MJPEG encoder.
std::vector<std::uint8_t> EncodeBmp(const AVFrame* bgr) {
    const int w = bgr->width, h = bgr->height;
    const int row = (w * 3 + 3) & ~3;
    const std::uint32_t image = static_cast<std::uint32_t>(row * h), file = 54 + image;
    std::vector<std::uint8_t> out(file, 0);
    auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
    out[0] = 'B'; out[1] = 'M';
    put32(2, file); put32(10, 54); put32(14, 40); put32(18, static_cast<std::uint32_t>(w));
    put32(22, static_cast<std::uint32_t>(-h)); // top-down
    out[26] = 1; out[28] = 24; put32(34, image);
    for (int y = 0; y < h; ++y)
        std::copy_n(bgr->data[0] + static_cast<std::ptrdiff_t>(y) * bgr->linesize[0], w * 3, out.begin() + 54 + static_cast<std::ptrdiff_t>(y) * row);
    return out;
}

class Encoder {
public:
    ~Encoder() {
        avcodec_free_context(&jpeg_);
        sws_freeContext(sws_);
        av_frame_free(&scaled_);
        av_packet_free(&packet_);
    }

    // Returns the encoded image and its file extension.
    bool Encode(const AVFrame* source, int width, std::vector<std::uint8_t>* bytes, const char** extension) {
        if (!source || source->width <= 0 || source->height <= 0) return false;
        const double aspect = source->sample_aspect_ratio.num > 0 && source->sample_aspect_ratio.den > 0
                                  ? av_q2d(source->sample_aspect_ratio) : 1.0;
        const int out_w = std::max(16, width) & ~1;
        const int out_h = std::max(16, static_cast<int>(out_w * source->height / (source->width * aspect) + 0.5)) & ~1;
        if (!Prepare(out_w, out_h)) return false;
        sws_ = sws_getCachedContext(sws_, source->width, source->height, static_cast<AVPixelFormat>(source->format), out_w, out_h, format_,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;
        if (av_frame_make_writable(scaled_) < 0) return false;
        sws_scale(sws_, source->data, source->linesize, 0, source->height, scaled_->data, scaled_->linesize);
        if (!jpeg_) {
            *bytes = EncodeBmp(scaled_);
            *extension = ".bmp";
            return true;
        }
        scaled_->pts = frame_index_++;
        scaled_->quality = FF_QP2LAMBDA * 4;
        if (avcodec_send_frame(jpeg_, scaled_) < 0) return false;
        if (avcodec_receive_packet(jpeg_, packet_) < 0) return false;
        bytes->assign(packet_->data, packet_->data + packet_->size);
        av_packet_unref(packet_);
        *extension = ".jpg";
        return true;
    }

private:
    bool Prepare(int w, int h) {
        if (scaled_ && scaled_->width == w && scaled_->height == h) return true;
        avcodec_free_context(&jpeg_);
        av_frame_free(&scaled_);
        if (!packet_) packet_ = av_packet_alloc();
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        format_ = codec ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_BGR24;
        if (codec) {
            jpeg_ = avcodec_alloc_context3(codec);
            jpeg_->width = w;
            jpeg_->height = h;
            jpeg_->pix_fmt = AV_PIX_FMT_YUVJ420P;
            jpeg_->color_range = AVCOL_RANGE_JPEG;
            jpeg_->time_base = {1, 25};
            jpeg_->flags |= AV_CODEC_FLAG_QSCALE;
            jpeg_->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
            if (avcodec_open2(jpeg_, codec, nullptr) < 0) { avcodec_free_context(&jpeg_); format_ = AV_PIX_FMT_BGR24; }
        }
        scaled_ = av_frame_alloc();
        scaled_->width = w;
        scaled_->height = h;
        scaled_->format = format_;
        return av_frame_get_buffer(scaled_, 32) >= 0;
    }

    AVCodecContext* jpeg_{};
    SwsContext* sws_{};
    AVFrame* scaled_{};
    AVPacket* packet_{};
    AVPixelFormat format_{AV_PIX_FMT_YUVJ420P};
    std::int64_t frame_index_{};
};
} // namespace

int GenerateThumbnails(const std::string& media_utf8, const std::string& out_dir_utf8, std::int64_t interval_ms, int width,
                       const ThumbnailCallback& callback) {
    if (interval_ms < 10'000) interval_ms = 10'000;
    const fs::path out_dir = Utf8Path(out_dir_utf8);
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, media_utf8.c_str(), nullptr, nullptr);
    if (result < 0) { callback(ThumbnailEvent::Error, 0, FfError("PC_ERR_OPEN_INPUT", result)); return -1; }
    avformat_find_stream_info(format, nullptr);
    const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) { callback(ThumbnailEvent::Error, 0, "PC_ERR_NO_VIDEO_STREAM"); avformat_close_input(&format); return -1; }
    AVStream* stream = format->streams[stream_index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codec) { callback(ThumbnailEvent::Error, 0, "PC_ERR_CODEC_NOT_FOUND"); avformat_close_input(&format); return -1; }
    avcodec_parameters_to_context(codec, stream->codecpar);
    codec->pkt_timebase = stream->time_base;
    codec->thread_count = 0;                 // all cores: one 4K HEVC keyframe at a time
    codec->skip_frame = AVDISCARD_NONKEY;    // only keyframes are needed
    result = avcodec_open2(codec, decoder, nullptr);
    if (result < 0) {
        callback(ThumbnailEvent::Error, 0, FfError("PC_ERR_DECODER_OPEN", result));
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return -1;
    }

    const std::int64_t start_ms = format->start_time == AV_NOPTS_VALUE ? 0 : av_rescale_q(format->start_time, AV_TIME_BASE_Q, kMs);
    const std::int64_t duration_ms = format->duration == AV_NOPTS_VALUE ? 0 : av_rescale_q(format->duration, AV_TIME_BASE_Q, kMs);
    int status = 0;
    if (callback(ThumbnailEvent::Info, duration_ms, {}) != 0) status = 1;

    Encoder encoder;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<std::uint8_t> bytes;
    for (std::int64_t segment = 0; status == 0 && duration_ms > 0 && segment < duration_ms; segment += interval_ms) {
        const std::string stem = std::to_string(segment / 1000);
        fs::path existing;
        for (const char* ext : {".jpg", ".bmp"})
            if (fs::exists(out_dir / (stem + ext), ec)) existing = out_dir / (stem + ext);
        if (!existing.empty()) {
            if (callback(ThumbnailEvent::Image, segment, PathUtf8(existing)) != 0) status = 1;
            continue;
        }
        // The very first segment would mostly be a black frame or a logo: look a little later.
        const std::int64_t capture = segment == 0 ? std::min<std::int64_t>({30'000, interval_ms / 2, duration_ms / 2}) : segment;
        const std::int64_t ts = av_rescale_q(capture + start_ms, kMs, stream->time_base);
        avformat_seek_file(format, stream_index, std::numeric_limits<std::int64_t>::min(), ts, ts, 0);
        avcodec_flush_buffers(codec);
        bool got = false;
        for (int reads = 0; !got && reads < 2000 && av_read_frame(format, packet) >= 0; ++reads) {
            if (packet->stream_index == stream_index && avcodec_send_packet(codec, packet) >= 0)
                got = avcodec_receive_frame(codec, frame) >= 0;
            av_packet_unref(packet);
        }
        if (!got) { // flush: frame threading may still hold the keyframe
            avcodec_send_packet(codec, nullptr);
            got = avcodec_receive_frame(codec, frame) >= 0;
        }
        const char* extension = ".jpg";
        if (got && encoder.Encode(frame, width, &bytes, &extension)) {
            const fs::path file = out_dir / (stem + extension);
            if (WriteAtomically(file, bytes.data(), bytes.size()) && callback(ThumbnailEvent::Image, segment, PathUtf8(file)) != 0) status = 1;
        } else if (callback(ThumbnailEvent::Error, segment, "PC_WARN_THUMBNAIL_DECODE") != 0) {
            status = 1;
        }
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return status;
}

namespace {
// Exact-frame source: decodes from the keyframe before each target up to the frame shown at
// that time. Uses D3D11VA when available (only the chosen frame is copied back), else all CPU
// cores. Consecutive targets close together continue decoding instead of seeking again, so a
// dense list (every 10 s) costs little more than decoding that stretch once.
class ExactSource {
public:
    ~ExactSource() {
        av_frame_free(&sw_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&codec_);
        av_buffer_unref(&hw_);
        avformat_close_input(&format_);
    }

    bool Open(const std::string& media, bool allow_hardware, std::string* error) {
        int result = avformat_open_input(&format_, media.c_str(), nullptr, nullptr);
        if (result < 0) { *error = FfError("PC_ERR_OPEN_INPUT", result); return false; }
        avformat_find_stream_info(format_, nullptr);
        stream_index_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index_ < 0) { *error = "PC_ERR_NO_VIDEO_STREAM"; return false; }
        stream_ = format_->streams[stream_index_];
        start_ms_ = format_->start_time == AV_NOPTS_VALUE ? 0 : av_rescale_q(format_->start_time, AV_TIME_BASE_Q, kMs);
        duration_ms_ = format_->duration == AV_NOPTS_VALUE ? 0 : av_rescale_q(format_->duration, AV_TIME_BASE_Q, kMs);
        const AVRational rate = stream_->avg_frame_rate.num > 0 ? stream_->avg_frame_rate : AVRational{24, 1};
        frame_ts_ = std::max<std::int64_t>(1, av_rescale_q(1, av_inv_q(rate), stream_->time_base));
        if (allow_hardware && OpenDecoder(true)) return Allocate();
        if (OpenDecoder(false)) return Allocate();
        *error = "PC_ERR_DECODER_OPEN";
        return false;
    }

    [[nodiscard]] std::int64_t duration_ms() const { return duration_ms_; }

    // The frame displayed at `ms` (media time from 0), as a CPU frame. Null if none.
    const AVFrame* Capture(std::int64_t ms) {
        const std::int64_t target = av_rescale_q(ms + start_ms_, kMs, stream_->time_base);
        // Continue forward from the current position when the target is a little ahead.
        const bool sequential = !eof_ && have_last_ && target > last_pts_ && target - last_pts_ < av_rescale_q(12'000, kMs, stream_->time_base);
        if (!sequential) {
            avformat_seek_file(format_, stream_index_, std::numeric_limits<std::int64_t>::min(), target, target, 0);
            avcodec_flush_buffers(codec_);
            eof_ = false;
        }
        codec_->skip_frame = AVDISCARD_NONREF; // frames nobody references are not needed until close to the target
        const AVFrame* found = nullptr;
        for (int reads = 0; !found && reads < 6000; ++reads) {
            if (!eof_) {
                const int result = av_read_frame(format_, packet_);
                if (result < 0) { eof_ = true; avcodec_send_packet(codec_, nullptr); }
                else if (packet_->stream_index == stream_index_) {
                    const std::int64_t pts = packet_->pts != AV_NOPTS_VALUE ? packet_->pts : packet_->dts;
                    if (pts != AV_NOPTS_VALUE && pts >= target - 4 * frame_ts_) codec_->skip_frame = AVDISCARD_DEFAULT;
                    avcodec_send_packet(codec_, packet_);
                    av_packet_unref(packet_);
                } else {
                    av_packet_unref(packet_);
                    continue;
                }
            }
            for (;;) {
                const int got = avcodec_receive_frame(codec_, frame_);
                if (got < 0) { if (eof_ && got == AVERROR_EOF) return Finish(); break; }
                const std::int64_t pts = frame_->best_effort_timestamp;
                const std::int64_t duration = frame_->duration > 0 ? frame_->duration : frame_ts_;
                if (pts != AV_NOPTS_VALUE) { last_pts_ = pts; have_last_ = true; }
                // Keep the latest frame before the target as a fallback (end of file, gaps).
                if (pts == AV_NOPTS_VALUE || pts + duration > target) { found = ToCpu(frame_); av_frame_unref(frame_); break; }
                KeepFallback(frame_);
                av_frame_unref(frame_);
            }
        }
        return found ? found : Finish();
    }

private:
    static AVPixelFormat PickHardware(AVCodecContext*, const AVPixelFormat* formats) {
        for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
            if (*f == AV_PIX_FMT_D3D11) return *f;
        return formats[0];
    }

    bool OpenDecoder(bool hardware) {
        avcodec_free_context(&codec_);
        av_buffer_unref(&hw_);
        const AVCodec* decoder = avcodec_find_decoder(stream_->codecpar->codec_id);
        if (!decoder) return false;
        codec_ = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(codec_, stream_->codecpar);
        codec_->pkt_timebase = stream_->time_base;
        if (hardware) {
            if (av_hwdevice_ctx_create(&hw_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0) return false;
            codec_->hw_device_ctx = av_buffer_ref(hw_);
            codec_->get_format = &ExactSource::PickHardware;
            codec_->extra_hw_frames = 8;
        } else {
            codec_->thread_count = 0;
        }
        return avcodec_open2(codec_, decoder, nullptr) >= 0;
    }

    bool Allocate() {
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        sw_ = av_frame_alloc();
        fallback_ = av_frame_alloc();
        return packet_ && frame_ && sw_ && fallback_;
    }

    const AVFrame* ToCpu(AVFrame* frame) {
        av_frame_unref(sw_);
        if (frame->hw_frames_ctx) {
            if (av_hwframe_transfer_data(sw_, frame, 0) < 0) return nullptr;
            av_frame_copy_props(sw_, frame);
        } else if (av_frame_ref(sw_, frame) < 0) {
            return nullptr;
        }
        return sw_;
    }

    void KeepFallback(AVFrame* frame) {
        av_frame_unref(fallback_);
        av_frame_ref(fallback_, frame);
    }

    const AVFrame* Finish() {
        if (!fallback_ || !fallback_->buf[0]) return nullptr;
        const AVFrame* frame = ToCpu(fallback_);
        av_frame_unref(fallback_);
        return frame;
    }

    AVFormatContext* format_{};
    AVStream* stream_{};
    int stream_index_{-1};
    AVCodecContext* codec_{};
    AVBufferRef* hw_{};
    AVPacket* packet_{};
    AVFrame* frame_{};
    AVFrame* sw_{};
    AVFrame* fallback_{};
    std::int64_t start_ms_{};
    std::int64_t duration_ms_{};
    std::int64_t frame_ts_{1};
    std::int64_t last_pts_{};
    bool have_last_{};
    bool eof_{};
};
} // namespace

int GenerateThumbnailsAt(const std::string& media_utf8, const std::string& out_dir_utf8, std::vector<std::int64_t> times_ms,
                         int width, bool allow_hardware, const ThumbnailCallback& callback) {
    const fs::path out_dir = Utf8Path(out_dir_utf8);
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    std::sort(times_ms.begin(), times_ms.end());
    times_ms.erase(std::unique(times_ms.begin(), times_ms.end()), times_ms.end());
    // Frames made earlier go out first, before the (slow) open and probe of the video, so
    // cached chapter cards fill in at once instead of waiting behind it.
    std::vector<std::int64_t> missing;
    for (const std::int64_t ms : times_ms) {
        if (ms < 0) continue;
        const std::string stem = "x" + std::to_string(ms); // exact frames; "<seconds>" files are keyframe approximations
        fs::path existing;
        for (const char* ext : {".jpg", ".bmp"})
            if (fs::exists(out_dir / (stem + ext), ec)) existing = out_dir / (stem + ext);
        if (existing.empty()) missing.push_back(ms);
        else if (callback(ThumbnailEvent::Image, ms, PathUtf8(existing)) != 0) return 1;
    }
    if (missing.empty() && !times_ms.empty()) return 0;
    ExactSource source;
    std::string error;
    if (!source.Open(media_utf8, allow_hardware, &error)) { callback(ThumbnailEvent::Error, 0, error); return -1; }
    if (callback(ThumbnailEvent::Info, source.duration_ms(), {}) != 0) return 1;
    Encoder encoder;
    std::vector<std::uint8_t> bytes;
    for (const std::int64_t ms : missing) {
        if (source.duration_ms() > 0 && ms >= source.duration_ms()) continue;
        const std::string stem = "x" + std::to_string(ms);
        const AVFrame* frame = source.Capture(ms);
        const char* extension = ".jpg";
        if (frame && encoder.Encode(frame, width, &bytes, &extension)) {
            const fs::path file = out_dir / (stem + extension);
            if (WriteAtomically(file, bytes.data(), bytes.size()) && callback(ThumbnailEvent::Image, ms, PathUtf8(file)) != 0) return 1;
        } else if (callback(ThumbnailEvent::Error, ms, "PC_WARN_THUMBNAIL_DECODE") != 0) {
            return 1;
        }
    }
    return 0;
}

int ExtractArtwork(const std::string& media_utf8, const std::string& out_dir_utf8, const ThumbnailCallback& callback) {
    const fs::path out_dir = Utf8Path(out_dir_utf8);
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, media_utf8.c_str(), nullptr, nullptr);
    if (result < 0) { callback(ThumbnailEvent::Error, 0, FfError("PC_ERR_OPEN_INPUT", result)); return -1; }
    // Header only: attachments (Matroska) and attached pictures (MP4/M4V cover art) are in it.
    int status = 0;
    int index = 0;
    for (unsigned i = 0; i < format->nb_streams && status == 0; ++i) {
        const AVStream* stream = format->streams[i];
        const std::uint8_t* data = nullptr;
        int size = 0;
        std::string name;
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) && stream->attached_pic.data && stream->attached_pic.size > 0) {
            data = stream->attached_pic.data;
            size = stream->attached_pic.size;
            const bool png = stream->codecpar->codec_id == AV_CODEC_ID_PNG;
            name = "attached-" + std::to_string(index) + (png ? ".png" : ".jpg");
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT && stream->codecpar->extradata_size > 0) {
            const AVDictionaryEntry* mime = av_dict_get(stream->metadata, "mimetype", nullptr, 0);
            const AVDictionaryEntry* file = av_dict_get(stream->metadata, "filename", nullptr, 0);
            std::string lower = file ? file->value : "";
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool image = (mime && std::string(mime->value).rfind("image/", 0) == 0) || lower.ends_with(".jpg") || lower.ends_with(".jpeg") ||
                               lower.ends_with(".png") || lower.ends_with(".webp");
            if (!image || lower.empty() || lower.find_first_of("/\\:") != std::string::npos) continue;
            data = stream->codecpar->extradata;
            size = stream->codecpar->extradata_size;
            // Matroska cover art: cover.jpg, small_cover.jpg, cover_land.jpg ... Kept ASCII (the name
            // becomes a path and a URL) and always with an image extension the host serves.
            for (auto& c : lower)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
            if (!(lower.ends_with(".jpg") || lower.ends_with(".jpeg") || lower.ends_with(".png") || lower.ends_with(".webp")))
                lower += (mime && std::string(mime->value) == "image/png") ? ".png" : (mime && std::string(mime->value) == "image/webp") ? ".webp" : ".jpg";
            name = "att-" + lower;
        } else {
            continue;
        }
        ++index;
        const fs::path file = out_dir / name;
        if (!fs::exists(file, ec) && !WriteAtomically(file, data, static_cast<std::size_t>(size))) continue;
        if (callback(ThumbnailEvent::Image, index, PathUtf8(file)) != 0) status = 1;
    }
    avformat_close_input(&format);
    if (status == 0) {
        const std::uint8_t done = '1';
        WriteAtomically(out_dir / "artwork.done", &done, 1); // marker: this file was examined
    }
    return status;
}

} // namespace vp::media_tools
