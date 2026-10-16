// Browser streams for LAN clients: see remux_stream.h.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <libavutil/hwcontext_d3d11va.h>

#include "remux_stream.h"
#include "lan_server.h" // PathToUtf8 / PathFromUtf8
#include "json.h"
#if !defined(VP_ENGINE_MPV)
#include "playbackcore_api.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <wrl/client.h>

namespace fs = std::filesystem;

namespace vp::remux {
namespace {

std::timed_mutex g_processed_stream_mutex;

template <typename F>
struct ScopeExit {
    F callback;
    ~ScopeExit() noexcept { callback(); }
};

template <typename F>
ScopeExit(F) -> ScopeExit<F>;

std::string FfError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

struct InputFile {
    AVFormatContext* format = nullptr;
    ~InputFile() { avformat_close_input(&format); }
    bool Open(const fs::path& file, std::string* error) {
        const std::string utf8 = vp::lan::PathToUtf8(file);
        int result = avformat_open_input(&format, utf8.c_str(), nullptr, nullptr);
        if (result >= 0) result = avformat_find_stream_info(format, nullptr);
        if (result < 0) { if (error) *error = "open: " + FfError(result); return false; }
        return true;
    }
    std::int64_t StartUs() const { return format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time; }
};

std::string Tag(AVStream* stream, const char* key) {
    const AVDictionaryEntry* entry = av_dict_get(stream->metadata, key, nullptr, 0);
    return entry && entry->value ? entry->value : "";
}

bool BrowserVideo(AVCodecID id) { return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC || id == AV_CODEC_ID_AV1 || id == AV_CODEC_ID_VP9; }
bool BrowserAudioCopy(AVCodecID id) { return id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_MP3; }
bool TextSubtitle(AVCodecID id) {
    return id == AV_CODEC_ID_ASS || id == AV_CODEC_ID_SSA || id == AV_CODEC_ID_SUBRIP || id == AV_CODEC_ID_TEXT || id == AV_CODEC_ID_MOV_TEXT ||
           id == AV_CODEC_ID_WEBVTT;
}

std::string FileKey(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    const auto written = fs::last_write_time(file, ec).time_since_epoch().count();
    const std::uint64_t key = std::hash<std::string>{}(vp::lan::PathToUtf8(file) + "|" + std::to_string(size) + "|" + std::to_string(written));
    char text[32];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(key));
    return text;
}

int DefaultAudio(AVFormatContext* format) { return av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0); }

// Seeks to the key frame at/before ms (relative to the file start) and returns that key frame's
// decode time in microseconds (absolute), leaving the demuxer positioned at it. INT64_MIN on failure.
std::int64_t SeekToKeyFrame(InputFile& in, int video, std::int64_t ms, AVPacket* first_video_packet, std::vector<AVPacket*>* early = nullptr) {
    AVStream* st = in.format->streams[video];
    const std::int64_t target_us = in.StartUs() + std::max<std::int64_t>(0, ms) * 1000;
    AVPacket* packet = av_packet_alloc();
    std::int64_t found = INT64_MIN;
    // Byte-interpolated seeking (MPEG-TS, some AVI) can land after the target: back off until the
    // first key frame read is at or before it.
    static const std::int64_t kBackoffMs[] = {0, 1000, 3000, 8000, 20000, 60000};
    for (const std::int64_t back : kBackoffMs) {
        found = INT64_MIN;
        const std::int64_t seek_us = std::max(in.StartUs(), target_us - back * 1000);
        if (ms > 0 || back > 0) {
            const std::int64_t ts = av_rescale_q(seek_us, AV_TIME_BASE_Q, st->time_base);
            if (av_seek_frame(in.format, video, ts, AVSEEK_FLAG_BACKWARD) < 0)
                avformat_seek_file(in.format, -1, INT64_MIN, seek_us, seek_us, 0);
        }
        if (early) { for (AVPacket* p : *early) av_packet_free(&p); early->clear(); }
        for (int guard = 0; guard < 20000 && av_read_frame(in.format, packet) >= 0; ++guard) {
            // Encoders with look-ahead put up to a second of audio before the first video packet:
            // keep it (the caller drops what lies before the start).
            if (early && packet->stream_index != video && early->size() < 4000) { early->push_back(av_packet_clone(packet)); av_packet_unref(packet); continue; }
            if (packet->stream_index == video && (packet->flags & AV_PKT_FLAG_KEY)) {
                // Matroska key frames carry no dts. With B-frames the decode time is the display
                // time minus the reorder delay; using the pts here would start video that much
                // later than audio, so estimate the dts and store it on the packet.
                if (packet->dts == AV_NOPTS_VALUE && packet->pts != AV_NOPTS_VALUE) {
                    std::int64_t delay = 0;
                    if (st->codecpar->video_delay > 0 && st->avg_frame_rate.num > 0)
                        delay = av_rescale_q(st->codecpar->video_delay, av_inv_q(st->avg_frame_rate), st->time_base);
                    packet->dts = packet->pts - delay;
                }
                const std::int64_t t = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                if (t != AV_NOPTS_VALUE) {
                    found = av_rescale_q(t, st->time_base, AV_TIME_BASE_Q);
                    break;
                }
            }
            av_packet_unref(packet);
        }
        if (found != INT64_MIN && (found <= target_us + 1000 || seek_us <= in.StartUs())) break;
        av_packet_unref(packet);
        if (ms <= 0) break;
    }
    if (found != INT64_MIN && first_video_packet) av_packet_move_ref(first_video_packet, packet);
    av_packet_free(&packet);
    return found;
}

// ---- AVIO output to the HTTP connection
struct Sink {
    const WriteFn* write;
    bool failed = false;
    std::int64_t bytes = 0;
    std::int64_t max_write_ns = 0;
    std::uint64_t writes_over_50ms = 0;
};
#if defined(FF_API_AVIO_WRITE_NONCONST) && FF_API_AVIO_WRITE_NONCONST
int WritePacket(void* opaque, std::uint8_t* data, int size) {
#else
int WritePacket(void* opaque, const std::uint8_t* data, int size) {
#endif
    auto* sink = static_cast<Sink*>(opaque);
    if (sink->failed) return AVERROR(EPIPE);
    const auto write_start = std::chrono::steady_clock::now();
    const bool written = sink->write && *sink->write && (*sink->write)(data, static_cast<std::size_t>(size));
    const auto write_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - write_start).count();
    sink->max_write_ns = std::max(sink->max_write_ns, write_ns);
    if (write_ns > 50'000'000LL) ++sink->writes_over_50ms;
    if (!written) { sink->failed = true; return AVERROR(EPIPE); }
    sink->bytes += size;
    return size;
}

struct AudioEncoder {
    AVCodecContext* decoder = nullptr;
    AVCodecContext* encoder = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    std::int64_t next_pts = AV_NOPTS_VALUE; // in 1/sample_rate
    std::int64_t trim = 0;                  // output samples still to drop (before the start)
    ~AudioEncoder() {
        avcodec_free_context(&decoder);
        avcodec_free_context(&encoder);
        swr_free(&swr);
        if (fifo) av_audio_fifo_free(fifo);
        av_frame_free(&frame);
        av_packet_free(&packet);
    }
};

#if !defined(VP_ENGINE_MPV)
// Captures the processed GPU frame at PlaybackCore's output tap, converts the
// current SDR RGB surface to NV12, and emits a browser-compatible H.264 fMP4
// stream. This first implementation is intentionally single-session and video-only.
class ProcessedVideoMux {
public:
    ProcessedVideoMux(const fs::path& file, std::int64_t start_ms, int audio_stream,
                      const std::function<bool(const std::string&)>& begin, const WriteFn& write,
                      bool require_upscale, bool frame_interpolation)
        : file_(file), start_ms_(start_ms), audio_stream_(audio_stream), begin_(begin), write_(write), sink_{&write_}, require_upscale_(require_upscale),
          frame_interpolation_(frame_interpolation) {}
    ProcessedVideoMux(const ProcessedVideoMux&) = delete;
    ProcessedVideoMux& operator=(const ProcessedVideoMux&) = delete;
    ~ProcessedVideoMux() {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        av_packet_free(&audio_input_packet_);
        av_frame_free(&audio_decode_frame_);
        avcodec_free_context(&encoder_);
        av_buffer_unref(&frames_ref_);
        av_buffer_unref(&device_ref_);
        if (output_) {
            if (output_->pb) {
                av_freep(&output_->pb->buffer);
                avio_context_free(&output_->pb);
            }
            avformat_free_context(output_);
        }
    }

    static int32_t PC_CALL OnCoreFrame(void* user, const pc_output_frame* frame) {
        return user && frame ? static_cast<ProcessedVideoMux*>(user)->OnFrame(*frame) : 0;
    }

