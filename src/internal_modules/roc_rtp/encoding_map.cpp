/*
 * Copyright (c) 2017 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_rtp/encoding_map.h"
#include "roc_audio/opus_config.h"
#include "roc_audio/pcm_decoder.h"
#include "roc_audio/pcm_encoder.h"
#include "roc_audio/sample_format.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#ifdef ROC_TARGET_OPUS
#include "roc_audio/opus_decoder.h"
#include "roc_audio/opus_encoder.h"
#endif

namespace roc {
namespace rtp {

EncodingMap::EncodingMap(core::IArena& arena)
    : node_pool_("encoding_pool", arena)
    , node_map_(arena) {
    if (!add_builtin_encoding(PayloadType_L16_Mono)
        || !add_builtin_encoding(PayloadType_L16_Stereo)) {
        roc_panic("encoding map: can't add builtin encoding");
    }
}

const Encoding* EncodingMap::find_by_pt(unsigned int pt) const {
    core::Mutex::Lock lock(mutex_);

    if (core::SharedPtr<Node> node = node_map_.find(pt)) {
        return &node->encoding;
    }

    return NULL;
}

const Encoding* EncodingMap::find_by_spec(const audio::SampleSpec& spec) const {
    core::Mutex::Lock lock(mutex_);

    for (core::SharedPtr<Node> node = node_map_.front(); node != NULL;
         node = node_map_.nextof(*node)) {
        if (node->encoding.sample_spec == spec) {
            return &node->encoding;
        }
    }

    return NULL;
}

bool EncodingMap::add_encoding(Encoding enc) {
    core::Mutex::Lock lock(mutex_);

    return add_encoding_(enc);
}

bool EncodingMap::add_builtin_encoding(unsigned int payload_type) {
    Encoding enc;
    if (!make_builtin_(payload_type, enc)) {
        roc_log(LogError,
                "encoding map: failed to register builtin encoding: unknown payload type %u",
                payload_type);
        return false;
    }

#ifndef ROC_TARGET_OPUS
    if (payload_type == PayloadType_Opus_Mono || payload_type == PayloadType_Opus_Stereo) {
        roc_log(LogError,
                "encoding map: failed to register builtin encoding:"
                " Opus support is disabled at build time");
        return false;
    }
#endif

    core::Mutex::Lock lock(mutex_);

    if (core::SharedPtr<Node> node = node_map_.find(payload_type)) {
        if (node->encoding.sample_spec == enc.sample_spec
            && node->encoding.packet_flags == enc.packet_flags) {
            return true;
        }

        roc_log(LogError,
                "encoding map: failed to register builtin encoding:"
                " payload type %u already exists with different parameters",
                payload_type);
        return false;
    }

    return add_encoding_(enc);
}

bool EncodingMap::add_encoding_(const Encoding& src_enc) {
    Encoding enc = src_enc;

    if (enc.payload_type == 0) {
        roc_panic("encoding map: bad encoding: invalid payload type");
    }

    if (!enc.sample_spec.is_valid()) {
        roc_panic("encoding map: bad encoding: invalid sample spec");
    }

    find_codecs_(enc);

    if (!enc.new_encoder || !enc.new_decoder) {
        roc_panic("encoding map: bad encoding: invalid codec functions");
    }

    if (node_map_.find(enc.payload_type)) {
        roc_log(
            LogError,
            "encoding map: failed to register encoding: payload type %u already exists",
            enc.payload_type);
        return false;
    }

    core::SharedPtr<Node> node = new (node_pool_) Node(node_pool_, enc);

    if (!node) {
        roc_log(LogError,
                "encoding map: failed to register encoding: pool allocation failed");
        return false;
    }

    if (!node_map_.insert(*node)) {
        roc_log(LogError,
                "encoding map: failed to register encoding: hashmap allocation failed");
        return false;
    }

    return true;
}

void EncodingMap::add_builtin_(const Encoding& enc) {
    if (!add_encoding_(enc)) {
        roc_panic("encoding map: can't add builtin encoding");
    }
}

bool EncodingMap::make_builtin_(unsigned int payload_type, Encoding& enc) const {
    enc = Encoding();
    enc.payload_type = payload_type;
    enc.packet_flags = packet::Packet::FlagAudio;

    switch (payload_type) {
    case PayloadType_L16_Mono:
        enc.sample_spec = audio::SampleSpec(
            44100, audio::PcmFormat_SInt16_Be, audio::ChanLayout_Surround,
            audio::ChanOrder_Smpte, audio::ChanMask_Surround_Mono);
        return true;

    case PayloadType_L16_Stereo:
        enc.sample_spec = audio::SampleSpec(
            44100, audio::PcmFormat_SInt16_Be, audio::ChanLayout_Surround,
            audio::ChanOrder_Smpte, audio::ChanMask_Surround_Stereo);
        return true;

    case PayloadType_Opus_Mono:
        return audio::make_opus_sample_spec(1, enc.sample_spec);

    case PayloadType_Opus_Stereo:
        return audio::make_opus_sample_spec(2, enc.sample_spec);
    }

    return false;
}

void EncodingMap::find_codecs_(Encoding& enc) {
    if (enc.new_encoder && enc.new_decoder) {
        return;
    }

    switch (enc.sample_spec.sample_format()) {
    case audio::SampleFormat_Pcm:
        if (!enc.new_encoder) {
            enc.new_encoder = &audio::PcmEncoder::construct;
        }
        if (!enc.new_decoder) {
            enc.new_decoder = &audio::PcmDecoder::construct;
        }
        break;

    case audio::SampleFormat_Opus:
#ifdef ROC_TARGET_OPUS
        if (!enc.new_encoder) {
            enc.new_encoder = &audio::OpusEncoder::construct;
        }
        if (!enc.new_decoder) {
            enc.new_decoder = &audio::OpusDecoder::construct;
        }
#endif
        break;

    case audio::SampleFormat_Invalid:
        break;
    }
}

} // namespace rtp
} // namespace roc
