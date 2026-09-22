Epoch-anchored segment numbering options
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:segment_number_epoch:

    An RFC 3339 UTC instant, for example ``2026-01-01T00:00:00Z``, declaring
    the wall-clock instant that corresponds to media timeline zero (PTS 0).
    Empty by default, meaning segment numbers are a plain counter starting
    at ``--start_segment_number``, as usual.

    When set, the segment number for a segment starting at PTS ``pts`` is::

        floor((epoch + pts) / segment_duration)

    counted from the Unix epoch, not from ``--start_segment_number``. This
    number reaches the ``$Number$`` segment filename, the DASH
    ``SegmentTemplate`` ``startNumber``, and the HLS
    ``EXT-X-MEDIA-SEQUENCE``. Independent packager instances configured with
    the same ``--segment_number_epoch`` and fed frame-aligned input therefore
    agree on the number for a given segment, which is what lets a player or
    origin arbitrate between them on a per-segment basis.

    Both legs of a redundant pair (see :doc:`redundant_input_options`) must
    be configured with the same value, or their segment numbers will not
    agree.

    Rejected in combination with an explicit ``--start_segment_number``,
    since epoch-anchored numbering must be identical across instances and
    ignores that flag anyway. There is one known gap in this check: abseil
    has no predicate for "was this flag explicitly set", so the check
    compares the supplied value against the documented default of 1. Passing
    ``--start_segment_number=1`` alongside ``--segment_number_epoch`` is
    therefore not rejected, but numbering is still correct in that case,
    because ``start_segment_number`` is simply ignored once the epoch is set.

Restart safety and wrap resolution
-----------------------------------

Input PTS is a 33-bit value that wraps around roughly every 26.5 hours at a
90kHz timescale. To keep numbering identical across a restart, the first
sample after startup seeds the wrap count from the system clock: it picks
the wrap count that places the implied stream instant closest to "now".
This only needs the system clock to be right to within about half a wrap
period (roughly 13 hours at 90kHz), not to the second, since a restarted
instance and one that never stopped will resolve the same wrap count as
long as both are within that window.

A warning is logged once, on the first sample, when the anchor implies a
stream instant more than half a wrap period from the system clock. That is
exactly the condition under which the resolved wrap count, and therefore
every segment number derived from it, may be wrong.

.. important::

    This restart-seeding behaviour is what makes the flag correct for
    **live** input, where PTS tracks wall clock: concurrent and restarted
    instances all consult the same system clock and converge on the same
    wrap count and the same segment numbers.

    It does not have the same meaning for **file or VOD** input, where PTS
    zero is simply start-of-file and carries no wall-clock relationship. For
    file input, segment numbers become a function of when the packager
    happened to run, not of the file's content: reprocessing the same file a
    week later produces different segment numbers. An anchor far in the
    past, such as ``1970-01-01T00:00:00Z``, makes this obvious immediately,
    since the seeded wrap count is then chosen to land near the current
    system clock rather than near the file's own timeline. This flag is
    intended for live input; for file input, do not expect the anchor to be
    honoured literally.

Known limitations
------------------

- **Low latency.** In LL-DASH and LL-HLS, the low latency segmenter builds
  its output filenames from an internal segment counter rather than the
  epoch-anchored number, so filenames are not epoch-anchored in low latency
  mode. This is not yet fixed.
- **HLS media sequence seed.** An explicit ``--hls_media_sequence_number``
  takes precedence: when one is supplied, the epoch-anchored number is not
  adopted into the HLS media sequence.
- **Segment boundaries are not aligned by this flag.** Boundaries still
  derive from input PTS, so alignment across redundant legs remains the
  encoders' responsibility, via epoch-locked IDR placement. This flag only
  makes the numbers agree once the boundaries already do.
- **No degraded-input detection or discontinuity signalling.** This flag
  does not detect degraded input, and does not signal a discontinuity when
  an input source switches (for example on a ``redundant://`` failover).
- **Key rotation is unaffected.** Key rotation is still anchored to DTS, not
  to this epoch, so two instances can still diverge on crypto period
  boundaries even with matching segment numbers.

Example: a redundant pair with matching segment numbers
---------------------------------------------------------

Two independent packager instances, each fed one leg of a redundant pair,
configured to agree on segment numbers::

    # Instance A
    packager \
      'in=udp://239.1.1.1:5000?interface=10.0.0.1,stream=video,init_segment=video_init.mp4,segment_template=video_$Number$.m4s' \
      --segment_number_epoch=2026-01-01T00:00:00Z \
      --mpd_output a.mpd

    # Instance B, on the other leg
    packager \
      'in=udp://239.2.2.2:5000?interface=10.0.1.1,stream=video,init_segment=video_init.mp4,segment_template=video_$Number$.m4s' \
      --segment_number_epoch=2026-01-01T00:00:00Z \
      --mpd_output b.mpd

As long as both instances receive frame-aligned input (the same content, on
the same wall-clock timeline), segment ``N`` from instance A and segment
``N`` from instance B cover the same media interval, so an origin or player
can pick either one for a given segment number.
