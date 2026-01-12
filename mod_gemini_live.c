/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * mod_gemini_live.c -- Ultra-low-latency bidirectional audio streaming for Gemini Live API
 *
 * This module provides a "dumb pipe" for audio between FreeSWITCH and an external
 * TCP socket. It is designed to integrate with Google's Gemini Multimodal Live API
 * via an external sidecar application controlled through ESL.
 *
 * Design Philosophy:
 * - Minimal latency: no buffering delays, immediate playback
 * - Simple: audio only, all call control via ESL
 * - Fast: pure raw PCM on socket, no parsing overhead
 *
 */

#include <switch.h>

#define GEMINI_INPUT_RATE   16000   /* Send to Gemini at 16kHz */
#define GEMINI_OUTPUT_RATE  24000   /* Receive from Gemini at 24kHz */
#define GEMINI_BUG_NAME     "gemini_live"
#define GEMINI_PRIVATE      "_gemini_live_"

/* Maximum audio queue size: 90 seconds at 48kHz (worst case session rate)
 * Gemini may generate audio faster than real-time, so queue must hold entire turn.
 * Memory: ~8.6MB at 48kHz, ~2.9MB at 16kHz, ~1.4MB at 8kHz per call */
#define GEMINI_QUEUE_MAX_SIZE  (48000 * 2 * 90)

/* Duration to discard incoming audio after flush (microseconds)
 * This allows in-flight packets to clear before resuming playback */
#define GEMINI_DISCARD_DURATION_US  500000  /* 500ms */

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_gemini_live_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_gemini_live_load);
SWITCH_MODULE_DEFINITION(mod_gemini_live, mod_gemini_live_load, mod_gemini_live_shutdown, NULL);

typedef struct {
    /* Session reference */
    switch_core_session_t *session;
    switch_channel_t *channel;
    switch_media_bug_t *bug;
    switch_memory_pool_t *pool;

    /* Socket */
    switch_socket_t *sock;

    /* Threading */
    switch_thread_t *thread;
    switch_mutex_t *mutex;
    volatile uint8_t running;

    /* Inbound audio queue (from Gemini, waiting to play) */
    switch_buffer_t *audio_queue;
    volatile uint8_t flush_flag;
    switch_time_t discard_until;  /* Discard incoming audio until this timestamp (microseconds) */

    /* Resamplers */
    switch_audio_resampler_t *read_resampler;   /* session → 16kHz (to Gemini) */
    switch_audio_resampler_t *write_resampler;  /* 24kHz → session (from Gemini) */

    /* Session codec info */
    uint32_t session_rate;
    uint32_t read_ptime;
    uint32_t write_ptime;

    /* Frame sizes in bytes (16-bit samples) */
    uint32_t session_frame_bytes;    /* Bytes per frame at session rate */
    uint32_t gemini_in_frame_bytes;  /* Bytes per frame at 16kHz */
    uint32_t gemini_out_frame_bytes; /* Bytes per frame at 24kHz */

    /* Write codec for direct frame injection */
    switch_codec_t write_codec;
    switch_frame_t write_frame;
    uint8_t write_frame_data[SWITCH_RECOMMENDED_BUFFER_SIZE];

} gemini_ctx_t;

/* Forward declarations */
static switch_bool_t gemini_media_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);
static void *SWITCH_THREAD_FUNC gemini_socket_thread(switch_thread_t *thread, void *obj);

/*
 * Socket Reader Thread
 *
 * Runs in background, receives raw PCM from sidecar, pushes to queue.
 * Exits when socket closes or channel hangs up.
 */
