/*
 * Copyright (c) 2026 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_audio/opus_packetizer.h"
#include "roc_audio/sample_spec_to_str.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"

#include <algorithm>
#include <cstring>

namespace roc {
namespace audio {

OpusPacketizer::OpusPacketizer(packet::IWriter& writer,
                               packet::IComposer& composer,
                               packet::ISequencer& sequencer,
                               OpusEncoder& payload_encoder,
                               packet::PacketFactory& packet_factory,
                               core::nanoseconds_t packet_length,
                               const SampleSpec& sample_spec,
                               core::IArena& arena)
    : writer_(writer)
    , composer_(composer)
    , sequencer_(sequencer)
    , payload_encoder_(payload_encoder)
    , packet_factory_(packet_factory)
    , packet_buffer_(arena)
    , temp_payload_(arena)
    , sample_spec_(sample_spec)
    , samples_per_packet_(opus_packet_length_2_samples_per_chan(packet_length))
    , packet_pos_(0)
    , packet_cts_(0)
    , capture_ts_(0)
    , valid_(false) {
    roc_panic_if_msg(!sample_spec_.is_valid() || !sample_spec_.is_raw(),
                     "opus packetizer: required valid raw sample spec: %s",
                     sample_spec_to_str(sample_spec_).c_str());

    if (sample_spec_.sample_rate() != OpusSampleRate || !is_opus_packet_length(packet_length)) {
        roc_log(LogError,
                "opus packetizer: invalid config: packet_length=%.3fms sample_spec=%s",
                (double)packet_length / core::Millisecond,
                sample_spec_to_str(sample_spec_).c_str());
        return;
    }

    if (!packet_buffer_.resize(samples_per_packet_ * sample_spec_.num_channels())
        || !temp_payload_.resize(OpusMaxPacketBytes)) {
        roc_log(LogError, "opus packetizer: can't allocate buffers");
        return;
    }

    valid_ = true;
}

bool OpusPacketizer::is_valid() const {
    return valid_;
}

size_t OpusPacketizer::sample_rate() const {
    return sample_spec_.sample_rate();
}

const PacketizerMetrics& OpusPacketizer::metrics() const {
    return metrics_;
}

void OpusPacketizer::write(Frame& frame) {
    if (frame.num_raw_samples() % sample_spec_.num_channels() != 0) {
        roc_panic("opus packetizer: unexpected frame size");
    }

    const sample_t* buffer_ptr = frame.raw_samples();
    size_t buffer_samples = frame.num_raw_samples() / sample_spec_.num_channels();
    capture_ts_ = frame.capture_timestamp();

    while (buffer_samples != 0) {
        if (packet_pos_ == 0) {
            packet_cts_ = capture_ts_;
        }

        const size_t n_requested =
            std::min(buffer_samples, samples_per_packet_ - packet_pos_);

        memcpy(packet_buffer_.data() + packet_pos_ * sample_spec_.num_channels(), buffer_ptr,
               n_requested * sample_spec_.num_channels() * sizeof(sample_t));

        buffer_ptr += n_requested * sample_spec_.num_channels();
        buffer_samples -= n_requested;
        packet_pos_ += n_requested;

        if (capture_ts_) {
            capture_ts_ += sample_spec_.samples_per_chan_2_ns(n_requested);
        }

        if (packet_pos_ == samples_per_packet_) {
            if (!emit_packet_()) {
                return;
            }
        }
    }
}

void OpusPacketizer::flush() {
    if (packet_pos_ == 0) {
        return;
    }

    memset(packet_buffer_.data() + packet_pos_ * sample_spec_.num_channels(), 0,
           (samples_per_packet_ - packet_pos_) * sample_spec_.num_channels()
               * sizeof(sample_t));

    (void)emit_packet_();
}

bool OpusPacketizer::emit_packet_() {
    size_t payload_size = 0;
    if (!payload_encoder_.encode(packet_buffer_.data(), samples_per_packet_,
                                 temp_payload_.data(), temp_payload_.size(),
                                 payload_size)) {
        return false;
    }

    packet::PacketPtr packet = create_packet_(payload_size);
    if (!packet) {
        return false;
    }

    memcpy(packet->payload().data(), temp_payload_.data(), payload_size);

    sequencer_.next(*packet, packet_cts_, (packet::stream_timestamp_t)samples_per_packet_);

    const status::StatusCode code = writer_.write(packet);
    roc_panic_if(code != status::StatusOK);

    metrics_.packet_count++;
    metrics_.payload_count += payload_size;

    packet_pos_ = 0;
    packet_cts_ = 0;

    return true;
}

packet::PacketPtr OpusPacketizer::create_packet_(size_t payload_size) {
    packet::PacketPtr packet = packet_factory_.new_packet();
    if (!packet) {
        roc_log(LogError, "opus packetizer: can't allocate packet");
        return NULL;
    }

    packet->add_flags(packet::Packet::FlagAudio);

    core::Slice<uint8_t> buffer = packet_factory_.new_packet_buffer();
    if (!buffer) {
        roc_log(LogError, "opus packetizer: can't allocate buffer");
        return NULL;
    }

    if (!composer_.prepare(*packet, buffer, payload_size)) {
        roc_log(LogError, "opus packetizer: can't prepare packet");
        return NULL;
    }
    packet->add_flags(packet::Packet::FlagPrepared);
    packet->set_buffer(buffer);

    return packet;
}

} // namespace audio
} // namespace roc