    int32_t OnFrame(const pc_output_frame& input) noexcept {
        const auto callback_start = std::chrono::steady_clock::now();
        RecordCallbackArrival(input, callback_start);
        [[maybe_unused]] ScopeExit callback_timer{[this, &input, callback_start] {
            RecordCallbackWork(input, std::chrono::steady_clock::now() - callback_start);
        }};
        try {
            if (failed_) return 0;
            if (require_upscale_ && !input.upscaled)
                return Fail("PlaybackCore did not produce an upscaled frame (unsupported source or upscaler bypass)");
            if (!input.texture || input.width < 2 || input.height < 2) return Fail("invalid processed video frame");
            RecordInputTiming(input);
            const auto setup_start = std::chrono::steady_clock::now();
            const bool encoder_ready = EnsureEncoder(input);
            max_encoder_setup_ns_ = std::max(max_encoder_setup_ns_, std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - setup_start).count());
            if (!encoder_ready) return 0;
            if (!ConvertAndEncode(input)) return 0;
            return sink_.failed ? 0 : 1;
        } catch (const std::exception& e) {
            try { return Fail(std::string("super-resolution stream exception: ") + e.what()); }
            catch (...) { failed_ = true; return 0; }
        } catch (...) {
            try { return Fail("unknown super-resolution stream exception"); }
            catch (...) { failed_ = true; return 0; }
        }
    }

    bool Finish() {
        if (!started_ || failed_) return false;
        if (!PumpAudio(INT64_MAX)) return false;
        if (!DrainEncoder(true)) return false;
        const int trailer = av_write_trailer(output_);
        avio_flush(output_->pb);
        if (trailer < 0 || sink_.failed) {
            if (error_.empty()) error_ = trailer < 0 ? "MP4 trailer: " + FfError(trailer) : "client disconnected";
            failed_ = true;
            return false;
        }
        return true;
    }

    bool started() const { return started_; }
    bool failed() const { return failed_ || sink_.failed; }
    const std::string& error() const { return error_; }
    std::string TimingSummary() const {
        std::ostringstream summary;
        summary << "output_size=" << (encoder_ ? encoder_->width : 0) << "x" << (encoder_ ? encoder_->height : 0)
                << " frames_in=" << input_frames_
                << " frames_submitted=" << submitted_frames_
                << " rate_limited=" << rate_limited_frames_
                << " pts_backwards=" << backwards_input_pts_
                << " input_gaps_over_2frames=" << large_input_gaps_
                << " max_input_gap_ms=" << max_input_gap_ns_ / 1'000'000
                << " max_submitted_gap_ms=" << max_submitted_gap_ns_ / 1'000'000
                << " callback_gaps_over_2frames=" << large_callback_gaps_
                << " max_callback_gap_ms=" << max_callback_gap_ns_ / 1'000'000
                << " callback_work_over_frame_budget=" << callback_work_over_budget_
                << " max_callback_work_ms=" << max_callback_work_ns_ / 1'000'000
                << " max_encoder_setup_ms=" << max_encoder_setup_ns_ / 1'000'000
                << " max_gpu_surface_acquire_ms=" << max_gpu_surface_acquire_ns_ / 1'000'000
                << " max_gpu_blt_submit_ms=" << max_gpu_blt_ns_ / 1'000'000
                << " max_audio_pump_ms=" << max_audio_pump_ns_ / 1'000'000
                << " max_encoder_submit_ms=" << max_encoder_submit_ns_ / 1'000'000
                << " max_encoder_drain_ms=" << max_encoder_drain_ns_ / 1'000'000
                << " max_http_write_ms=" << sink_.max_write_ns / 1'000'000
                << " http_writes_over_50ms=" << sink_.writes_over_50ms;
        return summary.str();
    }

