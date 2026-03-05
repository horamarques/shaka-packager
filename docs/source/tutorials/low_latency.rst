############################################
Low Latency Streaming (LL-DASH and LL-HLS)
############################################

************
Introduction
************

If ``--low_latency_dash_mode`` is enabled, low latency DASH (LL-DASH) packaging will be used.

This will reduce overall latency by ensuring that the media segments are chunk encoded and delivered via an aggregating response.
The combination of these features will ensure that overall latency can be decoupled from the segment duration.
For low latency to be achieved, the output of Shaka Packager must be combined with a delivery system which can chain together a set of aggregating responses, such as chunked transfer encoding under HTTP/1.1 or a HTTP/2 or HTTP/3 connection.
The output of Shaka Packager must be played with a DASH client that understands the availabilityTimeOffset MPD value.
Furthermore, the player should also understand the throughput estimation and ABR challenges that arise when operating in the low latency regime.

This tutorial covers LL-DASH packaging and uses features from the DASH, HTTP upload, and FFmpeg piping tutorials.
For more information on DASH, see :doc:`dash`; for HTTP upload, see :doc:`http_upload`;
for FFmpeg piping, see :doc:`ffmpeg_piping`;
for full documentation, see :doc:`/documentation`.

*************
Documentation
*************

Getting started
===============

To enable LL-DASH mode, set the ``--low_latency_dash_mode`` flag to ``true``. 

All HTTP requests will use chunked transfer encoding:
``Transfer-Encoding: chunked``.

.. note::

    Both LL-DASH and LL-HLS are supported. The two modes are mutually exclusive;
    use ``--low_latency_dash_mode`` for MPEG-DASH output and
    ``--low_latency_hls_mode`` for HLS output.

Synopsis
========

Here is a basic example of the LL-DASH support. 
The LL-DASH setup borrows features from "FFmpeg piping" and "HTTP upload",
see :doc:`ffmpeg_piping` and :doc:`http_upload`.

Define UNIX pipe to connect ffmpeg with packager::

    export PIPE=/tmp/bigbuckbunny.fifo
    mkfifo ${PIPE}

Acquire and transcode RTMP stream::

    ffmpeg -fflags nobuffer -threads 0 -y \
        -i rtmp://184.72.239.149/vod/mp4:bigbuckbunny_450.mp4 \
        -pix_fmt yuv420p -vcodec libx264 -preset:v superfast -acodec aac \
        -f mpegts pipe: > ${PIPE}

Configure and run packager::

    # Define upload URL
    export UPLOAD_URL=http://localhost:6767/ll-dash

    # Go
    packager \
        "input=${PIPE},stream=audio,init_segment=${UPLOAD_URL}_init.m4s,segment_template=${UPLOAD_URL}/bigbuckbunny-audio-aac-\$Number%04d\$.m4s" \
        "input=${PIPE},stream=video,init_segment=${UPLOAD_URL}_init.m4s,segment_template=${UPLOAD_URL}/bigbuckbunny-video-h264-450-\$Number%04d\$.m4s" \
        --io_block_size 65536 \
        --segment_duration 2 \
        --low_latency_dash_mode=true \
        --utc_timings "urn:mpeg:dash:utc:http-xsdate:2014"="https://time.akamai.com/?iso" \
        --mpd_output "${UPLOAD_URL}/bigbuckbunny.mpd" \


*************************
Low Latency Compatibility
*************************

For low latency to be achieved, the processes handling Shaka Packager's output, such as the server and player,
must support LL-DASH streaming.

Delivery Pipeline
=================
Shaka Packager will upload the LL-DASH content to the specified output via HTTP chunked transfer encoding.
The server must have the ability to handle this type of request. If using a proxy or shim for cloud authentication,
these services must also support HTTP chunked transfer encoding.

Examples of supporting content delivery systems:

* `AWS MediaStore <https://aws.amazon.com/mediastore/>`_
* `s3-upload-proxy <https://github.com/fsouza/s3-upload-proxy>`_
* `Streamline Low Latency DASH preview <https://github.com/streamlinevideo/low-latency-preview>`_
* `go-chunked-streaming-server <https://github.com/mjneil/go-chunked-streaming-server>`_

Player
======
The player must support LL-DASH playout.
LL-DASH requires the player to be able to interpret ``availabilityTimeOffset`` values from the DASH MPD.
The player should also recognize the the throughput estimation and ABR challenges that arise with low latency streaming.

Examples of supporting players:

* `Shaka Player <https://github.com/shaka-project/shaka-player>`_
* `dash.js <https://github.com/Dash-Industry-Forum/dash.js>`_
* `Streamline Low Latency DASH preview <https://github.com/streamlinevideo/low-latency-preview>`_


