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

#include "xrdp.h"
#include "arch.h"
#include "os_calls.h"
#include <dlfcn.h>
#include "xrdp_encoder_openh264.h"

static void *openh264lib = NULL;
static int h264_init_success = 0;

typedef int (*pfn_create_openh264_encoder)(ISVCEncoder **ppEncoder);
typedef void (*pfn_destroy_openh264_encoder)(ISVCEncoder *pEncoder);
typedef void (*pfn_get_openh264_version)(OpenH264Version *pVersion);

pfn_create_openh264_encoder create_openh264_encoder = NULL;
pfn_destroy_openh264_encoder destroy_open_h264_encoder = NULL;
pfn_get_openh264_version get_openh264_version = NULL;

char* OPENH264_LIBRARY = "libogon-openh264.so";

int
xrdp_encoder_openh264_encode(void *handle, int session,
                        	 int enc_width, int enc_height, int format, const char *data,
                         	 char *cdata, int *cdata_bytes)
{
	SFrameBSInfo info;
	SSourcePicture *sourcePicture = NULL;
    SSourcePicture pic_in;
    SFrameBSInfo frame_out;
    ISVCEncoder *encoder;
    int32_t width, height;
	int i, j, status;

	if (!handle || !h264_init_success) {
		return 0;
	}

	struct openh264_context *h264;
	h264 = (struct openh264_context *) handle;

	width = (int32_t)h264->scrWidth;
	height = (int32_t)h264->scrHeight;

	if (status != 0) {
		LOG(LOG_LEVEL_ERROR, "yuv conversion failed");
		return 0;
	}

	h264->frameRate = 24;

	memset(&info, 0, sizeof(SFrameBSInfo));

	if ((data != NULL) && (encoder != NULL))
    {
        pic_in.iColorFormat = EVideoFormatType::videoFormatI420;
        pic_in.iStride[0] = width;
        pic_in.iStride[1] = pic_in.iStride[2] = width >> 1;
        pic_in.pData[0] = (unsigned char *)data;
        pic_in.pData[1] = pic_in.pData[0] + width * height;
        pic_in.pData[2] = pic_in.pData[1] + width * height / 4;

        // prepare output buffer
        SFrameBSInfo bsInfo = { 0 };
        bsInfo.iBufferStatus = 0;
        bsInfo.iPayloadSize = *cdata_bytes;
        bsInfo.pBsBuf = (uint8_t *)cdata;

        // encode picture
        if (encoder->EncodeFrame(&srcPic, &bsInfo) != 0 ||
            bsInfo.iBufferStatus != 1 ||
            bsInfo.iLayerNum != 1 ||
            bsInfo.iNalCount < 1)
        {
            return -3;
        }
	}


	memcpy(pic_in.img.plane[0], data, full_size);
    memcpy(pic_in.img.plane[1], data + full_size, quarter_size);
    memcpy(pic_in.img.plane[2], data + full_size * 5 / 4, quarter_size);

	sourcePicture = &h264->pic1;

	status = (*h264->pEncoder)->EncodeFrame(h264->pEncoder, sourcePicture, &info);

	if (status != 0) {
		LOG(LOG_LEVEL_ERROR, "Failed to encode frame");
		return 0;
	}

	if (info.eFrameType == videoFrameTypeSkip) {
		LOG(LOG_LEVEL_WARNING, "frame was skipped!");
		return 0;
	}

	*ppDstData = info.sLayerInfo[0].pBsBuf;
	*pDstSize = 0;

	for (i = 0; i < info.iLayerNum; i++) {
		for (j = 0; j < info.sLayerInfo[i].iNalCount; j++) {
			*pDstSize += info.sLayerInfo[i].pNalLengthInByte[j];
		}
	}
	/* WLog_DBG(TAG, "ENCODED SIZE (mode=%"PRIu32"): %"PRIu32" byte (%"PRIu32" bits)", avcMode, *pDstSize, (*pDstSize) * 8); */

	/**
	 * TODO:
	 * Maybe there is a better way to detect if encoding the same
	 * buffer again will actually improve quality.
	 * For now we consider 10 encodings with size <= h264->nullValue in a row
	 * as final.
	 */

	if (*pDstSize > h264->nullValue) {
		h264->nullCount = 0;
	} else {
		h264->nullCount++;
	}

	return 1;
}


