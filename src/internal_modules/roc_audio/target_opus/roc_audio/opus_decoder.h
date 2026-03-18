/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

//! @file roc_audio/target_opus/roc_audio/opus_decoder.h
//! @brief Opus decoder.

#ifndef ROC_AUDIO_OPUS_DECODER_H_
#define ROC_AUDIO_OPUS_DECODER_H_

#include "roc_audio/iframe_decoder.h"
#include "roc_audio/opus_config.h"
#include "roc_audio/sample_spec.h"
#include "roc_core/array.h"
#include "roc_core/iarena.h"
#include "roc_core/noncopyable.h"

struct OpusDecoder;

namespace roc {
namespace audio {

class OpusDecoder : public IFrameDecoder, public core::NonCopyable<> {
public:
    static IFrameDecoder* construct(core::IArena& arena, const SampleSpec& sample_spec);

    OpusDecoder(core::IArena& arena, const SampleSpec& sample_spec);
    ~OpusDecoder();

    bool is_valid() const;

    virtual packet::stream_timestamp_t position() const;
    virtual packet::stream_timestamp_t available() const;
    virtual size_t decoded_sample_count(const void* frame_data, size_t frame_size) const;
    virtual void begin(packet::stream_timestamp_t frame_position,
                       const void* frame_data,
                       size_t frame_size);
    virtual size_t read(sample_t* samples, size_t n_samples);
    virtual size_t shift(size_t n_samples);
    virtual void end();

    bool decode(const void* frame_data,
                size_t frame_size,
                sample_t* samples,
                size_t sample_capacity,
                size_t& decoded_samples);
    bool decode_fec(const void* frame_data,
                    size_t frame_size,
                    size_t n_samples,
                    sample_t* samples,
                    size_t sample_capacity,
                    size_t& decoded_samples);
    bool decode_missing(size_t n_samples,
                        sample_t* samples,
                        size_t sample_capacity,
                        size_t& decoded_samples);

private:
    bool is_supported_frame_size_(size_t n_samples) const;
    bool decode_imp_(const unsigned char* frame_data,
                     size_t frame_size,
                     sample_t* samples,
                     size_t sample_capacity,
                     size_t n_samples,
                     int decode_fec,
                     size_t& decoded_samples);

    core::Array<sample_t, 256> decode_buffer_;
    ::OpusDecoder* decoder_;
    const SampleSpec sample_spec_;
    const size_t n_chans_;

    packet::stream_timestamp_t stream_pos_;
    packet::stream_timestamp_t stream_avail_;
    size_t decode_offset_;

    bool active_;
    bool valid_;
};

} // namespace audio
} // namespace roc

#endif // ROC_AUDIO_OPUS_DECODER_H_