static void *SWITCH_THREAD_FUNC gemini_socket_thread(switch_thread_t *thread, void *obj)
{
    gemini_ctx_t *ctx = (gemini_ctx_t *)obj;
    uint8_t recv_buf[8192];
    switch_size_t recv_len;
    switch_status_t status;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                      "Gemini socket thread started\n");

    while (ctx->running && switch_channel_ready(ctx->channel)) {
        recv_len = sizeof(recv_buf);
        status = switch_socket_recv(ctx->sock, (char *)recv_buf, &recv_len);

        if (status != SWITCH_STATUS_SUCCESS || recv_len == 0) {
            /* Socket closed or error */
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                              "Gemini socket closed or error (status=%d, len=%zu)\n",
                              status, recv_len);
            break;
        }

        /* Received raw 24kHz PCM from Gemini via sidecar */
        if (recv_len > 0) {
            int16_t *pcm_in = (int16_t *)recv_buf;
            uint32_t samples_in = recv_len / sizeof(int16_t);
            void *pcm_out = recv_buf;
            uint32_t bytes_out = recv_len;

            /* Resample 24kHz → session rate if needed */
            if (ctx->write_resampler) {
                switch_resample_process(ctx->write_resampler, pcm_in, samples_in);
                pcm_out = ctx->write_resampler->to;
                bytes_out = ctx->write_resampler->to_len * sizeof(int16_t);
            }

            /* Check for flush flag - clear queue and enter timed discard mode */
            if (ctx->flush_flag) {
                switch_size_t flushed_bytes;
                switch_mutex_lock(ctx->mutex);
                flushed_bytes = switch_buffer_inuse(ctx->audio_queue);
                switch_buffer_zero(ctx->audio_queue);
                ctx->flush_flag = 0;
                ctx->discard_until = switch_time_now() + GEMINI_DISCARD_DURATION_US;
                switch_mutex_unlock(ctx->mutex);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                                  "Gemini interrupted: flushed %zu bytes, discarding for %dms\n",
                                  flushed_bytes, GEMINI_DISCARD_DURATION_US / 1000);
                continue;
            }

            /* Discard incoming audio during discard window (clears in-flight packets) */
            if (ctx->discard_until > 0 && switch_time_now() < ctx->discard_until) {
                continue;  /* Silently discard */
            } else if (ctx->discard_until > 0) {
                /* Discard window expired, resume normal operation */
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                                  "Gemini audio resumed after discard window\n");
                ctx->discard_until = 0;
            }

            /* Push resampled audio to queue */
            switch_mutex_lock(ctx->mutex);
            if (switch_buffer_inuse(ctx->audio_queue) + bytes_out > GEMINI_QUEUE_MAX_SIZE) {
                switch_size_t excess = (switch_buffer_inuse(ctx->audio_queue) + bytes_out) - GEMINI_QUEUE_MAX_SIZE;
                switch_buffer_toss(ctx->audio_queue, excess);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_WARNING,
                    "Queue overflow, dropped %zu bytes\n", excess);
            }
            switch_buffer_write(ctx->audio_queue, pcm_out, bytes_out);
            switch_mutex_unlock(ctx->mutex);

            /* Write frames with proper 20ms pacing */
            while (switch_channel_ready(ctx->channel) && ctx->running) {
                switch_size_t queue_bytes;

                /* Check for flush flag - interrupt playback immediately */
                if (ctx->flush_flag) {
                    switch_size_t flushed_bytes;
                    switch_mutex_lock(ctx->mutex);
                    flushed_bytes = switch_buffer_inuse(ctx->audio_queue);
                    switch_buffer_zero(ctx->audio_queue);
                    ctx->flush_flag = 0;
                    ctx->discard_until = switch_time_now() + GEMINI_DISCARD_DURATION_US;
                    switch_mutex_unlock(ctx->mutex);
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                                      "Gemini interrupted: flushed %zu bytes, discarding for %dms\n",
                                      flushed_bytes, GEMINI_DISCARD_DURATION_US / 1000);
                    break;
                }

                switch_mutex_lock(ctx->mutex);
                queue_bytes = switch_buffer_inuse(ctx->audio_queue);

                if (queue_bytes < ctx->session_frame_bytes) {
                    switch_mutex_unlock(ctx->mutex);
                    break; /* Not enough data for a full frame, wait for more */
                }

                /* Read one frame worth of data */
                switch_buffer_read(ctx->audio_queue, ctx->write_frame_data, ctx->session_frame_bytes);
                switch_mutex_unlock(ctx->mutex);

                /* Set up and write the frame */
                ctx->write_frame.data = ctx->write_frame_data;
                ctx->write_frame.datalen = ctx->session_frame_bytes;
                ctx->write_frame.samples = ctx->session_frame_bytes / sizeof(int16_t);

                status = switch_core_session_write_frame(ctx->session, &ctx->write_frame, SWITCH_IO_FLAG_NONE, 0);

                if (status != SWITCH_STATUS_SUCCESS) {
                    break;
                }

                /* Sleep for frame duration to maintain proper 20ms pacing */
                switch_yield(ctx->read_ptime * 1000); /* Convert ms to microseconds */
            }
        }
    }

    ctx->running = 0;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                      "Gemini socket thread exiting\n");

    return NULL;
}

