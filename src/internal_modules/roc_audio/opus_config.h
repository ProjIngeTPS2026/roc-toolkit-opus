/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

//! @file roc_audio/opus_config.h
//! @brief Opus helpers and config.

#ifndef ROC_AUDIO_OPUS_CONFIG_H_
#define ROC_AUDIO_OPUS_CONFIG_H_

#include "roc_audio/sample_spec.h"
#include "roc_core/time.h"

namespace roc {
namespace audio {

const size_t OpusSampleRate = 48000;
const size_t OpusMaxPacketBytes = 1276;
const size_t OpusMaxFrameSamplesPerChan = 2880;
const core::nanoseconds_t OpusDefaultPacketLength = 20 * core::Millisecond;

enum OpusApplication {
    OpusApplication_Default,
    OpusApplication_Audio,
    OpusApplication_Voip,
    OpusApplication_LowDelay
};

enum OpusVbrMode {
    OpusVbr_Default,
    OpusVbr_Off,
    OpusVbr_On,
    OpusVbr_Constrained
};

struct OpusConfig {
    int bitrate;
    unsigned complexity;
    OpusApplication application;
    OpusVbrMode vbr;
    bool enable_inband_fec;
    bool enable_dtx;
    unsigned expected_loss_percent;

    OpusConfig();

    void deduce_defaults(size_t num_channels);
    bool is_valid(size_t num_channels) const;
};

bool make_opus_sample_spec(size_t num_channels, SampleSpec& sample_spec);
bool is_opus(const SampleSpec& sample_spec);
bool is_opus_packet_length(core::nanoseconds_t packet_length);
size_t opus_packet_length_2_samples_per_chan(core::nanoseconds_t packet_length);

} // namespace audio
} // namespace roc

#endif // ROC_AUDIO_OPUS_CONFIG_H_
