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

#include <wels/codec_api.h>
#include <wels/codec_def.h>

//#ifdef WITH_OPENH264

typedef struct openh264_context {
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

int
ogon_openh264_library_open(void);
void
ogon_openh264_library_close(void);
void *
xrdp_encoder_openh264_create(uint32_t scrWidth, uint32_t scrHeight, uint32_t scrStride);
int
xrdp_encoder_openh264_encode(void *handle, int session,
                        	 int width, int height, int format, const char *data,
                         	 char *cdata, int *cdata_bytes);
int
xrdp_encoder_openh264_delete(void *handle);
struct openh264_context *
ogon_openh264_context_new(uint32_t scrWidth, uint32_t scrHeight, uint32_t scrStride);

//#endif /* WITH_OPENH264 defined   */
#endif /* _XRDP_ENCODER_OPENH264_H */
