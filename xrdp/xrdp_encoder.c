/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Laxmikant Rashinkar 2004-2014
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Encoder
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include "fifo.h"
#include "ms-rdpbcgr.h"
#include "string_calls.h"
#include "thread_calls.h"
#include "xrdp.h"
#include "xrdp_egfx.h"
#include "xrdp_encoder.h"

#ifdef XRDP_RFXCODEC
#include "rfxcodec_encode.h"
#endif

#ifdef XRDP_VANILLA_NVIDIA_CODEC
#include "xrdp_encoder_nvenc.h"
#endif

#ifdef XRDP_X264
#include "xrdp_encoder_x264.h"
#endif

#ifdef XRDP_OPENH264
#include "xrdp_encoder_openh264.h"
#endif

#define DEFAULT_XRDP_GFX_FRAMES_IN_FLIGHT 2
/* limits used for validate env var XRDP_GFX_FRAMES_IN_FLIGHT */
#define MIN_XRDP_GFX_FRAMES_IN_FLIGHT 1
#define MAX_XRDP_GFX_FRAMES_IN_FLIGHT 16

#define DEFAULT_XRDP_GFX_MAX_COMPRESSED_BYTES (3 * 1024 * 1024)
/* limits used for validate env var XRDP_GFX_MAX_COMPRESSED_BYTES */
#define MIN_XRDP_GFX_MAX_COMPRESSED_BYTES (64 * 1024)
#define MAX_XRDP_GFX_MAX_COMPRESSED_BYTES (256 * 1024 * 1024)

#define XRDP_SURCMD_PREFIX_BYTES 256
#define OUT_DATA_BYTES_DEFAULT_SIZE (16 * 1024 * 1024)

#ifdef XRDP_RFXCODEC
/*
 * LH3 LL3, HH3 HL3, HL2 LH2, LH1 HH2, HH1 HL1
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdprfx/3e9c8af4-7539-4c9d-95de-14b1558b902c
 */

/* standard quality */
static const unsigned char g_rfx_quantization_values_std[] =
{
    0x66, 0x66, 0x77, 0x87, 0x98,
    0x76, 0x77, 0x88, 0x98, 0x99
};

/* low quality */
static const unsigned char g_rfx_quantization_values_lq[] =
{
    0x66, 0x66, 0x77, 0x88, 0x98,
    0x76, 0x77, 0x88, 0x98, 0xA9
};

/* ultra low quality */
static const unsigned char g_rfx_quantization_values_ulq[] =
{
    0x66, 0x66, 0x77, 0x87, 0x98,
    0xBB, 0xBB, 0xBB, 0xBB, 0xBB /* TODO: tentative value */
};
#endif

/*****************************************************************************/
#if defined(XRDP_X264)
static int
process_enc_h264(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
#endif

#if defined(XRDP_RFXCODEC)
static int
process_enc_rfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
#endif

static int
process_enc_egfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);

static int
process_enc_jpg(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);

/*****************************************************************************/
/* Item destructor for self->fifo_to_proc */
static void
xrdp_enc_data_destructor(void *item, void *closure)
{
    XRDP_ENC_DATA *enc = (XRDP_ENC_DATA *)item;

    if (ENC_IS_BIT_SET(enc->flags, ENC_FLAGS_GFX_BIT))
    {
        g_free(enc->u.gfx.cmd);
    }
    else
    {
        g_free(enc->u.sc.drects);
        g_free(enc->u.sc.crects);
    }

    g_free(enc);
}

/* Item destructor for self->fifo_processed */
static void
xrdp_enc_data_done_destructor(void *item, void *closure)
{
    XRDP_ENC_DATA_DONE *enc_done = (XRDP_ENC_DATA_DONE *)item;
    g_free(enc_done->comp_pad_data);
    g_free(enc_done);
}

/*****************************************************************************/
struct xrdp_enc_rect calculate_bounding_box(short* boxes, int numBoxes)
{
    struct xrdp_enc_rect boundingBox;

    boundingBox.x = INT16_MAX;
    boundingBox.y = INT16_MAX;
    boundingBox.cx = INT16_MIN;
    boundingBox.cy = INT16_MIN;

    for (int i = 0; i < numBoxes; ++i)
    {
        int location = i * 4;
        short x = boxes[location + 0];
        short y = boxes[location + 1];
        short cx = boxes[location + 2];
        short cy = boxes[location + 3];

        boundingBox.x = MIN(boundingBox.x, x);
        boundingBox.y = MIN(boundingBox.y, y);
        boundingBox.cx = MAX(boundingBox.cx, cx);
        boundingBox.cy = MAX(boundingBox.cy, cy);
    }

    return boundingBox;
}

