/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/opus_depacketizer.h"
#include "roc_audio/sample_spec_to_str.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_status/code_to_str.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace roc {
namespace audio {

namespace {

const core::nanoseconds_t LogInterval = 20 * core::Second;

inline void write_zeros(sample_t* buf, size_t bufsz) {
    memset(buf, 0, bufsz * sizeof(sample_t));
}

inline void write_beep(sample_t* buf, size_t bufsz, size_t sample_rate) {
    const double Pi = 3.14159265358979323846;
    for (size_t n = 0; n < bufsz; n++) {
        buf[n] = (sample_t)std::sin(2 * Pi / sample_rate * 880 * n);
    }
}

bool supported_frame_size(size_t n_samples) {
    return n_samples == 120 || n_samples == 240 || n_samples == 480 || n_samples == 960
        || n_samples == 1920 || n_samples == 2880;
}

} // namespace

OpusDepacketizer::OpusDepacketizer(packet::IReader& reader,
                                   OpusDecoder& payload_decoder,
                                   const SampleSpec& sample_spec,
                                   bool beep,
                                   core::IArena& arena)
    : reader_(reader)
    , payload_decoder_(payload_decoder)
    , sample_spec_(sample_spec)
    , decoded_buffer_(arena)
    , stream_ts_(0)
    , next_capture_ts_(0)
    , valid_capture_ts_(false)
    , zero_samples_(0)
    , missing_samples_(0)
    , packet_samples_(0)
    , last_packet_duration_(0)
    , chunk_offset_(0)
    , chunk_size_(0)
    , chunk_source_(ChunkSource_None)
    , rate_limiter_(LogInterval)
    , beep_(beep)
    , first_packet_(true)
    , valid_(false) {
    roc_panic_if_msg(!sample_spec_.is_valid() || !sample_spec_.is_raw(),
                     "opus depacketizer: required valid raw sample spec: %s",
                     sample_spec_to_str(sample_spec_).c_str());

    if (sample_spec_.sample_rate() != OpusSampleRate) {
        roc_log(LogError, "opus depacketizer: unsupported sample rate");
        return;
    }

    if (!decoded_buffer_.resize(OpusMaxFrameSamplesPerChan * sample_spec_.num_channels())) {
        roc_log(LogError, "opus depacketizer: can't allocate decode buffer");
        return;
    }

    valid_ = true;
}

bool OpusDepacketizer::is_valid() const {
    return valid_;
}

bool OpusDepacketizer::is_started() const {
    return !first_packet_;
}

bool OpusDepacketizer::read(Frame& frame) {
    read_frame_(frame);
    report_stats_();
    return true;
}

packet::stream_timestamp_t OpusDepacketizer::next_timestamp() const {
    return first_packet_ ? 0 : stream_ts_;
}

void OpusDepacketizer::read_frame_(Frame& frame) {
    if (frame.num_raw_samples() % sample_spec_.num_channels() != 0) {
        roc_panic("opus depacketizer: unexpected frame size");
    }

    sample_t* buff_ptr = frame.raw_samples();
    sample_t* buff_end = frame.raw_samples() + frame.num_raw_samples();
    FrameInfo info;

    while (buff_ptr < buff_end) {
        buff_ptr = read_samples_(buff_ptr, buff_end, info);
    }

    set_frame_props_(frame, info);
}

sample_t* OpusDepacketizer::read_samples_(sample_t* buff_ptr,
                                          sample_t* buff_end,
                                          FrameInfo& info) {
    if (!ensure_chunk_(info)) {
        const size_t n_samples = (size_t)(buff_end - buff_ptr) / sample_spec_.num_channels();

        if (beep_) {
            write_beep(buff_ptr, n_samples * sample_spec_.num_channels(),
                       sample_spec_.sample_rate());
        } else {
            write_zeros(buff_ptr, n_samples * sample_spec_.num_channels());
        }

        if (!info.capture_ts && valid_capture_ts_) {
            info.capture_ts =
                next_capture_ts_ - sample_spec_.samples_overall_2_ns(info.n_filled_samples);
        }
        if (valid_capture_ts_) {
            next_capture_ts_ += sample_spec_.samples_overall_2_ns(
                n_samples * sample_spec_.num_channels());
        }

        if (first_packet_) {
            zero_samples_ += (packet::stream_timestamp_t)n_samples;
        } else {
            missing_samples_ += (packet::stream_timestamp_t)n_samples;
        }

        info.n_filled_samples += n_samples * sample_spec_.num_channels();
        stream_ts_ += (packet::stream_timestamp_t)n_samples;
        return buff_end;
    }

    const size_t to_copy =
        std::min((size_t)(buff_end - buff_ptr), chunk_size_ - chunk_offset_);
    memcpy(buff_ptr, decoded_buffer_.data() + chunk_offset_, to_copy * sizeof(sample_t));

    const size_t copied_per_chan = to_copy / sample_spec_.num_channels();

    if (!info.capture_ts && valid_capture_ts_) {
        info.capture_ts =
            next_capture_ts_ - sample_spec_.samples_overall_2_ns(info.n_filled_samples);
    }
    if (valid_capture_ts_) {
        next_capture_ts_ += sample_spec_.samples_overall_2_ns(to_copy);
    }

    if (chunk_source_ == ChunkSource_Packet) {
        info.n_decoded_samples += to_copy;
        packet_samples_ += (packet::stream_timestamp_t)copied_per_chan;
    } else if (first_packet_) {
        zero_samples_ += (packet::stream_timestamp_t)copied_per_chan;
    } else {
        missing_samples_ += (packet::stream_timestamp_t)copied_per_chan;
    }

    info.n_filled_samples += to_copy;
    chunk_offset_ += to_copy;
    stream_ts_ += (packet::stream_timestamp_t)copied_per_chan;

    if (chunk_offset_ == chunk_size_) {
        chunk_offset_ = 0;
        chunk_size_ = 0;
        chunk_source_ = ChunkSource_None;
    }

    return buff_ptr + to_copy;
}

bool OpusDepacketizer::ensure_chunk_(FrameInfo& info) {
    if (chunk_offset_ < chunk_size_) {
        return true;
    }

    chunk_offset_ = 0;
    chunk_size_ = 0;
    chunk_source_ = ChunkSource_None;

    if (!fetch_packet_(info) && !next_packet_) {
        if (first_packet_ || last_packet_duration_ == 0
            || !decode_synthetic_chunk_(supported_frame_size(last_packet_duration_)
                                            ? (size_t)last_packet_duration_
                                            : 0)) {
            return false;
        }
        return true;
    }

    if (first_packet_) {
        roc_log(LogDebug, "opus depacketizer: got first packet: zero_samples=%lu",
                (unsigned long)zero_samples_);
        stream_ts_ = next_packet_->stream_timestamp();
        first_packet_ = false;
    }

    if (next_packet_
        && !packet::stream_timestamp_lt(stream_ts_, next_packet_->stream_timestamp())) {
        return decode_packet_chunk_();
    }

    return decode_missing_chunk_();
}

bool OpusDepacketizer::fetch_packet_(FrameInfo& info) {
    if (next_packet_) {
        return true;
    }

    while ((next_packet_ = read_packet_())) {
        const packet::stream_timestamp_t pkt_end =
            next_packet_->stream_timestamp() + next_packet_->duration();

        if (first_packet_ || packet::stream_timestamp_lt(stream_ts_, pkt_end)) {
            return true;
        }

        roc_log(LogDebug, "opus depacketizer: dropping late packet: ts=%lu pkt_ts=%lu",
                (unsigned long)stream_ts_,
                (unsigned long)next_packet_->stream_timestamp());
        info.n_dropped_packets++;
        next_packet_ = NULL;
    }

    return false;
}

bool OpusDepacketizer::decode_packet_chunk_() {
    size_t decoded_samples = 0;
    if (!payload_decoder_.decode(next_packet_->payload().data(),
                                 next_packet_->payload().size(), decoded_buffer_.data(),
                                 decoded_buffer_.size(), decoded_samples)) {
        next_packet_ = NULL;
        return decode_missing_chunk_();
    }

    const packet::stream_timestamp_t pkt_timestamp = next_packet_->stream_timestamp();
    size_t skip_samples = 0;
    if (packet::stream_timestamp_lt(pkt_timestamp, stream_ts_)) {
        skip_samples = (size_t)packet::stream_timestamp_diff(stream_ts_, pkt_timestamp);
        if (skip_samples >= decoded_samples) {
            next_packet_ = NULL;
            return false;
        }
    }

    chunk_offset_ = skip_samples * sample_spec_.num_channels();
    chunk_size_ = decoded_samples * sample_spec_.num_channels();
    chunk_source_ = ChunkSource_Packet;

    if (next_packet_->capture_timestamp() != 0) {
        next_capture_ts_ = next_packet_->capture_timestamp()
            + sample_spec_.samples_per_chan_2_ns(skip_samples);
        valid_capture_ts_ = true;
    }

    last_packet_duration_ = (packet::stream_timestamp_t)decoded_samples;
    next_packet_ = NULL;
    return true;
}

bool OpusDepacketizer::decode_missing_chunk_() {
    packet::stream_timestamp_t gap = 0;
    if (next_packet_) {
        gap = (packet::stream_timestamp_t)packet::stream_timestamp_diff(
            next_packet_->stream_timestamp(), stream_ts_);
    }

    size_t conceal_samples = 0;
    if (gap != 0) {
        const packet::stream_timestamp_t pref =
            last_packet_duration_ != 0 ? last_packet_duration_ : next_packet_->duration();
        conceal_samples = (size_t)std::min(gap, pref);
    } else if (last_packet_duration_ != 0) {
        conceal_samples = (size_t)last_packet_duration_;
    }

    if (conceal_samples == 0 || !supported_frame_size(conceal_samples)) {
        return decode_synthetic_chunk_(conceal_samples);
    }

    size_t decoded_samples = 0;
    bool ok = false;

    if (next_packet_ && conceal_samples == next_packet_->duration()) {
        ok = payload_decoder_.decode_fec(next_packet_->payload().data(),
                                         next_packet_->payload().size(), conceal_samples,
                                         decoded_buffer_.data(), decoded_buffer_.size(),
                                         decoded_samples);
    } else {
        ok = payload_decoder_.decode_missing(conceal_samples, decoded_buffer_.data(),
                                             decoded_buffer_.size(), decoded_samples);
    }

    if (!ok || decoded_samples == 0) {
        return decode_synthetic_chunk_(conceal_samples);
    }

    chunk_offset_ = 0;
    chunk_size_ = decoded_samples * sample_spec_.num_channels();
    chunk_source_ = ChunkSource_Conceal;
    last_packet_duration_ = (packet::stream_timestamp_t)decoded_samples;

    if (next_packet_ && next_packet_->capture_timestamp() != 0) {
        const core::nanoseconds_t conceal_ns =
            sample_spec_.samples_per_chan_2_ns(decoded_samples);
        next_capture_ts_ =
            next_packet_->capture_timestamp() > conceal_ns
                ? next_packet_->capture_timestamp() - conceal_ns
                : 0;
        if (!valid_capture_ts_ && next_capture_ts_ != 0) {
            valid_capture_ts_ = true;
        }
    }

    return true;
}

bool OpusDepacketizer::decode_synthetic_chunk_(size_t n_samples) {
    if (n_samples == 0 || decoded_buffer_.size() < n_samples * sample_spec_.num_channels()) {
        return false;
    }

    if (beep_) {
        write_beep(decoded_buffer_.data(), n_samples * sample_spec_.num_channels(),
                   sample_spec_.sample_rate());
    } else {
        write_zeros(decoded_buffer_.data(), n_samples * sample_spec_.num_channels());
    }

    chunk_offset_ = 0;
    chunk_size_ = n_samples * sample_spec_.num_channels();
    chunk_source_ = ChunkSource_Synthetic;
    last_packet_duration_ = (packet::stream_timestamp_t)n_samples;
    return true;
}

packet::PacketPtr OpusDepacketizer::read_packet_() {
    packet::PacketPtr pp;
    const status::StatusCode code = reader_.read(pp);
    if (code != status::StatusOK) {
        if (code != status::StatusNoData) {
            roc_log(LogError, "opus depacketizer: failed to read packet: status=%s",
                    status::code_to_str(code));
        }

        return NULL;
    }

    return pp;
}

void OpusDepacketizer::set_frame_props_(Frame& frame, const FrameInfo& info) {
    unsigned flags = 0;

    if (info.n_decoded_samples != 0) {
        flags |= Frame::FlagNotBlank;
    }

    if (info.n_decoded_samples < frame.num_raw_samples()) {
        flags |= Frame::FlagNotComplete;
    }

    if (info.n_dropped_packets != 0) {
        flags |= Frame::FlagPacketDrops;
    }

    frame.set_flags(flags);
    frame.set_duration(frame.num_raw_samples() / sample_spec_.num_channels());

    if (info.capture_ts > 0) {
        frame.set_capture_timestamp(info.capture_ts);
    }
}

void OpusDepacketizer::report_stats_() {
    if (!rate_limiter_.allow()) {
        return;
    }

    const size_t total_samples = missing_samples_ + packet_samples_;
    const double loss_ratio =
        total_samples != 0 ? (double)missing_samples_ / total_samples : 0.;

    roc_log(LogDebug, "opus depacketizer: ts=%lu loss_ratio=%.5lf",
            (unsigned long)stream_ts_, loss_ratio);
}

} // namespace audio
} // namespace roc
