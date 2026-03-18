/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

//! @file roc_audio/target_opus/roc_audio/opus_packetizer.h
//! @brief Opus packetizer.

#ifndef ROC_AUDIO_OPUS_PACKETIZER_H_
#define ROC_AUDIO_OPUS_PACKETIZER_H_

#include "roc_audio/opus_encoder.h"
#include "roc_audio/packetizer.h"
#include "roc_audio/sample_spec.h"
#include "roc_core/array.h"
#include "roc_core/iarena.h"
#include "roc_core/noncopyable.h"
#include "roc_packet/icomposer.h"
#include "roc_packet/isequencer.h"
#include "roc_packet/iwriter.h"
#include "roc_packet/packet_factory.h"

namespace roc {
namespace audio {

class OpusPacketizer : public IPacketizer, public core::NonCopyable<> {
public:
    OpusPacketizer(packet::IWriter& writer,
                   packet::IComposer& composer,
                   packet::ISequencer& sequencer,
                   OpusEncoder& payload_encoder,
                   packet::PacketFactory& packet_factory,
                   core::nanoseconds_t packet_length,
                   const SampleSpec& sample_spec,
                   core::IArena& arena);

    virtual bool is_valid() const;
    virtual size_t sample_rate() const;
    virtual const PacketizerMetrics& metrics() const;
    virtual void write(Frame& frame);
    virtual void flush();

private:
    bool emit_packet_();
    packet::PacketPtr create_packet_(size_t payload_size);

    packet::IWriter& writer_;
    packet::IComposer& composer_;
    packet::ISequencer& sequencer_;
    OpusEncoder& payload_encoder_;
    packet::PacketFactory& packet_factory_;

    core::Array<sample_t, 256> packet_buffer_;
    core::Array<uint8_t, OpusMaxPacketBytes> temp_payload_;

    const SampleSpec sample_spec_;
    const size_t samples_per_packet_;

    size_t packet_pos_;
    core::nanoseconds_t packet_cts_;
    core::nanoseconds_t capture_ts_;

    PacketizerMetrics metrics_;
    bool valid_;
};

} // namespace audio
} // namespace roc

#endif // ROC_AUDIO_OPUS_PACKETIZER_H_
