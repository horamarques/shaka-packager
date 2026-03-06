// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp2t/ts_section_scte35.h>

#include <absl/log/log.h>

#include <packager/media/base/bit_reader.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/formats/mp2t/mp2t_common.h>

namespace shaka {
namespace media {
namespace mp2t {

namespace {

// SCTE-35 table_id (splice_info_section).
const int kScte35TableId = 0xFC;

// SCTE-35 splice command types.
const int kSpliceNull = 0x00;
const int kSpliceInsert = 0x05;
const int kTimeSignal = 0x06;

// Segmentation descriptor tag and identifier.
const int kSegmentationDescriptorTag = 0x02;
const uint32_t kCueiIdentifier = 0x43554549;  // "CUEI"

// PTS clock frequency.
const double kPtsTimescale = 90000.0;

// Convert a 33-bit PTS value with pts_adjustment to seconds.
double PtsToSeconds(uint64_t pts, uint64_t pts_adjustment) {
  // Per SCTE-35 section 9.6: splice time = pts_time + pts_adjustment (mod 2^33)
  uint64_t adjusted_pts = (pts + pts_adjustment) & 0x1FFFFFFFFULL;
  return static_cast<double>(adjusted_pts) / kPtsTimescale;
}

}  // namespace

TsSectionScte35::TsSectionScte35(const Scte35EventCB& scte35_event_cb)
    : scte35_event_cb_(scte35_event_cb) {}

TsSectionScte35::~TsSectionScte35() {}

bool TsSectionScte35::ParsePsiSection(BitReader* bit_reader) {
  // Capture the raw section data for pass-through before we start parsing.
  const size_t section_size = bit_reader->bits_available() / 8;
  const uint8_t* section_data = bit_reader->current_byte_ptr();

  // Parse splice_info_section() header per SCTE-35 Table 5.
  int table_id;
  RCHECK(bit_reader->ReadBits(8, &table_id));
  RCHECK(table_id == kScte35TableId);

  int section_syntax_indicator;
  int private_indicator;
  int reserved;
  int section_length;
  RCHECK(bit_reader->ReadBits(1, &section_syntax_indicator));
  RCHECK(bit_reader->ReadBits(1, &private_indicator));
  RCHECK(bit_reader->ReadBits(2, &reserved));
  RCHECK(bit_reader->ReadBits(12, &section_length));
  RCHECK(section_length <= 4093);

  // section_syntax_indicator should be 0 for SCTE-35.
  // Don't fail on this — some encoders may set it incorrectly.
  if (section_syntax_indicator != 0) {
    DVLOG(1) << "SCTE-35: unexpected section_syntax_indicator=1";
  }

  int protocol_version;
  RCHECK(bit_reader->ReadBits(8, &protocol_version));

  int encrypted_packet;
  int encryption_algorithm;
  RCHECK(bit_reader->ReadBits(1, &encrypted_packet));
  RCHECK(bit_reader->ReadBits(6, &encryption_algorithm));

  // Skip encrypted sections — we cannot parse them.
  if (encrypted_packet) {
    DVLOG(1) << "Skipping encrypted SCTE-35 section";
    return true;
  }

  uint64_t pts_adjustment;
  RCHECK(bit_reader->ReadBits(33, &pts_adjustment));

  int cw_index;
  RCHECK(bit_reader->ReadBits(8, &cw_index));

  int tier;
  RCHECK(bit_reader->ReadBits(12, &tier));

  int splice_command_length;
  RCHECK(bit_reader->ReadBits(12, &splice_command_length));

  int splice_command_type;
  RCHECK(bit_reader->ReadBits(8, &splice_command_type));

  Scte35Event event;
  bool emit_event = false;

  switch (splice_command_type) {
    case kSpliceNull:
      // Heartbeat — silently ignore.
      DVLOG(3) << "SCTE-35: splice_null (heartbeat)";
      return true;

    case kSpliceInsert:
      RCHECK(ParseSpliceInsert(bit_reader, pts_adjustment, &event));
      emit_event = true;
      break;

    case kTimeSignal:
      RCHECK(ParseTimeSignal(bit_reader, pts_adjustment, &event));
      emit_event = true;
      break;

    default:
      DVLOG(1) << "SCTE-35: ignoring command type 0x" << std::hex
               << splice_command_type;
      return true;
  }

  if (emit_event) {
    // Parse splice descriptors (after the splice command).
    int descriptor_loop_length;
    RCHECK(bit_reader->ReadBits(16, &descriptor_loop_length));

    if (descriptor_loop_length > 0) {
      RCHECK(ParseSpliceDescriptors(bit_reader, descriptor_loop_length,
                                     &event));
    }

    // Store raw section data for downstream pass-through (e.g., base64 for
    // HLS EXT-X-DATERANGE SCTE35-CMD attribute).
    event.cue_data.assign(reinterpret_cast<const char*>(section_data),
                          section_size);

    DVLOG(1) << "SCTE-35 event: id=" << event.id
             << " type=" << event.type
             << " time=" << event.start_time_in_seconds << "s"
             << " duration=" << event.duration_in_seconds << "s";

    scte35_event_cb_(std::make_shared<Scte35Event>(std::move(event)));
  }

  return true;
}

void TsSectionScte35::ResetPsiSection() {}

bool TsSectionScte35::ParseSpliceInsert(BitReader* bit_reader,
                                        uint64_t pts_adjustment,
                                        Scte35Event* event) {
  // Parse splice_insert() per SCTE-35 Table 9.
  uint32_t splice_event_id;
  RCHECK(bit_reader->ReadBits(32, &splice_event_id));

  event->id = std::to_string(splice_event_id);

  int splice_event_cancel_indicator;
  RCHECK(bit_reader->ReadBits(1, &splice_event_cancel_indicator));
  RCHECK(bit_reader->SkipBits(7));  // reserved

  if (splice_event_cancel_indicator) {
    DVLOG(2) << "SCTE-35: splice_insert cancel for event " << splice_event_id;
    return true;
  }

  int out_of_network_indicator;
  int program_splice_flag;
  int duration_flag;
  int splice_immediate_flag;
  RCHECK(bit_reader->ReadBits(1, &out_of_network_indicator));
  RCHECK(bit_reader->ReadBits(1, &program_splice_flag));
  RCHECK(bit_reader->ReadBits(1, &duration_flag));
  RCHECK(bit_reader->ReadBits(1, &splice_immediate_flag));
  RCHECK(bit_reader->SkipBits(4));  // reserved

  if (program_splice_flag && !splice_immediate_flag) {
    // Program-level splice with a specified time.
    double splice_time_seconds = 0;
    RCHECK(ParseSpliceTime(bit_reader, pts_adjustment, &splice_time_seconds));
    event->start_time_in_seconds = splice_time_seconds;
  }

  if (!program_splice_flag) {
    // Component-level splice — read per-component times.
    int component_count;
    RCHECK(bit_reader->ReadBits(8, &component_count));
    for (int i = 0; i < component_count; i++) {
      RCHECK(bit_reader->SkipBits(8));  // component_tag
      if (!splice_immediate_flag) {
        double unused_time = 0;
        RCHECK(ParseSpliceTime(bit_reader, pts_adjustment, &unused_time));
      }
    }
    // Use the first component's time if program_splice not set.
    // For simplicity, we use the overall PTS which is 0 for component splice.
  }

  if (duration_flag) {
    // Parse break_duration().
    int auto_return;
    RCHECK(bit_reader->ReadBits(1, &auto_return));
    RCHECK(bit_reader->SkipBits(6));  // reserved
    uint64_t duration;
    RCHECK(bit_reader->ReadBits(33, &duration));
    event->duration_in_seconds =
        static_cast<double>(duration) / kPtsTimescale;
  }

  int unique_program_id;
  int avail_num;
  int avails_expected;
  RCHECK(bit_reader->ReadBits(16, &unique_program_id));
  RCHECK(bit_reader->ReadBits(8, &avail_num));
  RCHECK(bit_reader->ReadBits(8, &avails_expected));

  // Map out_of_network_indicator to a cue type marker.
  // out_of_network=1 → cue-out (ad start), out_of_network=0 → cue-in (ad end)
  // We encode this in the type field: positive for out, negative for in.
  // Issue C (Scte35→CueEvent converter) will use the raw cue_data to get
  // the actual out_of_network value, but we store a hint here.
  event->type = out_of_network_indicator ? 0x34 : 0x35;
  // 0x34 = Provider Placement Opportunity Start
  // 0x35 = Provider Placement Opportunity End

  return true;
}

bool TsSectionScte35::ParseTimeSignal(BitReader* bit_reader,
                                      uint64_t pts_adjustment,
                                      Scte35Event* event) {
  // Parse time_signal() per SCTE-35 Table 10.
  // time_signal contains only a splice_time().
  double splice_time_seconds = 0;
  RCHECK(ParseSpliceTime(bit_reader, pts_adjustment, &splice_time_seconds));
  event->start_time_in_seconds = splice_time_seconds;

  return true;
}

bool TsSectionScte35::ParseSpliceTime(BitReader* bit_reader,
                                      uint64_t pts_adjustment,
                                      double* out_time_seconds) {
  // Parse splice_time() per SCTE-35 Table 11.
  int time_specified_flag;
  RCHECK(bit_reader->ReadBits(1, &time_specified_flag));

  if (time_specified_flag) {
    RCHECK(bit_reader->SkipBits(6));  // reserved
    uint64_t pts_time;
    RCHECK(bit_reader->ReadBits(33, &pts_time));
    *out_time_seconds = PtsToSeconds(pts_time, pts_adjustment);
  } else {
    RCHECK(bit_reader->SkipBits(7));  // reserved
    *out_time_seconds = 0;
  }

  return true;
}

bool TsSectionScte35::ParseSpliceDescriptors(BitReader* bit_reader,
                                             int descriptor_loop_length,
                                             Scte35Event* event) {
  // Parse splice_descriptor() loop per SCTE-35 Table 12.
  int remaining = descriptor_loop_length;

  while (remaining >= 6) {  // Minimum: tag(1) + length(1) + identifier(4)
    int splice_descriptor_tag;
    int descriptor_length;
    RCHECK(bit_reader->ReadBits(8, &splice_descriptor_tag));
    RCHECK(bit_reader->ReadBits(8, &descriptor_length));
    remaining -= 2;
    RCHECK(remaining >= descriptor_length);

    if (splice_descriptor_tag == kSegmentationDescriptorTag &&
        descriptor_length >= 4) {
      // Read CUEI identifier.
      uint32_t identifier;
      RCHECK(bit_reader->ReadBits(32, &identifier));

      if (identifier == kCueiIdentifier && descriptor_length >= 9) {
        // Parse segmentation_descriptor() per SCTE-35 Table 19.
        uint32_t segmentation_event_id;
        RCHECK(bit_reader->ReadBits(32, &segmentation_event_id));

        // Use segmentation_event_id as the event ID for time_signal events.
        if (event->id.empty()) {
          event->id = std::to_string(segmentation_event_id);
        }

        int segmentation_event_cancel_indicator;
        RCHECK(bit_reader->ReadBits(1, &segmentation_event_cancel_indicator));
        RCHECK(bit_reader->SkipBits(7));  // reserved

        if (!segmentation_event_cancel_indicator) {
          int program_segmentation_flag;
          int segmentation_duration_flag;
          int delivery_not_restricted_flag;
          RCHECK(bit_reader->ReadBits(1, &program_segmentation_flag));
          RCHECK(bit_reader->ReadBits(1, &segmentation_duration_flag));
          RCHECK(bit_reader->ReadBits(1, &delivery_not_restricted_flag));

          if (!delivery_not_restricted_flag) {
            RCHECK(bit_reader->SkipBits(5));  // restriction flags
          } else {
            RCHECK(bit_reader->SkipBits(5));  // reserved
          }

          if (!program_segmentation_flag) {
            int component_count;
            RCHECK(bit_reader->ReadBits(8, &component_count));
            // Each component: tag(8) + reserved(7) + pts_offset(33) = 48 bits
            RCHECK(bit_reader->SkipBits(48 * component_count));
          }

          if (segmentation_duration_flag) {
            uint64_t segmentation_duration;
            RCHECK(bit_reader->ReadBits(40, &segmentation_duration));
            event->duration_in_seconds =
                static_cast<double>(segmentation_duration) / kPtsTimescale;
          }

          int segmentation_upid_type;
          int segmentation_upid_length;
          RCHECK(bit_reader->ReadBits(8, &segmentation_upid_type));
          RCHECK(bit_reader->ReadBits(8, &segmentation_upid_length));
          RCHECK(bit_reader->SkipBits(8 * segmentation_upid_length));

          int segmentation_type_id;
          RCHECK(bit_reader->ReadBits(8, &segmentation_type_id));
          event->type = segmentation_type_id;

          // Skip segment_num and segments_expected.
          RCHECK(bit_reader->SkipBits(16));

          // Skip optional sub_segment fields based on segmentation_type_id.
          // Types 0x34, 0x36, 0x38, 0x3A have sub_segment fields.
          if (segmentation_type_id == 0x34 ||
              segmentation_type_id == 0x36 ||
              segmentation_type_id == 0x38 ||
              segmentation_type_id == 0x3A) {
            // sub_segment_num(8) + sub_segments_expected(8)
            if (bit_reader->bits_available() >= 16) {
              RCHECK(bit_reader->SkipBits(16));
            }
          }
        }

        // We've fully parsed this segmentation descriptor.
        remaining -= descriptor_length;
        continue;
      }

      // Not a CUEI segmentation descriptor — skip remaining bytes.
      int bytes_consumed = 4;  // identifier already read
      RCHECK(bit_reader->SkipBits(8 * (descriptor_length - bytes_consumed)));
    } else {
      // Unknown descriptor — skip it.
      RCHECK(bit_reader->SkipBits(8 * descriptor_length));
    }
    remaining -= descriptor_length;
  }

  // Skip any remaining bytes in the descriptor loop.
  if (remaining > 0) {
    RCHECK(bit_reader->SkipBits(8 * remaining));
  }

  return true;
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka
