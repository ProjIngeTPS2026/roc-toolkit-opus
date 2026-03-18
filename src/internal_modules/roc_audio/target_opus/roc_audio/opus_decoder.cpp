/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/opus_decoder.h"
#include "roc_audio/sample_spec_to_str.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"

#include <algorithm>
#include <cstring>
#include <opus/opus.h>

namespace roc {
namespace audio {

namespace {

bool supported_frame_size(size_t n_samples) {
    return n_samples == 120 || n_samples == 240 || n_samples == 480 || n_samples == 960
        || n_samples == 1920 || n_samples == 2880;
}

} // namespace

IFrameDecoder* OpusDecoder::construct(core::IArena& arena, const SampleSpec& sample_spec) {
    return new (arena) OpusDecoder(arena, sample_spec);
}

OpusDecoder::OpusDecoder(core::IArena& arena, const SampleSpec& sample_spec)
    : decode_buffer_(arena)
    , decoder_(NULL)
    , sample_spec_(sample_spec)
    , n_chans_(sample_spec.num_channels())
    , stream_pos_(0)
    , stream_avail_(0)
    , decode_offset_(0)
    , active_(false)
    , valid_(false) {
    if (!is_opus(sample_spec_) || sample_spec_.sample_rate() != OpusSampleRate) {
        roc_log(LogError, "opus decoder: invalid sample spec: %s",
                sample_spec_to_str(sample_spec_).c_str());
        return;
    }

    if (!(n_chans_ == 1 || n_chans_ == 2)) {
        roc_log(LogError, "opus decoder: unsupported channel count: %lu",
                (unsigned long)n_chans_);
        return;
    }

    if (!decode_buffer_.resize(OpusMaxFrameSamplesPerChan * n_chans_)) {
        roc_log(LogError, "opus decoder: can't allocate decode buffer");
        return;
    }

    int err = OPUS_OK;
    decoder_ =
        opus_decoder_create((opus_int32)sample_spec_.sample_rate(), (int)n_chans_, &err);
    if (!decoder_ || err != OPUS_OK) {
        roc_log(LogError, "opus decoder: can't create decoder: err=%d", err);
        decoder_ = NULL;
        return;
    }

    valid_ = true;
}

OpusDecoder::~OpusDecoder() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
    }
}

bool OpusDecoder::is_valid() const {
    return valid_;
}

packet::stream_timestamp_t OpusDecoder::position() const {
    return stream_pos_;
}

packet::stream_timestamp_t OpusDecoder::available() const {
    return stream_avail_;
}

size_t OpusDecoder::decoded_sample_count(const void* frame_data, size_t frame_size) const {
    roc_panic_if_not(frame_data);

    const int ret = opus_packet_get_nb_samples((const unsigned char*)frame_data,
                                               (opus_int32)frame_size,
                                               (opus_int32)sample_spec_.sample_rate());
    return ret > 0 ? (size_t)ret : 0;
}

void OpusDecoder::begin(packet::stream_timestamp_t frame_position,
                        const void* frame_data,
                        size_t frame_size) {
    roc_panic_if_not(frame_data);
    roc_panic_if_not(valid_);

    if (active_) {
        roc_panic("opus decoder: unpaired begin/end");
    }

    size_t decoded_samples = 0;
    if (!decode(frame_data, frame_size, decode_buffer_.data(), decode_buffer_.size(),
                decoded_samples)) {
        roc_panic("opus decoder: can't decode packet");
    }

    stream_pos_ = frame_position;
    stream_avail_ = (packet::stream_timestamp_t)decoded_samples;
    decode_offset_ = 0;
    active_ = true;
}

size_t OpusDecoder::read(sample_t* samples, size_t n_samples) {
    if (!active_) {
        roc_panic("opus decoder: read should be called only between begin/end");
    }

    const size_t to_read = std::min(n_samples, (size_t)stream_avail_);
    if (to_read != 0) {
        memcpy(samples, decode_buffer_.data() + decode_offset_ * n_chans_,
               to_read * n_chans_ * sizeof(sample_t));
        decode_offset_ += to_read;
        stream_pos_ += (packet::stream_timestamp_t)to_read;
        stream_avail_ -= (packet::stream_timestamp_t)to_read;
    }

    return to_read;
}

size_t OpusDecoder::shift(size_t n_samples) {
    if (!active_) {
        roc_panic("opus decoder: shift should be called only between begin/end");
    }

    const size_t to_shift = std::min(n_samples, (size_t)stream_avail_);
    decode_offset_ += to_shift;
    stream_pos_ += (packet::stream_timestamp_t)to_shift;
    stream_avail_ -= (packet::stream_timestamp_t)to_shift;
    return to_shift;
}

void OpusDecoder::end() {
    if (!active_) {
        roc_panic("opus decoder: unpaired begin/end");
    }

    stream_avail_ = 0;
    decode_offset_ = 0;
    active_ = false;
}

bool OpusDecoder::decode(const void* frame_data,
                         size_t frame_size,
                         sample_t* samples,
                         size_t sample_capacity,
                         size_t& decoded_samples) {
    return decode_imp_((const unsigned char*)frame_data, frame_size, samples,
                       sample_capacity, 0, 0, decoded_samples);
}

bool OpusDecoder::decode_fec(const void* frame_data,
                             size_t frame_size,
                             size_t n_samples,
                             sample_t* samples,
                             size_t sample_capacity,
                             size_t& decoded_samples) {
    return decode_imp_((const unsigned char*)frame_data, frame_size, samples,
                       sample_capacity, n_samples, 1, decoded_samples);
}

bool OpusDecoder::decode_missing(size_t n_samples,
                                 sample_t* samples,
                                 size_t sample_capacity,
                                 size_t& decoded_samples) {
    return decode_imp_(NULL, 0, samples, sample_capacity, n_samples, 0, decoded_samples);
}

bool OpusDecoder::is_supported_frame_size_(size_t n_samples) const {
    return supported_frame_size(n_samples);
}

bool OpusDecoder::decode_imp_(const unsigned char* frame_data,
                              size_t frame_size,
                              sample_t* samples,
                              size_t sample_capacity,
                              size_t n_samples,
                              int decode_fec,
                              size_t& decoded_samples) {
    decoded_samples = 0;

    if (!valid_ || !samples) {
        return false;
    }

    size_t requested_samples = n_samples;
    if (requested_samples == 0) {
        if (!frame_data) {
            return false;
        }
        requested_samples = decoded_sample_count(frame_data, frame_size);
    }

    if (!is_supported_frame_size_(requested_samples)) {
        roc_log(LogError, "opus decoder: unsupported frame size: %lu",
                (unsigned long)requested_samples);
        return false;
    }

    if (sample_capacity < requested_samples * n_chans_) {
        roc_log(LogError, "opus decoder: output buffer too small");
        return false;
    }

    const int ret =
        opus_decode_float(decoder_, frame_data, (opus_int32)frame_size, samples,
                          (int)requested_samples, decode_fec);
    if (ret < 0) {
        roc_log(LogError, "opus decoder: decoding failed: err=%d", ret);
        return false;
    }

    decoded_samples = (size_t)ret;
    return true;
}

} // namespace audio
} // namespace roc
