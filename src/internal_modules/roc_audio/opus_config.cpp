/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/opus_config.h"
#include "roc_audio/channel_defs.h"

namespace roc {
namespace audio {

OpusConfig::OpusConfig()
    : bitrate(0)
    , complexity(0)
    , application(OpusApplication_Default)
    , vbr(OpusVbr_Default)
    , enable_inband_fec(false)
    , enable_dtx(false)
    , expected_loss_percent(0) {
}

void OpusConfig::deduce_defaults(size_t num_channels) {
    if (bitrate == 0) {
        bitrate = num_channels > 1 ? 64000 : 32000;
    }
    if (complexity == 0) {
        complexity = 8;
    }
    if (application == OpusApplication_Default) {
        application = OpusApplication_Audio;
    }
    if (vbr == OpusVbr_Default) {
        vbr = OpusVbr_Constrained;
    }
}

bool OpusConfig::is_valid(size_t num_channels) const {
    if (!(num_channels == 1 || num_channels == 2)) {
        return false;
    }
    if (bitrate <= 0) {
        return false;
    }
    if (complexity > 10) {
        return false;
    }
    if (application == OpusApplication_Default || application > OpusApplication_LowDelay) {
        return false;
    }
    if (vbr == OpusVbr_Default || vbr > OpusVbr_Constrained) {
        return false;
    }
    if (expected_loss_percent > 100) {
        return false;
    }
    return true;
}

bool make_opus_sample_spec(size_t num_channels, SampleSpec& sample_spec) {
    sample_spec.clear();

    sample_spec.set_sample_format(SampleFormat_Opus);
    sample_spec.set_sample_rate(OpusSampleRate);

    if (num_channels == 1) {
        sample_spec.channel_set().set_layout(ChanLayout_Surround);
        sample_spec.channel_set().set_order(ChanOrder_Smpte);
        sample_spec.channel_set().set_mask(ChanMask_Surround_Mono);
        return true;
    }

    if (num_channels == 2) {
        sample_spec.channel_set().set_layout(ChanLayout_Surround);
        sample_spec.channel_set().set_order(ChanOrder_Smpte);
        sample_spec.channel_set().set_mask(ChanMask_Surround_Stereo);
        return true;
    }

    sample_spec.clear();
    return false;
}

bool is_opus(const SampleSpec& sample_spec) {
    return sample_spec.sample_format() == SampleFormat_Opus;
}

bool is_opus_packet_length(core::nanoseconds_t packet_length) {
    return packet_length == core::Second / 400
        || packet_length == core::Second / 200
        || packet_length == core::Second / 100
        || packet_length == core::Second / 50
        || packet_length == core::Second / 25
        || packet_length == core::Second * 3 / 50;
}

size_t opus_packet_length_2_samples_per_chan(core::nanoseconds_t packet_length) {
    return SampleSpec(OpusSampleRate, Sample_RawFormat, ChanLayout_Surround,
                      ChanOrder_Smpte, ChanMask_Surround_Mono)
        .ns_2_samples_per_chan(packet_length);
}

} // namespace audio
} // namespace roc
