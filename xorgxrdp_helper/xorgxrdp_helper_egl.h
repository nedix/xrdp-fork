/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2022
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
 */

#ifndef _XORGXRDP_HELPER_EGL_H
#define _XORGXRDP_HELPER_EGL_H

int
xorgxrdp_helper_inf_egl_init(void);
int
xorgxrdp_helper_inf_egl_create_image(Pixmap pixmap, inf_image_t *inf_image);
int
xorgxrdp_helper_inf_egl_destroy_image(inf_image_t inf_image);
int
xorgxrdp_helper_inf_egl_bind_tex_image(inf_image_t inf_image);
int
xorgxrdp_helper_inf_egl_release_tex_image(inf_image_t inf_image);

#endif