/*****************************************************************************/
struct xrdp_encoder *
xrdp_encoder_create(struct xrdp_mm *mm)
{
    LOG_DEVEL(LOG_LEVEL_TRACE, "xrdp_encoder_create:");

    struct xrdp_encoder *self;
    struct xrdp_client_info *client_info;
    char buf[1024];
    int pid;

    client_info = mm->wm->client_info;

    /* RemoteFX 7.1 requires LAN but GFX does not */
    if (client_info->mcs_connection_type != CONNECTION_TYPE_LAN)
    {
        if ((mm->egfx_flags & (XRDP_EGFX_H264 | XRDP_EGFX_RFX_PRO)) == 0)
        {
            return 0;
        }
    }
    if (client_info->bpp < 24)
    {
        return 0;
    }

    self = g_new0(struct xrdp_encoder, 1);

    if (self == NULL)
    {
        return NULL;
    }

    self->mm = mm;
    self->process_enc = process_enc_egfx;

    if (client_info->jpeg_codec_id != 0)
    {
        LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: starting jpeg codec session");

        self->codec_id = client_info->jpeg_codec_id;
        self->in_codec_mode = 1;
        self->codec_quality = client_info->jpeg_prop[0];
        client_info->capture_code = CC_SIMPLE;
        client_info->capture_format = XRDP_a8b8g8r8;
        self->process_enc = process_enc_jpg;
    }

#if defined(XRDP_X264)
    else if (mm->egfx_flags & XRDP_EGFX_H264)
    {
        LOG(LOG_LEVEL_INFO,
            "xrdp_encoder_create: starting h264 codec session gfx");

        self->in_codec_mode = 1;
        client_info->capture_code = CC_GFX_A2;
        client_info->capture_format = XRDP_nv12_709fr;
        self->gfx = 1;
    }
    else if (client_info->h264_codec_id != 0)
    {
        LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: starting h264 codec session");
        self->codec_id = client_info->h264_codec_id;
        self->in_codec_mode = 1;
        client_info->capture_code = CC_SUF_A2;
        client_info->capture_format =
#if defined(AVC444)
            /* XRDP_yuv444_709fr */
            (32 << 24) | (67 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
#else
            /* XRDP_nv12_709fr */
            (12 << 24) | (66 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
#endif

        self->process_enc = process_enc_h264;
    }
#endif

#if defined(XRDP_RFXCODEC)
    if (mm->egfx_flags & XRDP_EGFX_RFX_PRO)
    {
        LOG(LOG_LEVEL_INFO,
            "xrdp_encoder_create: starting gfx rfx pro codec session");

        client_info->capture_code = CC_GFX_PRO;
        self->gfx = 1;
        self->in_codec_mode = 1;
        self->num_quants = 2;
        self->process_enc = process_enc_rfx;
        self->quant_idx_u = 1;
        self->quant_idx_v = 1;
        self->quant_idx_y = 0;

        switch (client_info->mcs_connection_type)
        {
            case CONNECTION_TYPE_MODEM:
            case CONNECTION_TYPE_BROADBAND_LOW:
            case CONNECTION_TYPE_SATELLITE:
                self->quants = (const char *) g_rfx_quantization_values_ulq;
            break;
            case CONNECTION_TYPE_BROADBAND_HIGH:
            case CONNECTION_TYPE_WAN:
                self->quants = (const char *) g_rfx_quantization_values_lq;
            break;
            case CONNECTION_TYPE_LAN:
            case CONNECTION_TYPE_AUTODETECT: /* not implemented yet */
            default:
                self->quants = (const char *) g_rfx_quantization_values_std;

        }
    }
    else if (client_info->rfx_codec_id != 0)
    {
        LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: starting rfx codec session");

        client_info->capture_code = CC_SUF_RFX;
        self->codec_id = client_info->rfx_codec_id;
        self->in_codec_mode = 1;
        self->process_enc = process_enc_rfx;

        self->codec_handle_rfx = rfxcodec_encode_create(mm->wm->screen->width,
                                                        mm->wm->screen->height,
                                                        RFX_FORMAT_YUV,
                                                        0);
    }
#elif defined(XRDP_X264)
    // self->codec_handle_x264 = xrdp_encoder_x264_create(); /* TODO */

    else if (client_info->h264_codec_id != 0)
    {
        LOG_DEVEL(LOG_LEVEL_INFO,
                  "xrdp_encoder_create: starting h264 codec session");

        client_info->capture_code = 3;
        client_info->capture_format =
#if defined(AVC444)
            /* XRDP_yuv444_709fr */
            (32 << 24) | (67 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
#else
            /* XRDP_nv12 */
            (12 << 24) | (64 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
#endif

        self->codec_id = client_info->h264_codec_id;
        self->in_codec_mode = 1;
        self->process_enc = process_enc_h264;
    }
    else if (client_info->jpeg_codec_id != 0)
    {
        LOG_DEVEL(LOG_LEVEL_INFO,
                  "xrdp_encoder_create: starting jpeg codec session");

        client_info->capture_code = 0;
        client_info->capture_format =
            /* XRDP_a8b8g8r8 */
            (32 << 24) | (3 << 16) | (8 << 12) | (8 << 8) | (8 << 4) | 8;
        self->codec_id = client_info->jpeg_codec_id;
        self->codec_quality = client_info->jpeg_prop[0];
        self->in_codec_mode = 1;
        self->process_enc = process_enc_jpg;
#elif defined(XRDP_OPENH264)
        self->codec_handle_openh264 = xrdp_encoder_openh264_create();
#elif defined(XRDP_VANILLA_NVIDIA_CODEC)
        self->codec_handle_nvenc = xrdp_encoder_nvenc_create();
    }
    else
    {
        g_free(self);

        return 0;
    }
#endif

    LOG_DEVEL(LOG_LEVEL_INFO,
              "init_xrdp_encoder: initializing encoder codec_id %d",
              self->codec_id);

    /* setup required FIFOs */
    self->fifo_to_proc = fifo_create(xrdp_enc_data_destructor);
    self->fifo_processed = fifo_create(xrdp_enc_data_done_destructor);
    self->mutex = tc_mutex_create();

    pid = g_getpid();

    /* setup wait objects for signalling */
    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_event_to_proc", pid);
    self->xrdp_encoder_event_to_proc = g_create_wait_obj(buf);

    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_event_processed", pid);
    self->xrdp_encoder_event_processed = g_create_wait_obj(buf);

    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_term", pid);
    self->xrdp_encoder_term_request = g_create_wait_obj(buf);
    self->xrdp_encoder_term_done = g_create_wait_obj(buf);

    if (client_info->gfx)
    {
        const char *env_var = g_getenv("XRDP_GFX_FRAMES_IN_FLIGHT");
        self->frames_in_flight = DEFAULT_XRDP_GFX_FRAMES_IN_FLIGHT;

        if (env_var != NULL)
        {
            int fif = g_atoix(env_var);
            if (fif >= MIN_XRDP_GFX_FRAMES_IN_FLIGHT &&
                    fif <= MAX_XRDP_GFX_FRAMES_IN_FLIGHT)
            {
                self->frames_in_flight = fif;
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_FRAMES_IN_FLIGHT set to %d", fif);
            }
            else
            {
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_FRAMES_IN_FLIGHT set but invalid %s",
                    env_var);
            }
        }

        env_var = g_getenv("XRDP_GFX_MAX_COMPRESSED_BYTES");
        self->max_compressed_bytes = DEFAULT_XRDP_GFX_MAX_COMPRESSED_BYTES;

        if (env_var != NULL)
        {
            int mcb = g_atoix(env_var);
            if (mcb >= MIN_XRDP_GFX_MAX_COMPRESSED_BYTES &&
                    mcb <= MAX_XRDP_GFX_MAX_COMPRESSED_BYTES)
            {
                self->max_compressed_bytes = mcb;
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_MAX_COMPRESSED_BYTES set to %d", mcb);
            }
            else
            {
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_MAX_COMPRESSED_BYTES set but invalid %s",
                    env_var);
            }
        }

        LOG_DEVEL(LOG_LEVEL_INFO,
                  "Using %d max_compressed_bytes for encoder",
                  self->max_compressed_bytes);
    }
    else
    {
        self->frames_in_flight = client_info->max_unacknowledged_frame_count;
        self->max_compressed_bytes = client_info->max_fastpath_frag_bytes & ~15;
    }

    /* make sure frames_in_flight is at least 1 */
    self->frames_in_flight = MAX(self->frames_in_flight, 1);

    /* create thread to process messages */
    tc_thread_create(proc_enc_msg, self);

    return self;
}

/*****************************************************************************/
void
xrdp_encoder_delete(struct xrdp_encoder *self)
{
    int index;
    (void)index;

    LOG_DEVEL(LOG_LEVEL_INFO, "xrdp_encoder_delete:");

    if (self == 0)
    {
        return;
    }
    if (self->in_codec_mode == 0)
    {
        return;
    }
    /* tell worker thread to shut down */
    g_set_wait_obj(self->xrdp_encoder_term_request);
    g_obj_wait(&self->xrdp_encoder_term_done, 1, NULL, 0, 5000);

    if (!g_is_wait_obj_set(self->xrdp_encoder_term_done))
    {
        LOG(LOG_LEVEL_WARNING, "Encoder failed to shut down cleanly");
    }

#if defined(XRDP_RFXCODEC)
    for (index = 0; index < 16; index++)
    {
        if (self->codec_handle_prfx_gfx[index] != NULL)
        {
            rfxcodec_encode_destroy(self->codec_handle_prfx_gfx[index]);
        }
    }
    if (self->codec_handle_rfx != NULL)
    {
        rfxcodec_encode_destroy(self->codec_handle_rfx);
    }
#elif defined(XRDP_X264)
    for (index = 0; index < 16; index++)
    {
        if (self->codec_handle_h264_gfx[index] != NULL)
        {
            xrdp_encoder_x264_delete(self->codec_handle_h264_gfx[index]);
        }
    }
    else if (self->process_enc == process_enc_h264)
    {
        xrdp_encoder_x264_delete(self->codec_handle_h264);
    }
#elif defined(XRDP_OPENH264)
    else if (self->process_enc == process_enc_h264)
    {
        xrdp_encoder_openh264_delete(self->codec_handle);
    }
#elif defined(XRDP_VANILLA_NVIDIA_CODEC)
    else if (self->process_enc == process_enc_h264)
    {
        xrdp_encoder_nvenc_delete(self->codec_handle);
    }
#endif

    /* destroy wait objects used for signalling */
    g_delete_wait_obj(self->xrdp_encoder_event_to_proc);
    g_delete_wait_obj(self->xrdp_encoder_event_processed);
    g_delete_wait_obj(self->xrdp_encoder_term_request);
    g_delete_wait_obj(self->xrdp_encoder_term_done);

    /* cleanup fifos */
    fifo_delete(self->fifo_to_proc, NULL);
    fifo_delete(self->fifo_processed, NULL);
    tc_mutex_delete(self->mutex);
    g_free(self);
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_jpg(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    XRDP_ENC_DATA_DONE *enc_done;
    char *out_data;
    int count;
    int cx;
    int cy;
    int error;
    int index;
    int out_data_bytes;
    int quality;
    int x;
    int y;
    struct fifo *fifo_processed;
    tbus event_processed;
    tbus mutex;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_jpg:");

    count = enc->u.sc.num_crects;
    event_processed = self->xrdp_encoder_event_processed;
    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    quality = self->codec_quality;

    for (index = 0; index < count; index++)
    {
        x = enc->u.sc.crects[index * 4 + 0];
        y = enc->u.sc.crects[index * 4 + 1];
        cx = enc->u.sc.crects[index * 4 + 2];
        cy = enc->u.sc.crects[index * 4 + 3];

        if (cx < 1 || cy < 1)
        {
            LOG_DEVEL(LOG_LEVEL_WARNING, "process_enc_jpg: error 1");
            continue;
        }

        LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_jpg: x %d y %d cx %d cy %d",
                  x, y, cx, cy);

        out_data_bytes = MAX((cx + 4) * cy * 4, 8192);
        if (
            (out_data_bytes < 1)
            || (out_data_bytes > OUT_DATA_BYTES_DEFAULT_SIZE))
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "process_enc_jpg: error 2");
            return 1;
        }

        out_data = (char *) g_malloc(out_data_bytes
                                     + XRDP_SURCMD_PREFIX_BYTES + 2, 0);
        if (out_data == 0)
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "process_enc_jpg: error 3");
            return 1;
        }

        out_data[256] = 0; /* header bytes */
        out_data[257] = 0;

        error = libxrdp_codec_jpeg_compress(self->mm->wm->session, 0,
                                            enc->u.sc.data,
                                            enc->u.sc.width,
                                            enc->u.sc.height,
                                            enc->u.sc.width * 4,
                                            x, y, cx, cy,
                                            quality,
                                            out_data
                                                + XRDP_SURCMD_PREFIX_BYTES
                                                + 2,
                                            &out_data_bytes);

        if (error < 0)
        {
            LOG_DEVEL(LOG_LEVEL_ERROR,
                      "process_enc_jpg: jpeg error %d "
                      "bytes %d", error, out_data_bytes);

            g_free(out_data);

            return 1;
        }

        LOG_DEVEL(LOG_LEVEL_WARNING,
                  "jpeg error %d bytes %d",
                  error,
                  out_data_bytes);

        enc_done = (XRDP_ENC_DATA_DONE *)
                   g_malloc(sizeof(XRDP_ENC_DATA_DONE), 1);

        enc_done->comp_bytes = out_data_bytes + 2;
        enc_done->comp_pad_data = out_data;
        enc_done->enc = enc;
        enc_done->last = index == (enc->u.sc.num_crects - 1);
        enc_done->pad_bytes = 256;

        enc_done->rect.x = x;
        enc_done->rect.y = y;
        enc_done->rect.cx = cx;
        enc_done->rect.cy = cy;

        /* done with msg */
        /* inform main thread done */
        tc_mutex_lock(mutex);
        fifo_add_item(fifo_processed, enc_done);
        tc_mutex_unlock(mutex);

        /* signal completion for main thread */
        g_set_wait_obj(event_processed);
    }

    return 0;
}

