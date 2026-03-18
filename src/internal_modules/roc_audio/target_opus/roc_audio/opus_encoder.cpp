/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/opus_encoder.h"
#include "roc_audio/sample_spec_to_str.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"

#include <algorithm>
#include <cstring>
#include <opus/opus.h>

namespace roc {
namespace audio {

namespace {

int opus_application(OpusApplication application) {
    switch (application) {
    case OpusApplication_Audio:
        return OPUS_APPLICATION_AUDIO;
    case OpusApplication_Voip:
        return OPUS_APPLICATION_VOIP;
    case OpusApplication_LowDelay:
        return OPUS_APPLICATION_RESTRICTED_LOWDELAY;
    case OpusApplication_Default:
        break;
    }

    return OPUS_APPLICATION_AUDIO;
}

bool supported_frame_size(size_t n_samples) {
    return n_samples == 120 || n_samples == 240 || n_samples == 480 || n_samples == 960
        || n_samples == 1920 || n_samples == 2880;
}

} // namespace

IFrameEncoder* OpusEncoder::construct(core::IArena& arena, const SampleSpec& sample_spec) {
    return new (arena) OpusEncoder(arena, sample_spec, OpusConfig());
}

OpusEncoder::OpusEncoder(core::IArena& arena,
                         const SampleSpec& sample_spec,
                         const OpusConfig& config)
    : input_buffer_(arena)
    , encoder_(NULL)
    , sample_spec_(sample_spec)
    , config_(config)
    , n_chans_(sample_spec.num_channels())
    , frame_data_(NULL)
    , frame_size_(0)
    , input_pos_(0)
    , valid_(false) {
    if (!is_opus(sample_spec_) || sample_spec_.sample_rate() != OpusSampleRate) {
        roc_log(LogError, "opus encoder: invalid sample spec: %s",
                sample_spec_to_str(sample_spec_).c_str());
        return;
    }

    if (!(n_chans_ == 1 || n_chans_ == 2)) {
        roc_log(LogError, "opus encoder: unsupported channel count: %lu",
                (unsigned long)n_chans_);
        return;
    }

    config_.deduce_defaults(n_chans_);
    if (!config_.is_valid(n_chans_)) {
        roc_log(LogError, "opus encoder: invalid config");
        return;
    }

    if (!input_buffer_.resize(OpusMaxFrameSamplesPerChan * n_chans_)) {
        roc_log(LogError, "opus encoder: can't allocate input buffer");
        return;
    }

    int err = OPUS_OK;
    encoder_ = opus_encoder_create((opus_int32)sample_spec_.sample_rate(), (int)n_chans_,
                                   opus_application(config_.application), &err);
    if (!encoder_ || err != OPUS_OK) {
        roc_log(LogError, "opus encoder: can't create encoder: err=%d", err);
        encoder_ = NULL;
        return;
    }

    if (!apply_config_()) {
        opus_encoder_destroy(encoder_);
        encoder_ = NULL;
        return;
    }

    valid_ = true;
}

OpusEncoder::~OpusEncoder() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
    }
}

bool OpusEncoder::is_valid() const {
    return valid_;
}

size_t OpusEncoder::encoded_byte_count(size_t) const {
    return OpusMaxPacketBytes;
}

void OpusEncoder::begin(void* frame_data, size_t frame_size) {
    roc_panic_if_not(frame_data);
    roc_panic_if_not(valid_);

    if (frame_data_) {
        roc_panic("opus encoder: unpaired begin/end");
    }

    frame_data_ = frame_data;
    frame_size_ = frame_size;
    input_pos_ = 0;
}

size_t OpusEncoder::write(const sample_t* samples, size_t n_samples) {
    roc_panic_if_not(valid_);

    if (!frame_data_) {
        roc_panic("opus encoder: write should be called only between begin/end");
    }

    const size_t capacity = input_buffer_.size() / n_chans_;
    const size_t avail = capacity > input_pos_ ? capacity - input_pos_ : 0;
    const size_t to_copy = std::min(n_samples, avail);

    if (to_copy != 0) {
        memcpy(input_buffer_.data() + input_pos_ * n_chans_, samples,
               to_copy * n_chans_ * sizeof(sample_t));
        input_pos_ += to_copy;
    }

    return to_copy;
}

void OpusEncoder::end() {
    if (!frame_data_) {
        roc_panic("opus encoder: unpaired begin/end");
    }

    size_t encoded_size = 0;
    if (!encode(input_buffer_.data(), input_pos_, frame_data_, frame_size_, encoded_size)) {
        roc_panic("opus encoder: can't encode frame");
    }

    frame_data_ = NULL;
    frame_size_ = 0;
    input_pos_ = 0;
}

bool OpusEncoder::encode(const sample_t* samples,
                         size_t n_samples,
                         void* frame_data,
                         size_t frame_size,
                         size_t& encoded_size) {
    encoded_size = 0;

    if (!valid_ || !samples || !frame_data) {
        return false;
    }

    if (!is_supported_frame_size_(n_samples)) {
        roc_log(LogError, "opus encoder: unsupported frame size: %lu",
                (unsigned long)n_samples);
        return false;
    }

    const int ret = opus_encode_float(encoder_, samples, (int)n_samples,
                                      (unsigned char*)frame_data, (opus_int32)frame_size);
    if (ret < 0) {
        roc_log(LogError, "opus encoder: encoding failed: err=%d", ret);
        return false;
    }

    encoded_size = (size_t)ret;
    return true;
}

bool OpusEncoder::apply_config_() {
    if (opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrate)) != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY((int)config_.complexity))
            != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(config_.enable_inband_fec ? 1 : 0))
            != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_DTX(config_.enable_dtx ? 1 : 0))
            != OPUS_OK
        || opus_encoder_ctl(encoder_,
                            OPUS_SET_PACKET_LOSS_PERC((int)config_.expected_loss_percent))
            != OPUS_OK) {
        roc_log(LogError, "opus encoder: failed to apply configuration");
        return false;
    }

    switch (config_.vbr) {
    case OpusVbr_On:
        if (opus_encoder_ctl(encoder_, OPUS_SET_VBR(1)) != OPUS_OK
            || opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(0)) != OPUS_OK) {
            roc_log(LogError, "opus encoder: failed to enable VBR");
            return false;
        }
        break;

    case OpusVbr_Constrained:
        if (opus_encoder_ctl(encoder_, OPUS_SET_VBR(1)) != OPUS_OK
            || opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(1)) != OPUS_OK) {
            roc_log(LogError, "opus encoder: failed to enable constrained VBR");
            return false;
        }
        break;

    case OpusVbr_Off:
        if (opus_encoder_ctl(encoder_, OPUS_SET_VBR(0)) != OPUS_OK) {
            roc_log(LogError, "opus encoder: failed to disable VBR");
            return false;
        }
        break;

    case OpusVbr_Default:
        break;
    }

    return true;
}

bool OpusEncoder::is_supported_frame_size_(size_t n_samples) const {
    return supported_frame_size(n_samples);
}

} // namespace audio
} // namespace roc
