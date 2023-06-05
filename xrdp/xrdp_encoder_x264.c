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

    //width = (width + 15) & ~15;    /* codec bitstream width must be a multiple of 16 */
    //height = (height + 15) & ~15;  /* codec bitstream height must be a multiple of 16 */

    LOG(LOG_LEVEL_TRACE, "xrdp_encoder_x264_encode:");
    xg = (struct x264_global *) handle;
    xe = &(xg->encoders[session]);
    if ((xe->x264_enc_han == 0) || (xe->width != width) || (xe->height != height))
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
            //x264_param_default_preset(&(xe->x264_params), "superfast", "zerolatency");
            //x264_param_default_preset(&(xe->x264_params), "ultrafast", "zerolatency");
            x264_param_default_preset(&(xe->x264_params), "ultrafast", "zerolatency");

            xe->x264_params.i_width = width;
            xe->x264_params.i_height = height;
            xe->x264_params.i_threads = 1;
            xe->x264_params.i_fps_num = 24;
            xe->x264_params.i_fps_den = 1;
            xe->x264_params.rc.i_rc_method = X264_RC_CQP;
            xe->x264_params.rc.i_qp_constant = 23;
            //xe->x264_params.b_open_gop = 1;
            //xe->x264_params.i_slice_max_size = 0;
            //xe->x264_params.b_vfr_input = 0;
            //xe->x264_params.b_sliced_threads = 0;
            //xe->x264_params.i_nal_hrd = 0;
            //xe->x264_params.b_pic_struct = 0;
            //xe->x264_params.b_repeat_headers = 1;
            //xe->x264_params.i_bframe = 2;
            //xe->x264_params.i_keyint_max = 2;
            //xe->x264_params.b_cabac = 1;
            //xe->x264_params.i_bframe = 0;
            //xe->x264_params.b_full_recon = 1;
            //xe->x264_params.b_vfr_input = 0;
            //xe->x264_params.i_bframe_pyramid = 1;
            //xe->x264_params.i_bframe_adaptive = 1;
            //xe->x264_params.b_interlaced = 0;
            //xe->x264_params.b_fake_interlaced = 1;
            //xe->x264_params.i_frame_packing = 6;
            //xe->x264_params.i_bframe_adaptive = 1;
            //xe->x264_params.b_pic_struct = 1;
            //xe->x264_params.b_stitchable = 0;
            //xe->x264_params.rc.b_mb_tree = 1;
            //xe->x264_params.b_annexb = 0;
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
        //int full_size = width * height;
        //int quarter_size = full_size / 4;

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

        frame_area = xe->x264_params.i_width * xe->x264_params.i_height;
        dst8 += frame_area;
        for (index = 0; index < height; index++)
        {
            g_memcpy(dst8, src8, width / 2);
            src8 += width / 2;
            dst8 += xe->x264_params.i_width / 2;
        }

        g_memset(&pic_in, 0, sizeof(pic_in));
        pic_in.img.i_csp = X264_CSP_I420;
        pic_in.img.i_plane = 3;
        pic_in.i_pic_struct = format + 2;
        pic_in.img.plane[0] = (unsigned char *) (xe->yuvdata);
        pic_in.img.plane[1] = (unsigned char *) (xe->yuvdata + frame_area);
        pic_in.img.plane[2] = (unsigned char *) (xe->yuvdata + frame_area * 5 / 4);
        pic_in.img.i_stride[0] = xe->x264_params.i_width;
        pic_in.img.i_stride[1] = xe->x264_params.i_width / 2;
        pic_in.img.i_stride[2] = xe->x264_params.i_width / 2;

        //pic_in.i_pic_struct = PIC_STRUCT_AUTO;

        //x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);
        // Copy input image to x264 picture structure
        //memcpy(pic_in.img.plane[0], data, full_size);
        //memcpy(pic_in.img.plane[1], data + full_size, quarter_size);
        //memcpy(pic_in.img.plane[2], data + full_size * 5 / 4, quarter_size);
        //pic_in.param->b_annexb = 1;
        pic_in.param = &xe->x264_params;
        //pic_in.i_type = X264_TYPE_KEYFRAME;

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
        int total_size = 0;
        for (int i = 0; i < num_nals; ++i)
        {
            x264_nal_t *nal = nals + i;
            int size = nal->i_payload;
            uint8_t* payload = nal->p_payload;
            char* write_location = cdata + total_size;
            int nalUnitType = nal->i_type;
            uint32_t t;
            // Check slice type
            // if ((nalUnitType == 1 || nalUnitType == 2) && nal->i_ref_idc != 0)
            // {
            //     // B-frame detected
            //     LOG(LOG_LEVEL_INFO, "B-frame found!\n");
            //     continue;
            // }

            switch (nalUnitType)
            {
                case NAL_SLICE:
                case NAL_SLICE_IDR:
                case NAL_SPS:
                case NAL_PPS:
                case NAL_AUD:
                    //LOG(LOG_LEVEL_INFO, "P: 0x%02X, 0x%02X, 0x%02X, 0x%02X", payload[0], payload[1], payload[2], payload[3]);
                    //= payload[0] >> 4 | payload[1] >> 3 | payload[2] >> 2 | payload[3];
                    g_memcpy(&t, payload, 4);
                    t = bswap_32(t);
                    if (t != 0x00000001)
                    {
                        const char *d = "\x00\x00\x00\x01";
                        g_memcpy(write_location, d, 4);
                        g_memcpy(write_location + 4, payload + 3, size - 3);
                        size += 1;
                        g_memcpy(&t, write_location, 4);
                        t = bswap_32(t);
                        LOG(LOG_LEVEL_INFO, "P Int: 0x%08X", t);
                    }
                    else
                    {
                        g_memcpy(write_location, payload, size);
                    }
                    total_size += size;
                    break;
                default:
                    continue;
            }
        }
        *cdata_bytes = total_size;

        //g_memcpy(cdata, nals[0].p_payload, frame_size);
        //*cdata_bytes = frame_size;
    }
    return 0;
}