private:
    int32_t Fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        failed_ = true;
        return 0;
    }

    void RecordInputTiming(const pc_output_frame& input) noexcept {
        ++input_frames_;
        if (input.pts_ns == AV_NOPTS_VALUE) return;
        if (last_input_pts_ns_ != AV_NOPTS_VALUE) {
            const std::int64_t gap = input.pts_ns - last_input_pts_ns_;
            if (gap <= 0) {
                ++backwards_input_pts_;
            } else {
                max_input_gap_ns_ = std::max(max_input_gap_ns_, gap);
                const std::int64_t two_frames_ns = input.duration_ns > 0
                    ? std::max<std::int64_t>(50'000'000LL, input.duration_ns * 2)
                    : 50'000'000LL;
                if (gap > two_frames_ns) ++large_input_gaps_;
            }
        }
        last_input_pts_ns_ = input.pts_ns;
    }

    void RecordCallbackArrival(const pc_output_frame& input, std::chrono::steady_clock::time_point now) noexcept {
        if (last_callback_wall_.time_since_epoch().count() != 0) {
            const auto gap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_callback_wall_).count();
            if (gap > 0) {
                max_callback_gap_ns_ = std::max(max_callback_gap_ns_, gap);
                const std::int64_t two_frames_ns = input.duration_ns > 0
                    ? std::max<std::int64_t>(50'000'000LL, input.duration_ns * 2)
                    : 50'000'000LL;
                if (gap > two_frames_ns) ++large_callback_gaps_;
            }
        }
        last_callback_wall_ = now;
    }

    void RecordCallbackWork(const pc_output_frame& input, std::chrono::steady_clock::duration elapsed) noexcept {
        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        max_callback_work_ns_ = std::max(max_callback_work_ns_, elapsed_ns);
        const std::int64_t frame_budget_ns = input.duration_ns > 0 ? input.duration_ns : 41'666'667LL;
        if (elapsed_ns > frame_budget_ns) ++callback_work_over_budget_;
    }

    void RecordSubmittedTiming(std::int64_t pts_ns) noexcept {
        if (last_submitted_input_pts_ns_ != AV_NOPTS_VALUE && pts_ns != AV_NOPTS_VALUE) {
            const std::int64_t gap = pts_ns - last_submitted_input_pts_ns_;
            if (gap > 0) max_submitted_gap_ns_ = std::max(max_submitted_gap_ns_, gap);
        }
        last_submitted_input_pts_ns_ = pts_ns;
        ++submitted_frames_;
    }

    bool SetupAudio() {
        if (!audio_input_.Open(file_, &error_)) {
            Fail("cannot open source for stream audio: " + error_);
            return false;
        }
        audio_index_ = audio_stream_;
        if (audio_index_ < 0 || audio_index_ >= static_cast<int>(audio_input_.format->nb_streams) ||
            audio_input_.format->streams[audio_index_]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            audio_index_ = DefaultAudio(audio_input_.format);
        if (audio_index_ < 0) return true;
        audio_in_ = audio_input_.format->streams[audio_index_];
        audio_out_ = avformat_new_stream(output_, nullptr);
        if (!audio_out_) { Fail("cannot create MP4 audio track"); return false; }
        audio_copy_ = BrowserAudioCopy(audio_in_->codecpar->codec_id);
        if (audio_copy_) {
            if (avcodec_parameters_copy(audio_out_->codecpar, audio_in_->codecpar) < 0) {
                Fail("cannot copy source audio track metadata");
                return false;
            }
            audio_out_->codecpar->codec_tag = 0;
            audio_out_->time_base = audio_in_->time_base;
        } else {
            const AVCodec* decoder = avcodec_find_decoder(audio_in_->codecpar->codec_id);
            const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
            aenc_.decoder = decoder ? avcodec_alloc_context3(decoder) : nullptr;
            aenc_.encoder = encoder ? avcodec_alloc_context3(encoder) : nullptr;
            if (!aenc_.decoder || !aenc_.encoder || avcodec_parameters_to_context(aenc_.decoder, audio_in_->codecpar) < 0 ||
                avcodec_open2(aenc_.decoder, decoder, nullptr) < 0) {
                Fail(std::string("cannot decode stream audio ") + avcodec_get_name(audio_in_->codecpar->codec_id));
                return false;
            }
            aenc_.decoder->pkt_timebase = audio_in_->time_base;
            aenc_.encoder->sample_rate = 48000;
            aenc_.encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&aenc_.encoder->ch_layout, 2);
            aenc_.encoder->bit_rate = 192000;
            aenc_.encoder->time_base = AVRational{1, 48000};
            if (output_->oformat->flags & AVFMT_GLOBALHEADER) aenc_.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(aenc_.encoder, encoder, nullptr) < 0) { Fail("AAC audio encoder is unavailable"); return false; }
            if (avcodec_parameters_from_context(audio_out_->codecpar, aenc_.encoder) < 0) {
                Fail("cannot configure AAC output track");
                return false;
            }
            audio_out_->time_base = aenc_.encoder->time_base;
            aenc_.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 4096);
            aenc_.frame = av_frame_alloc();
            aenc_.packet = av_packet_alloc();
            if (!aenc_.fifo || !aenc_.frame || !aenc_.packet) { Fail("cannot allocate AAC audio buffers"); return false; }
            aenc_.next_pts = 0;
        }
        const std::string language = Tag(audio_in_, "language");
        if (!language.empty()) av_dict_set(&audio_out_->metadata, "language", language.c_str(), 0);
        audio_offset_ts_ = av_rescale_q(audio_input_.StartUs() + std::max<std::int64_t>(0, start_ms_) * 1000,
                                        AV_TIME_BASE_Q, audio_in_->time_base);
        if (av_seek_frame(audio_input_.format, audio_index_, audio_offset_ts_, AVSEEK_FLAG_BACKWARD) < 0) {
            Fail("cannot seek source audio to the stream start position");
            return false;
        }
        audio_input_packet_ = av_packet_alloc();
        if (!audio_input_packet_) { Fail("cannot allocate audio input packet"); return false; }
        if (!audio_copy_) {
            audio_decode_frame_ = av_frame_alloc();
            if (!audio_decode_frame_) { Fail("cannot allocate decoded audio frame"); return false; }
        }
        return true;
    }

    bool WriteAudioPackets(bool flush) {
        if (!aenc_.encoder || !aenc_.packet || !audio_out_) return true;
        if (flush) {
            const int sent = avcodec_send_frame(aenc_.encoder, nullptr);
            if (sent < 0 && sent != AVERROR_EOF && sent != AVERROR(EAGAIN)) {
                Fail("AAC encoder flush: " + FfError(sent));
                return false;
            }
        }
        for (;;) {
            const int result = avcodec_receive_packet(aenc_.encoder, aenc_.packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) { Fail("AAC encoder output: " + FfError(result)); return false; }
            aenc_.packet->stream_index = audio_out_->index;
            if (aenc_.packet->pts != AV_NOPTS_VALUE) aenc_.packet->pts += aenc_.encoder->initial_padding;
            if (aenc_.packet->dts != AV_NOPTS_VALUE) aenc_.packet->dts += aenc_.encoder->initial_padding;
            av_packet_rescale_ts(aenc_.packet, aenc_.encoder->time_base, audio_out_->time_base);
            aenc_.packet->pos = -1;
            const int written = av_interleaved_write_frame(output_, aenc_.packet);
            av_packet_unref(aenc_.packet);
            if (written < 0 || sink_.failed) {
                Fail(written < 0 ? "AAC MP4 packet: " + FfError(written) : "client disconnected");
                return false;
            }
        }
        return true;
    }

    bool WriteAudioFrames(bool flush) {
        if (!aenc_.encoder || !aenc_.fifo || !audio_out_) return true;
        const int frame_size = aenc_.encoder->frame_size > 0 ? aenc_.encoder->frame_size : 1024;
        while (av_audio_fifo_size(aenc_.fifo) >= frame_size || (flush && av_audio_fifo_size(aenc_.fifo) > 0)) {
            const int samples = std::min(frame_size, av_audio_fifo_size(aenc_.fifo));
            av_frame_unref(aenc_.frame);
            aenc_.frame->nb_samples = samples;
            aenc_.frame->format = AV_SAMPLE_FMT_FLTP;
            aenc_.frame->sample_rate = 48000;
            av_channel_layout_copy(&aenc_.frame->ch_layout, &aenc_.encoder->ch_layout);
            if (av_frame_get_buffer(aenc_.frame, 0) < 0 ||
                av_audio_fifo_read(aenc_.fifo, reinterpret_cast<void**>(aenc_.frame->data), samples) != samples) {
                Fail("cannot prepare AAC input frame");
                return false;
            }
            aenc_.frame->pts = aenc_.next_pts;
            aenc_.next_pts += samples;
            int sent = avcodec_send_frame(aenc_.encoder, aenc_.frame);
            if (sent == AVERROR(EAGAIN)) {
                if (!WriteAudioPackets(false)) return false;
                sent = avcodec_send_frame(aenc_.encoder, aenc_.frame);
            }
            if (sent < 0) { Fail("AAC encoder rejected an audio frame: " + FfError(sent)); return false; }
            if (!WriteAudioPackets(false)) return false;
        }
        return !flush || WriteAudioPackets(true);
    }

    bool DecodeAudioPacket(const AVPacket* packet) {
        if (!aenc_.decoder || !aenc_.fifo) return true;
        int sent = avcodec_send_packet(aenc_.decoder, packet);
        if (sent < 0 && packet) return true; // Ignore a damaged packet and continue.
        for (;;) {
            const int received = avcodec_receive_frame(aenc_.decoder, audio_decode_frame_);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
            if (received < 0) { Fail("audio decode failed: " + FfError(received)); return false; }
            AVFrame* decoded = audio_decode_frame_;
            if (!aenc_.swr) {
                AVChannelLayout input_layout;
                if (decoded->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !decoded->ch_layout.nb_channels)
                    av_channel_layout_default(&input_layout, decoded->ch_layout.nb_channels ? decoded->ch_layout.nb_channels : 2);
                else
                    av_channel_layout_copy(&input_layout, &decoded->ch_layout);
                const int configured = swr_alloc_set_opts2(&aenc_.swr, &aenc_.encoder->ch_layout, AV_SAMPLE_FMT_FLTP, 48000,
                                                           &input_layout, static_cast<AVSampleFormat>(decoded->format),
                                                           decoded->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&input_layout);
                if (configured < 0 || swr_init(aenc_.swr) < 0) {
                    av_frame_unref(audio_decode_frame_);
                    Fail("cannot initialize audio resampling");
                    return false;
                }
                const std::int64_t pts = decoded->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE) {
                    const std::int64_t relative = av_rescale_q(pts - audio_offset_ts_, audio_in_->time_base, AVRational{1, 48000});
                    if (relative < 0) aenc_.trim = -relative;
                    else if (relative > 0 && relative < 240000) {
                        std::uint8_t** silence = nullptr;
                        if (av_samples_alloc_array_and_samples(&silence, nullptr, 2, static_cast<int>(relative), AV_SAMPLE_FMT_FLTP, 0) < 0) {
                            av_frame_unref(audio_decode_frame_);
                            Fail("cannot allocate initial audio silence");
                            return false;
                        }
                        av_samples_set_silence(silence, 0, static_cast<int>(relative), 2, AV_SAMPLE_FMT_FLTP);
                        const int written = av_audio_fifo_write(aenc_.fifo, reinterpret_cast<void**>(silence), static_cast<int>(relative));
                        av_freep(&silence[0]);
                        av_freep(&silence);
                        if (written != relative) {
                            av_frame_unref(audio_decode_frame_);
                            Fail("cannot align AAC audio to the processed video");
                            return false;
                        }
                    }
                }
            }
            const int capacity = std::max(32, swr_get_out_samples(aenc_.swr, decoded->nb_samples) + 32);
            std::uint8_t** converted = nullptr;
            if (av_samples_alloc_array_and_samples(&converted, nullptr, 2, capacity, AV_SAMPLE_FMT_FLTP, 0) < 0) {
                av_frame_unref(audio_decode_frame_);
                Fail("cannot allocate resampled audio");
                return false;
            }
            const int samples = swr_convert(aenc_.swr, converted, capacity,
                                            const_cast<const std::uint8_t**>(decoded->extended_data), decoded->nb_samples);
            av_frame_unref(audio_decode_frame_);
            if (samples < 0) {
                av_freep(&converted[0]);
                av_freep(&converted);
                Fail("audio resampling failed: " + FfError(samples));
                return false;
            }
            int drop = static_cast<int>(std::min<std::int64_t>(aenc_.trim, samples));
            aenc_.trim -= drop;
            const int kept = samples - drop;
            if (kept > 0) {
                const int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLTP);
                std::uint8_t* planes[2] = {converted[0] + drop * bytes_per_sample, converted[1] + drop * bytes_per_sample};
                if (av_audio_fifo_write(aenc_.fifo, reinterpret_cast<void**>(planes), kept) != kept) {
                    av_freep(&converted[0]);
                    av_freep(&converted);
                    Fail("cannot queue resampled audio");
                    return false;
                }
            }
            av_freep(&converted[0]);
            av_freep(&converted);
            if (!WriteAudioFrames(false)) return false;
        }
        return true;
    }

    bool PumpAudio(std::int64_t relative_ns) {
        if (!audio_in_ || !audio_input_packet_ || audio_finalized_) return true;
        const bool drain_all = relative_ns == INT64_MAX;
        const std::int64_t target = drain_all ? INT64_MAX : audio_offset_ts_ +
            av_rescale_q(std::max<std::int64_t>(0, relative_ns), AVRational{1, 1'000'000'000}, audio_in_->time_base);
        while (!audio_eof_) {
            if (!audio_pending_) {
                int result = 0;
                do {
                    result = av_read_frame(audio_input_.format, audio_input_packet_);
                    if (result < 0) { audio_eof_ = true; break; }
                    if (audio_input_packet_->stream_index != audio_index_) av_packet_unref(audio_input_packet_);
                } while (audio_input_packet_->stream_index != audio_index_ && result >= 0);
                if (audio_eof_) break;
                audio_pending_ = true;
            }
            const std::int64_t packet_time = audio_input_packet_->pts != AV_NOPTS_VALUE
                ? audio_input_packet_->pts : audio_input_packet_->dts;
            if (!drain_all && packet_time != AV_NOPTS_VALUE && packet_time > target) break;
            bool ok = true;
            if (audio_copy_) {
                const std::int64_t presentation = audio_input_packet_->pts != AV_NOPTS_VALUE
                    ? audio_input_packet_->pts : audio_input_packet_->dts;
                if (presentation == AV_NOPTS_VALUE || presentation >= audio_offset_ts_) {
                    if (audio_input_packet_->pts != AV_NOPTS_VALUE) audio_input_packet_->pts -= audio_offset_ts_;
                    if (audio_input_packet_->dts != AV_NOPTS_VALUE) audio_input_packet_->dts -= audio_offset_ts_;
                    audio_input_packet_->stream_index = audio_out_->index;
                    av_packet_rescale_ts(audio_input_packet_, audio_in_->time_base, audio_out_->time_base);
                    audio_input_packet_->pos = -1;
                    const int written = av_interleaved_write_frame(output_, audio_input_packet_);
                    ok = written >= 0 && !sink_.failed;
                    if (!ok) Fail(written < 0 ? "audio MP4 packet: " + FfError(written) : "client disconnected");
                }
            } else {
                ok = DecodeAudioPacket(audio_input_packet_);
            }
            av_packet_unref(audio_input_packet_);
            audio_pending_ = false;
            if (!ok) return false;
        }
        if (drain_all && audio_eof_ && !audio_finalized_) {
            if (!audio_copy_) {
                if (!DecodeAudioPacket(nullptr)) return false;
                if (aenc_.swr) {
                    const int capacity = std::max(32, swr_get_out_samples(aenc_.swr, 0) + 32);
                    std::uint8_t** converted = nullptr;
                    if (av_samples_alloc_array_and_samples(&converted, nullptr, 2, capacity, AV_SAMPLE_FMT_FLTP, 0) < 0) {
                        Fail("cannot flush audio resampler");
                        return false;
                    }
                    const int samples = swr_convert(aenc_.swr, converted, capacity, nullptr, 0);
                    if (samples > 0 && av_audio_fifo_write(aenc_.fifo, reinterpret_cast<void**>(converted), samples) != samples) {
                        av_freep(&converted[0]);
                        av_freep(&converted);
                        Fail("cannot queue final audio samples");
                        return false;
                    }
                    av_freep(&converted[0]);
                    av_freep(&converted);
                }
                if (!WriteAudioFrames(true)) return false;
            }
            audio_finalized_ = true;
        }
        return true;
    }

    bool EnsureEncoder(const pc_output_frame& input) {
        if (encoder_) {
            if (input.width == processed_input_width_ && input.height == processed_input_height_)
                return true;
            Fail("processed input dimensions changed during stream: expected=" +
                 std::to_string(processed_input_width_) + "x" + std::to_string(processed_input_height_) +
                 " frame=" + std::to_string(input.width) + "x" + std::to_string(input.height));
            return false;
        }
        auto* texture = static_cast<ID3D11Texture2D*>(input.texture);
        D3D11_TEXTURE2D_DESC source_desc{};
        texture->GetDesc(&source_desc);
        const DXGI_FORMAT source_format = static_cast<DXGI_FORMAT>(input.dxgi_format);
        if (source_desc.Width < input.width || source_desc.Height < input.height || source_desc.Format != source_format ||
            source_desc.SampleDesc.Count != 1 || input.array_slice >= source_desc.ArraySize) {
            Fail("invalid processed GPU texture layout: frame=" + std::to_string(input.width) + "x" + std::to_string(input.height) +
                 " format=" + std::to_string(input.dxgi_format) + " texture=" + std::to_string(source_desc.Width) + "x" +
                 std::to_string(source_desc.Height) + " format=" + std::to_string(static_cast<unsigned>(source_desc.Format)) +
                 " samples=" + std::to_string(source_desc.SampleDesc.Count) + " slice=" + std::to_string(input.array_slice) + "/" +
                 std::to_string(source_desc.ArraySize) + " sr=" + std::to_string(require_upscale_) +
                 " frc=" + std::to_string(frame_interpolation_));
            return false;
        }
        if (!device_) {
            texture->GetDevice(device_.GetAddressOf());
            if (!device_) { Fail("cannot get the processed frame's D3D11 device"); return false; }
            device_->GetImmediateContext(context_.GetAddressOf());
            if (!context_) { Fail("cannot get the processed frame's D3D11 context"); return false; }
            Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
            if (SUCCEEDED(device_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
        }
        if (FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_))) {
            Fail("D3D11 video processing is unavailable on the processed frame device");
            return false;
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_mf");
        if (!codec) { Fail("H.264 Media Foundation encoder is unavailable"); return false; }
        const int nominal_fps = frame_interpolation_ ? 60 : input.duration_ns > 0
            ? static_cast<int>(std::clamp<std::int64_t>((1'000'000'000LL + input.duration_ns / 2) / input.duration_ns, 1, 240))
            : 30;
        // Keep super-resolution inference unchanged while capping the encoded LAN frame at 1080p.
        // Preserve orientation and aspect ratio; D3D11 VideoProcessor performs the GPU downscale.
        const bool landscape = input.width >= input.height;
        const double max_output_width = landscape ? 1920.0 : 1080.0;
        const double max_output_height = landscape ? 1080.0 : 1920.0;
        const double output_scale = std::min({
            1.0,
            max_output_width / static_cast<double>(input.width),
            max_output_height / static_cast<double>(input.height)});
        const int output_width = static_cast<int>(static_cast<double>(input.width) * output_scale) & ~1;
        const int output_height = static_cast<int>(static_cast<double>(input.height) * output_scale) & ~1;
        if (output_width < 2 || output_height < 2) { Fail("upscaled output dimensions are too small"); return false; }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC processor_desc{};
        processor_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        processor_desc.InputFrameRate = {static_cast<UINT>(nominal_fps), 1};
        processor_desc.InputWidth = input.width;
        processor_desc.InputHeight = input.height;
        processor_desc.OutputFrameRate = {static_cast<UINT>(nominal_fps), 1};
        processor_desc.OutputWidth = static_cast<UINT>(output_width);
        processor_desc.OutputHeight = static_cast<UINT>(output_height);
        processor_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&processor_desc, processor_enum_.GetAddressOf());
        if (FAILED(hr) || !processor_enum_) { Fail("cannot create D3D11 RGB-to-NV12 processor enumerator"); return false; }
        UINT input_support = 0, output_support = 0;
        if (FAILED(processor_enum_->CheckVideoProcessorFormat(source_format, &input_support)) ||
            !(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
            Fail("D3D11 video processor does not support processed texture input format=" +
                 std::to_string(static_cast<unsigned>(source_format)) + " size=" + std::to_string(input.width) + "x" +
                 std::to_string(input.height) + " sr=" + std::to_string(require_upscale_) +
                 " frc=" + std::to_string(frame_interpolation_));
            return false;
        }
        if (FAILED(processor_enum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &output_support)) ||
            !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            Fail("D3D11 video processor cannot produce NV12 on this GPU");
            return false;
        }
        hr = video_device_->CreateVideoProcessor(processor_enum_.Get(), 0, processor_.GetAddressOf());
        if (FAILED(hr) || !processor_) { Fail("cannot create D3D11 video processor"); return false; }

        // Give FFmpeg the same D3D11 device as the upscaler. The processor writes
        // NV12 directly into FFmpeg's hardware-frame pool; h264_mf consumes those
        // textures without mapping them to system memory.
        device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!device_ref_) { Fail("cannot allocate FFmpeg D3D11 device context"); return false; }
        auto* hw_device = reinterpret_cast<AVHWDeviceContext*>(device_ref_->data);
        auto* d3d_device = reinterpret_cast<AVD3D11VADeviceContext*>(hw_device->hwctx);
        d3d_device->device = device_.Get();
        d3d_device->device->AddRef();
        int hw_result = av_hwdevice_ctx_init(device_ref_);
        if (hw_result < 0) { Fail("cannot initialize FFmpeg D3D11 device: " + FfError(hw_result)); return false; }
        frames_ref_ = av_hwframe_ctx_alloc(device_ref_);
        if (!frames_ref_) { Fail("cannot allocate FFmpeg D3D11 frame pool"); return false; }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_ref_->data);
        frames->format = AV_PIX_FMT_D3D11;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = output_width;
        frames->height = output_height;
        // Let FFmpeg allocate individual textures on demand. A fixed D3D11
        // array can be rejected by some video-processor / Media Foundation
        // encoder drivers even when a single NV12 surface is supported.
        frames->initial_pool_size = 0;
        auto* d3d_frames = reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx);
        d3d_frames->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        hw_result = av_hwframe_ctx_init(frames_ref_);
        if (hw_result < 0) { Fail("cannot initialize GPU NV12 frame pool: " + FfError(hw_result)); return false; }

        encoder_ = avcodec_alloc_context3(codec);
        if (!encoder_) { Fail("cannot allocate H.264 encoder"); return false; }
        encoder_->width = output_width;
        encoder_->height = output_height;
        encoder_->pix_fmt = AV_PIX_FMT_D3D11;
        encoder_->sw_pix_fmt = AV_PIX_FMT_NV12;
        encoder_->hw_frames_ctx = av_buffer_ref(frames_ref_);
        if (!encoder_->hw_frames_ctx) { Fail("cannot attach GPU NV12 frame pool to H.264 encoder"); return false; }
        encoder_->time_base = AVRational{1, 90000};
        encoder_->framerate = AVRational{nominal_fps, 1};
        encoder_->sample_aspect_ratio = AVRational{1, 1};
        encoder_->bit_rate = std::clamp<std::int64_t>(static_cast<std::int64_t>(encoder_->width) * encoder_->height * 3 * nominal_fps / 30,
                                                       2'000'000, 30'000'000);
        encoder_->gop_size = 60;
        encoder_->max_b_frames = 0;
        encoder_->profile = AV_PROFILE_H264_MAIN;
        encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        encoder_->color_range = AVCOL_RANGE_MPEG;
        encoder_->colorspace = AVCOL_SPC_BT709;
        encoder_->color_primaries = AVCOL_PRI_BT709;
        encoder_->color_trc = AVCOL_TRC_BT709;
        AVDictionary* encoder_options = nullptr;
        av_dict_set(&encoder_options, "hw_encoding", "1", 0);
        hw_result = avcodec_open2(encoder_, codec, &encoder_options);
        const bool hardware_option_unused = av_dict_get(encoder_options, "hw_encoding", nullptr, 0) != nullptr;
        av_dict_free(&encoder_options);
        if (hw_result < 0) { Fail("cannot open GPU-input H.264 Media Foundation encoder: " + FfError(hw_result)); return false; }
        if (hardware_option_unused) { Fail("H.264 Media Foundation encoder did not accept hw_encoding=1"); return false; }

        if (avformat_alloc_output_context2(&output_, nullptr, "mp4", nullptr) < 0 || !output_) {
            Fail("MP4 muxer is unavailable");
            return false;
        }
        output_->flags |= AVFMT_FLAG_CUSTOM_IO;
        const int buffer_size = 64 * 1024;
        auto* buffer = static_cast<unsigned char*>(av_malloc(buffer_size));
        if (!buffer) { Fail("cannot allocate MP4 output buffer"); return false; }
        output_->pb = avio_alloc_context(buffer, buffer_size, 1, &sink_, nullptr, &WritePacket, nullptr);
        if (!output_->pb) { av_free(buffer); Fail("cannot create MP4 output stream"); return false; }
        AVStream* stream = avformat_new_stream(output_, nullptr);
        if (!stream || avcodec_parameters_from_context(stream->codecpar, encoder_) < 0) {
            Fail("cannot configure H.264 MP4 stream");
            return false;
        }
        stream->time_base = encoder_->time_base;
        stream->codecpar->codec_tag = MKTAG('a', 'v', 'c', '1');
        if (!SetupAudio()) return false;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+default_base_moof+negative_cts_offsets", 0);
        av_dict_set(&options, "frag_duration", "1000000", 0);
        if (!begin_("video/mp4")) {
            av_dict_free(&options);
            Fail("could not start HTTP video stream");
            return false;
        }
        started_ = true;
        const int header = avformat_write_header(output_, &options);
        av_dict_free(&options);
        if (header < 0) { Fail("MP4 header: " + FfError(header)); return false; }
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!frame_ || !packet_) { Fail("cannot allocate H.264 frame buffers"); return false; }
        frame_->format = encoder_->pix_fmt;
        frame_->width = encoder_->width;
        frame_->height = encoder_->height;
        frame_->color_range = AVCOL_RANGE_MPEG;
        frame_->colorspace = AVCOL_SPC_BT709;
        frame_->color_primaries = AVCOL_PRI_BT709;
        frame_->color_trc = AVCOL_TRC_BT709;
        frame_->sample_aspect_ratio = encoder_->sample_aspect_ratio;
        processed_input_width_ = input.width;
        processed_input_height_ = input.height;
        return true;
    }

    bool ConvertAndEncode(const pc_output_frame& input) {
        // Composition can follow a high-refresh host display. Keep the LAN output
        // within the 60 fps range supported by ordinary phones and browsers.
        constexpr std::int64_t kMinOutputIntervalNs = 1'000'000'000LL / 60;
        if (frame_interpolation_ && last_output_pts_ns_ != AV_NOPTS_VALUE &&
            input.pts_ns > last_output_pts_ns_ && input.pts_ns - last_output_pts_ns_ < kMinOutputIntervalNs) {
            ++rate_limited_frames_;
            return true;
        }
        auto* source = static_cast<ID3D11Texture2D*>(input.texture);
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_desc.Width < input.width || source_desc.Height < input.height ||
            source_desc.Format != static_cast<DXGI_FORMAT>(input.dxgi_format) ||
            source_desc.SampleDesc.Count != 1 || input.array_slice >= source_desc.ArraySize) {
            Fail("processed GPU frame layout changed during the stream");
            return false;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc{};
        input_view_desc.FourCC = 0;
        input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_view_desc.Texture2D.MipSlice = 0;
        input_view_desc.Texture2D.ArraySlice = input.array_slice;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
        HRESULT hr = video_device_->CreateVideoProcessorInputView(source, processor_enum_.Get(), &input_view_desc,
                                                                   input_view.GetAddressOf());
        if (FAILED(hr) || !input_view) { Fail("cannot create D3D11 input view for processed frame"); return false; }

        av_frame_unref(frame_);
        const auto surface_acquire_start = std::chrono::steady_clock::now();
        const int hw_result = av_hwframe_get_buffer(frames_ref_, frame_, 0);
        max_gpu_surface_acquire_ns_ = std::max(max_gpu_surface_acquire_ns_, std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - surface_acquire_start).count());
        if (hw_result < 0) { Fail("cannot allocate GPU NV12 output frame: " + FfError(hw_result)); return false; }
        auto* target = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
        const UINT target_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));
        D3D11_TEXTURE2D_DESC target_desc{};
        target->GetDesc(&target_desc);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
        if (target_desc.ArraySize > 1) {
            output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            output_view_desc.Texture2DArray.MipSlice = 0;
            output_view_desc.Texture2DArray.FirstArraySlice = target_slice;
            output_view_desc.Texture2DArray.ArraySize = 1;
        } else {
            output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            output_view_desc.Texture2D.MipSlice = 0;
        }
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
        hr = video_device_->CreateVideoProcessorOutputView(target, processor_enum_.Get(), &output_view_desc,
                                                            output_view.GetAddressOf());
        if (FAILED(hr) || !output_view) { Fail("cannot create D3D11 output view for NV12 encoder frame"); return false; }

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space{};
        const DXGI_FORMAT source_format = static_cast<DXGI_FORMAT>(input.dxgi_format);
        if (source_format == DXGI_FORMAT_P010 || source_format == DXGI_FORMAT_P016 || source_format == DXGI_FORMAT_NV12) {
            input_space.YCbCr_Matrix = 1; // Processed HD YUV surfaces use BT.709.
            input_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        } else {
            input_space.RGB_Range = 0; // RGB input uses full range.
        }
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space{};
        output_space.YCbCr_Matrix = 1; // BT.709
        output_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input_space);
        video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output_space);
        RECT source_rect{0, 0, static_cast<LONG>(input.width), static_cast<LONG>(input.height)};
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source_rect);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view.Get();
        const auto gpu_blt_start = std::chrono::steady_clock::now();
        hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(), 0, 1, &stream);
        max_gpu_blt_ns_ = std::max(max_gpu_blt_ns_, std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - gpu_blt_start).count());
        if (FAILED(hr)) { Fail("GPU RGB-to-NV12 conversion failed"); return false; }

        frame_->color_range = AVCOL_RANGE_MPEG;
        frame_->colorspace = AVCOL_SPC_BT709;
        frame_->color_primaries = AVCOL_PRI_BT709;
        frame_->color_trc = AVCOL_TRC_BT709;
        frame_->sample_aspect_ratio = encoder_->sample_aspect_ratio;
        if (base_pts_ns_ == AV_NOPTS_VALUE) base_pts_ns_ = input.pts_ns;
        const std::int64_t relative_ns = std::max<std::int64_t>(0, input.pts_ns - base_pts_ns_);
        std::int64_t pts = av_rescale_q(relative_ns, AVRational{1, 1'000'000'000}, encoder_->time_base);
        if (last_pts_ != AV_NOPTS_VALUE && pts <= last_pts_) pts = last_pts_ + 1;
        last_pts_ = pts;
        frame_->pts = pts;
        const auto audio_pump_start = std::chrono::steady_clock::now();
        if (!PumpAudio(relative_ns)) return false;
        max_audio_pump_ns_ = std::max(max_audio_pump_ns_, std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - audio_pump_start).count());
        const auto encoder_submit_start = std::chrono::steady_clock::now();
        int sent = avcodec_send_frame(encoder_, frame_);
        if (sent == AVERROR(EAGAIN)) {
            if (!DrainPackets()) return false;
            sent = avcodec_send_frame(encoder_, frame_);
        }
        max_encoder_submit_ns_ = std::max(max_encoder_submit_ns_, std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - encoder_submit_start).count());
        if (sent < 0) { Fail("H.264 encoder rejected a processed frame: " + FfError(sent)); return false; }
        RecordSubmittedTiming(input.pts_ns);
        last_output_pts_ns_ = input.pts_ns;
        const auto encoder_drain_start = std::chrono::steady_clock::now();
        const bool drained = DrainEncoder(false);
        max_encoder_drain_ns_ = std::max(max_encoder_drain_ns_, std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - encoder_drain_start).count());
        return drained;
    }

    bool DrainEncoder(bool flush) {
        if (flush) {
            for (;;) {
                const int sent = avcodec_send_frame(encoder_, nullptr);
                if (sent == AVERROR(EAGAIN)) {
                    if (!DrainPackets()) return false;
                    continue;
                }
                if (sent < 0 && sent != AVERROR_EOF) {
                    Fail("H.264 encoder flush: " + FfError(sent));
                    return false;
                }
                break;
            }
        }
        return DrainPackets();
    }

    bool DrainPackets() {
        for (;;) {
            const int result = avcodec_receive_packet(encoder_, packet_);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) { Fail("H.264 encoder output: " + FfError(result)); return false; }
            packet_->stream_index = output_->streams[0]->index;
            av_packet_rescale_ts(packet_, encoder_->time_base, output_->streams[0]->time_base);
            packet_->pos = -1;
            const int written = av_interleaved_write_frame(output_, packet_);
            av_packet_unref(packet_);
            if (written < 0 || sink_.failed) {
                Fail(written < 0 ? "MP4 packet: " + FfError(written) : "client disconnected");
                return false;
            }
        }
        return true;
    }

    fs::path file_;
    std::int64_t start_ms_{};
    int audio_stream_{-1};
    const std::function<bool(const std::string&)>& begin_;
    const WriteFn& write_;
    Sink sink_;
    InputFile audio_input_;
    AudioEncoder aenc_;
    AVFormatContext* output_{};
    AVCodecContext* encoder_{};
    std::uint32_t processed_input_width_{};
    std::uint32_t processed_input_height_{};
    AVBufferRef* device_ref_{};
    AVBufferRef* frames_ref_{};
    AVFrame* frame_{};
    AVPacket* packet_{};
    AVPacket* audio_input_packet_{};
    AVFrame* audio_decode_frame_{};
    AVStream* audio_in_{};
    AVStream* audio_out_{};
    int audio_index_{-1};
    std::int64_t audio_offset_ts_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> processor_enum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    std::int64_t base_pts_ns_{AV_NOPTS_VALUE};
    std::int64_t last_pts_{AV_NOPTS_VALUE};
    std::int64_t last_output_pts_ns_{AV_NOPTS_VALUE};
    std::int64_t last_input_pts_ns_{AV_NOPTS_VALUE};
    std::int64_t last_submitted_input_pts_ns_{AV_NOPTS_VALUE};
    std::int64_t max_input_gap_ns_{};
    std::int64_t max_submitted_gap_ns_{};
    std::chrono::steady_clock::time_point last_callback_wall_{};
    std::int64_t max_callback_gap_ns_{};
    std::int64_t max_callback_work_ns_{};
    std::int64_t max_encoder_setup_ns_{};
    std::int64_t max_gpu_surface_acquire_ns_{};
    std::int64_t max_gpu_blt_ns_{};
    std::int64_t max_audio_pump_ns_{};
    std::int64_t max_encoder_submit_ns_{};
    std::int64_t max_encoder_drain_ns_{};
    std::uint64_t input_frames_{};
    std::uint64_t submitted_frames_{};
    std::uint64_t rate_limited_frames_{};
    std::uint64_t backwards_input_pts_{};
    std::uint64_t large_input_gaps_{};
    std::uint64_t large_callback_gaps_{};
    std::uint64_t callback_work_over_budget_{};
    std::string error_;
    bool require_upscale_{};
    bool frame_interpolation_{};
    bool started_{};
    bool failed_{};
    bool audio_copy_{};
    bool audio_pending_{};
    bool audio_eof_{};
    bool audio_finalized_{};
};
#endif

} // namespace

