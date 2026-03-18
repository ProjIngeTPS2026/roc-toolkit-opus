/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

//! @file roc_audio/target_opus/roc_audio/opus_encoder.h
//! @brief Opus encoder.

#ifndef ROC_AUDIO_OPUS_ENCODER_H_
#define ROC_AUDIO_OPUS_ENCODER_H_

#include "roc_audio/iframe_encoder.h"
#include "roc_audio/opus_config.h"
#include "roc_audio/sample_spec.h"
#include "roc_core/array.h"
#include "roc_core/iarena.h"
#include "roc_core/noncopyable.h"

struct OpusEncoder;

namespace roc {
namespace audio {

class OpusEncoder : public IFrameEncoder, public core::NonCopyable<> {
public:
    static IFrameEncoder* construct(core::IArena& arena, const SampleSpec& sample_spec);

    OpusEncoder(core::IArena& arena,
                const SampleSpec& sample_spec,
                const OpusConfig& config);
    ~OpusEncoder();

    bool is_valid() const;

    virtual size_t encoded_byte_count(size_t num_samples) const;
    virtual void begin(void* frame_data, size_t frame_size);
    virtual size_t write(const sample_t* samples, size_t n_samples);
    virtual void end();

    bool encode(const sample_t* samples,
                size_t n_samples,
                void* frame_data,
                size_t frame_size,
                size_t& encoded_size);

private:
    bool apply_config_();
    bool is_supported_frame_size_(size_t n_samples) const;

    core::Array<sample_t, 256> input_buffer_;
    ::OpusEncoder* encoder_;
    const SampleSpec sample_spec_;
    OpusConfig config_;
    const size_t n_chans_;

    void* frame_data_;
    size_t frame_size_;
    size_t input_pos_;

    bool valid_;
};

} // namespace audio
} // namespace roc

#endif // ROC_AUDIO_OPUS_ENCODER_H_