#if defined(XRDP_RFXCODEC)
/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_rfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    XRDP_ENC_DATA_DONE *enc_done;
    char *out_data;
    int all_tiles_written;
    int alloc_bytes;
    int count;
    int cx;
    int cy;
    int encode_flags;
    int encode_passes;
    int finished;
    int index;
    int out_data_bytes;
    int tiles_left;
    int tiles_written;
    int x;
    int y;
    struct fifo *fifo_processed;
    struct rfx_rect *rfxrects;
    struct rfx_tile *tiles;
    tbus event_processed;
    tbus mutex;

    LOG_DEVEL(LOG_LEVEL_DEBUG,
              "process_enc_rfx: num_crects %d num_drects %d",
              enc->u.sc.num_crects, enc->u.sc.num_drects);

    event_processed = self->xrdp_encoder_event_processed;
    fifo_processed = self->fifo_processed;
    mutex = self->mutex;

    all_tiles_written = 0;
    encode_passes = 0;
    do
    {
        out_data = NULL;
        out_data_bytes = 0;
        tiles_left = enc->u.sc.num_crects - all_tiles_written;
        tiles_written = 0;

        if ((tiles_left > 0) && (enc->u.sc.num_drects > 0))
        {
            alloc_bytes = XRDP_SURCMD_PREFIX_BYTES;
            alloc_bytes += self->max_compressed_bytes;
            alloc_bytes += sizeof(struct rfx_tile) * tiles_left +
                           sizeof(struct rfx_rect) * enc->u.sc.num_drects;

            out_data = g_new(char, alloc_bytes);

            if (out_data != NULL)
            {
                tiles = (struct rfx_tile *)
                        (out_data + XRDP_SURCMD_PREFIX_BYTES +
                         self->max_compressed_bytes);
                rfxrects = (struct rfx_rect *) (tiles + tiles_left);
                count = tiles_left;

                for (index = 0; index < count; index++)
                {
                    x = enc->u.sc.crects[(index + all_tiles_written) * 4 + 0];
                    y = enc->u.sc.crects[(index + all_tiles_written) * 4 + 1];
                    cx = enc->u.sc.crects[(index + all_tiles_written) * 4 + 2];
                    cy = enc->u.sc.crects[(index + all_tiles_written) * 4 + 3];

                    tiles[index].x = x;
                    tiles[index].y = y;
                    tiles[index].cx = cx;
                    tiles[index].cy = cy;
                    tiles[index].quant_y = self->quant_idx_y;
                    tiles[index].quant_cb = self->quant_idx_u;
                    tiles[index].quant_cr = self->quant_idx_v;
                }

                count = enc->u.sc.num_drects;

                for (index = 0; index < count; index++)
                {
                    x = enc->u.sc.drects[index * 4 + 0];
                    y = enc->u.sc.drects[index * 4 + 1];
                    cx = enc->u.sc.drects[index * 4 + 2];
                    cy = enc->u.sc.drects[index * 4 + 3];

                    rfxrects[index].x = x;
                    rfxrects[index].y = y;
                    rfxrects[index].cx = cx;
                    rfxrects[index].cy = cy;
                }

                out_data_bytes = self->max_compressed_bytes;

                encode_flags = 0;
                if ((enc->flags & KEY_FRAME_REQUESTED) && encode_passes == 0)
                {
                    encode_flags = RFX_FLAGS_PRO_KEY;
                }
                tiles_written = rfxcodec_encode_ex(self->codec_handle_rfx,
                                                   out_data
                                                   + XRDP_SURCMD_PREFIX_BYTES,
                                                   &out_data_bytes,
                                                   enc->u.sc.data,
                                                   enc->u.sc.width,
                                                   enc->u.sc.height,
                                                   ((enc->u.sc.width + 63)
                                                       & ~63) * 4,
                                                   rfxrects,
                                                   enc->u.sc.num_drects,
                                                   tiles,
                                                   enc->u.sc.num_crects,
                                                   self->quants,
                                                   self->num_quants,
                                                   encode_flags);
            }

            ++encode_passes;
        }

        LOG_DEVEL(LOG_LEVEL_DEBUG,
                  "process_enc_rfx: rfxcodec_encode tiles_written %d",
                  tiles_written);

        /* only if enc_done->comp_bytes is not zero is something sent
           to the client but you must always send something back even
           on error so Xorg can get ack */
        enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);

        if (enc_done == NULL)
        {
            return 1;
        }

        enc_done->comp_bytes = tiles_written > 0 ? out_data_bytes : 0;
        enc_done->comp_pad_data = out_data;
        enc_done->enc = enc;
        enc_done->frame_id = enc->u.sc.frame_id;
        enc_done->pad_bytes = XRDP_SURCMD_PREFIX_BYTES;

        enc_done->rect.x = enc->u.sc.left;
        enc_done->rect.y = enc->u.sc.top;
        enc_done->rect.cx = self->mm->wm->screen->width;
        enc_done->rect.cy = self->mm->wm->screen->height;

        if (self->gfx)
        {
            enc_done->flags = 2;
        }

        enc_done->continuation = all_tiles_written > 0;

        if (tiles_written > 0)
        {
            all_tiles_written += tiles_written;
        }

        finished = (
            all_tiles_written == enc->u.sc.num_crects) || (tiles_written < 0
        );

        enc_done->last = finished;

        /* done with msg */
        /* inform main thread done */
        tc_mutex_lock(mutex);
        fifo_add_item(fifo_processed, enc_done);
        tc_mutex_unlock(mutex);
    }

    while (!finished);

    /* signal completion for main thread */
    g_set_wait_obj(event_processed);

    return 0;
}
#endif

#if defined(XRDP_X264)
/*****************************************************************************/
static int
out_RFX_AVC420_METABLOCK(struct xrdp_egfx_rect *dst_rect,
                         struct stream *s,
                         struct xrdp_egfx_rect *rects,
                         int num_rects)
{
    int count;
    int index;
    struct xrdp_rect rect;
    struct xrdp_region *reg;

    /* RFX_AVC420_METABLOCK */
    s_push_layer(s, iso_hdr, 4); /* numRegionRects, set later */

    reg = xrdp_region_create(NULL);

    if (reg == NULL)
    {
        return 1;
    }

    for (index = 0; index < num_rects; index++)
    {
        rect.top = MAX(
            0, rects[index].y1 - dst_rect->y1 - 1
        );

        rect.right = MIN(
            dst_rect->x2 - dst_rect->x1, rects[index].x2 - dst_rect->x1 + 1
        );

        rect.bottom = MIN(
            dst_rect->y2 - dst_rect->y1, rects[index].y2 - dst_rect->y1 + 1
        );

        rect.left = MAX(
            0, rects[index].x1 - dst_rect->x1 - 1
        );

        xrdp_region_add_rect(reg, &rect);
    }

    index = 0;

    while (xrdp_region_get_rect(reg, index, &rect) == 0)
    {
        out_uint16_le(s, rect.top);
        out_uint16_le(s, rect.left);
        out_uint16_le(s, rect.bottom);
        out_uint16_le(s, rect.right);

        index++;
    }

    xrdp_region_delete(reg);

    count = index;

    while (index > 0)
    {
        out_uint8(s, 23); /* qp */
        out_uint8(s, 100); /* quality level 0..100 */

        index--;
    }

    s_push_layer(s, mcs_hdr, 0);
    s_pop_layer(s, iso_hdr);
    out_uint32_le(s, count); /* numRegionRects */
    s_pop_layer(s, mcs_hdr);

    return 0;
}
#endif

#if defined(XRDP_X264) \
    || defined(XRDP_OPENH264) \
    || defined(XRDP_VANILLA_NVIDIA_CODEC)
static int
build_rfx_avc420_metablock(
    struct stream *s, short *rrects, int rcount, int width, int height)
{
    const uint8_t p = 0; // Progressively encoded flag.
    const uint8_t qp = 22; // Default set by Microsoft.
    const uint8_t r = 0; // Required to be 0.
    int index;
    int qpVal = 0;
    int x, y, cx, cy;
    qpVal |= (p & 1) << 7;
    qpVal |= (r & 1) << 6;
    qpVal |= qp & 0x3F;
    struct xrdp_enc_rect rect;

    out_uint32_le(s, rcount); /* numRegionRects */