// ---------------------------------------------------------------- probe
std::string ProbeJson(const fs::path& file, const fs::path& cache_dir) {
    InputFile in;
    std::string error;
    if (!in.Open(file, &error)) return {};
    AVFormatContext* f = in.format;
    const int video = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio = DefaultAudio(f);
    std::string out = "{\"durationMs\":" + std::to_string(f->duration > 0 ? f->duration / 1000 : 0);
    if (video >= 0) {
        AVStream* st = f->streams[video];
        const AVCodecParameters* p = st->codecpar;
        const char* profile = avcodec_profile_name(p->codec_id, p->profile);
        const char* pix = av_get_pix_fmt_name(static_cast<AVPixelFormat>(p->format));
        const double fps = st->avg_frame_rate.den ? av_q2d(st->avg_frame_rate) : 0.0;
        char fps_text[32];
        std::snprintf(fps_text, sizeof fps_text, "%.3f", fps);
        out += ",\"video\":{\"index\":" + std::to_string(video) + ",\"codec\":" + vp::Json::Quote(avcodec_get_name(p->codec_id)) +
               ",\"profile\":" + vp::Json::Quote(profile ? profile : "") + ",\"width\":" + std::to_string(p->width) +
               ",\"height\":" + std::to_string(p->height) + ",\"pixFmt\":" + vp::Json::Quote(pix ? pix : "") + ",\"fps\":" + fps_text +
               ",\"browser\":" + (BrowserVideo(p->codec_id) ? "true" : "false") + "}";
    }
    std::string audios = "[", subs = "[", fonts = "[";
    const std::string key = FileKey(file);
    for (unsigned i = 0; i < f->nb_streams; ++i) {
        AVStream* st = f->streams[i];
        const AVCodecParameters* p = st->codecpar;
        const std::string common = "{\"index\":" + std::to_string(i) + ",\"codec\":" + vp::Json::Quote(avcodec_get_name(p->codec_id)) +
                                   ",\"lang\":" + vp::Json::Quote(Tag(st, "language")) + ",\"title\":" + vp::Json::Quote(Tag(st, "title")) +
                                   ",\"default\":" + ((st->disposition & AV_DISPOSITION_DEFAULT) ? "true" : "false");
        if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            audios += std::string(audios.size() > 1 ? "," : "") + common + ",\"channels\":" + std::to_string(p->ch_layout.nb_channels) +
                      ",\"copy\":" + (BrowserAudioCopy(p->codec_id) ? "true" : "false") + ",\"best\":" + (static_cast<int>(i) == audio ? "true" : "false") + "}";
        } else if (p->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            subs += std::string(subs.size() > 1 ? "," : "") + common + ",\"forced\":" + ((st->disposition & AV_DISPOSITION_FORCED) ? "true" : "false") +
                    ",\"text\":" + (TextSubtitle(p->codec_id) ? "true" : "false") + "}";
        } else if (p->codec_type == AVMEDIA_TYPE_ATTACHMENT && p->extradata_size > 0 && !cache_dir.empty()) {
            // Embedded fonts for ASS subtitles, extracted once per file version.
            std::string name = Tag(st, "filename"), mime = Tag(st, "mimetype"), lower = name;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool font = mime.find("font") != std::string::npos || lower.ends_with(".ttf") || lower.ends_with(".otf") || lower.ends_with(".ttc");
            if (!font || name.empty() || name.find_first_of("/\\:") != std::string::npos) continue;
            const fs::path dir = cache_dir / "fonts" / key;
            const fs::path target = dir / vp::lan::PathFromUtf8(name);
            std::error_code ec;
            if (!fs::exists(target, ec)) {
                fs::create_directories(dir, ec);
                std::ofstream write(target, std::ios::binary);
                write.write(reinterpret_cast<const char*>(p->extradata), p->extradata_size);
                if (!write) continue;
            }
            fonts += std::string(fonts.size() > 1 ? "," : "") + vp::Json::Quote("fonts/" + key + "/" + name);
        }
    }
    out += ",\"audio\":" + audios + "],\"subtitles\":" + subs + "],\"fonts\":" + fonts + "]}";
    return out;
}