/*
 * Media Bug Callback
 *
 * Called by FreeSWITCH media thread every ptime (typically 20ms).
 * Must never block.
 */
static switch_bool_t gemini_media_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    gemini_ctx_t *ctx = (gemini_ctx_t *)user_data;

    if (!ctx) {
        return SWITCH_FALSE;
    }

    switch (type) {

    case SWITCH_ABC_TYPE_INIT:
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                          "Gemini media bug initialized (session_rate=%u)\n", ctx->session_rate);
        break;

    case SWITCH_ABC_TYPE_READ_REPLACE:
        /* MIC AUDIO: FreeSWITCH → Gemini (via sidecar) */
        {
            switch_frame_t *frame = switch_core_media_bug_get_read_replace_frame(bug);
            static int read_frame_log_count = 0;

            if (frame && frame->data && frame->datalen > 0 && ctx->sock && ctx->running) {
                /* Debug: Log frame details for first few frames */
                if (read_frame_log_count < 5) {
                    /* Calculate audio level (peak amplitude) */
                    int16_t *samples = (int16_t *)frame->data;
                    uint8_t *raw_bytes = (uint8_t *)frame->data;
                    int max_amp = 0;
                    uint32_t i;
                    for (i = 0; i < frame->datalen / 2 && i < 160; i++) {
                        int amp = samples[i] < 0 ? -samples[i] : samples[i];
                        if (amp > max_amp) max_amp = amp;
                    }
                    /* Show first 8 raw bytes to identify encoding */
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_WARNING,
                        "READ_REPLACE frame: datalen=%u, rate=%u, codec=%s, samples=%u, peak_amp=%d, "
                        "first8bytes=[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                        (unsigned)frame->datalen, frame->rate,
                        frame->codec ? frame->codec->implementation->iananame : "NULL",
                        frame->samples, max_amp,
                        raw_bytes[0], raw_bytes[1], raw_bytes[2], raw_bytes[3],
                        raw_bytes[4], raw_bytes[5], raw_bytes[6], raw_bytes[7]);
                    read_frame_log_count++;
                }
                int16_t *pcm_in = (int16_t *)frame->data;
                uint32_t samples_in = frame->datalen / sizeof(int16_t);
                void *pcm_out = frame->data;
                switch_size_t send_len = frame->datalen;
                static int frame_count = 0;
                static int last_logged_peak = 0;
                frame_count++;

                /* Log periodically if audio level changes significantly */
                if (frame_count % 250 == 0) { /* Every 5 seconds */
                    int16_t *samples = (int16_t *)frame->data;
                    int max_amp = 0;
                    uint32_t j;
                    for (j = 0; j < frame->datalen / 2 && j < 160; j++) {
                        int amp = samples[j] < 0 ? -samples[j] : samples[j];
                        if (amp > max_amp) max_amp = amp;
                    }
                    if (max_amp != last_logged_peak || frame_count <= 500) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_WARNING,
                            "READ frame #%d: peak_amp=%d (%.1f%%)\n",
                            frame_count, max_amp, max_amp * 100.0 / 32768);
                        last_logged_peak = max_amp;
                    }
                }

                /* Resample session rate → 16kHz if needed */
                if (ctx->read_resampler) {
                    switch_resample_process(ctx->read_resampler, pcm_in, samples_in);
                    pcm_out = ctx->read_resampler->to;
                    send_len = ctx->read_resampler->to_len * sizeof(int16_t);
                }

                /* Non-blocking send to socket */
                /* If send fails (buffer full), drop the packet - better than blocking */
                switch_socket_send_nonblock(ctx->sock, pcm_out, &send_len);
            }
        }
        break;

    case SWITCH_ABC_TYPE_WRITE_REPLACE:
        /*
         * SPEAKER AUDIO: Gemini → FreeSWITCH
         * NOTE: We're NOT using WRITE_REPLACE for playback anymore.
         * The socket thread handles direct frame writing with proper pacing.
         * We keep this callback to handle flush requests only.
         */
        {
            switch_mutex_lock(ctx->mutex);
            /* Check flush flag (set by uuid_gemini_flush API) */
            if (ctx->flush_flag) {
                switch_buffer_zero(ctx->audio_queue);
                ctx->flush_flag = 0;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_DEBUG,
                                  "Gemini audio queue flushed\n");
            }
            switch_mutex_unlock(ctx->mutex);
        }
        break;

    case SWITCH_ABC_TYPE_CLOSE:
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(ctx->session), SWITCH_LOG_INFO,
                          "Gemini media bug closing\n");

        /* Signal thread to stop */
        ctx->running = 0;

        /* Close socket to unblock recv() in thread */
        if (ctx->sock) {
            switch_socket_shutdown(ctx->sock, SWITCH_SHUTDOWN_READWRITE);
            switch_socket_close(ctx->sock);
            ctx->sock = NULL;
        }

        /* Clean up resamplers */
        if (ctx->read_resampler) {
            switch_resample_destroy(&ctx->read_resampler);
        }
        if (ctx->write_resampler) {
            switch_resample_destroy(&ctx->write_resampler);
        }

        /* Clean up write codec */
        if (switch_core_codec_ready(&ctx->write_codec)) {
            switch_core_codec_destroy(&ctx->write_codec);
        }

        /* Buffer will be freed when session pool is destroyed */
        break;

    default:
        break;
    }

    return SWITCH_TRUE;
}

