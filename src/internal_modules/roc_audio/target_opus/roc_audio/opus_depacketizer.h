/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

//! @file roc_audio/target_opus/roc_audio/opus_depacketizer.h
//! @brief Opus depacketizer.

#ifndef ROC_AUDIO_OPUS_DEPACKETIZER_H_
#define ROC_AUDIO_OPUS_DEPACKETIZER_H_

#include "roc_audio/depacketizer.h"
#include "roc_audio/opus_decoder.h"
#include "roc_audio/sample_spec.h"
#include "roc_core/array.h"
#include "roc_core/iarena.h"
#include "roc_core/rate_limiter.h"
#include "roc_packet/ireader.h"

namespace roc {
namespace audio {

class OpusDepacketizer : public IDepacketizer, public core::NonCopyable<> {
public:
    OpusDepacketizer(packet::IReader& reader,
                     OpusDecoder& payload_decoder,
                     const SampleSpec& sample_spec,
                     bool beep,
                     core::IArena& arena);

    virtual bool is_valid() const;
    virtual bool is_started() const;
    virtual bool read(Frame& frame);
    virtual packet::stream_timestamp_t next_timestamp() const;

private:
    enum ChunkSource {
        ChunkSource_None,
        ChunkSource_Packet,
        ChunkSource_Conceal,
        ChunkSource_Synthetic
    };

    struct FrameInfo {
        size_t n_decoded_samples;
        size_t n_filled_samples;
        size_t n_dropped_packets;
        core::nanoseconds_t capture_ts;

        FrameInfo()
            : n_decoded_samples(0)
            , n_filled_samples(0)
            , n_dropped_packets(0)
            , capture_ts(0) {
        }
    };

    void read_frame_(Frame& frame);
    sample_t* read_samples_(sample_t* buff_ptr, sample_t* buff_end, FrameInfo& info);
    bool ensure_chunk_(FrameInfo& info);
    bool fetch_packet_(FrameInfo& info);
    bool decode_packet_chunk_();
    bool decode_missing_chunk_();
    bool decode_synthetic_chunk_(size_t n_samples);
    packet::PacketPtr read_packet_();
    void set_frame_props_(Frame& frame, const FrameInfo& info);
    void report_stats_();

    packet::IReader& reader_;
    OpusDecoder& payload_decoder_;
    const SampleSpec sample_spec_;
    core::Array<sample_t, 256> decoded_buffer_;

    packet::PacketPtr next_packet_;

    packet::stream_timestamp_t stream_ts_;
    core::nanoseconds_t next_capture_ts_;
    bool valid_capture_ts_;

    packet::stream_timestamp_t zero_samples_;
    packet::stream_timestamp_t missing_samples_;
    packet::stream_timestamp_t packet_samples_;
    packet::stream_timestamp_t last_packet_duration_;

    size_t chunk_offset_;
    size_t chunk_size_;
    ChunkSource chunk_source_;

    core::RateLimiter rate_limiter_;
    const bool beep_;

    bool first_packet_;
    bool valid_;
};

} // namespace audio
} // namespace roc

#endif // ROC_AUDIO_OPUS_DEPACKETIZER_H_