// ---------------------------------------------------------------- seek point
std::string SeekPointJson(const fs::path& file, std::int64_t ms) {
    InputFile in;
    std::string error;
    if (!in.Open(file, &error)) return {};
    const int video = av_find_best_stream(in.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) return {};
    const std::int64_t us = SeekToKeyFrame(in, video, ms, nullptr);
    if (us == INT64_MIN) return {};
    return "{\"startMs\":" + std::to_string((us - in.StartUs()) / 1000) + "}"; // may be slightly negative (B-frame delay)
}

// ---------------------------------------------------------------- remux
bool Remux(const fs::path& file, std::int64_t start_ms, int audio_stream, const std::function<bool(const std::string&)>& begin, const WriteFn& write,
           std::string* error) {
    InputFile in;
    if (!in.Open(file, error)) return false;
    AVFormatContext* f = in.format;
    const int video = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) { if (error) *error = "no video stream"; return false; }
    AVStream* vin = f->streams[video];
    if (!BrowserVideo(vin->codecpar->codec_id)) {
        if (error) *error = std::string("video codec ") + avcodec_get_name(vin->codecpar->codec_id) + " cannot play in a browser";
        return false;
    }
    int audio = audio_stream;
    if (audio < 0 || audio >= static_cast<int>(f->nb_streams) || f->streams[audio]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) audio = DefaultAudio(f);
    for (unsigned i = 0; i < f->nb_streams; ++i)
        if (static_cast<int>(i) != video && static_cast<int>(i) != audio) f->streams[i]->discard = AVDISCARD_ALL;

    // Output: fragmented MP4 written straight to the connection.
    Sink sink{&write};
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "mp4", nullptr) < 0 || !out) { if (error) *error = "mp4 muxer unavailable"; return false; }
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> out_guard(out, [](AVFormatContext* o) {
        if (o->pb) { av_freep(&o->pb->buffer); avio_context_free(&o->pb); }
        avformat_free_context(o);
    });
    constexpr int kBuffer = 64 * 1024;
    auto* buffer = static_cast<unsigned char*>(av_malloc(kBuffer));
    out->pb = avio_alloc_context(buffer, kBuffer, 1, &sink, nullptr, &WritePacket, nullptr);
    out->flags |= AVFMT_FLAG_CUSTOM_IO;

    AVStream* vout = avformat_new_stream(out, nullptr);
    avcodec_parameters_copy(vout->codecpar, vin->codecpar);
    const AVCodecID vid = vin->codecpar->codec_id;
    vout->codecpar->codec_tag = vid == AV_CODEC_ID_HEVC ? MKTAG('h', 'v', 'c', '1') : vid == AV_CODEC_ID_H264 ? MKTAG('a', 'v', 'c', '1')
                              : vid == AV_CODEC_ID_AV1 ? MKTAG('a', 'v', '0', '1') : MKTAG('v', 'p', '0', '9');
    vout->time_base = vin->time_base;

    AudioEncoder aenc;
    AVStream* aout = nullptr;
    AVStream* ain = audio >= 0 ? f->streams[audio] : nullptr;
    const bool copy_audio = ain && BrowserAudioCopy(ain->codecpar->codec_id);
    if (ain) {
        aout = avformat_new_stream(out, nullptr);
        if (copy_audio) {
            avcodec_parameters_copy(aout->codecpar, ain->codecpar);
            aout->codecpar->codec_tag = 0;
            aout->time_base = ain->time_base;
        } else {
            const AVCodec* dec = avcodec_find_decoder(ain->codecpar->codec_id);
            const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
            aenc.decoder = dec ? avcodec_alloc_context3(dec) : nullptr;
            aenc.encoder = enc ? avcodec_alloc_context3(enc) : nullptr;
            if (!aenc.decoder || !aenc.encoder || avcodec_parameters_to_context(aenc.decoder, ain->codecpar) < 0 ||
                avcodec_open2(aenc.decoder, dec, nullptr) < 0) {
                if (error) *error = std::string("cannot decode audio ") + avcodec_get_name(ain->codecpar->codec_id);
                return false;
            }
            aenc.decoder->pkt_timebase = ain->time_base;
            aenc.encoder->sample_rate = 48000;
            aenc.encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&aenc.encoder->ch_layout, 2);
            aenc.encoder->bit_rate = 192000;
            aenc.encoder->time_base = AVRational{1, 48000};
            if (out->oformat->flags & AVFMT_GLOBALHEADER) aenc.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(aenc.encoder, enc, nullptr) < 0) { if (error) *error = "AAC encoder unavailable"; return false; }
            avcodec_parameters_from_context(aout->codecpar, aenc.encoder);
            aout->time_base = aenc.encoder->time_base;
            aenc.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 4096);
            aenc.frame = av_frame_alloc();
            aenc.packet = av_packet_alloc();
        }
        const std::string lang = Tag(ain, "language");
        if (!lang.empty()) av_dict_set(&aout->metadata, "language", lang.c_str(), 0);
    }

    AVPacket* packet = av_packet_alloc();
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet_guard(packet, [](AVPacket* p) { av_packet_free(&p); });
    std::vector<AVPacket*> early;
    std::unique_ptr<std::vector<AVPacket*>, void (*)(std::vector<AVPacket*>*)> early_guard(&early, [](std::vector<AVPacket*>* v) {
        for (AVPacket* p : *v) av_packet_free(&p);
    });
    const std::int64_t base_us = SeekToKeyFrame(in, video, start_ms, packet, &early);
    if (base_us == INT64_MIN) { if (error) *error = "no key frame at the requested time"; return false; }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+default_base_moof+negative_cts_offsets", 0);
    av_dict_set(&options, "frag_duration", "2000000", 0);
    // Keep the source clock (Matroska: 1/1000) so timestamps are copied exactly instead of being
    // re-derived from the frame rate.
    if (vin->time_base.num == 1 && vin->time_base.den >= 1000 && vin->time_base.den <= 1000000)
        av_dict_set_int(&options, "video_track_timescale", vin->time_base.den, 0);
    if (!begin("video/mp4")) { av_dict_free(&options); return true; }
    const int header = avformat_write_header(out, &options);
    av_dict_free(&options);
    if (header < 0) { if (error) *error = "mp4 header: " + FfError(header); return false; }

    const auto offset_in = [&](AVStream* st) { return av_rescale_q(base_us, AV_TIME_BASE_Q, st->time_base); };
    const std::int64_t video_offset = offset_in(vin);
    const std::int64_t audio_offset = ain ? offset_in(ain) : 0;

    const auto write_audio_frames = [&](bool flush) -> bool {
        // Pull 1024-sample frames from the FIFO into the AAC encoder.
        const int frame_size = aenc.encoder->frame_size > 0 ? aenc.encoder->frame_size : 1024;
        while (av_audio_fifo_size(aenc.fifo) >= frame_size || (flush && av_audio_fifo_size(aenc.fifo) > 0)) {
            const int n = std::min(frame_size, av_audio_fifo_size(aenc.fifo));
            av_frame_unref(aenc.frame);
            aenc.frame->nb_samples = n;
            aenc.frame->format = AV_SAMPLE_FMT_FLTP;
            aenc.frame->sample_rate = 48000;
            av_channel_layout_copy(&aenc.frame->ch_layout, &aenc.encoder->ch_layout);
            if (av_frame_get_buffer(aenc.frame, 0) < 0) return false;
            av_audio_fifo_read(aenc.fifo, reinterpret_cast<void**>(aenc.frame->data), n);
            aenc.frame->pts = aenc.next_pts;
            aenc.next_pts += n;
            if (avcodec_send_frame(aenc.encoder, aenc.frame) < 0) return false;
            while (avcodec_receive_packet(aenc.encoder, aenc.packet) >= 0) {
                aenc.packet->stream_index = aout->index;
                if (aenc.packet->pts != AV_NOPTS_VALUE) aenc.packet->pts += aenc.encoder->initial_padding;
                if (aenc.packet->dts != AV_NOPTS_VALUE) aenc.packet->dts += aenc.encoder->initial_padding;
                av_packet_rescale_ts(aenc.packet, aenc.encoder->time_base, aout->time_base);
                if (av_interleaved_write_frame(out, aenc.packet) < 0) return false;
            }
        }
        if (flush) {
            avcodec_send_frame(aenc.encoder, nullptr);
            while (avcodec_receive_packet(aenc.encoder, aenc.packet) >= 0) {
                aenc.packet->stream_index = aout->index;
                if (aenc.packet->pts != AV_NOPTS_VALUE) aenc.packet->pts += aenc.encoder->initial_padding;
                if (aenc.packet->dts != AV_NOPTS_VALUE) aenc.packet->dts += aenc.encoder->initial_padding;
                av_packet_rescale_ts(aenc.packet, aenc.encoder->time_base, aout->time_base);
                if (av_interleaved_write_frame(out, aenc.packet) < 0) return false;
            }
        }
        return true;
    };
    const auto decode_audio = [&](AVPacket* pkt) -> bool {
        if (avcodec_send_packet(aenc.decoder, pkt) < 0 && pkt) return true; // skip a damaged packet
        AVFrame* frame = av_frame_alloc();
        bool ok = true;
        while (ok && avcodec_receive_frame(aenc.decoder, frame) >= 0) {
            const std::int64_t pts = frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE && pts + av_rescale_q(frame->nb_samples, AVRational{1, frame->sample_rate}, ain->time_base) <= audio_offset) {
                av_frame_unref(frame);
                continue; // entirely before the start
            }
            if (!aenc.swr) {
                AVChannelLayout in_layout;
                if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !frame->ch_layout.nb_channels)
                    av_channel_layout_default(&in_layout, frame->ch_layout.nb_channels ? frame->ch_layout.nb_channels : 2);
                else
                    av_channel_layout_copy(&in_layout, &frame->ch_layout);
                const int r = swr_alloc_set_opts2(&aenc.swr, &aenc.encoder->ch_layout, AV_SAMPLE_FMT_FLTP, 48000, &in_layout,
                                                  static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&in_layout);
                if (r < 0 || swr_init(aenc.swr) < 0) { ok = false; break; }
                aenc.next_pts = 0;
                // Samples before the stream start are trimmed so audio lines up with video.
                if (pts != AV_NOPTS_VALUE) {
                    // The AAC encoder emits `initial_padding` priming samples first; their packets are
                    // shifted forward by that amount below, so start that much later here.
                    const std::int64_t first = av_rescale_q(pts - audio_offset, ain->time_base, AVRational{1, 48000}) - aenc.encoder->initial_padding;
                    // MP4 fragments start every track at its first sample, so a late start is
                    // filled with silence instead of a timestamp gap.
                    if (first > 0 && first < 48000 * 5) {
                        uint8_t** silence = nullptr;
                        if (av_samples_alloc_array_and_samples(&silence, nullptr, 2, static_cast<int>(first), AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                            av_samples_set_silence(silence, 0, static_cast<int>(first), 2, AV_SAMPLE_FMT_FLTP);
                            av_audio_fifo_write(aenc.fifo, reinterpret_cast<void**>(silence), static_cast<int>(first));
                            av_freep(&silence[0]);
                            av_freep(&silence);
                        }
                    } else if (first < 0) {
                        aenc.trim = -first;
                    }
                }
            }
            const int max_out = swr_get_out_samples(aenc.swr, frame->nb_samples) + 32;
            uint8_t** converted = nullptr;
            if (av_samples_alloc_array_and_samples(&converted, nullptr, 2, max_out, AV_SAMPLE_FMT_FLTP, 0) < 0) { ok = false; break; }
            const int got = swr_convert(aenc.swr, converted, max_out, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (got > 0) {
                av_audio_fifo_write(aenc.fifo, reinterpret_cast<void**>(converted), got);
                if (aenc.trim > 0) {
                    const int drop = static_cast<int>(std::min<std::int64_t>(aenc.trim, av_audio_fifo_size(aenc.fifo)));
                    av_audio_fifo_drain(aenc.fifo, drop);
                    aenc.trim -= drop;
                }
            }
            av_freep(&converted[0]);
            av_freep(&converted);
            av_frame_unref(frame);
            ok = write_audio_frames(false);
        }
        av_frame_free(&frame);
        return ok;
    };

    // The key-frame packet found by the seek is written first.
    bool ok = true;
    bool have = true;
    std::int64_t key_pts = AV_NOPTS_VALUE; // rebased pts of the first key frame
    std::size_t early_next = 0;
    std::int64_t last_video_dts = AV_NOPTS_VALUE;
    const std::int64_t frame_step = vin->avg_frame_rate.num > 0 ? std::max<std::int64_t>(1, av_rescale_q(1, av_inv_q(vin->avg_frame_rate), vin->time_base)) : 1;
    while (ok && !sink.failed) {
        if (!have) {
            if (early_next < early.size()) av_packet_move_ref(packet, early[early_next++]);
            else if (av_read_frame(f, packet) < 0) break;
        }
        have = false;
        if (packet->stream_index == video) {
            if (packet->pts != AV_NOPTS_VALUE) packet->pts -= video_offset;
            if (packet->dts != AV_NOPTS_VALUE) packet->dts -= video_offset;
            // Matroska stores no decode times; MP4 needs monotonic ones (dts <= pts). Continue
            // from the key frame's estimated dts by one frame duration per packet.
            if (packet->dts == AV_NOPTS_VALUE || (last_video_dts != AV_NOPTS_VALUE && packet->dts <= last_video_dts)) {
                const std::int64_t step = packet->duration > 0 ? packet->duration : frame_step;
                packet->dts = last_video_dts == AV_NOPTS_VALUE ? (packet->pts != AV_NOPTS_VALUE ? packet->pts : 0) : last_video_dts + step;
                if (packet->pts != AV_NOPTS_VALUE && packet->dts > packet->pts) packet->dts = packet->pts;
                if (last_video_dts != AV_NOPTS_VALUE && packet->dts <= last_video_dts) packet->dts = last_video_dts + 1;
            }
            // Leading frames of an open GOP that display before the key frame cannot be decoded: drop.
            if (packet->flags & AV_PKT_FLAG_KEY) { if (key_pts == AV_NOPTS_VALUE) key_pts = packet->pts; }
            else if (packet->pts != AV_NOPTS_VALUE && key_pts != AV_NOPTS_VALUE && packet->pts < key_pts) { av_packet_unref(packet); continue; }
            last_video_dts = packet->dts;
            packet->stream_index = vout->index;
            av_packet_rescale_ts(packet, vin->time_base, vout->time_base);
            packet->pos = -1;
            ok = av_interleaved_write_frame(out, packet) >= 0;
        } else if (ain && packet->stream_index == audio) {
            if (copy_audio) {
                const std::int64_t t = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                if (t != AV_NOPTS_VALUE && t < audio_offset) { av_packet_unref(packet); continue; }
                if (packet->pts != AV_NOPTS_VALUE) packet->pts -= audio_offset;
                if (packet->dts != AV_NOPTS_VALUE) packet->dts -= audio_offset;
                packet->stream_index = aout->index;
                av_packet_rescale_ts(packet, ain->time_base, aout->time_base);
                packet->pos = -1;
                ok = av_interleaved_write_frame(out, packet) >= 0;
            } else {
                ok = decode_audio(packet);
            }
        }
        av_packet_unref(packet);
    }
    if (!sink.failed) {
        if (aenc.decoder && aenc.swr) {
            decode_audio(nullptr); // sends the end-of-stream packet and drains the decoder
            write_audio_frames(true);
        }
        av_write_trailer(out);
        avio_flush(out->pb);
    }
    return true;
}