/*
 * Application Entry Point
 *
 * Called when ESL executes: execute gemini_live <host> <port>
 * Sets up socket, resamplers, thread, and media bug, then returns immediately.
 */
SWITCH_STANDARD_APP(gemini_live_start)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_memory_pool_t *pool = switch_core_session_get_pool(session);
    switch_codec_implementation_t read_impl = { 0 };
    switch_sockaddr_t *sa = NULL;
    switch_threadattr_t *thd_attr = NULL;
    gemini_ctx_t *ctx = NULL;
    char *host = NULL;
    char *port_str = NULL;
    int port = 0;
    char *argv[2] = { 0 };
    int argc;
    char *mycmd = NULL;

    /* Parse arguments: <host> <port> */
    if (zstr(data)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Usage: gemini_live <host> <port>\n");
        return;
    }

    mycmd = switch_core_session_strdup(session, data);
    argc = switch_split(mycmd, ' ', argv);

    if (argc != 2) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Usage: gemini_live <host> <port>\n");
        return;
    }

    host = argv[0];
    port_str = argv[1];
    port = atoi(port_str);

    if (port <= 0 || port > 65535) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Invalid port: %s\n", port_str);
        return;
    }

    /* Get session codec info */
    if (switch_core_session_get_read_impl(session, &read_impl) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to get read codec implementation\n");
        return;
    }

    /* Allocate context from session pool (auto-freed on hangup) */
    ctx = switch_core_session_alloc(session, sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));

    ctx->session = session;
    ctx->channel = channel;
    ctx->pool = pool;
    ctx->session_rate = read_impl.actual_samples_per_second;
    ctx->read_ptime = read_impl.microseconds_per_packet / 1000;

    /* Calculate frame sizes for 20ms ptime */
    ctx->session_frame_bytes = (ctx->session_rate / 1000) * ctx->read_ptime * sizeof(int16_t);
    ctx->gemini_in_frame_bytes = (GEMINI_INPUT_RATE / 1000) * ctx->read_ptime * sizeof(int16_t);
    ctx->gemini_out_frame_bytes = (GEMINI_OUTPUT_RATE / 1000) * ctx->read_ptime * sizeof(int16_t);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "Gemini Live: session_rate=%u, ptime=%ums, frame_bytes=%u\n",
                      ctx->session_rate, ctx->read_ptime, ctx->session_frame_bytes);

    /* Create mutex */
    if (switch_mutex_init(&ctx->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to create mutex\n");
        return;
    }

    /* Create audio queue */
    if (switch_buffer_create_dynamic(&ctx->audio_queue, 4096, 8192, GEMINI_QUEUE_MAX_SIZE) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to create audio queue\n");
        return;
    }

    /* Create resamplers if needed */
    if (ctx->session_rate != GEMINI_INPUT_RATE) {
        /* Session → 16kHz for sending to Gemini */
        if (switch_resample_create(&ctx->read_resampler,
                                   ctx->session_rate,
                                   GEMINI_INPUT_RATE,
                                   (uint32_t)(GEMINI_INPUT_RATE * 0.02 * 2), /* 20ms buffer */
                                   SWITCH_RESAMPLE_QUALITY,
                                   1) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "Failed to create read resampler (%u → %u)\n",
                              ctx->session_rate, GEMINI_INPUT_RATE);
            return;
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "Created read resampler: %u → %u Hz\n",
                          ctx->session_rate, GEMINI_INPUT_RATE);
    }

    if (ctx->session_rate != GEMINI_OUTPUT_RATE) {
        /* 24kHz → Session for receiving from Gemini */
        if (switch_resample_create(&ctx->write_resampler,
                                   GEMINI_OUTPUT_RATE,
                                   ctx->session_rate,
                                   (uint32_t)(ctx->session_rate * 0.02 * 2), /* 20ms buffer */
                                   SWITCH_RESAMPLE_QUALITY,
                                   1) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "Failed to create write resampler (%u → %u)\n",
                              GEMINI_OUTPUT_RATE, ctx->session_rate);
            if (ctx->read_resampler) {
                switch_resample_destroy(&ctx->read_resampler);
            }
            return;
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "Created write resampler: %u → %u Hz\n",
                          GEMINI_OUTPUT_RATE, ctx->session_rate);
    }

    /* Resolve host address */
    if (switch_sockaddr_info_get(&sa, host, SWITCH_UNSPEC, port, 0, pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to resolve address: %s:%d\n", host, port);
        goto error;
    }

    /* Create socket */
    if (switch_socket_create(&ctx->sock, switch_sockaddr_get_family(sa),
                             SOCK_STREAM, SWITCH_PROTO_TCP, pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to create socket\n");
        goto error;
    }

    /* Connect to sidecar */
    if (switch_socket_connect(ctx->sock, sa) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to connect to %s:%d\n", host, port);
        goto error;
    }

    /* CRITICAL: Disable Nagle's algorithm for low latency */
    switch_socket_opt_set(ctx->sock, SWITCH_SO_TCP_NODELAY, 1);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "Connected to Gemini sidecar at %s:%d\n", host, port);

    /* Initialize write codec for direct frame injection (L16 at session rate) */
    if (switch_core_codec_init(&ctx->write_codec,
                               "L16",
                               NULL,
                               NULL,
                               ctx->session_rate,
                               ctx->read_ptime,
                               1, /* channels */
                               SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
                               NULL,
                               pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to initialize write codec\n");
        goto error;
    }
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "Initialized L16 write codec at %uHz\n", ctx->session_rate);

    /* Initialize write frame */
    memset(&ctx->write_frame, 0, sizeof(ctx->write_frame));
    ctx->write_frame.codec = &ctx->write_codec;
    ctx->write_frame.data = ctx->write_frame_data;
    ctx->write_frame.buflen = sizeof(ctx->write_frame_data);

    /* Start socket reader thread */
    ctx->running = 1;
    switch_threadattr_create(&thd_attr, pool);
    switch_threadattr_detach_set(thd_attr, 1);
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

    if (switch_thread_create(&ctx->thread, thd_attr, gemini_socket_thread, ctx, pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to create socket thread\n");
        ctx->running = 0;
        goto error;
    }

    /* Attach media bug */
    if (switch_core_media_bug_add(session, GEMINI_BUG_NAME, NULL,
                                  gemini_media_callback, ctx, 0,
                                  SMBF_READ_REPLACE | SMBF_WRITE_REPLACE | SMBF_NO_PAUSE,
                                  &ctx->bug) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to attach media bug\n");
        ctx->running = 0;
        goto error;
    }

    /* Store context in channel for API access */
    switch_channel_set_private(channel, GEMINI_PRIVATE, ctx);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "Gemini Live audio pipe established\n");

    return;

error:
    if (ctx->sock) {
        switch_socket_close(ctx->sock);
        ctx->sock = NULL;
    }
    if (ctx->read_resampler) {
        switch_resample_destroy(&ctx->read_resampler);
    }
    if (ctx->write_resampler) {
        switch_resample_destroy(&ctx->write_resampler);
    }
    if (switch_core_codec_ready(&ctx->write_codec)) {
        switch_core_codec_destroy(&ctx->write_codec);
    }
    if (ctx->audio_queue) {
        switch_buffer_destroy(&ctx->audio_queue);
    }
}

