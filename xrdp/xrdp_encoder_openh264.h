/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg and Christopher Pitstick, 2023
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
 * openh264 Encoder
 */

#ifndef _XRDP_ENCODER_OPENH264_H
#define _XRDP_ENCODER_OPENH264_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WITH_OPENH264

typedef struct {
	ISVCEncoder *pEncoder;
	SSourcePicture pic1;
	SSourcePicture pic2;
	uint32_t scrWidth;
	uint32_t scrHeight;
	uint32_t scrStride;
	uint32_t maxBitRate;
	uint32_t frameRate;
	uint32_t bitRate;
	uint32_t nullCount;
	uint32_t nullValue;
} openh264_context;

int ogon_openh264_library_open(void);
void ogon_openh264_library_close(void);
int ogon_openh264_compress(openh264_context *h264, uint32_t newFrameRate,
                            uint32_t targetFrameSizeInBits, uint8_t *data, uint8_t **ppDstData,
                            uint32_t *pDstSize, ogon_openh264_compress_mode avcMode, int *pOptimizable);
void ogon_openh264_context_free(openh264_context *h264);
struct openh264_context *ogon_openh264_context_new(uint32_t scrWidth, uint32_t scrHeight, uint32_t scrStride);

#endif /* WITH_OPENH264 defined   */
#endif /* _XRDP_ENCODER_OPENH264_H */