    for (index = 0; index < rcount; index++)
    {
        int location = index * 4;
        x = rrects[location + 0];
        y = rrects[location + 1];
        cx = rrects[location + 2];
        cy = rrects[location + 3];

        rect.x = MAX(0, x);
        rect.y = MAX(0, y);
        rect.cx = MIN(x + cx, width);
        rect.cy = MIN(y + cy, height);

        /* RDPGFX_RECT16 */
        out_uint16_le(s, rect.x);
        out_uint16_le(s, rect.y);
        out_uint16_le(s, rect.cx);
        out_uint16_le(s, rect.cy);
    }

    for (index = 0; index < rcount; index++)
    {
        // 2.2.4.4.2 RDPGFX_AVC420_QUANT_QUALITY
        out_uint8(s, qpVal); /* qp */
        out_uint8(s, 100); /* quality level 0..100 (Microsoft uses 100) */
    }

    int comp_bytes_pre = 4 + rcount * 8 + rcount * 2;

    return comp_bytes_pre;
}

static XRDP_ENC_DATA_DONE *
build_enc_h264_avc444_yuv420_and_chroma420_stream(
    struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    XRDP_ENC_DATA_DONE *enc_done;
    char *out_data;
    int comp_bytes_pre;
    int enc_done_flags;
    int error;
    int index;
    int out_data_bytes;
    int rcount;
    int scr_height;
    int scr_width;
    short *rrects;
    struct stream *s;
    struct stream ls;
    struct xrdp_enc_gfx_cmd *enc_gfx_cmd = &(enc->u.gfx);

    LOG(LOG_LEVEL_DEBUG,
        "process_enc_x264: num_crects %d num_drects %d",
        enc->u.sc.num_crects, enc->u.sc.num_drects);

    rcount = enc->u.sc.num_drects;
    rrects = enc->u.sc.drects;
    scr_height = self->mm->wm->screen->height;
    scr_width = self->mm->wm->screen->width;

    if (rcount > 15)
    {
        rcount = enc->u.sc.num_crects;
        rrects = enc->u.sc.crects;
    }

    out_data_bytes = 128 * 1024 * 1024;
    index = XRDP_SURCMD_PREFIX_BYTES + 16 + 2 + enc->u.sc.num_drects * 8;
    out_data = g_new0(char, out_data_bytes + index);

    if (out_data == NULL)
    {
        return 0;
    }

    s = &ls;
    g_memset(s, 0, sizeof(struct stream));

    ls.data = out_data + XRDP_SURCMD_PREFIX_BYTES;
    ls.p = ls.data;
    ls.size = out_data_bytes + index;

#if defined(AVC444)
    comp_bytes_pre = 0;
    out_data_bytes = 0;
#endif

    if (!self->gfx)
    {

        out_uint32_le(s, 0); /* flags */
        out_uint32_le(s, 0); /* session id */

        out_uint16_le(s, enc->u.sc.width); /* src_width */
        out_uint16_le(s, enc->u.sc.height); /* src_height */
        out_uint16_le(s, enc->u.sc.width); /* dst_width */
        out_uint16_le(s, enc->u.sc.height); /* dst_height */

        out_uint16_le(s, rcount);

        for (index = 0; index < rcount; index++)
        {
            out_uint16_le(s, rrects[index * 4 + 0]);
            out_uint16_le(s, rrects[index * 4 + 1]);
            out_uint16_le(s, rrects[index * 4 + 2]);
            out_uint16_le(s, rrects[index * 4 + 3]);
        }

#if defined(AVC444)
        /* size of avc420EncodedBitmapstream1 */
        s_push_layer(s, mcs_hdr, 4);
#else
        s_push_layer(s, iso_hdr, 4);
#endif

        comp_bytes_pre = 4 + 4 + 2 + 2 + 2 + 2 + 2 + rcount * 8 + 4;
        enc_done_flags = 0;
    }
    else
    {
        /* RFX_AVC420_METABLOCK */
        comp_bytes_pre = build_rfx_avc420_metablock(s, rrects, rcount,
                                                    scr_width, scr_height);
        enc_done_flags = 1;
    }

    error = 0;

    if (enc->flags & 1)
    {
        /* already compressed */
        uint8_t *ud = (uint8_t *) (enc->u.sc.data);
        int cbytes = ud[0] | (ud[1] << 8) | (ud[2] << 16) | (ud[3] << 24);

        if ((cbytes < 1) || (cbytes > out_data_bytes))
        {
            LOG(LOG_LEVEL_INFO, "process_enc_h264: bad h264 bytes %d", cbytes);
            g_free(out_data);
            return 0;
        }

        LOG(LOG_LEVEL_DEBUG,
            "process_enc_h264: already compressed and size is %d",
            cbytes);

        out_data_bytes = cbytes;

        g_memcpy(s->p, enc->u.sc.data + 4, out_data_bytes);
    }
    else
    {

#if defined(XRDP_X264)
        error = xrdp_encoder_x264_encode(self->codec_handle_x264,
                                         0, 0, 0,
                                         enc->u.sc.width,
                                         enc->u.sc.height,
                                         enc->u.sc.width, /* twidth */
                                         enc->u.sc.height, /* theight */
                                         0,
                                         enc_gfx_cmd->data,
                                         enc->u.sc.crects,
                                         enc->u.sc.num_crects,
                                         s->p,
                                         &out_data_bytes);
#elif defined(XRDP_OPENH264)
        error = xrdp_encoder_openh264_encode(self->codec_handle_openh264, 0,
                                             enc->u.sc.width, enc->u.sc.height,
                                             0, enc->u.sc.data,
                                             s->p, &out_data_bytes);
#endif

    }
    LOG_DEVEL(LOG_LEVEL_TRACE,
              "process_enc_h264: xrdp_encoder_x264_encode rv %d "
              "out_data_bytes %d width %d height %d",
              error, out_data_bytes, enc->u.sc.width, enc->u.sc.height);

    if (error != 0)
    {
        LOG_DEVEL(LOG_LEVEL_TRACE,
                  "process_enc_h264: xrdp_encoder_x264_encode failed rv %d",
                  error);

        g_free(out_data);

        return 0;
    }

#if !defined(AVC444)
    s->end = s->p + out_data_bytes;
#else
    s->p += out_data_bytes;

    // TODO: Specify LC code here
    uint8_t LC = 0b00;
    uint32_t bitstream = ((uint32_t) (comp_bytes_pre + out_data_bytes)
                         & 0x3FFFFFFFUL)
                         | ((LC & 0x03UL) << 30UL);

    /* chroma 444 */
    comp_bytes_pre = build_rfx_avc420_metablock(s, rrects, rcount,
                                                 scr_width, scr_height);

    out_data_bytes = 128 * 1024 * 1024;

    if (enc->flags & 1)
    {
        /* already compressed */
        uint8_t *ud = (uint8_t *) (enc->u.sc.data);
        int cbytes = ud[0] | (ud[1] << 8) | (ud[2] << 16) | (ud[3] << 24);

        if ((cbytes < 1) || (cbytes > out_data_bytes))
        {
            LOG(LOG_LEVEL_INFO, "process_enc_h264: bad h264 bytes %d", cbytes);
            g_free(out_data);
            return 0;
        }

        LOG(LOG_LEVEL_DEBUG,
            "process_enc_h264: already compressed and size is %d", cbytes);

        out_data_bytes = cbytes;

        g_memcpy(s->p, enc->u.sc.data + 4, out_data_bytes);
    }
    else
    {

#if defined(XRDP_X264)
        error = xrdp_encoder_x264_encode(self->codec_handle_x264,
                                         0, 0, 0,
                                         enc->u.sc.width,
                                         enc->u.sc.height,
                                         enc->u.sc.width, /* twidth */
                                         enc->u.sc.height, /* theight */
                                         0,
                                         enc_gfx_cmd->data,
                                         enc->u.sc.crects,
                                         enc->u.sc.num_crects,
                                         s->p,
                                         &out_data_bytes);
#elif defined(XRDP_OPENH264)
        error = xrdp_encoder_openh264_encode(self->codec_handle_openh264, 0,
                                             enc->u.sc.width,
                                             enc->u.sc.height,
                                             0,
                                             enc->u.sc.data
                                             + (
                                                 enc->u.sc.height
                                                 * enc->u.sc.width
                                             ) * 3 / 2,
                                             s->p,
                                             &out_data_bytes);
#endif

    }

    if (error != 0)
    {
        LOG_DEVEL(LOG_LEVEL_TRACE,
                  "process_enc_h264: xrdp_encoder_x264_encode failed rv %d",
                  error);
        g_free(out_data);

        return 0;
    }

    s->p += out_data_bytes;

    s_push_layer(s, sec_hdr, 0);
    s_pop_layer(s, mcs_hdr);
    out_uint32_le(s, bitstream);
    s_pop_layer(s, sec_hdr);

    s->end = s->p;
#endif

    if (s->iso_hdr != NULL)
    {
        /* not used in gfx */
        s_pop_layer(s, iso_hdr);
        out_uint32_le(s, out_data_bytes);
    }

    enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);

    if (enc_done == NULL)
    {
        return 0;
    }

