/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <CppUTest/TestHarness.h>

#include <cmath>

#include "roc_audio/opus_config.h"
#include "roc_audio/opus_decoder.h"
#include "roc_audio/opus_encoder.h"
#include "roc_core/heap_arena.h"

namespace roc {
namespace audio {

namespace {

enum {
    FrameSamples = 960,
};

core::HeapArena arena;

SampleSpec make_raw_spec(size_t num_channels) {
    return SampleSpec(OpusSampleRate, Sample_RawFormat, ChanLayout_Surround,
                      ChanOrder_Smpte, num_channels == 1 ? ChanMask_Surround_Mono
                                                          : ChanMask_Surround_Stereo);
}

SampleSpec make_packet_spec(size_t num_channels) {
    SampleSpec sample_spec;
    CHECK(make_opus_sample_spec(num_channels, sample_spec));
    return sample_spec;
}

void fill_samples(sample_t* samples, size_t num_channels) {
    const double Pi = 3.14159265358979323846;

    for (size_t n = 0; n < FrameSamples; n++) {
        const sample_t value =
            (sample_t)std::sin(2.0 * Pi * 440.0 * n / OpusSampleRate);
        for (size_t c = 0; c < num_channels; c++) {
            samples[n * num_channels + c] = value;
        }
    }
}

void check_codec_smoke(size_t num_channels) {
    const SampleSpec raw_spec = make_raw_spec(num_channels);
    const SampleSpec packet_spec = make_packet_spec(num_channels);

    OpusConfig config;

    OpusEncoder encoder(arena, packet_spec, config);
    CHECK(encoder.is_valid());

    OpusDecoder decoder(arena, packet_spec);
    CHECK(decoder.is_valid());

    sample_t input[FrameSamples * 2] = {};
    sample_t output[FrameSamples * 2] = {};
    uint8_t packet[OpusMaxPacketBytes] = {};

    fill_samples(input, num_channels);

    size_t encoded_size = 0;
    CHECK(encoder.encode(input, FrameSamples, packet, sizeof(packet), encoded_size));
    CHECK(encoded_size > 0);
    CHECK(encoded_size <= sizeof(packet));

    size_t decoded_samples = 0;
    CHECK(decoder.decode(packet, encoded_size, output, FrameSamples * num_channels,
                         decoded_samples));
    CHECK_EQUAL((size_t)FrameSamples, decoded_samples);

    double energy = 0;
    for (size_t n = 0; n < decoded_samples * num_channels; n++) {
        energy += std::fabs((double)output[n]);
    }
    CHECK(energy > 1.0);

    CHECK(decoder.decode_missing(FrameSamples, output, FrameSamples * num_channels,
                                 decoded_samples));
    CHECK_EQUAL((size_t)FrameSamples, decoded_samples);

    LONGS_EQUAL((long)OpusSampleRate, (long)raw_spec.sample_rate());
    LONGS_EQUAL((long)OpusSampleRate, (long)packet_spec.sample_rate());
}

} // namespace

TEST_GROUP(opus_encoder_decoder) {};

TEST(opus_encoder_decoder, smoke_mono) {
    check_codec_smoke(1);
}

TEST(opus_encoder_decoder, smoke_stereo) {
    check_codec_smoke(2);
}

} // namespace audio
} // namespace roc