#if !defined(VP_ENGINE_MPV)
bool ProcessedVideoStream(const fs::path& file, std::int64_t start_ms, int audio_stream,
                          bool enable_upscale, bool enable_interpolation,
                          const StreamProcessingSettings& settings,
                          const std::function<bool(const std::string& mime)>& begin,
                          const WriteFn& write, std::string* error, std::string* diagnostics) {
    if (diagnostics) diagnostics->clear();
    std::unique_lock<std::timed_mutex> stream_lock(g_processed_stream_mutex, std::defer_lock);
    if (!stream_lock.try_lock_for(std::chrono::seconds(5))) {
        if (error) *error = "another server-processed stream remained active after waiting 5 seconds";
        return false;
    }
    ProcessedVideoMux mux(file, start_ms, audio_stream, begin, write, enable_upscale, enable_interpolation);
    pc_player* player = pc_create();
    if (!player) { if (error) *error = "cannot create server playback session"; return false; }
    const auto player_guard = std::unique_ptr<pc_player, void (*)(pc_player*)>(player, [](pc_player* p) { pc_destroy(p); });
    const auto fail_api = [&](const char* operation, int result) {
        if (result == PC_OK) return false;
        if (error) *error = std::string(operation) + " failed (" + std::to_string(result) + ")";
        return true;
    };
    const std::int32_t interpolation_backend = settings.interpolation_backend == PC_FRAME_INTERPOLATION_RIFE
        ? PC_FRAME_INTERPOLATION_RIFE : PC_FRAME_INTERPOLATION_AMF;
    const bool use_rife = enable_interpolation && interpolation_backend == PC_FRAME_INTERPOLATION_RIFE;
    const bool enable_rife_restore = use_rife && settings.rife_restore;
    const bool enable_rife_warp = use_rife && enable_upscale && settings.rife_output_warp;
    const bool enable_frc_dedup = enable_interpolation && settings.frc_dedup;
    if (fail_api("composition output", pc_set_render_target(player, PC_TARGET_COMPOSITION, nullptr, 1280, 720)) ||
        fail_api("configure frame interpolation", pc_set_frame_interpolation_ex(player, enable_interpolation ? 1 : 0, interpolation_backend, nullptr)) ||
        fail_api("configure RIFE restoration", pc_set_rife_restore(player, enable_rife_restore ? 1 : 0,
                  enable_rife_restore ? settings.rife_restore_model.c_str() : nullptr)) ||
        fail_api("configure RIFE output warp", pc_set_rife_output_warp(player, enable_rife_warp ? 1 : 0)) ||
        fail_api("configure duplicate-aware interpolation", pc_set_rife_dedup(player, enable_frc_dedup ? 1 : 0)) ||
        fail_api("disable software decode fallback", pc_set_software_decode_fallback(player, 0)) ||
        fail_api("set output frame callback", pc_set_output_frame_callback(player, &ProcessedVideoMux::OnCoreFrame, &mux))) return false;

    if (enable_upscale) {
        std::int32_t mode = settings.mode;
        if (mode == PC_UPSCALE_OFF) mode = PC_UPSCALE_ANIME4K_ACNET;
        if (fail_api("select upscaler model", pc_set_animejanai_model(player, settings.animejanai_model)) ||
            fail_api("enable server upscaling", pc_set_upscale_ex(player, mode, settings.inference_height))) return false;
    } else if (fail_api("disable server upscaling", pc_set_upscale_ex(player, PC_UPSCALE_OFF, settings.inference_height))) {
        return false;
    }
    const std::string utf8 = vp::lan::PathToUtf8(file);
    if (fail_api("open server playback", pc_open(player, utf8.c_str(), std::max<std::int64_t>(0, start_ms), PC_OPEN_NO_AUDIO))) return false;

    pc_state state{};
    bool ended = false;
    for (;;) {
        if (mux.failed()) {
            pc_stop(player);
            break;
        }
        pc_get_state(player, &state);
        if (state.state == PC_STATE_ENDED) { ended = true; break; }
        if (state.state == PC_STATE_ERROR || state.state == PC_STATE_STOPPED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ended && !mux.failed()) mux.Finish();
    if (error) {
        if (!mux.error().empty()) *error = mux.error();
        else if (state.state == PC_STATE_ERROR && state.error[0]) *error = state.error;
    }
    const bool ok = mux.started() && !mux.failed();
    if (diagnostics) *diagnostics = mux.TimingSummary();
    return ok;
}
#endif

// ---------------------------------------------------------------- subtitles
namespace {
std::string AssTime(std::int64_t ms) {
    if (ms < 0) ms = 0;
    char text[32];
    std::snprintf(text, sizeof text, "%d:%02d:%02d.%02d", static_cast<int>(ms / 3600000), static_cast<int>(ms / 60000 % 60),
                  static_cast<int>(ms / 1000 % 60), static_cast<int>(ms / 10 % 100));
    return text;
}
std::mutex g_subtitle_mutex;
} // namespace

std::string SubtitleAss(const fs::path& file, int stream, const fs::path& cache_dir) {
    std::lock_guard<std::mutex> lock(g_subtitle_mutex); // one full-file read at a time
    const fs::path cached = cache_dir / "subs" / (FileKey(file) + "_" + std::to_string(stream) + ".ass");
    {
        std::ifstream read(cached, std::ios::binary);
        if (read) { std::stringstream s; s << read.rdbuf(); return s.str(); }
    }
    InputFile in;
    std::string error;
    if (!in.Open(file, &error) || stream < 0 || stream >= static_cast<int>(in.format->nb_streams)) return {};
    AVStream* st = in.format->streams[stream];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE || !TextSubtitle(st->codecpar->codec_id)) return {};
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> dec_guard(dec, [](AVCodecContext* c) { avcodec_free_context(&c); });
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) return {};
    dec->pkt_timebase = st->time_base;
    if (avcodec_open2(dec, codec, nullptr) < 0) return {};
    for (unsigned i = 0; i < in.format->nb_streams; ++i)
        if (static_cast<int>(i) != stream) in.format->streams[i]->discard = AVDISCARD_ALL;

    std::string header = dec->subtitle_header ? std::string(reinterpret_cast<const char*>(dec->subtitle_header), dec->subtitle_header_size) : std::string();
    while (!header.empty() && (header.back() == '\0')) header.pop_back();
    if (header.find("[Events]") == std::string::npos)
        header += "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    if (header.back() != '\n') header += '\n';

    std::string events;
    const std::int64_t start_us = in.StartUs();
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(in.format, packet) >= 0) {
        if (packet->stream_index == stream && packet->pts != AV_NOPTS_VALUE) {
            AVSubtitle sub{};
            int got = 0;
            if (avcodec_decode_subtitle2(dec, &sub, &got, packet) >= 0 && got) {
                const std::int64_t pts_ms = (av_rescale_q(packet->pts, st->time_base, AV_TIME_BASE_Q) - start_us) / 1000;
                const std::int64_t begin = pts_ms + sub.start_display_time;
                std::int64_t end = sub.end_display_time > sub.start_display_time ? pts_ms + sub.end_display_time
                                 : packet->duration > 0 ? pts_ms + av_rescale_q(packet->duration, st->time_base, AVRational{1, 1000}) : begin + 3000;
                if (end <= begin) end = begin + 1000;
                for (unsigned r = 0; r < sub.num_rects; ++r) {
                    if (!sub.rects[r]->ass) continue;
                    // FFmpeg's ASS events: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text".
                    std::string line = sub.rects[r]->ass;
                    if (line.rfind("Dialogue:", 0) == 0) { events += line + "\n"; continue; }
                    std::size_t comma = line.find(',');
                    if (comma == std::string::npos) continue;
                    std::string rest = line.substr(comma + 1); // drop ReadOrder
                    const std::size_t layer_end = rest.find(',');
                    if (layer_end == std::string::npos) continue;
                    events += "Dialogue: " + rest.substr(0, layer_end) + "," + AssTime(begin) + "," + AssTime(end) + rest.substr(layer_end) + "\n";
                }
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    const std::string script = header + events;
    std::error_code ec;
    fs::create_directories(cached.parent_path(), ec);
    std::ofstream write(cached, std::ios::binary);
    write << script;
    return script;
}

// ---------------------------------------------------------------- LAN server glue
void InstallLanHooks(vp::lan::LanHooks& hooks, const fs::path& cache_dir, StreamProcessingSettingsFn processing_settings) {
    hooks.probe = [cache_dir](const fs::path& file) { return ProbeJson(file, cache_dir); };
    hooks.seekpoint = [](const fs::path& file, std::int64_t ms) { return SeekPointJson(file, ms); };
    hooks.subtitle = [cache_dir](const fs::path& file, int stream) { return SubtitleAss(file, stream, cache_dir); };
    const auto stream_log = hooks.log;
    hooks.remux = [processing_settings = std::move(processing_settings), stream_log](const vp::lan::Request& request, const fs::path& file, vp::lan::Connection& conn) {
        std::int64_t start = 0;
        int audio = -1;
        try { start = std::stoll(request.Query("start", "0")); } catch (...) {}
        try { audio = std::stoi(request.Query("audio", "-1")); } catch (...) {}
        bool started = false;
        std::string error;
        const auto begin = [&](const std::string& mime) {
            started = conn.BeginChunked(200, mime, "Access-Control-Allow-Origin: *\r\n");
            return started;
        };
        const WriteFn write = [&](const std::uint8_t* data, std::size_t size) { return conn.SendChunk(data, size); };
        const bool enable_upscale = request.Query("sr") == "1";
        const bool enable_interpolation = request.Query("frc") == "1";
        if (enable_upscale || enable_interpolation) {
#if defined(VP_ENGINE_MPV)
            const std::string body = "{\"error\":\"server-processed streams are not available in the mpv edition\"}";
            conn.SendText("HTTP/1.1 415 Unsupported Media Type\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            return true;
#else
            const StreamProcessingSettings settings = processing_settings ? processing_settings() : StreamProcessingSettings{};
            std::string diagnostics;
            const bool ok = ProcessedVideoStream(file, start, audio, enable_upscale, enable_interpolation, settings,
                                                 begin, write, &error, &diagnostics);
            if (stream_log && !diagnostics.empty())
                stream_log("processed video stream timing: sr=" + std::to_string(enable_upscale) +
                           " frc=" + std::to_string(enable_interpolation) +
                           " inference_height=" + std::to_string(settings.inference_height) + " " + diagnostics);
            if (started) {
                if (!ok && stream_log) stream_log("processed video stream failed after response start: " +
                    (error.empty() ? std::string("unknown stream error") : error));
                if (!conn.closed()) conn.EndChunked();
                return true;
            }
            if (!ok) {
                if (stream_log) stream_log("processed video stream failed before response start: " +
                    (error.empty() ? std::string("unknown stream error") : error));
                const std::string body = "{\"error\":" + vp::Json::Quote(error.empty() ? "server-processed video stream failed" : error) + "}";
                conn.SendText("HTTP/1.1 415 Unsupported Media Type\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                return true;
            }
            return false;
#endif
        }
        const bool ok = Remux(
            file, start, audio,
            begin, write, &error);
        if (started) {
            if (!conn.closed()) conn.EndChunked();
            return true;
        }
        if (!ok) {
            const std::string body = "{\"error\":" + vp::Json::Quote(error) + "}";
            conn.SendText("HTTP/1.1 415 Unsupported Media Type\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            return true;
        }
        return false;
    };
}

} // namespace vp::remux