#if defined(AVC444)
    enc_done->comp_bytes = 4
        + comp_bytes_pre * 2
        + out_data_bytes * 2;
#else
    enc_done->comp_bytes = comp_bytes_pre + out_data_bytes;
#endif

    enc_done->comp_pad_data = out_data;
    enc_done->enc = enc;
    enc_done->flags = enc_done_flags;
    enc_done->last = 1;
    enc_done->pad_bytes = XRDP_SURCMD_PREFIX_BYTES;
    enc_done->rect.cx = scr_width;
    enc_done->rect.cy = scr_height;

    return enc_done;
}

#if defined(AVC444)
static XRDP_ENC_DATA_DONE *
build_enc_h264_avc444_yuv420_stream(
    struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    if (!self->gfx)
    {
        return 0;
    }

    XRDP_ENC_DATA_DONE *enc_done;
    char *out_data;
    int comp_bytes_pre;
    int enc_done_flags;
    int error;
    int index;
    int out_data_bytes;
    int rcount;
    int scr_height;
    int scr_width;
    short *rrects;
    struct stream *s;
    struct stream ls;
    struct xrdp_enc_gfx_cmd *enc_gfx_cmd = &(enc->u.gfx);

    LOG(LOG_LEVEL_DEBUG,
        "build_enc_h264_avc444_yuv420_stream: num_crects %d num_drects %d",
        enc->u.sc.num_crects, enc->u.sc.num_drects);

    error = 0;
    rcount = enc->u.sc.num_drects;
    rrects = enc->u.sc.drects;
    scr_height = self->mm->wm->screen->height;
    scr_width = self->mm->wm->screen->width;

    if (rcount > 15)
    {
        rcount = enc->u.sc.num_crects;
        rrects = enc->u.sc.crects;
    }

    out_data_bytes = 128 * 1024 * 1024;
    index = XRDP_SURCMD_PREFIX_BYTES + 16 + 2 + rcount * 8;
    out_data = g_new(char, out_data_bytes + index);

    if (out_data == NULL)
    {
        return 0;
    }

    s = &ls;
    g_memset(s, 0, sizeof(struct stream));

    ls.data = out_data + XRDP_SURCMD_PREFIX_BYTES;
    ls.p = ls.data;
    ls.size = out_data_bytes + index;

    /* size of avc420EncodedBitmapstream1 */
    s_push_layer(s, mcs_hdr, 4);

    /* RFX_AVC420_METABLOCK */
    comp_bytes_pre = build_rfx_avc420_metablock(
        s, rrects, rcount, scr_width, scr_height);

    enc_done_flags = 1;

    if (enc->flags & 1)
    {
        /* already compressed */
        uint8_t *ud = (uint8_t *) (enc->u.sc.data);
        int cbytes = ud[0] | (ud[1] << 8) | (ud[2] << 16) | (ud[3] << 24);
        if ((cbytes < 1) || (cbytes > out_data_bytes))
        {
            LOG(LOG_LEVEL_INFO, "process_enc_h264: bad h264 bytes %d", cbytes);
            g_free(out_data);
            return 0;
        }
        LOG(LOG_LEVEL_DEBUG,
            "process_enc_h264: already compressed and size is %d", cbytes);
        out_data_bytes = cbytes;
        g_memcpy(s->p, enc->u.sc.data + 4, out_data_bytes);
    }
    else
    {

#if defined(XRDP_X264)
        error = xrdp_encoder_x264_encode(self->codec_handle_x264,
                                         0, 0, 0,
                                         enc->u.sc.width,
                                         enc->u.sc.height,
                                         enc->u.sc.width, /* twidth */
                                         enc->u.sc.height, /* theight */
                                         0,
                                         enc_gfx_cmd->data,
                                         enc->u.sc.crects,
                                         enc->u.sc.num_crects,
                                         s->p,
                                         &out_data_bytes);
#elif defined(XRDP_OPENH264)
        error = xrdp_encoder_openh264_encode(self->codec_handle_openh264,
                                             0,
                                             enc->u.sc.width,
                                             enc->u.sc.height,
                                             0,
                                             enc->u.sc.data,
                                             s->p,
                                             &out_data_bytes);
#elif defined(XRDP_VANILLA_NVIDIA_CODEC)
        error = xrdp_encoder_nvenc_encode(self->codec_handle_nvenc,
                                          0,
                                          enc->u.sc.width,
                                          enc->u.sc.height,
                                          0,
                                          enc->u.sc.data,
                                          s->p,
                                          &out_data_bytes);
#endif
    }

    LOG(LOG_LEVEL_INFO,
              "process_enc_h264: xrdp_encoder_nvenc_encode_yuv420 rv %d "
              "out_data_bytes %d width %d height %d",
              error, out_data_bytes, enc->u.sc.width, enc->u.sc.height);

    if (error != 0)
    {
        LOG_DEVEL(LOG_LEVEL_TRACE,
                  "process_enc_h264: xrdp_encoder_x264_encode failed rv %d",
                  error);
        g_free(out_data);
        return 0;
    }

    s->p += out_data_bytes;

    s_push_layer(s, sec_hdr, 0);
    s_pop_layer(s, mcs_hdr);

    // TODO: Specify LC code here
    const uint8_t LC = 0x01;
    uint32_t bitstream =
         ((uint32_t)(comp_bytes_pre + out_data_bytes) & 0x3FFFFFFFUL)
         | ((LC & 0x03UL) << 30UL);

    out_uint32_le(s, bitstream);
    s_pop_layer(s, sec_hdr);

    s->end = s->p;

    if (s->iso_hdr != NULL)
    {
        /* not used in gfx */
        s_pop_layer(s, iso_hdr);
        out_uint32_le(s, out_data_bytes);
    }

    enc_done = g_new0(XRDP_ENC_DATA_DONE, 1); /* TODO: type */

    if (enc_done == NULL)
    {
        return 0;
    }

    enc_done->comp_bytes = 4 + comp_bytes_pre + out_data_bytes;
    enc_done->comp_pad_data = out_data;
    enc_done->enc = enc;
    enc_done->flags = enc_done_flags;
    enc_done->last = 1;
    enc_done->out_data_bytes = out_data_bytes;
    enc_done->pad_bytes = 256;
    enc_done->rect.cx = scr_width;
    enc_done->rect.cy = scr_height;

    return enc_done;
}

static XRDP_ENC_DATA_DONE *
build_enc_h264_avc444_chroma420_stream(
    struct xrdp_encoder *self, XRDP_ENC_DATA *enc, XRDP_ENC_DATA_DONE *enc_done)
{
    if (!self->gfx)
    {
        return 0;
    }

    char *out_data;
    int comp_bytes_pre;
    int error;
    int index;
    int out_data_bytes;
    int rcount;
    int scr_width, scr_height;
    short *rrects;
    struct stream *s;
    struct stream ls;
    struct xrdp_enc_gfx_cmd *enc_gfx_cmd = &(enc->u.gfx);

    LOG(LOG_LEVEL_DEBUG,
        "build_enc_h264_avc444_chroma420_stream: num_crects %d num_drects %d",
        enc->u.sc.num_crects, enc->u.sc.num_drects);

    rcount = enc->u.sc.num_drects;
    rrects = enc->u.sc.drects;
    scr_height = self->mm->wm->screen->height;
    scr_width = self->mm->wm->screen->width;

    if (rcount > 15)
    {
        rcount = enc->u.sc.num_crects;
        rrects = enc->u.sc.crects;
    }

    index = XRDP_SURCMD_PREFIX_BYTES + 16 + 2 + rcount * 8;
    out_data = g_new0(char, out_data_bytes + index);
    out_data_bytes = 128 * 1024 * 1024;

    if (out_data == NULL)
    {
        return 0;
    }

    s = &ls;
    g_memset(s, 0, sizeof(struct stream));

    ls.data = out_data + XRDP_SURCMD_PREFIX_BYTES;
    ls.p = ls.data;
    ls.size = out_data_bytes + index;

    /* size of avc420EncodedBitmapstream1 */
    s_push_layer(s, mcs_hdr, 4);

    /* RFX_AVC420_METABLOCK */
    comp_bytes_pre = build_rfx_avc420_metablock(
        s, rrects, rcount, scr_width, scr_height);

    error = 0;

