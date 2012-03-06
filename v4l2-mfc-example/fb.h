/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Framebuffer operations header file
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef INCLUDE_FB_H
#define INCLUDE_FB_H

/* Open and mmap frame buffer. Also read its properties */
int	fb_open(struct instance *i, char *name);
/* Unmap and close the framebuffer */
void	fb_close(struct instance *i);
/* Set virtual y offset of the frame buffer, this is used for vsync
 * synchronisation */
int	fb_set_virt_y_offset(struct instance *i, int yoffs);
/* Wait for vsync synchronisation */
int	fb_wait_for_vsync(struct instance *i);

#endif /* INCLUDE_FB_H */

