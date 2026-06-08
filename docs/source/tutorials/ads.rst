Ad Insertion
============

Shaka Packager can precondition content for
`Dynamic Ad Insertion <http://bit.ly/2KK10DD>`_ with Google Ad Manager
or other ad-insertion workflows. Both DASH and HLS are supported.

There are two ways to signal ad breaks:

1. **Manual cue points** using ``--ad_cues`` — you supply explicit
   timestamps on the command line.
2. **SCTE-35 automatic detection** — when the input is an MPEG-TS file
   containing SCTE-35 splice commands (``splice_insert`` or ``time_signal``),
   Shaka Packager automatically parses them and converts them into cue events
   with no additional flags required.

.. _manual-cue-points:

Manual Cue Points
-----------------

Synopsis
^^^^^^^^

::

    $ packager <stream_descriptor> ... \
      --ad_cues <start_time[;start_time]...> \
      [Other options, e.g. DRM options, DASH options, HLS options]

Examples
^^^^^^^^

The examples below use the H264 streams created in :doc:`encoding`.

Three midroll cue markers are inserted at 10 minutes, 30 minutes and 50 minutes
respectively.

* DASH with live profile::

    $ packager \
      'in=h264_baseline_360p_600.mp4,stream=audio,init_segment=audio/init.mp4,segment_template=audio/$Number$.m4s' \
      'in=input_text.vtt,stream=text,init_segment=text/init.mp4,segment_template=text/$Number$.m4s' \
      'in=h264_baseline_360p_600.mp4,stream=video,init_segment=h264_360p/init.mp4,segment_template=h264_360p/$Number$.m4s' \
      'in=h264_main_480p_1000.mp4,stream=video,init_segment=h264_480p/init.mp4,segment_template=h264_480p/$Number$.m4s' \
      'in=h264_main_720p_3000.mp4,stream=video,init_segment=h264_720p/init.mp4,segment_template=h264_720p/$Number$.m4s' \
      'in=h264_high_1080p_6000.mp4,stream=video,init_segment=h264_1080p/init.mp4,segment_template=h264_1080p/$Number$.m4s' \
      --ad_cues 600;1800;3000 \
      --generate_static_live_mpd --mpd_output h264.mpd

* DASH with on-demand profile::

    $ packager \
      in=h264_baseline_360p_600.mp4,stream=audio,output=audio.mp4 \
      in=input_text.vtt,stream=text,output=output_text.mp4 \
      in=h264_baseline_360p_600.mp4,stream=video,output=h264_360p.mp4 \
      in=h264_main_480p_1000.mp4,stream=video,output=h264_480p.mp4 \
      in=h264_main_720p_3000.mp4,stream=video,output=h264_720p.mp4 \
      in=h264_high_1080p_6000.mp4,stream=video,output=h264_1080p.mp4 \
      --ad_cues 600;1800;3000 \
      --mpd_output h264.mpd

This generates six single-segment media files, one per stream, spanning multiple
periods. There may be problems handling this type of DASH contents in some
players, although it is recommended by `DASH IF IOP <http://bit.ly/2B0HL9q>`_.
Use the below option if your player does not like it.

* DASH with on-demand profile but one file per Period::

    $ packager \
      'in=h264_baseline_360p_600.mp4,stream=audio,output=audio_$Number$.mp4' \
      'in=input_text.vtt,stream=text,output=output_text_$Number$.mp4' \
      'in=h264_baseline_360p_600.mp4,stream=video,output=h264_360p_$Number$.mp4' \
      'in=h264_main_480p_1000.mp4,stream=video,output=h264_480p_$Number$.mp4' \
      'in=h264_main_720p_3000.mp4,stream=video,output=h264_720p_$Number$.mp4' \
      'in=h264_high_1080p_6000.mp4,stream=video,output=h264_1080p_$Number$.mp4' \
      --ad_cues 600;1800;3000 \
      --mpd_output h264.mpd

* HLS using transport streams::

    $ packager \
      'in=h264_baseline_360p_600.mp4,stream=audio,segment_template=audio_$Number$.aac' \
      'in=input_text.vtt,stream=text,segment_template=output_text_$Number$.vtt' \
      'in=h264_baseline_360p_600.mp4,stream=video,segment_template=h264_360p_$Number$.ts' \
      'in=h264_main_480p_1000.mp4,stream=video,segment_template=h264_480p_$Number$.ts' \
      'in=h264_main_720p_3000.mp4,stream=video,segment_template=h264_720p_$Number$.ts' \
      'in=h264_high_1080p_6000.mp4,stream=video,segment_template=h264_1080p_$Number$.ts' \
      --ad_cues 600;1800;3000 \
      --hls_master_playlist_output h264_master.m3u8

.. _scte35-automatic-detection:

SCTE-35 Automatic Detection
----------------------------

When the input is an MPEG-TS file containing SCTE-35 splice commands, Shaka
Packager automatically detects and processes them. No additional flags are
required beyond normal packaging options.

How it works
^^^^^^^^^^^^

1. The MPEG-TS demuxer detects the SCTE-35 PID via the **CUEI registration
   descriptor** (``0x43554549``) in the Program Map Table (PMT).
2. SCTE-35 ``splice_info_section()`` messages are parsed, supporting both
   ``splice_insert`` (command type ``0x05``) and ``time_signal`` (command type
   ``0x06``) commands.
3. For ``time_signal`` commands with segmentation descriptors, the
   ``segmentation_type_id`` determines the cue type:

   - **Even IDs** (``0x30``, ``0x32``, ``0x34``, ...) map to **cue-out**
     (ad break start).
   - **Odd IDs** (``0x31``, ``0x33``, ``0x35``, ...) map to **cue-in**
     (ad break end).
   - Other IDs map to a generic **cue point**.

4. Each detected cue event is aligned to the next keyframe boundary across all
   streams and triggers a segment split.

Output signaling
^^^^^^^^^^^^^^^^

**HLS**: SCTE-35 cue events are signaled using ``#EXT-X-DATERANGE`` tags with
the original binary splice data hex-encoded in ``SCTE35-OUT`` or ``SCTE35-IN``
attributes, as defined in `RFC 8216bis <https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis>`_.
The cue-out / cue-in distinction is derived from the SCTE-35 segmentation type
(see above): out points emit ``SCTE35-OUT``, return-to-program points emit
``SCTE35-IN``.  When the splice carries a break duration, the out point also
emits ``PLANNED-DURATION``::

    #EXT-X-DATERANGE:ID="splice-1",START-DATE="2024-01-15T10:05:00Z",SCTE35-OUT=0xFC301600...,PLANNED-DURATION=30.000

.. note::

    ``START-DATE`` is anchored to a wall-clock reference taken when packaging
    starts.  This reference is now established regardless of the
    ``--hls_program_date_time`` flag, so DATERANGE timestamps are valid even
    when EXT-X-PROGRAM-DATE-TIME output is disabled.

**DASH**: SCTE-35 cue events generate an ``<EventStream>`` element within the
corresponding ``<Period>`` using the ``urn:scte:scte35:2013:xml`` scheme.
The binary splice data is Base64-encoded inside a ``<Signal><Binary>`` element.
A 90 kHz ``timescale`` is used so that ``presentationTime`` and ``duration``
keep sub-second (frame-accurate) precision::

    <EventStream schemeIdUri="urn:scte:scte35:2013:xml" timescale="90000">
      <Event presentationTime="54000000" duration="2700000">
        <Signal xmlns="http://www.scte.org/schemas/35/2016">
          <Binary>/DAGAAAAAAAAAP/wBQb+AAAAAAA=</Binary>
        </Signal>
      </Event>
    </EventStream>

Example
^^^^^^^

Package an MPEG-TS file with embedded SCTE-35 markers for both DASH and HLS::

    $ packager \
      'in=live_feed.ts,stream=audio,init_segment=audio/init.mp4,segment_template=audio/$Number$.m4s,playlist_name=audio.m3u8' \
      'in=live_feed.ts,stream=video,init_segment=video/init.mp4,segment_template=video/$Number$.m4s,playlist_name=video.m3u8' \
      --mpd_output manifest.mpd \
      --hls_master_playlist_output master.m3u8

The SCTE-35 markers in ``live_feed.ts`` are automatically parsed, and the output
manifests will contain the appropriate ad break signaling without any
``--ad_cues`` flag.

Combining manual cues with SCTE-35
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

You can use ``--ad_cues`` alongside SCTE-35 input. Manual cue points and
SCTE-35 detected cue points are merged. Duplicate cue points at the same
timestamp are automatically deduplicated.

Low latency
^^^^^^^^^^^

SCTE-35 detection and signaling are not aware of low latency: the same pipeline
runs whether or not ``--low_latency_dash_mode`` / ``--low_latency_hls_mode`` is
set.  A detected cue forces a full-segment boundary, and the resulting
``#EXT-X-DATERANGE`` (HLS) or ``<EventStream>`` (DASH) is emitted exactly as in
regular mode.  In LL-HLS the new ``#EXT-X-DATERANGE`` is written into the
playlist immediately before the first ``#EXT-X-PART`` of the segment that starts
at the splice point.

.. note::

    SCTE-35 detection from MPEG-TS input is a Velocix fork feature; it is not
    present in upstream shaka-project. Upstream supports only manual
    ``--ad_cues``.

Configuration options
---------------------

.. include:: /options/ads_options.rst