    if (enc->flags & 1)
    {
        /* already compressed */
        uint8_t *ud
            = (uint8_t *) (enc->u.sc.data + enc_done->out_data_bytes + 4);

        int cbytes = ud[0] | (ud[1] << 8) | (ud[2] << 16) | (ud[3] << 24);

        if ((cbytes < 1) || (cbytes > out_data_bytes))
        {
            LOG(LOG_LEVEL_INFO, "process_enc_h264: bad h264 bytes %d", cbytes);
            g_free(out_data);
            return 0;
        }

        LOG(LOG_LEVEL_DEBUG,
            "process_enc_h264: already compressed and size is %d", cbytes);

        out_data_bytes = cbytes;

        g_memcpy(
            s->p,
            enc->u.sc.data + enc_done->out_data_bytes + 8,
            out_data_bytes);
    }
    else
    {

#if defined(XRDP_X264)
        error = xrdp_encoder_x264_encode(self->codec_handle_x264,
                                         0, 0, 0,
                                         enc->u.sc.width,
                                         enc->u.sc.height,
                                         enc->u.sc.width, /* twidth */
                                         enc->u.sc.height, /* theight */
                                         0, enc_gfx_cmd->data,
                                         enc->u.sc.crects,
                                         enc->u.sc.num_crects,
                                         s->p,
                                         &out_data_bytes);
#elif defined(XRDP_OPENH264)
        error = xrdp_encoder_openh264_encode(self->codec_handle_openh264,
                                             0,
                                             enc->u.sc.width,
                                             enc->u.sc.height,
                                             0,
                                             enc->u.sc.data
                                                + enc->u.sc.height
                                                * enc->u.sc.width * 3 / 2,
                                             s->p,
                                             &out_data_bytes);
#elif defined(XRDP_VANILLA_NVIDIA_CODEC)
        error = xrdp_encoder_nvenc_encode(self->codec_handle_nvenc, 0,
                                          enc->u.sc.width, enc->u.sc.height, 0,
                                          enc->u.sc.data
                                              + enc->u.sc.height
                                              * enc->u.sc.width * 3 / 2,
                                          s->p,
                                          &out_data_bytes);
#endif

    }
    LOG(LOG_LEVEL_INFO,
              "process_enc_h264: xrdp_encoder_nvenc_encode_chroma420 rv %d "
              "out_data_bytes %d width %d height %d",
              error, out_data_bytes, enc->u.sc.width, enc->u.sc.height);

    if (error != 0)
    {
        LOG_DEVEL(LOG_LEVEL_TRACE,
                  "process_enc_h264: xrdp_encoder_x264_encode failed rv %d",
                  error);

        g_free(out_data);

        return 0;
    }

    s->p += out_data_bytes;

    s_push_layer(s, sec_hdr, 0);
    s_pop_layer(s, mcs_hdr);

    // TODO: Specify LC code here
    const uint8_t LC = 0x02;
    const uint32_t bitstream = ((LC & 0x03UL) << 30UL);

    out_uint32_le(s, bitstream);
    s_pop_layer(s, sec_hdr);

    s->end = s->p;

    if (s->iso_hdr != NULL)
    {
        /* not used in gfx */
        s_pop_layer(s, iso_hdr);
        out_uint32_le(s, out_data_bytes);
    }

    enc_done->out_data_bytes = out_data_bytes;
    enc_done->comp_bytes = 4 + comp_bytes_pre + out_data_bytes;
    enc_done->pad_bytes = 256;
    enc_done->comp_pad_data = out_data;

    return enc_done;
}
#endif

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_h264(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    XRDP_ENC_DATA_DONE *enc_done = NULL;
    struct fifo *fifo_processed;
    tbus event_processed;
    tbus mutex;

    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    event_processed = self->xrdp_encoder_event_processed;

    int mode = 1; /* TODO: h264 mode selection */

    switch (mode) {
        case 0:
            enc_done = build_enc_h264_avc444_yuv420_and_chroma420_stream(
                self, enc);

            break;
#if defined(AVC444)
        case 1:
            enc_done = build_enc_h264_avc444_yuv420_stream(self, enc);

            break;
        case 2:
            enc_done = build_enc_h264_avc444_chroma420_stream(
                self, enc, enc_done);

            break;
#endif
    }

    if (enc_done == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "process_enc_h264: failed to build stream");
        return 1;
    }

    enc_done->rect = calculate_bounding_box(
        enc->u.sc.drects, enc->u.sc.num_drects);

    /* done with msg */
    /* inform main thread done */
    tc_mutex_lock(mutex);
    fifo_add_item(fifo_processed, enc_done);
    tc_mutex_unlock(mutex);

    /* signal completion for main thread */
    g_set_wait_obj(event_processed);

    return 0;
}
#endif

/*****************************************************************************/
static int
gfx_send_done(struct xrdp_encoder *self, XRDP_ENC_DATA *enc,
              int comp_bytes, int pad_bytes, char *comp_pad_data,
              int got_frame_id, int frame_id, int is_last)
{
    XRDP_ENC_DATA_DONE *enc_done;

    enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);

    if (enc_done == NULL)
    {
        return 1;
    }

    ENC_SET_BIT(enc_done->flags, ENC_DONE_FLAGS_GFX_BIT);

    enc_done->comp_bytes = comp_bytes;
    enc_done->comp_pad_data = comp_pad_data;
    enc_done->enc = enc;
    enc_done->last = is_last;
    enc_done->pad_bytes = pad_bytes;

    if (got_frame_id)
    {
        ENC_SET_BIT(enc_done->flags, ENC_DONE_FLAGS_FRAME_ID_BIT);
        enc_done->frame_id = frame_id;
    }

    /* inform main thread done */
    tc_mutex_lock(self->mutex);
    fifo_add_item(self->fifo_processed, enc_done);
    tc_mutex_unlock(self->mutex);

    /* signal completion for main thread */
    g_set_wait_obj(self->xrdp_encoder_event_processed);
    return 0;
}

/*****************************************************************************/
static struct stream *
gfx_wiretosurface1(struct xrdp_encoder *self,
                   struct xrdp_egfx_bulk *bulk,
                   struct stream *in_s,
                   XRDP_ENC_DATA *enc)
{

#if defined(XRDP_X264)
    int bitmap_data_length;
    int codec_id;
    int error;
    int flags;
    int index;
    int num_rects_c;
    int num_rects_d;
    int pixel_format;
    int surface_id;
    short *crects;
    short height;
    short left;
    short theight;
    short top;
    short twidth;
    short width;
    struct stream *rv;
    struct stream *s;
    struct stream ls;
    struct xrdp_egfx_rect *c_rects;
    struct xrdp_egfx_rect *d_rects;
    struct xrdp_egfx_rect dst_rect;
    struct xrdp_enc_gfx_cmd *enc_gfx_cmd = &(enc->u.gfx);

    s = &ls;

    g_memset(s, 0, sizeof(struct stream));

    s->size = self->max_compressed_bytes;
    s->data = g_new(char, s->size);

    if (s->data == NULL)
    {
        return NULL;
    }

    s->p = s->data;

    if (!s_check_rem(in_s, 11))
    {
        g_free(s->data);
        return NULL;
    }

    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, codec_id);
    in_uint8(in_s, pixel_format);
    in_uint32_le(in_s, flags);
    in_uint16_le(in_s, num_rects_d);

    if ((num_rects_d < 1) || (num_rects_d > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_d * 8)))
    {
        g_free(s->data);

        return NULL;
    }

    d_rects = g_new0(struct xrdp_egfx_rect, num_rects_d);

    if (d_rects == NULL)
    {
        g_free(s->data);

        return NULL;
    }

    for (index = 0; index < num_rects_d; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);

        d_rects[index].x1 = left;
        d_rects[index].y1 = top;
        d_rects[index].x2 = left + width;
        d_rects[index].y2 = top + height;
    }

    if (!s_check_rem(in_s, 2))
    {
        g_free(s->data);
        g_free(d_rects);

        return NULL;
    }

    in_uint16_le(in_s, num_rects_c);

    if ((num_rects_c < 1) || (num_rects_c > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_c * 8)))
    {
        g_free(s->data);
        g_free(d_rects);

        return NULL;
    }

    c_rects = g_new0(struct xrdp_egfx_rect, num_rects_c);

    if (c_rects == NULL)
    {
        g_free(s->data);
        g_free(d_rects);

        return NULL;
    }

    crects = g_new(short, num_rects_c * 4);

    if (crects == NULL)
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);

        return NULL;
    }

    g_memcpy(crects, in_s->p, num_rects_c * 2 * 4);

    for (index = 0; index < num_rects_c; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);

        c_rects[index].x1 = left;
        c_rects[index].y1 = top;
        c_rects[index].x2 = left + width;
        c_rects[index].y2 = top + height;
    }

    if (!s_check_rem(in_s, 8))
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        g_free(crects);

        return NULL;
    }

    in_uint16_le(in_s, left);
    in_uint16_le(in_s, top);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);

    twidth = width;
    theight = height;
    dst_rect.x1 = 0;
    dst_rect.y1 = 0;
    dst_rect.x2 = width;
    dst_rect.y2 = height;

    LOG_DEVEL(LOG_LEVEL_INFO, "gfx_wiretosurface1: left %d top "
              "%d width %d height %d",
              left, top, width, height);

    /* RFX_AVC420_METABLOCK */
    if (out_RFX_AVC420_METABLOCK(&dst_rect, s, d_rects, num_rects_d) != 0)
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        g_free(crects);

        LOG(LOG_LEVEL_INFO, "10");

        return NULL;
    }

    g_free(c_rects);
    g_free(d_rects);

    if (ENC_IS_BIT_SET(flags, 0))
    {
        /* already compressed */
        out_uint8a(s, enc_gfx_cmd->data, enc_gfx_cmd->data_bytes);
    }
    else
    {
        /* assume NV12 format */
        if (twidth * theight * 3 / 2 > enc_gfx_cmd->data_bytes)
        {
            g_free(s->data);
            g_free(crects);

            return NULL;
        }

        bitmap_data_length = s_rem_out(s);

        if (self->codec_handle_x264 == NULL)
        {
            self->codec_handle_x264 = xrdp_encoder_x264_create();

            if (self->codec_handle_x264 == NULL)
            {
                g_free(s->data);
                g_free(crects);

                return NULL;
            }
        }

        error = xrdp_encoder_x264_encode(self->codec_handle_x264,
                                         0, 0, 0,
                                         width, height, twidth, theight,
                                         0, enc_gfx_cmd->data,
                                         enc->u.sc.crects, enc->u.sc.num_crects,
                                         s->p, &bitmap_data_length);

        if (error == 0)
        {
            xstream_seek(s, bitmap_data_length);
        }
        else
        {
            g_free(s->data);
            g_free(crects);

            return NULL;
        }
    }

    s_mark_end(s);
    bitmap_data_length = (int) (s->end - s->data);

    rv = xrdp_egfx_wire_to_surface1(bulk,
                                    surface_id,
                                    codec_id,
                                    pixel_format,
                                    &dst_rect,
                                    s->data,
                                    bitmap_data_length);

    g_free(s->data);
    g_free(crects);

    return rv;
