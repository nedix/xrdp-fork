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
#include <byteswap.h>

#include "xrdp.h"
#include "arch.h"
#include "os_calls.h"
#include "xrdp_encoder_openh264.h"

//static void *openh264lib = NULL;

// typedef int (*pfn_create_openh264_encoder)(ISVCEncoder **ppEncoder);
// typedef void (*pfn_destroy_openh264_encoder)(ISVCEncoder *pEncoder);
// typedef void (*pfn_get_openh264_version)(OpenH264Version *pVersion);

// pfn_create_openh264_encoder create_openh264_encoder = NULL;
// pfn_destroy_openh264_encoder destroy_open_h264_encoder = NULL;
// pfn_get_openh264_version get_openh264_version = NULL;

//char* OPENH264_LIBRARY = "libopenh264.so";

int
xrdp_encoder_openh264_encode(void *handle, int session,
                        	 int enc_width, int enc_height, int format, const char *data,
                         	 char *cdata, int *cdata_bytes)
{
	SFrameBSInfo info;
	SSourcePicture *sourcePicture = NULL;
	int status;
	int i, j;

	LOG(LOG_LEVEL_INFO, "xrdp_encoder_openh264_encode:");

	if (!handle) {
		return 0;
	}

	struct openh264_context *h264 = (struct openh264_context *) handle;

	if (h264->pEncoder == NULL) {
		xrdp_encoder_openh264_open(h264, enc_width, enc_height);
	}

	memset(&info, 0, sizeof(info));
	sourcePicture = &h264->pic1;

	h264->frameRate = 24;

	int full_size = enc_width * enc_height;
    int quarter_size = full_size / 4;

	memcpy(sourcePicture->pData[0], data, full_size);
	memcpy(sourcePicture->pData[1], data + full_size, quarter_size);
	memcpy(sourcePicture->pData[2], data + full_size * 5 / 4, quarter_size);

	status = (*h264->pEncoder)->EncodeFrame(h264->pEncoder, sourcePicture, &info);

	if (status != 0) {
		LOG(LOG_LEVEL_INFO, "Failed to encode frame");
		return 0;
	}

	if (info.eFrameType == videoFrameTypeSkip) {
		LOG(LOG_LEVEL_INFO, "frame was skipped!");
		return 0;
	}

	// *cdata_bytes = info.iFrameSizeInBytes;
	// g_memcpy(cdata, info.sLayerInfo[0].pBsBuf, *cdata_bytes);

	*cdata_bytes = 0;
	for (i = 0; i < info.iLayerNum; i++) {
		SLayerBSInfo *layer = info.sLayerInfo + i;
		int size = 0;
		char* write_location = cdata + *cdata_bytes;
		for (j = 0; j < layer->iNalCount; j++) {
			unsigned char* payload = layer->pBsBuf + layer->pNalLengthInByte[j];
			size = layer->pNalLengthInByte[j];
			g_memcpy(&t, payload, 4);
			t = bswap_32(t);
			if (t != 0x00000001) {
				g_memcpy(write_location, "\x00\x00\x00\x01", 4);
				g_memcpy(write_location + 4, payload + 3, size - 3);
				size += 1;
			}
		}
		LOG(LOG_LEVEL_INFO, "OpenH264 layer: %d, Size: %d", i, size);
		g_memcpy(cdata + *cdata_bytes, layer->pBsBuf, size);
		*cdata_bytes += size;
	}
	LOG(LOG_LEVEL_INFO, "OpenH264 total size: %d", *cdata_bytes);

	return 0;
}


int
xrdp_encoder_openh264_delete(void *handle)
{
	struct openh264_context *h264 = (struct openh264_context *)handle;
	if (!h264) {
		return 0;
	}

	if (h264->pEncoder) {
		WelsDestroySVCEncoder(h264->pEncoder);
	}

	g_free(h264->pic1.pData[0]);
	g_free(h264->pic1.pData[1]);
	g_free(h264->pic1.pData[2]);

	g_free(h264->pic2.pData[0]);
	g_free(h264->pic2.pData[1]);
	g_free(h264->pic2.pData[2]);

	free(h264);
	h264 = NULL;

	return 0;
}