/*
 * API: uuid_gemini_flush
 *
 * Flushes the audio queue for a session. Called by sidecar via ESL when
 * Gemini signals end-of-turn or interruption.
 *
 * Usage: uuid_gemini_flush <uuid>
 */
SWITCH_STANDARD_API(uuid_gemini_flush_function)
{
    switch_core_session_t *target_session = NULL;
    switch_channel_t *channel = NULL;
    gemini_ctx_t *ctx = NULL;
    char *uuid = NULL;
    char *mycmd = NULL;

    if (zstr(cmd)) {
        stream->write_function(stream, "-ERR Usage: uuid_gemini_flush <uuid>\n");
        return SWITCH_STATUS_SUCCESS;
    }

    /* Duplicate command string since we may modify it */
    mycmd = strdup(cmd);
    if (!mycmd) {
        stream->write_function(stream, "-ERR Memory allocation failed\n");
        return SWITCH_STATUS_SUCCESS;
    }

    /* Trim whitespace */
    uuid = mycmd;
    while (*uuid == ' ' || *uuid == '\t') uuid++;
    {
        char *end = uuid + strlen(uuid) - 1;
        while (end > uuid && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
            *end-- = '\0';
        }
    }

    target_session = switch_core_session_locate(uuid);
    if (!target_session) {
        stream->write_function(stream, "-ERR Session not found: %s\n", uuid);
        free(mycmd);
        return SWITCH_STATUS_SUCCESS;
    }

    channel = switch_core_session_get_channel(target_session);
    ctx = switch_channel_get_private(channel, GEMINI_PRIVATE);

    if (!ctx) {
        stream->write_function(stream, "-ERR Gemini not active on session: %s\n", uuid);
        switch_core_session_rwunlock(target_session);
        free(mycmd);
        return SWITCH_STATUS_SUCCESS;
    }

    /* Set flush flag - media callback will clear the queue */
    switch_mutex_lock(ctx->mutex);
    ctx->flush_flag = 1;
    switch_mutex_unlock(ctx->mutex);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(target_session), SWITCH_LOG_INFO,
                      "Gemini flush requested\n");

    stream->write_function(stream, "+OK\n");

    switch_core_session_rwunlock(target_session);
    free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