int
xrdp_encoder_openh264_delete(void *handle)
{
	struct openh264_context *h264 = (struct openh264_context *)handle;
	if (!h264) {
		return 0;
	}

	if (h264->pEncoder) {
		destroy_open_h264_encoder(h264->pEncoder);
	}

	g_free(h264->pic1.pData[0]);
	g_free(h264->pic1.pData[1]);
	g_free(h264->pic1.pData[2]);

	g_free(h264->pic2.pData[0]);
	g_free(h264->pic2.pData[1]);
	g_free(h264->pic2.pData[2]);

	free(h264);

	return 0;
}

/*****************************************************************************/
void *xrdp_encoder_openh264_create(uint32_t scrWidth, uint32_t scrHeight, uint32_t scrStride)
{
	struct openh264_context *h264 = NULL;
	uint32_t h264Width;
	uint32_t h264Height;
	SEncParamExt encParamExt;
	SBitrateInfo bitrate;
	size_t ysize, usize, vsize;

	if (!h264_init_success) {
		LOG(LOG_LEVEL_ERROR, "Cannot create OpenH264 context: library was not initialized");
		return NULL;
	}

	if (scrWidth < 16 || scrHeight < 16) {
		LOG(LOG_LEVEL_ERROR, "Error: Minimum height and width for OpenH264 is 16 but we got %"PRIu32" x %"PRIu32"", scrWidth, scrHeight);
		return NULL;
	}

	if (scrWidth % 16) {
		LOG(LOG_LEVEL_WARNING, "WARNING: screen width %"PRIu32" is not a multiple of 16. Expect degraded H.264 performance!", scrWidth);
	}

	if (!(h264 = (struct openh264_context *)calloc(1, sizeof(openh264_context)))) {
		LOG(LOG_LEVEL_ERROR, "Failed to allocate OpenH264 context");
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
	h264->scrStride = scrStride;

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

	if (!(h264->pic2.pData[0] = (unsigned char*) g_malloc(ysize, 1))) {
		goto err;
	}
	if (!(h264->pic2.pData[1] = (unsigned char*) g_malloc(usize, 1))) {
		goto err;
	}
	if (!(h264->pic2.pData[2] = (unsigned char*) g_malloc(vsize, 1))) {
		goto err;
	}

	memset(h264->pic1.pData[0], 0, ysize);
	memset(h264->pic1.pData[1], 0, usize);
	memset(h264->pic1.pData[2], 0, vsize);

	memset(h264->pic2.pData[0], 0, ysize);
	memset(h264->pic2.pData[1], 0, usize);
	memset(h264->pic2.pData[2], 0, vsize);

	if ((create_openh264_encoder(&h264->pEncoder) != 0) || !h264->pEncoder) {
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
			destroy_open_h264_encoder(h264->pEncoder);
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

void ogon_openh264_library_close(void) {
	if (openh264lib) {
		dlclose(openh264lib);
		openh264lib = NULL;
	}
}

int xrdp_encoder_openh264_open(void) 
{
	char* libh264;
	OpenH264Version cver;

	if (h264_init_success) {
		LOG(LOG_LEVEL_WARNING, "OpenH264 was already successfully initialized");
		return 1;
	}

	libh264 = getenv("LIBOPENH264");
	if (libh264) {
        LOG(LOG_LEVEL_DEBUG, "Loading OpenH264 library specified in environment: %s", libh264);
		if (!(openh264lib = dlopen(libh264, RTLD_NOW))) {
			LOG(LOG_LEVEL_ERROR, "Failed to load OpenH264 library: %s", dlerror());
			/* don't fail yet, we'll try to load the default library below ! */
		}
	}

	if (!openh264lib) {
		if (!(openh264lib = (void *)g_load_library(OPENH264_LIBRARY))) {
			LOG(LOG_LEVEL_WARNING, "Failed to load OpenH264 library: %s", dlerror());
			goto fail;
		}
	}

	if (!(create_openh264_encoder = (pfn_create_openh264_encoder)dlsym(openh264lib, "WelsCreateSVCEncoder"))) {
		LOG(LOG_LEVEL_ERROR, "Failed to get OpenH264 encoder creation function: %s", dlerror());
		goto fail;
	}

	if (!(destroy_open_h264_encoder = (pfn_destroy_openh264_encoder)dlsym(openh264lib, "WelsDestroySVCEncoder"))) {
		LOG(LOG_LEVEL_ERROR, "Failed to get OpenH264 encoder destroy function: %s", dlerror());
		goto fail;
	}

	if (!(get_openh264_version = (pfn_get_openh264_version)dlsym(openh264lib, "WelsGetCodecVersionEx"))) {
		LOG(LOG_LEVEL_ERROR, "Failed to get OpenH264 version function: %s", dlerror());
		goto fail;
	}

	g_memset(&cver, 0, sizeof(cver));

	get_openh264_version(&cver);

	LOG(LOG_LEVEL_DEBUG, "OpenH264 codec version: %u.%u.%u.%u",
			cver.uMajor, cver.uMinor, cver.uRevision, cver.uReserved);

	// if (cver.uMajor != OPENH264_MAJOR || cver.uMinor != OPENH264_MINOR) {
	// 	WLog_ERR(TAG, "The loaded OpenH264 library is incompatible with this build (%d.%d.%d.%d)",
	// 		OPENH264_MAJOR, OPENH264_MINOR, OPENH264_REVISION, OPENH264_RESERVED);
	// 	goto fail;
	// }

	LOG(LOG_LEVEL_DEBUG, "Successfully initialized OpenH264 library");

	h264_init_success = 1;
	return 1;

fail:
	create_openh264_encoder = NULL;
	destroy_open_h264_encoder = NULL;
	get_openh264_version = NULL;
	ogon_openh264_library_close();
	h264_init_success = 0;
	return 0;
}

/*****************************************************************************/
// void *
// xrdp_encoder_openh264_create(void)
// {
//     struct x264_global *xg;

//     LOG_DEVEL(LOG_LEVEL_TRACE, "xrdp_encoder_x264_create:");
//     xg = (struct x264_global *) g_malloc(sizeof(struct x264_global), 1);
//     if (xg == 0)
//     {
//         return 0;
//     }
//     return xg;
// }

// /*****************************************************************************/
// int
// xrdp_encoder_x264_delete(void *handle)
// {
//     struct x264_global *xg;
//     struct x264_encoder *xe;
//     int index;

//     if (handle == 0)
//     {
//         return 0;
//     }
//     xg = (struct x264_global *) handle;
//     for (index = 0; index < 16; index++)
//     {
//         xe = &(xg->encoders[index]);
//         if (xe->x264_enc_han != 0)
//         {
//             x264_encoder_close(xe->x264_enc_han);
//         }
//         g_free(xe->yuvdata);
//     }
//     g_free(xg);
//     return 0;
// }

// /*****************************************************************************/
// int
// xrdp_encoder_x264_encode(void *handle, int session,
//                          int width, int height, int format, const char *data,
//                          char *cdata, int *cdata_bytes)
// {
//     struct x264_global *xg;
//     struct x264_encoder *xe;
//     //const char *src8;
//     //char *dst8;
//     //int index;
//     x264_nal_t *nals;
//     int num_nals;
//     int frame_size;
//     //int frame_area;

//     x264_picture_t pic_in;
//     x264_picture_t pic_out;

//     width = (width + 15) & ~15;
// 	height = (height + 15) & ~15; 

//     LOG(LOG_LEVEL_TRACE, "xrdp_encoder_x264_encode:");
//     xg = (struct x264_global *) handle;
//     xe = &(xg->encoders[session]);
//     if ((xe->x264_enc_han == 0) || (xe->width != width) || (xe->height != height))
//     {
//         if (xe->x264_enc_han != 0)
//         {
//             x264_encoder_close(xe->x264_enc_han);
//             xe->x264_enc_han = 0;
//             g_free(xe->yuvdata);
//             xe->yuvdata = 0;
//         }
//         if ((width > 0) && (height > 0))
//         {
//             //x264_param_default_preset(&(xe->x264_params), "superfast", "zerolatency");
//             //x264_param_default_preset(&(xe->x264_params), "ultrafast", "zerolatency");
//             x264_param_default_preset(&(xe->x264_params), "veryfast", "zerolatency");
//             xe->x264_params.i_threads = 10;
//             xe->x264_params.i_width = width;
//             xe->x264_params.i_height = height;
//             xe->x264_params.i_fps_num = 24;
//             xe->x264_params.i_fps_den = 1;
//             xe->x264_params.b_repeat_headers = 1;
//             xe->x264_params.i_slice_count = 1;
//             //xe->x264_params.b_cabac = 1;
//             xe->x264_params.i_bframe = 0;
//             xe->x264_params.b_annexb = 1;
//             xe->x264_params.rc.i_rc_method = X264_RC_CQP;
//             xe->x264_params.rc.i_qp_constant = 23;
//             //x264_param_apply_profile(&(xe->x264_params), "high");
//             x264_param_apply_profile(&(xe->x264_params), "high444");
//             xe->x264_enc_han = x264_encoder_open(&(xe->x264_params));
//             if (xe->x264_enc_han == 0)
//             {
//                 return 1;
//             }
//             xe->yuvdata = (char *) g_malloc(width * height * 2, 0);
//             if (xe->yuvdata == 0)
//             {
//                 x264_encoder_close(xe->x264_enc_han);
//                 xe->x264_enc_han = 0;
//                 return 2;
//             }
//         }
//         xe->width = width;
//         xe->height = height;
//     }

//     if ((data != 0) && (xe->x264_enc_han != 0))
//     {
//         // src8 = data;
//         // dst8 = xe->yuvdata;
//         // for (index = 0; index < height; index++)
//         // {
//         //     g_memcpy(dst8, src8, width);
//         //     src8 += width;
//         //     dst8 += xe->x264_params.i_width;
//         // }

//         // src8 = data;
//         // src8 += width * height;
//         // dst8 = xe->yuvdata;

//         // frame_area = xe->x264_params.i_width * xe->x264_params.i_height - 1;
//         // dst8 += frame_area;
//         // for (index = 0; index < height / 2; index++)
//         // {
//         //     g_memcpy(dst8, src8, width / 2);
//         //     src8 += width / 2;
//         //     dst8 += xe->x264_params.i_width / 2;
//         // }

//         // g_memset(&pic_in, 0, sizeof(pic_in));
//         // pic_in.img.i_csp = X264_CSP_I420;
//         // pic_in.img.i_plane = 3;
//         // pic_in.img.plane[0] = (unsigned char *) (xe->yuvdata);
//         // pic_in.img.plane[1] = (unsigned char *) (xe->yuvdata + frame_area);
//         // pic_in.img.plane[2] = (unsigned char *) (xe->yuvdata + frame_area + frame_area / 4);
//         // pic_in.img.i_stride[0] = xe->x264_params.i_width;
//         // pic_in.img.i_stride[1] = xe->x264_params.i_width / 2;
//         // pic_in.img.i_stride[2] = xe->x264_params.i_width / 2;

//         x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);

//         int full_size = width * height;
//         int quarter_size = full_size / 4;

//         // Copy input image to x264 picture structure
//         memcpy(pic_in.img.plane[0], data, full_size);
//         memcpy(pic_in.img.plane[1], data + full_size, quarter_size);
//         memcpy(pic_in.img.plane[2], data + full_size * 5 / 4, quarter_size);

//         if (format == 1) {
//             pic_in.i_type = X264_TYPE_KEYFRAME;
//         }

//         num_nals = 0;
//         frame_size = x264_encoder_encode(xe->x264_enc_han, &nals, &num_nals,
//                                          &pic_in, &pic_out);
//         //LOG(LOG_LEVEL_TRACE, "i_type %d", pic_out.i_type);
//         if (frame_size < 1)
//         {
//             return 3;
//         }
//         if (*cdata_bytes < frame_size)
//         {
//             return 4;
//         }
//         g_memcpy(cdata, nals[0].p_payload, frame_size);
//         *cdata_bytes = frame_size;
//         x264_picture_clean(&pic_in);
//     }
//     return 0;
// }


// /*****************************************************************************/
// int
// xrdp_encoder_openh264_encode(void *handle, int session, int width, int height, int format, const char *data, char *cdata, int *cdata_bytes)
// {
//     struct oh264_global *og;
//     SSourcePicture pic_in;
//     SFrameBSInfo frame_out;
//     ISVCEncoder *encoder;
//     EResult result;
//     int frame_size;

//     width = (width + 15) & ~15;
//     height = (height + 15) & ~15;

//     LOG(LOG_LEVEL_TRACE, "xrdp_encoder_openh264_encode:");
//     og = (struct oh264_global *) handle;
//     encoder = og->encoders[session];
//     if ((encoder == NULL) || (og->width[session] != width) || (og->height[session] != height))
//     {
//         if (encoder != NULL)
//         {
//             encoder->Uninitialize();
//             WelsDestroySVCEncoder(encoder);
//             og->encoders[session] = NULL;
//             delete[] og->yuvdata[session];
//             og->yuvdata[session] = NULL;
//         }
//         if ((width > 0) && (height > 0))
//         {
//             SEncParamBase encParam;
//             memset(&encParam, 0, sizeof(SEncParamBase));
//             encParam.iUsageType = CAMERA_VIDEO_REAL_TIME;
//             encParam.iPicWidth = width;
//             encParam.iPicHeight = height;
//             encParam.iTargetBitrate = 256; // Change bitrate as desired
//             encParam.iRCMode = RC_BITRATE_MODE;
//             encParam.fMaxFrameRate = 24.0;
//             encParam.iSpatialLayerNum = 1;
//             encParam.sSpatialLayers[0].iVideoWidth = width;
//             encParam.sSpatialLayers[0].iVideoHeight = height;
//             encParam.sSpatialLayers[0].fFrameRate = encParam.fMaxFrameRate;
//             encParam.sSpatialLayers[0].iSpatialBitrate = encParam.iTargetBitrate;
//             encParam.iMaxQP = 40;
//             encParam.iMinQP = 10;
//             encParam.iTemporalLayerNum = 1;
//             encParam.sSpatialLayers[0].iTemporalLayerNum = 1;
//             encParam.iMultipleThreadIdc = 1;
//             encoder = NULL;
//             result = WelsCreateSVCEncoder(&encoder);
//             if (result != cmResultSuccess || encoder == NULL)
//             {
//                 return 1;
//             }
//             result = encoder->Initialize(&encParam);
//             if (result != cmResultSuccess)
//             {
//                 encoder->Uninitialize();
//                 WelsDestroySVCEncoder(encoder);
//                 return 1;
//             }
//             og->yuvdata[session] = new char[width * height * 3 / 2];
//             if (og->yuvdata[session] == NULL)
//             {
//                 encoder->Uninitialize();
//                 WelsDestroySVCEncoder(encoder);
//                 return 2;
//             }
//         }
//         og->width[session] = width;
//         og->height[session] = height;
//         og->encoders[session] = encoder;
//     }

//     if ((data != NULL) && (encoder != NULL))
//     {
//         pic_in.iColorFormat = EVideoFormatType::videoFormatI420;
//         pic_in.iStride[0] = width;
//         pic_in.iStride[1] = pic_in.iStride[2] = width >> 1;
//         pic_in.pData[0] = (unsigned char *)data;
//         pic_in.pData[1] = pic_in.pData[0] + width * height;
//         pic_in.pData[2] = pic_in.pData[1] + width * height / 4;

//         // prepare output buffer
//         SFrameBSInfo bsInfo = { 0 };
//         bsInfo.iBufferStatus = 0;
//         bsInfo.iPayloadSize = *cdata_bytes;
//         bsInfo.pBsBuf = (uint8_t *)cdata;

//         // encode picture
//         if (encoder->EncodeFrame(&srcPic, &bsInfo) != 0 ||
//             bsInfo.iBufferStatus != 1 ||
//             bsInfo.iLayerNum != 1 ||
//             bsInfo.iNalCount < 1)
//         {
//             return -3;
//         }