#else
    (void)bulk;
    (void)enc;
    (void)in_s;
    (void)self;

    return NULL;
#endif

}

/*****************************************************************************/
static struct stream *
gfx_wiretosurface2(struct xrdp_encoder *self,
                   struct xrdp_egfx_bulk *bulk,
                   struct stream *in_s,
                   XRDP_ENC_DATA *enc)
{
#ifdef XRDP_RFXCODEC
    char *bitmap_data;
    int bitmap_data_length;
    int codec_context_id;
    int codec_id;
    int flags;
    int index;
    int mon_index;
    int num_rects_c;
    int num_rects_d;
    int pixel_format;
    int surface_id;
    int tiles_compressed;
    int tiles_written;
    int total_tiles;
    short height;
    short left;
    short top;
    short width;
    struct rfx_rect *rfxrects;
    struct rfx_tile *tiles;
    struct stream *rv;

    if (!s_check_rem(in_s, 15))
    {
        return NULL;
    }

    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, codec_id);
    in_uint32_le(in_s, codec_context_id);
    in_uint8(in_s, pixel_format);
    in_uint32_le(in_s, flags);

    mon_index = (flags >> 28) & 0xF;

    in_uint16_le(in_s, num_rects_d);

    if ((num_rects_d < 1) || (num_rects_d > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_d * 8)))
    {
        return NULL;
    }

    rfxrects = g_new0(struct rfx_rect, num_rects_d);

    if (rfxrects == NULL)
    {
        return NULL;
    }

    for (index = 0; index < num_rects_d; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);

        rfxrects[index].x = left;
        rfxrects[index].y = top;
        rfxrects[index].cx = width;
        rfxrects[index].cy = height;
    }

    if (!s_check_rem(in_s, 2))
    {
        g_free(rfxrects);

        return NULL;
    }

    in_uint16_le(in_s, num_rects_c);

    if ((num_rects_c < 1) || (num_rects_c > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_c * 8)))
    {
        g_free(rfxrects);

        return NULL;
    }

    tiles = g_new0(struct rfx_tile, num_rects_c);

    if (tiles == NULL)
    {
        g_free(rfxrects);

        return NULL;
    }

    for (index = 0; index < num_rects_c; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);

        tiles[index].x = left;
        tiles[index].y = top;
        tiles[index].cx = width;
        tiles[index].cy = height;
        tiles[index].quant_y = self->quant_idx_y;
        tiles[index].quant_cb = self->quant_idx_u;
        tiles[index].quant_cr = self->quant_idx_v;
    }

    if (!s_check_rem(in_s, 8))
    {
        g_free(tiles);
        g_free(rfxrects);

        return NULL;
    }

    in_uint16_le(in_s, left);
    in_uint16_le(in_s, top);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);

    LOG_DEVEL(LOG_LEVEL_INFO,
              "gfx_wiretosurface2: left %d top %d width %d height %d "
              "mon_index %d",
              left, top, width, height, mon_index);

    if (self->codec_handle_prfx_gfx[mon_index] == NULL)
    {
        self->codec_handle_prfx_gfx[mon_index] = rfxcodec_encode_create(
                width,
                height,
                RFX_FORMAT_YUV,
                RFX_FLAGS_RLGR1 | RFX_FLAGS_PRO1);

        if (self->codec_handle_prfx_gfx[mon_index] == NULL)
        {
            g_free(tiles);
            g_free(rfxrects);

            return NULL;
        }
    }

    bitmap_data_length = self->max_compressed_bytes;
    bitmap_data = g_new(char, bitmap_data_length);

    if (bitmap_data == NULL)
    {
        g_free(tiles);
        g_free(rfxrects);

        return NULL;
    }

    rv = NULL;
    tiles_written = 0;
    total_tiles = num_rects_c;

    for (;;)
    {
        tiles_compressed = rfxcodec_encode(
                                           self->codec_handle_prfx_gfx[
                                               mon_index],
                                           bitmap_data,
                                           &bitmap_data_length,
                                           enc->u.gfx.data,
                                           width,
                                           height,
                                           ((width + 63) & ~63) * 4,
                                           rfxrects,
                                           num_rects_d,
                                           tiles + tiles_written,
                                           total_tiles - tiles_written,
                                           self->quants,
                                           self->num_quants);

        if (tiles_compressed < 1)
        {
            break;
        }

        tiles_written += tiles_compressed;

        rv = xrdp_egfx_wire_to_surface2(bulk, surface_id,
                                        codec_id, codec_context_id,
                                        pixel_format,
                                        bitmap_data, bitmap_data_length);

        if (rv == NULL)
        {
            break;
        }

        LOG_DEVEL(LOG_LEVEL_INFO, "gfx_wiretosurface2: "
                  "tiles_compressed %d total_tiles %d tiles_written %d",
                  tiles_compressed, total_tiles, tiles_written);

        if (tiles_written >= total_tiles)
        {
            /* ok, done with last tile set */
            break;
        }

        /* we have another tile set, send this one to main thread */
        if (gfx_send_done(self, enc, (int)(rv->end - rv->data), 0,
                          rv->data, 0, 0, 0) != 0)
        {
            free_stream(rv);
            rv = NULL;

            break;
        }

        g_free(rv); /* don't call free_stream() here so s->data is valid */

        rv = NULL;
        bitmap_data_length = self->max_compressed_bytes;
    }

    g_free(tiles);
    g_free(rfxrects);
    g_free(bitmap_data);

    return rv;
#else
    (void)bulk;
    (void)enc;
    (void)in_s;
    (void)self;

    return NULL;
#endif
}

/*****************************************************************************/
static struct stream *
gfx_solidfill(struct xrdp_encoder *self,
              struct xrdp_egfx_bulk *bulk,
              struct stream *in_s)
{
    char *ptr8;
    int num_rects;
    int pixel;
    int surface_id;
    struct xrdp_egfx_rect *rects;

    if (!s_check_rem(in_s, 8))
    {
        return NULL;
    }

    in_uint16_le(in_s, surface_id);
    in_uint32_le(in_s, pixel);
    in_uint16_le(in_s, num_rects);

    if (!s_check_rem(in_s, num_rects * 8))
    {
        return NULL;
    }

    in_uint8p(in_s, ptr8, num_rects * 8);

    rects = (struct xrdp_egfx_rect *) ptr8;

    return xrdp_egfx_fill_surface(bulk, surface_id, pixel, num_rects, rects);
}

/*****************************************************************************/
static struct stream *
gfx_surfacetosurface(struct xrdp_encoder *self,
                     struct xrdp_egfx_bulk *bulk,
                     struct stream *in_s)
{
    char *ptr8;
    int num_pts;
    int surface_id_dst;
    int surface_id_src;
    struct xrdp_egfx_point *pts;
    struct xrdp_egfx_rect *rects;

    if (!s_check_rem(in_s, 14))
    {
        return NULL;
    }

    in_uint16_le(in_s, surface_id_src);
    in_uint16_le(in_s, surface_id_dst);
    in_uint8p(in_s, ptr8, 8);

    rects = (struct xrdp_egfx_rect *) ptr8;

    in_uint16_le(in_s, num_pts);

    if (!s_check_rem(in_s, num_pts * 4))
    {
        return NULL;
    }

    in_uint8p(in_s, ptr8, num_pts * 4);

    pts = (struct xrdp_egfx_point *) ptr8;

    return xrdp_egfx_surface_to_surface(bulk, surface_id_src, surface_id_dst,
                                        rects, num_pts, pts);
}

/*****************************************************************************/
static struct stream *
gfx_createsurface(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk,
                  struct stream *in_s)
{
    int height;
    int pixel_format;
    int surface_id;
    int width;