/*****************************************************************************/
void *xrdp_encoder_openh264_create()
{
	struct openh264_context *h264 = NULL;
	LOG(LOG_LEVEL_INFO, "xrdp_encoder_openh264_create:");
	h264 = (struct openh264_context *) g_malloc(sizeof(openh264_context), 1);
	if (!h264) {
		LOG(LOG_LEVEL_ERROR, "Failed to allocate OpenH264 context");
	}
	return h264;
}

/*****************************************************************************/
void *xrdp_encoder_openh264_open(struct openh264_context *h264, uint32_t scrWidth, uint32_t scrHeight)
{
	LOG(LOG_LEVEL_INFO, "xrdp_encoder_openh264_open:");
	uint32_t h264Width;
	uint32_t h264Height;
	SEncParamExt encParamExt;
	SBitrateInfo bitrate;
	size_t ysize, usize, vsize;

	if (scrWidth < 16 || scrHeight < 16) {
		LOG(LOG_LEVEL_ERROR, "Error: Minimum height and width for OpenH264 is 16 but we got %"PRIu32" x %"PRIu32"", scrWidth, scrHeight);
		return NULL;
	}

	if (scrWidth % 16) {
		LOG(LOG_LEVEL_WARNING, "WARNING: screen width %"PRIu32" is not a multiple of 16. Expect degraded H.264 performance!", scrWidth);
	}

	if (!h264) {
		LOG(LOG_LEVEL_ERROR, "OpenH264 context is not initialized.");
		return NULL;
	}

	/**
	 * [MS-RDPEGFX 2.2.4.4 RFX_AVC420_BITMAP_STREAM]
	 *
	 * The width and height of the MPEG-4 AVC/H.264 codec bitstream MUST be aligned to a
	 * multiple of 16.
	 */

	h264Width = (scrWidth + 15) & ~15;    /* codec bitstream width must be a multiple of 16 */
	h264Height = (scrHeight + 15) & ~15;  /* codec bitstream height must be a multiple of 16 */

	h264->scrWidth = scrWidth;
	h264->scrHeight = scrHeight;
	h264->scrStride = scrWidth;

	h264->pic1.iPicWidth = h264->pic2.iPicWidth = h264Width;
	h264->pic1.iPicHeight = h264->pic2.iPicHeight = h264Height;
	h264->pic1.iColorFormat = h264->pic2.iColorFormat = videoFormatI420;

	h264->pic1.iStride[0] = h264->pic2.iStride[0] = h264Width;
	h264->pic1.iStride[1] = h264->pic2.iStride[1] = h264Width / 2;
	h264->pic1.iStride[2] = h264->pic2.iStride[2] = h264Width / 2;

	h264->frameRate = 20;
	h264->bitRate = 1000000 * 2; /* 2 Mbit/s */

	ysize = h264Width * h264Height;
	usize = vsize = ysize >> 2;

	if (!(h264->pic1.pData[0] = (unsigned char*) g_malloc(ysize, 1))) {
		goto err;
	}
	if (!(h264->pic1.pData[1] = (unsigned char*) g_malloc(usize, 1))) {
		goto err;
	}
	if (!(h264->pic1.pData[2] = (unsigned char*) g_malloc(vsize, 1))) {
		goto err;
	}

	// if (!(h264->pic2.pData[0] = (unsigned char*) g_malloc(ysize, 1))) {
	// 	goto err;
	// }
	// if (!(h264->pic2.pData[1] = (unsigned char*) g_malloc(usize, 1))) {
	// 	goto err;
	// }
	// if (!(h264->pic2.pData[2] = (unsigned char*) g_malloc(vsize, 1))) {
	// 	goto err;
	// }

	memset(h264->pic1.pData[0], 0, ysize);
	memset(h264->pic1.pData[1], 0, usize);
	memset(h264->pic1.pData[2], 0, vsize);

	// memset(h264->pic2.pData[0], 0, ysize);
	// memset(h264->pic2.pData[1], 0, usize);
	// memset(h264->pic2.pData[2], 0, vsize);

	//if ((create_openh264_encoder(&h264->pEncoder) != 0) || !h264->pEncoder) {
	if ((WelsCreateSVCEncoder(&h264->pEncoder) != 0) || !h264->pEncoder) {
		LOG(LOG_LEVEL_ERROR, "Failed to create H.264 encoder");
		goto err;
	}

	g_memset(&encParamExt, 0, sizeof(encParamExt));
	if ((*h264->pEncoder)->GetDefaultParams(h264->pEncoder, &encParamExt)) {
		LOG(LOG_LEVEL_ERROR, "Failed to retrieve H.264 default ext params");
		goto err;
	}

	encParamExt.iUsageType = SCREEN_CONTENT_REAL_TIME;
	encParamExt.iPicWidth = h264Width;
	encParamExt.iPicHeight = h264Height;
	encParamExt.iRCMode = RC_BITRATE_MODE;
	encParamExt.fMaxFrameRate = (float)h264->frameRate;
	encParamExt.iTargetBitrate = h264->bitRate;
	encParamExt.iMaxBitrate = UNSPECIFIED_BIT_RATE;
	encParamExt.bEnableDenoise = 0;
	encParamExt.bEnableLongTermReference = 0;
	encParamExt.bEnableFrameSkip = 0;
	encParamExt.iSpatialLayerNum = 1;
	encParamExt.sSpatialLayers[0].fFrameRate = encParamExt.fMaxFrameRate;
	encParamExt.sSpatialLayers[0].iVideoWidth = encParamExt.iPicWidth;
	encParamExt.sSpatialLayers[0].iVideoHeight = encParamExt.iPicHeight;
	encParamExt.sSpatialLayers[0].iSpatialBitrate = encParamExt.iTargetBitrate;
	encParamExt.sSpatialLayers[0].iMaxSpatialBitrate = encParamExt.iMaxBitrate;

	encParamExt.iMultipleThreadIdc = 1;
	encParamExt.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
	encParamExt.sSpatialLayers[0].sSliceArgument.uiSliceNum = 1;

	encParamExt.iEntropyCodingModeFlag = 0;
	encParamExt.bEnableFrameCroppingFlag = 0;

	if (encParamExt.iMultipleThreadIdc > 1) {
		encParamExt.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
		encParamExt.sSpatialLayers[0].sSliceArgument.uiSliceNum = encParamExt.iMultipleThreadIdc;
		h264->nullValue = 20 * encParamExt.iMultipleThreadIdc;
		LOG(LOG_LEVEL_DEBUG, "Using %hu threads for h.264 encoding (nullValue=%"PRIu32")", encParamExt.iMultipleThreadIdc, h264->nullValue);
	} else {
		h264->nullValue = 16;
	}

	if ((*h264->pEncoder)->InitializeExt(h264->pEncoder, &encParamExt)) {
		LOG(LOG_LEVEL_ERROR, "Failed to initialize H.264 encoder");
		goto err;
	}

	bitrate.iLayer = SPATIAL_LAYER_ALL;
	bitrate.iBitrate = h264->bitRate;
	if ((*h264->pEncoder)->SetOption(h264->pEncoder, ENCODER_OPTION_BITRATE, &bitrate)) {
		LOG(LOG_LEVEL_ERROR, "Failed to set encoder bitrate to %d", bitrate.iBitrate);
		goto err;
	}

	bitrate.iLayer = SPATIAL_LAYER_0;
	bitrate.iBitrate = 0;
	if ((*h264->pEncoder)->GetOption(h264->pEncoder, ENCODER_OPTION_MAX_BITRATE, &bitrate)) {
		LOG(LOG_LEVEL_ERROR, "Failed to get encoder max bitrate");
		goto err;
	}
	h264->maxBitRate = bitrate.iBitrate;
	/* WLog_DBG(TAG, "maxBitRate: %"PRIu32"", h264->maxBitRate); */

	return h264;

err:
	if (h264) {
		if (h264->pEncoder) {
			WelsDestroySVCEncoder(h264->pEncoder);
		}
		g_free(h264->pic1.pData[0]);
		g_free(h264->pic1.pData[1]);
		g_free(h264->pic1.pData[2]);
		g_free(h264->pic2.pData[0]);
		g_free(h264->pic2.pData[1]);
		g_free(h264->pic2.pData[2]);
		free(h264);
	}

	return NULL;
}