// bitStreamReader->U(8);

// 1 + 1 + 1 + 1 + 4 + 8
// 			seq_parameter_set_id = bitStreamReader->Uev();

// 			if (profile_idc  == 100 || profile_idc  == 110 ||
// 				profile_idc  == 122 || profile_idc  == 144)
// 			{

// 				chroma_format_idc = bitStreamReader->Uev();

// 				if (chroma_format_idc == 3)
// 				{
// 					separate_colour_plane_flag = bitStreamReader->U(1);
// 				}

// 				bit_depth_luma_minus8 = bitStreamReader->Uev();
// 				bit_depth_chroma_minus8 = bitStreamReader->Uev();
// 				qpprime_y_zero_transform_bypass_flag  = bitStreamReader->U(1);
// 				seq_scaling_matrix_present_flag =  bitStreamReader->U(1);

// 				if( seq_scaling_matrix_present_flag )
// 				{
// 					for(unsigned int ix = 0; ix < 8; ix++)
// 					{
// 						temp = bitStreamReader->U(1);

// 						if (temp)
// 						{
// 							ScalingList(ix, ix < 6 ? 16 : 64);
// 						}
// 					}
// 				}
// 			}


// 			log2_max_frame_num_minus4 = bitStreamReader->Uev();

// 			pic_order_cnt_type =  bitStreamReader->Uev();

// 			if (pic_order_cnt_type == 0)
// 			{
// 				log2_max_pic_order_cnt_lsb_minus4 = bitStreamReader->Uev();

// 			}

// 			else if (pic_order_cnt_type == 1)
// 			{
// 				delta_pic_order_always_zero_flag = bitStreamReader->U(1);
// 				offset_for_non_ref_pic = bitStreamReader->Sev();
// 				offset_for_top_to_bottom_field =  bitStreamReader->Sev();

// 				num_ref_frames_in_pic_order_cnt_cycle = bitStreamReader->Uev();

// 				for( int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++ )
// 				{

// 					int skippedParameter = bitStreamReader->Sev();
// 				}

// 			}



// 			num_ref_frames = bitStreamReader->Uev();
// 			gaps_in_frame_num_value_allowed_flag = bitStreamReader->U(1);

// 			pic_width_in_mbs_minus1 = bitStreamReader->Uev();
// 			pic_height_in_map_units_minus1 = bitStreamReader->Uev();


// profile_idc  =  bitStreamReader->U(8);

// 			constraint_set0_flag = bitStreamReader->U(1);
// 			constraint_set1_flag = bitStreamReader->U(1);
// 			constraint_set2_flag = bitStreamReader->U(1);
// 			constraint_set3_flag = bitStreamReader->U(1);
// 			reserved_zero_4bits = bitStreamReader->U(4);

// 			level_idc = bitStreamReader->U(8);
// 			seq_parameter_set_id = bitStreamReader->Uev();

// 			if (profile_idc  == 100 || profile_idc  == 110 ||
// 				profile_idc  == 122 || profile_idc  == 144)
// 			{

// 				chroma_format_idc = bitStreamReader->Uev();

// 				if (chroma_format_idc == 3)
// 				{
// 					separate_colour_plane_flag = bitStreamReader->U(1);
// 				}

// 				bit_depth_luma_minus8 = bitStreamReader->Uev();
// 				bit_depth_chroma_minus8 = bitStreamReader->Uev();
// 				qpprime_y_zero_transform_bypass_flag  = bitStreamReader->U(1);
// 				seq_scaling_matrix_present_flag =  bitStreamReader->U(1);

// 				if( seq_scaling_matrix_present_flag )
// 				{
// 					for(unsigned int ix = 0; ix < 8; ix++)
// 					{
// 						temp = bitStreamReader->U(1);

// 						if (temp)
// 						{
// 							ScalingList(ix, ix < 6 ? 16 : 64);
// 						}
// 					}
// 				}
// 			}


// 			log2_max_frame_num_minus4 = bitStreamReader->Uev();

// 			pic_order_cnt_type =  bitStreamReader->Uev();

// 			if (pic_order_cnt_type == 0)
// 			{
// 				log2_max_pic_order_cnt_lsb_minus4 = bitStreamReader->Uev();

// 			}

// 			else if (pic_order_cnt_type == 1)
// 			{
// 				delta_pic_order_always_zero_flag = bitStreamReader->U(1);
// 				offset_for_non_ref_pic = bitStreamReader->Sev();
// 				offset_for_top_to_bottom_field =  bitStreamReader->Sev();

// 				num_ref_frames_in_pic_order_cnt_cycle = bitStreamReader->Uev();

// 				for( int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++ )
// 				{

// 					int skippedParameter = bitStreamReader->Sev();
// 				}

// 			}



// 			num_ref_frames = bitStreamReader->Uev();
// 			gaps_in_frame_num_value_allowed_flag = bitStreamReader->U(1);

// 			pic_width_in_mbs_minus1 = bitStreamReader->Uev();
// 			pic_height_in_map_units_minus1 = bitStreamReader->Uev();



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