    if (!s_check_rem(in_s, 7))
    {
        return NULL;
    }

    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);
    in_uint8(in_s, pixel_format);

    return xrdp_egfx_create_surface(bulk,
                                    surface_id,
                                    width,
                                    height,
                                    pixel_format);
}

/*****************************************************************************/
static struct stream *
gfx_deletesurface(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk,
                  struct stream *in_s)
{
    int surface_id;

    if (!s_check_rem(in_s, 2))
    {
        return NULL;
    }

    in_uint16_le(in_s, surface_id);

    return xrdp_egfx_delete_surface(bulk, surface_id);
}

/*****************************************************************************/
static struct stream *
gfx_startframe(struct xrdp_encoder *self,
               struct xrdp_egfx_bulk *bulk,
               struct stream *in_s)
{
    int frame_id;
    int time_stamp;

    if (!s_check_rem(in_s, 8))
    {
        return NULL;
    }

    in_uint32_le(in_s, frame_id);
    in_uint32_le(in_s, time_stamp);

    return xrdp_egfx_frame_start(bulk, frame_id, time_stamp);
}

/*****************************************************************************/
static struct stream *
gfx_endframe(struct xrdp_encoder *self,
             struct xrdp_egfx_bulk *bulk, struct stream *in_s, int *aframe_id)
{
    int frame_id;

    if (!s_check_rem(in_s, 4))
    {
        return NULL;
    }

    in_uint32_le(in_s, frame_id);

    *aframe_id = frame_id;

    return xrdp_egfx_frame_end(bulk, frame_id);
}

/*****************************************************************************/
static struct stream *
gfx_resetgraphics(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk,
                  struct stream *in_s)
{
    int height;
    int index;
    int monitor_count;
    int width;
    struct monitor_info *mi;
    struct stream *rv;

    if (!s_check_rem(in_s, 12))
    {
        return NULL;
    }

    in_uint32_le(in_s, width);
    in_uint32_le(in_s, height);
    in_uint32_le(in_s, monitor_count);

    if (
        (monitor_count < 1)
        || (monitor_count > 16)
        || !s_check_rem(in_s, monitor_count * 20)
    )
    {
        return NULL;
    }

    mi = g_new0(struct monitor_info, monitor_count);

    if (mi == NULL)
    {
        return NULL;
    }

    for (index = 0; index < monitor_count; index++)
    {
        in_uint32_le(in_s, mi[index].left);
        in_uint32_le(in_s, mi[index].top);
        in_uint32_le(in_s, mi[index].right);
        in_uint32_le(in_s, mi[index].bottom);
        in_uint32_le(in_s, mi[index].is_primary);
    }

    rv = xrdp_egfx_reset_graphics(bulk, width, height, monitor_count, mi);

    g_free(mi);

    return rv;
}

/*****************************************************************************/
static struct stream *
gfx_mapsurfacetooutput(struct xrdp_encoder *self,
                       struct xrdp_egfx_bulk *bulk,
                       struct stream *in_s)
{
    int surface_id;
    int x;
    int y;

    if (!s_check_rem(in_s, 10))
    {
        return NULL;
    }

    in_uint16_le(in_s, surface_id);
    in_uint32_le(in_s, x);
    in_uint32_le(in_s, y);

    return xrdp_egfx_map_surface(bulk, surface_id, x, y);
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_egfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    char *holdend;
    char *holdp;
    int cmd_bytes;
    int cmd_id;
    int error;
    int frame_id;
    int got_frame_id;
    struct stream *s;
    struct stream in_s;
    struct xrdp_egfx_bulk *bulk;

    bulk = self->mm->egfx->bulk;

    g_memset(&in_s, 0, sizeof(in_s));

    in_s.data = enc->u.gfx.cmd;
    in_s.size = enc->u.gfx.cmd_bytes;
    in_s.p = in_s.data;
    in_s.end = in_s.data + in_s.size;

    while (s_check_rem(&in_s, 8))
    {
        s = NULL;
        frame_id = 0;
        got_frame_id = 0;
        holdp = in_s.p;

        in_uint16_le(&in_s, cmd_id);
        in_uint8s(&in_s, 2); /* flags */
        in_uint32_le(&in_s, cmd_bytes);

        if ((cmd_bytes < 8) || (cmd_bytes > 32 * 1024))
        {
            return 1;
        }

        holdend = in_s.end;
        in_s.end = holdp + cmd_bytes;

        LOG_DEVEL(LOG_LEVEL_INFO, "process_enc_egfx: cmd_id %d", cmd_id);

        switch (cmd_id)
        {
            case XR_RDPGFX_CMDID_WIRETOSURFACE_1:       /* 0x0001 */
                s = gfx_wiretosurface1(self, bulk, &in_s, enc);
                break;
            case XR_RDPGFX_CMDID_WIRETOSURFACE_2:       /* 0x0002 */
                s = gfx_wiretosurface2(self, bulk, &in_s, enc);
                break;
            case XR_RDPGFX_CMDID_SOLIDFILL:             /* 0x0004 */
                s = gfx_solidfill(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_SURFACETOSURFACE:      /* 0x0005 */
                s = gfx_surfacetosurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_CREATESURFACE:         /* 0x0009 */
                s = gfx_createsurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_DELETESURFACE:         /* 0x000A */
                s = gfx_deletesurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_STARTFRAME:            /* 0x000B */
                s = gfx_startframe(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_ENDFRAME:              /* 0x000C */
                s = gfx_endframe(self, bulk, &in_s, &frame_id);
                got_frame_id = 1;
                break;
            case XR_RDPGFX_CMDID_RESETGRAPHICS:         /* 0x000E */
                s = gfx_resetgraphics(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_MAPSURFACETOOUTPUT:    /* 0x000F */
                s = gfx_mapsurfacetooutput(self, bulk, &in_s);
                break;
            default:
                break;
        }

        /* setup for next cmd */
        in_s.p = holdp + cmd_bytes;
        in_s.end = holdend;

        if (s != NULL)
        {
            /* send message to main thread */
            error = gfx_send_done(self,
                                  enc,
                                  s->end - s->data,
                                  0,
                                  s->data,
                                  got_frame_id,
                                  frame_id,
                                  !s_check_rem(&in_s, 8));

            if (error != 0)
            {
                LOG(LOG_LEVEL_ERROR, "process_enc_egfx: gfx_send_done failed "
                    "error %d", error);
                free_stream(s);

                return 1;
            }

            g_free(s); /* don't call free_stream() here so s->data is valid */
        }
        else
        {
            LOG_DEVEL(LOG_LEVEL_INFO, "process_enc_egfx: nil");
        }
    }

    return 0;
}

/**
 * Encoder thread main loop
 *****************************************************************************/
THREAD_RV THREAD_CC
proc_enc_msg(void *arg)
{
    XRDP_ENC_DATA *enc;
    int cont;
    int robjs_count;
    int timeout;
    int wobjs_count;
    struct fifo *fifo_to_proc;
    struct xrdp_encoder *self;
    tbus event_to_proc;
    tbus lterm_obj;
    tbus mutex;
    tbus robjs[32];
    tbus term_obj;
    tbus wobjs[32];

    LOG_DEVEL(LOG_LEVEL_INFO, "proc_enc_msg: thread is running");

    self = (struct xrdp_encoder *) arg;

    if (self == 0)
    {
        LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: self nil");

        return 0;
    }

    event_to_proc = self->xrdp_encoder_event_to_proc;
    fifo_to_proc = self->fifo_to_proc;
    lterm_obj = self->xrdp_encoder_term_request;
    mutex = self->mutex;
    term_obj = g_get_term();

    cont = 1; /* TODO */

    while (cont)
    {
        timeout = -1;
        robjs_count = 0;
        wobjs_count = 0;
        robjs[robjs_count++] = term_obj;
        robjs[robjs_count++] = lterm_obj;
        robjs[robjs_count++] = event_to_proc;

        if (g_obj_wait(robjs, robjs_count, wobjs, wobjs_count, timeout) != 0)
        {
            /* error, should not get here */
            g_sleep(100);
        }

        if (g_is_wait_obj_set(term_obj)) /* global term */
        {
            LOG(LOG_LEVEL_DEBUG,
                "Received termination signal, stopping the encoder thread");
            break;
        }

        if (g_is_wait_obj_set(lterm_obj)) /* xrdp_mm term */
        {
            LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: xrdp_mm term");
            break;
        }

        if (g_is_wait_obj_set(event_to_proc))
        {
            /* clear it right away */
            g_reset_wait_obj(event_to_proc);
            /* get first msg */
            tc_mutex_lock(mutex);
            enc = (XRDP_ENC_DATA *) fifo_remove_item(fifo_to_proc);
            tc_mutex_unlock(mutex);
            while (enc != 0)
            {
                /* do work */
                self->process_enc(self, enc);

                /* get next msg */
                tc_mutex_lock(mutex);

                enc = (XRDP_ENC_DATA *) fifo_remove_item(fifo_to_proc);

                tc_mutex_unlock(mutex);
            }
        }

    } /* end while (cont) */

    g_set_wait_obj(self->xrdp_encoder_term_done);

    LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: thread exit");

    return 0;
}
