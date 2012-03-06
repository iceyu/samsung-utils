/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Argument parser
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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "common.h"
#include "parser.h"


void print_usage(char *name)
{
	// "d:f:i:m:c:V"
	printf("Usage:\n");
	printf("\t./%s\n", name);
	printf("\t-c <codec> - The codec of the encoded stream\n");
	printf("\t\t     Available codecs: mpeg4, h264\n");
	printf("\t-d <device>  - Frame buffer device (e.g. /dev/fb0)\n");
	printf("\t-f <device> - FIMC device (e.g. /dev/video4)\n");
	printf("\t-i <file> - Input file name\n");
	printf("\t-m <device> - MFC device (e.g. /dev/video8)\n");
	printf("\t-V - synchronise to vsync\n");
	//printf("\t- <device> - \n");
	printf("\tp2\n");
	printf("\n");
}

void init_to_defaults(struct instance *i)
{
	memset(i, 0, sizeof(*i));
}

int get_codec(char *str)
{
	if (strncasecmp("mpeg4", str, 5) == 0) {
		return V4L2_PIX_FMT_MPEG4;
	} else if (strncasecmp("h264", str, 5) == 0) {
		return V4L2_PIX_FMT_H264;
	}
	return 0;
}

int parse_args(struct instance *i, int argc, char **argv)
{
	int c;

	init_to_defaults(i);

	while ((c = getopt(argc, argv, "c:d:f:i:m:V")) != -1) {
		switch (c) {
		case 'c':
			i->parser.codec = get_codec(optarg);
			break;
		case 'd':
			i->fb.name = optarg;
			break;
		case 'f':
			i->fimc.name = optarg;
			break;
		case 'i':
			i->in.name = optarg;
			break;
		case 'm':
			i->mfc.name = optarg;
			break;
		case 'V':
			i->fb.double_buf = 1;
			break;
		default:
			err("Bad argument");
			return -1;
		}
	}

	if (!i->in.name || !i->fb.name || !i->fimc.name || !i->mfc.name) {
		err("The following arguments are required: -d -f -i -m -c");
		return -1;
	}

	if (!i->parser.codec) {
		err("Unknown or not set codec (-c)");
		return -1;
	}

	switch (i->parser.codec) {
	case V4L2_PIX_FMT_MPEG4:
		i->parser.func = parse_mpeg4_stream;
		break;
	case V4L2_PIX_FMT_H264:
		i->parser.func = parse_h264_stream;
		break;
	}

	return 0;
}

