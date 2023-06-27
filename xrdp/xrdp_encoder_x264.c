/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2016
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
 * x264 Encoder
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <x264.h>
#include "log.h"

#include "xrdp.h"
#include "arch.h"
#include "os_calls.h"
#include "xrdp_encoder_x264.h"

struct x264_encoder
{
    x264_t *x264_enc_han;
    char *yuvdata;
    x264_param_t x264_params;
    int width;
    int height;
};

struct x264_global
{
    struct x264_encoder encoders[16];
};

/*****************************************************************************/
void *
xrdp_encoder_x264_create(void)
{
    struct x264_global *x264 = NULL;
    LOG_DEVEL(LOG_LEVEL_TRACE, "xrdp_encoder_x264_create:");
    x264 = (struct x264_global *) g_malloc(sizeof(struct x264_global), 1);
    if (!x264) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate X264 context");
    }
    return x264;
}

/*****************************************************************************/
int
xrdp_encoder_x264_delete(void *handle)
{
    struct x264_global *xg;
    struct x264_encoder *xe;
    int index;

    if (handle == 0)
    {
        return 0;
    }
    xg = (struct x264_global *) handle;
    for (index = 0; index < 16; index++)
    {
        xe = &(xg->encoders[index]);
        if (xe->x264_enc_han != 0)
        {
            x264_encoder_close(xe->x264_enc_han);
        }
        g_free(xe->yuvdata);
    }
    g_free(xg);
    return 0;
}

/*****************************************************************************/
int
xrdp_encoder_x264_encode(void *handle, int session,
                         int width, int height, int format, const char *data,
                         char *cdata, int *cdata_bytes)
{
    struct x264_global *xg;
    struct x264_encoder *xe;
    const char *src8;
    char *dst8;
    int index;
    x264_nal_t *nals;
    int num_nals;
    int frame_size;
    int frame_area;

    x264_picture_t pic_in;
    x264_picture_t pic_out;

    LOG(LOG_LEVEL_TRACE, "xrdp_encoder_x264_encode:");
    xg = (struct x264_global *) handle;
    xe = &(xg->encoders[session]);
    if ((xe->x264_enc_han == 0) || (xe->width != width)
        || (xe->height != height))
    {
        if (xe->x264_enc_han != 0)
        {
            x264_encoder_close(xe->x264_enc_han);
            xe->x264_enc_han = 0;
            g_free(xe->yuvdata);
            xe->yuvdata = 0;
        }
        if ((width > 0) && (height > 0))
        {
            x264_param_default_preset(&(xe->x264_params),
                                      "veryfast", "zerolatency");
            xe->x264_params.i_width = width;
            xe->x264_params.i_height = height;
            xe->x264_params.i_threads = 1;
            xe->x264_params.i_fps_num = 24;
            xe->x264_params.i_fps_den = 1;
            x264_param_apply_profile(&(xe->x264_params), "high");

            xe->x264_enc_han = x264_encoder_open(&(xe->x264_params));
            if (xe->x264_enc_han == 0)
            {
                return 1;
            }
            xe->yuvdata = (char *) g_malloc(width * height * 3 / 2, 0);
            if (xe->yuvdata == 0)
            {
                x264_encoder_close(xe->x264_enc_han);
                xe->x264_enc_han = 0;
                return 2;
            }
        }
        xe->width = width;
        xe->height = height;
    }

    if ((data != 0) && (xe->x264_enc_han != 0))
    {
        src8 = data;
        dst8 = xe->yuvdata;
        for (index = 0; index < height; index++)
        {
            g_memcpy(dst8, src8, width);
            src8 += width;
            dst8 += xe->x264_params.i_width;
        }

        src8 = data;
        src8 += width * height;
        dst8 = xe->yuvdata;

        frame_area = xe->x264_params.i_width * xe->x264_params.i_height - 1;
        dst8 += frame_area;
        for (index = 0; index < height; index += 2)
        {
            g_memcpy(dst8, src8, width);
            src8 += width;
            dst8 += xe->x264_params.i_width;
        }

        pic_in.param = &xe->x264_params;

        g_memset(&pic_in, 0, sizeof(pic_in));
        pic_in.img.i_csp = X264_CSP_NV12;
        pic_in.img.i_plane = 2;
        pic_in.img.plane[0] = (unsigned char *) (xe->yuvdata);
        pic_in.img.plane[1] = (unsigned char *) (xe->yuvdata + frame_area);
        pic_in.img.i_stride[0] = width;
        pic_in.img.i_stride[1] = xe->x264_params.i_width;
        num_nals = 0;

        frame_size = x264_encoder_encode(xe->x264_enc_han, &nals, &num_nals,
                                         &pic_in, &pic_out);

        if (frame_size < 1)
        {
            return 3;
        }
        if (*cdata_bytes < frame_size)
        {
            return 4;
        }
        *cdata_bytes = 0;
        for (int i = 0; i < num_nals; ++i)
        {
            x264_nal_t *nal = nals + i;
            int size = nal->i_payload;
            uint8_t* payload = nal->p_payload;
            char* write_location = cdata + *cdata_bytes;
            int nalUnitType = nal->i_type;
            LOG(LOG_LEVEL_TRACE, "NalType is %d. Format is %d", 
                nalUnitType, format);
            switch (nalUnitType)
            {
                case NAL_SPS:
                case NAL_PPS:
                case NAL_SLICE:
                case NAL_SLICE_IDR:
                    *cdata_bytes += size;
                    if (nal->b_long_startcode)
                    {
                        g_memcpy(write_location, payload, size);
                        break;
                    }
                    LOG(LOG_LEVEL_DEBUG, "Expanding start code %d.",
                        nalUnitType);
                    g_memcpy(write_location, "\x00\x00\x00\x01", 4);
                    g_memcpy(write_location + 4, payload + 3, size - 3);
                    *cdata_bytes += 1;
                    break;
                default:
                    LOG(LOG_LEVEL_DEBUG, "Skipping NAL of type %d.",
                        nalUnitType);
                    continue;
            }
        }
    }
    return 0;
}