####################################
Low Latency HLS (LL-HLS) Streaming
####################################

************
Introduction
************

Apple Low Latency HLS (LL-HLS) is defined in `RFC 8216bis
<https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis>`_ and
reduces end-to-end latency by splitting each segment into smaller *partial
segments* that are published as soon as they are encoded.  Clients can begin
downloading a partial segment before the containing full segment is complete.

When ``--low_latency_hls_mode`` is enabled, Shaka Packager produces:

* **EXT-X-PART** tags for each partial segment (chunk), with ``BYTERANGE``
  attributes pointing into the containing segment file.
* An **EXT-X-PART-INF** header advertising the ``PART-TARGET`` duration.
* An **EXT-X-SERVER-CONTROL** header with ``CAN-BLOCK-RELOAD=YES`` and
  ``PART-HOLD-BACK`` set to three times the part target duration.
* An **EXT-X-PRELOAD-HINT** tag so that clients can issue a blocking request
  for the next partial segment before it is available.
* HLS playlist version 9, as required by the LL-HLS specification.

*************
Documentation
*************

Getting started
===============

Enable LL-HLS mode with the ``--low_latency_hls_mode`` flag.  You must also
provide an HLS master playlist output and use a LIVE or EVENT playlist type::

    --low_latency_hls_mode=true
    --hls_master_playlist_output <path_or_url>/master.m3u8
    --hls_playlist_type LIVE

Optional tuning
---------------

``--hls_part_target_duration`` (default ``0.5``) sets the target duration in
seconds for each partial segment.  This value is written into
``EXT-X-PART-INF:PART-TARGET`` and used to compute ``PART-HOLD-BACK``.

Synopsis
========

Here is a basic example using a UNIX pipe from FFmpeg::

    export PIPE=/tmp/bigbuckbunny.fifo
    mkfifo ${PIPE}

Transcode the source stream::

    ffmpeg -fflags nobuffer -threads 0 -y \
        -i rtmp://184.72.239.149/vod/mp4:bigbuckbunny_450.mp4 \
        -pix_fmt yuv420p -vcodec libx264 -preset:v superfast -acodec aac \
        -f mpegts pipe: > ${PIPE}

Run the packager::

    export OUTPUT_DIR=/var/www/hls/ll

    packager \
        "input=${PIPE},stream=audio,init_segment=${OUTPUT_DIR}/audio_init.mp4,segment_template=${OUTPUT_DIR}/audio_\$Number%04d\$.mp4,playlist_name=audio.m3u8" \
        "input=${PIPE},stream=video,init_segment=${OUTPUT_DIR}/video_init.mp4,segment_template=${OUTPUT_DIR}/video_\$Number%04d\$.mp4,playlist_name=video.m3u8" \
        --io_block_size 65536 \
        --segment_duration 2 \
        --low_latency_hls_mode=true \
        --hls_playlist_type LIVE \
        --hls_master_playlist_output "${OUTPUT_DIR}/master.m3u8"

Each segment file will contain one ``styp`` box followed by multiple
``moof+mdat`` chunks, one per video frame (or audio frame group).  The HLS
media playlist is rewritten after every chunk, so clients can start playing
within one partial-segment duration of the live edge.

*************************
Low Latency Compatibility
*************************

Delivery Pipeline
=================

Because LL-HLS clients issue byte-range requests for partial segments that
may still be growing on disk, the origin server must support:

* **HTTP/1.1 chunked transfer encoding** or **HTTP/2 / HTTP/3** for partial
  responses.
* **Blocking playlist reload** — the server should hold a ``GET`` request for
  the playlist until a new version is available (indicated by the
  ``_HLS_msn`` and ``_HLS_part`` query parameters).

Examples of supporting delivery systems:

* `AWS MediaStore <https://aws.amazon.com/mediastore/>`_
* `go-chunked-streaming-server <https://github.com/mjneil/go-chunked-streaming-server>`_
* Any CDN or origin that supports HTTP range requests on in-progress files.

Player
======

The player must support LL-HLS playout, including:

* ``EXT-X-PART`` / ``EXT-X-PART-INF`` parsing.
* ``EXT-X-PRELOAD-HINT`` for low-latency prefetch.
* Blocking playlist reload via ``_HLS_msn`` / ``_HLS_part`` query parameters.

Examples of supporting players:

* `Shaka Player <https://github.com/shaka-project/shaka-player>`_ (v4.3+)
* `HLS.js <https://github.com/video-dev/hls.js>`_ (v1.2+)
* Native Apple players (Safari, AVPlayer) on Apple platforms.
