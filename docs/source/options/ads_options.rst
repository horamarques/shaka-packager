Ads options
^^^^^^^^^^^

--ad_cues <start_time[;start_time]...>

    List of cuepoint markers separated by semicolon. The start_time represents
    the start of the cue marker in seconds (double precision) relative to the
    start of the program. This flag preconditions content for
    `Dynamic Ad Insertion <http://bit.ly/2KK10DD>`_ with Google Ad Manager.
    For DASH, multiple periods will be generated with period boundaries at the
    next key frame to the designated start times; For HLS, segments will be
    terminated at the next key frame to the designated start times and
    '#EXT-X-PLACEMENT-OPPORTUNITY' tag will be inserted after the segment in
    media playlist.

    When SCTE-35 markers are also present in the input MPEG-TS stream, manual
    cue points from ``--ad_cues`` and SCTE-35 detected cue points are merged.
    Duplicate cue points at the same timestamp are deduplicated automatically.

SCTE-35 automatic detection (no flag required)

    When the input is an MPEG-TS file containing SCTE-35 splice commands
    (``splice_insert`` or ``time_signal``), Shaka Packager automatically
    detects the SCTE-35 PID via the CUEI registration descriptor in the PMT
    and parses the splice data. No ``--ad_cues`` flag is needed.

    For DASH, an ``<EventStream>`` element with scheme
    ``urn:scte:scte35:2013:xml`` is added to the period containing the cue
    event. The binary splice data is Base64-encoded in a
    ``<Signal><Binary>`` element.

    For HLS, ``#EXT-X-DATERANGE`` tags are inserted with the original binary
    splice data hex-encoded in ``SCTE35-OUT`` or ``SCTE35-IN`` attributes,
    compliant with RFC 8216bis.