/*
 * API: uuid_gemini_stop
 *
 * Stops the Gemini audio pipe for a session.
 *
 * Usage: uuid_gemini_stop <uuid>
 */
SWITCH_STANDARD_API(uuid_gemini_stop_function)
{
    switch_core_session_t *target_session = NULL;
    switch_channel_t *channel = NULL;
    gemini_ctx_t *ctx = NULL;
    char *uuid = NULL;
    char *mycmd = NULL;

    if (zstr(cmd)) {
        stream->write_function(stream, "-ERR Usage: uuid_gemini_stop <uuid>\n");
        return SWITCH_STATUS_SUCCESS;
    }

    /* Duplicate command string since we may modify it */
    mycmd = strdup(cmd);
    if (!mycmd) {
        stream->write_function(stream, "-ERR Memory allocation failed\n");
        return SWITCH_STATUS_SUCCESS;
    }

    /* Trim whitespace */
    uuid = mycmd;
    while (*uuid == ' ' || *uuid == '\t') uuid++;
    {
        char *end = uuid + strlen(uuid) - 1;
        while (end > uuid && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
            *end-- = '\0';
        }
    }

    target_session = switch_core_session_locate(uuid);
    if (!target_session) {
        stream->write_function(stream, "-ERR Session not found: %s\n", uuid);
        free(mycmd);
        return SWITCH_STATUS_SUCCESS;
    }

    channel = switch_core_session_get_channel(target_session);
    ctx = switch_channel_get_private(channel, GEMINI_PRIVATE);

    if (!ctx) {
        stream->write_function(stream, "-ERR Gemini not active on session: %s\n", uuid);
        switch_core_session_rwunlock(target_session);
        free(mycmd);
        return SWITCH_STATUS_SUCCESS;
    }

    /* Remove media bug (triggers SWITCH_ABC_TYPE_CLOSE) */
    if (ctx->bug) {
        switch_core_media_bug_remove(target_session, &ctx->bug);
    }

    switch_channel_set_private(channel, GEMINI_PRIVATE, NULL);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(target_session), SWITCH_LOG_INFO,
                      "Gemini stopped\n");

    stream->write_function(stream, "+OK\n");

    switch_core_session_rwunlock(target_session);
    free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

/*
 * Module Load
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_gemini_live_load)
{
    switch_application_interface_t *app_interface;
    switch_api_interface_t *api_interface;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    /* Register application */
    SWITCH_ADD_APP(app_interface, "gemini_live",
                   "Gemini Live Audio Pipe",
                   "Ultra-low-latency bidirectional audio streaming for Gemini Live API",
                   gemini_live_start,
                   "<host> <port>",
                   SAF_MEDIA_TAP);

    /* Register API commands */
    SWITCH_ADD_API(api_interface, "uuid_gemini_flush",
                   "Flush Gemini audio queue (auto-resumes after 500ms)",
                   uuid_gemini_flush_function,
                   "<uuid>");

    SWITCH_ADD_API(api_interface, "uuid_gemini_stop",
                   "Stop Gemini audio pipe",
                   uuid_gemini_stop_function,
                   "<uuid>");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "mod_gemini_live loaded\n");

    return SWITCH_STATUS_SUCCESS;
}

/*
 * Module Shutdown
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_gemini_live_shutdown)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "mod_gemini_live unloaded\n");

    return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
