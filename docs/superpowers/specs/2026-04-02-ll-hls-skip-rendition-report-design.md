# LL-HLS: EXT-X-SKIP and EXT-X-RENDITION-REPORT

**Date:** 2026-04-02
**Status:** Approved
**Scope:** Two playlist-level LL-HLS optimizations for shaka-packager

## Background

The existing LL-HLS implementation in the fork covers core partial segment generation (EXT-X-PART, EXT-X-PRELOAD-HINT, EXT-X-SERVER-CONTROL, EXT-X-PART-INF). Two playlist-level optimizations from RFC 8216bis remain unimplemented:

1. **EXT-X-SKIP** (Section 4.4.5.2) - Delta playlist updates to reduce transfer size
2. **EXT-X-RENDITION-REPORT** (Section 4.4.5.4) - Cross-rendition state for faster switching

Both are automatically enabled when `low_latency_hls_mode` is active. No new CLI flags required.

---

## Feature 1: EXT-X-SKIP (Delta Playlist Updates)

### Spec Summary

When a client appends `_HLS_skip=YES` to a playlist request, the server may replace older segments with a single `#EXT-X-SKIP:SKIPPED-SEGMENTS=N` tag. The server advertises this capability via `CAN-SKIP-UNTIL=<seconds>` in `EXT-X-SERVER-CONTROL`.

### Design

#### CAN-SKIP-UNTIL in EXT-X-SERVER-CONTROL

- Added to the existing `EXT-X-SERVER-CONTROL` tag in `CreatePlaylistHeader()`
- Value: `6 * target_duration` (spec-recommended minimum)
- Only emitted when `low_latency_hls_mode` is true

Example output:
```
#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,PART-HOLD-BACK=1.500,CAN-SKIP-UNTIL=12.000
```

#### EXT-X-SKIP Tag Generation in WriteToFile

During `WriteToFile()`, before iterating `entries_`:

1. **Calculate skip threshold:** `can_skip_until = 6 * target_duration_`
2. **Count skippable segments:** Walk `entries_` from the front. A segment (kExtInf) is skippable if its distance from the end of the playlist (measured in accumulated duration from the tail) exceeds `can_skip_until` seconds. Count consecutive skippable segments as `skipped_count`.
3. **Emit EXT-X-SKIP:** If `skipped_count > 0`, output `#EXT-X-SKIP:SKIPPED-SEGMENTS=<skipped_count>` before the remaining entries.
4. **Skip rendering:** Skip the first `skipped_count` segment entries and their associated kExtPart entries during the entry iteration loop.
5. **Preserve non-skippable entries:** EXT-X-KEY and EXT-X-DISCONTINUITY entries within the skipped range MUST still be emitted (per spec requirement).

#### Implementation Approach

- No mutation of `entries_` - the skip logic is purely a rendering optimization in `WriteToFile()`
- A local counter tracks how many kExtInf entries to skip during the rendering loop
- The total playlist duration is already tracked via `current_buffer_depth_`, which can be used to compute the skip threshold boundary

#### Always-Delta Strategy

Since shaka-packager writes playlists to files (not an HTTP server), we always write the delta form when segments qualify for skipping. The origin/CDN is responsible for serving the full vs delta playlist based on the client's `_HLS_skip` query parameter. This is consistent with how `CAN-BLOCK-RELOAD=YES` is already handled.

---

## Feature 2: EXT-X-RENDITION-REPORT

### Spec Summary

Each media playlist includes `EXT-X-RENDITION-REPORT` tags for every other rendition in the same master playlist. This tells the client the latest media sequence number and partial segment index of sibling renditions, enabling faster adaptive switching without polling all playlists.

### Design

#### Sibling Registration

- `MediaPlaylist` gets a new method: `SetSiblingPlaylists(const std::vector<const MediaPlaylist*>& siblings)` which stores pointers to all other media playlists.
- Called by `MasterPlaylist::WriteMasterPlaylist()` once before the first write. The master playlist already holds references to all media playlists, so this is a simple iteration that excludes self.
- Sibling pointers are `const` - no cross-mutation, only read access for sequence numbers.

#### State Accessors on MediaPlaylist

Two new const accessor methods:

- `GetLastMediaSequenceNumber() const` - Returns `media_sequence_number_ + <count of kExtInf entries in entries_> - 1`. This is the MSN of the last segment in the playlist.
- `GetLastPartIndex() const` - Returns `pending_parts_.size() - 1` if there are pending parts, or the count of kExtPart entries associated with the last segment minus 1. Returns -1 if no parts exist.

#### Rendering in WriteToFile

After the main entry loop (and after pending parts / PRELOAD-HINT), but before EXT-X-ENDLIST:

```
#EXT-X-RENDITION-REPORT:URI="<relative_path>",LAST-MSN=<N>,LAST-PART=<P>
```

- `URI` is the sibling playlist's filename, made relative to the current playlist's path
- `LAST-MSN` from `sibling->GetLastMediaSequenceNumber()`
- `LAST-PART` from `sibling->GetLastPartIndex()` (omitted if -1, i.e., no partial segments)
- A playlist MUST NOT include a rendition report for itself

#### Thread Safety Note

Since shaka-packager processes streams in parallel threads, the sibling accessors read values that may be slightly stale (the sibling may be mid-write). This is acceptable per the spec - the values represent a conservative lower bound, and the client will get updated values on the next playlist fetch.

---

## Files to Modify

| File | Changes |
|------|---------|
| `include/packager/hls_params.h` | No changes needed (reuses `low_latency_hls_mode`) |
| `packager/hls/base/media_playlist.h` | Add `SetSiblingPlaylists()`, `GetLastMediaSequenceNumber()`, `GetLastPartIndex()`, sibling storage |
| `packager/hls/base/media_playlist.cc` | EXT-X-SKIP logic in `WriteToFile()`, CAN-SKIP-UNTIL in header, rendition report rendering |
| `packager/hls/base/master_playlist.cc` | Call `SetSiblingPlaylists()` before first write |
| `packager/hls/base/media_playlist_unittest.cc` | Tests for EXT-X-SKIP and EXT-X-RENDITION-REPORT |

## Testing Strategy

### EXT-X-SKIP Tests
1. **SkipOldSegments** - Verify that segments beyond `CAN-SKIP-UNTIL` are replaced by `EXT-X-SKIP:SKIPPED-SEGMENTS=N`
2. **PreserveKeysInSkippedRange** - Verify EXT-X-KEY entries within skipped range are still emitted
3. **PreserveDiscontinuityInSkippedRange** - Verify EXT-X-DISCONTINUITY entries within skipped range are still emitted
4. **NoSkipWhenNotEnoughSegments** - Verify no EXT-X-SKIP when all segments are within the skip threshold
5. **ServerControlIncludesCanSkipUntil** - Verify CAN-SKIP-UNTIL appears in header

### EXT-X-RENDITION-REPORT Tests
1. **RenditionReportForSiblings** - Verify correct URI, LAST-MSN, LAST-PART for sibling playlists
2. **NoSelfReport** - Verify playlist does not include a report for itself
3. **RenditionReportOmitsPartWhenNoParts** - Verify LAST-PART is omitted when sibling has no partial segments
4. **RenditionReportNotEmittedForVOD** - Verify no rendition reports in VOD playlists
