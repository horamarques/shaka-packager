// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_FORMATS_MP2T_TS_SECTION_SCTE35_H_
#define PACKAGER_MEDIA_FORMATS_MP2T_TS_SECTION_SCTE35_H_

#include <cstdint>
#include <functional>
#include <memory>

#include <packager/macros/classes.h>
#include <packager/media/formats/mp2t/ts_section_psi.h>

namespace shaka {
namespace media {

struct Scte35Event;

namespace mp2t {

/// Parser for SCTE-35 splice_info_section() carried in MPEG-TS.
/// Inherits from TsSectionPsi since SCTE-35 uses PSI section format.
///
/// Parses the following splice commands:
///   - splice_null (0x00) — heartbeat, silently ignored
///   - splice_insert (0x05) — ad insertion points
///   - time_signal (0x06) — timed events with segmentation descriptors
///
/// Emits Scte35Event objects via callback for downstream processing.
class TsSectionScte35 : public TsSectionPsi {
 public:
  /// Callback invoked for each parsed SCTE-35 event.
  typedef std::function<void(std::shared_ptr<Scte35Event>)> Scte35EventCB;

  explicit TsSectionScte35(const Scte35EventCB& scte35_event_cb);
  ~TsSectionScte35() override;

  // TsSectionPsi implementation.
  bool ParsePsiSection(BitReader* bit_reader) override;
  void ResetPsiSection() override;

 private:
  /// Parse a splice_insert() command.
  bool ParseSpliceInsert(BitReader* bit_reader,
                         uint64_t pts_adjustment,
                         Scte35Event* event);

  /// Parse a time_signal() command (splice_time structure).
  bool ParseTimeSignal(BitReader* bit_reader,
                       uint64_t pts_adjustment,
                       Scte35Event* event);

  /// Parse splice_time() — shared by splice_insert and time_signal.
  /// Returns the PTS in seconds, or -1 if time_specified_flag is 0.
  bool ParseSpliceTime(BitReader* bit_reader,
                       uint64_t pts_adjustment,
                       double* out_time_seconds);

  /// Parse splice_descriptor() loop after the splice command.
  /// Extracts segmentation_type_id from segmentation_descriptor().
  bool ParseSpliceDescriptors(BitReader* bit_reader,
                              int descriptor_loop_length,
                              Scte35Event* event);

  Scte35EventCB scte35_event_cb_;

  DISALLOW_COPY_AND_ASSIGN(TsSectionScte35);
};

}  // namespace mp2t
}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_FORMATS_MP2T_TS_SECTION_SCTE35_H_
