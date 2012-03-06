/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Framebuffer operations
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

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/fb.h>

#include "common.h"
#include "fb.h"

int fb_open(struct instance *i, char *name)
{
	struct fb_var_screeninfo fbinfo;
	int ret;

	i->fb.fd = open(name, O_RDWR);
	if (i->fb.fd < 0) {
		err("Failed to open frame buffer: %s", name);
		return -1;
	}

	ret = ioctl(i->fb.fd, FBIOGET_VSCREENINFO, &fbinfo);
	if (ret != 0) {
		err("Failed to get frame buffer properties");
		return -1;
	}
	dbg("Framebuffer properties: xres=%d, yres=%d, bpp=%d",
		fbinfo.xres, fbinfo.yres, fbinfo.bits_per_pixel);
	dbg("Virtual resolution: vxres=%d vyres=%d",
		fbinfo.xres_virtual, fbinfo.yres_virtual);

	i->fb.width		= fbinfo.xres;
	i->fb.height		= fbinfo.yres;
	i->fb.virt_width	= fbinfo.xres_virtual;
	i->fb.virt_height	= fbinfo.yres_virtual;
	i->fb.bpp		= fbinfo.bits_per_pixel;
	i->fb.stride		= i->fb.virt_width * i->fb.bpp / 8;
	i->fb.full_size		= i->fb.stride * i->fb.virt_height;
	i->fb.size		= i->fb.stride * fbinfo.yres;

	i->fb.p[0] = mmap(0, i->fb.full_size, PROT_WRITE | PROT_READ,
				MAP_SHARED, i->fb.fd, 0);

	i->fb.buffers = 1;

	if (i->fb.double_buf) {
		i->fb.p[1] = i->fb.p[0] + i->fb.size;
		i->fb.buffers = 2;
	}

	return fb_set_virt_y_offset(i, 0);
}

int fb_set_virt_y_offset(struct instance *i, int yoffs)
{
	struct fb_var_screeninfo var;
	int ret;

	ret = ioctl(i->fb.fd, FBIOGET_VSCREENINFO, &var);
	if (ret != 0) {
		err("Failed to get frame buffer screen information");
		return -1;
	}

	var.yoffset = yoffs;

	ret = ioctl(i->fb.fd, FBIOPAN_DISPLAY, &var);
	if (ret != 0) {
		err("Failed to set y_offset of frame buffer");
		return -1;
	}

	return 0;
}

int fb_wait_for_vsync(struct instance *i)
{
	int ret;
	unsigned long temp;

	ret = ioctl(i->fb.fd, FBIO_WAITFORVSYNC, &temp);
	if (ret < 0) {
		err("Wait for vsync failed");
		return -1;
	}
	return 0;
}

void fb_close(struct instance *i)
{
	fb_set_virt_y_offset(i, 0);
	munmap(i->fb.p[0], i->fb.full_size);
	close(i->fb.fd);
}

