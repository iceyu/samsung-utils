/*
 * mfc codec encoding example application
 * Andrzej Hajda <a.hajda@samsung.com>
 *
 * Command line helper functions.
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <stddef.h>

#include "common.h"
#include "args.h"

void print_usage(char const *name)
{
	printf("Usage: %s [args]\n"
	       "\t-i <device>   - FIMC camera device (e.g. /dev/video1)\n"
	       "\t                If not specified demo input device is used\n"
	       "\t-m <device>   - (required) MFC device (e.g. /dev/video8)\n"
	       "\t-o <file>     - Output file name\n"
	       "\t-c <codec>    - The codec of the encoded stream\n"
	       "\t                Available codecs: mpeg4, h263, h264\n"
	       "\t-d <duration> - Number of frames to encode\n"
	       "\t-r <rate>     - Frame rate\n"
	       "\t-b <bitrate>  - Bitrate\n"
	       "\t-s <size>     - Size of frame in format WxH\n"
		, name);
}

int get_codec(char *str)
{
	if (strncasecmp("mpeg4", str, 5) == 0)
		return V4L2_PIX_FMT_MPEG4;
	else if (strncasecmp("h263", str, 5) == 0)
		return V4L2_PIX_FMT_H263;
	else if (strncasecmp("h264", str, 5) == 0)
		return V4L2_PIX_FMT_H264;

	return 0;
}

void set_options_default(struct options *o)
{
	memset(o, 0, sizeof(*o));
	o->width = 176;
	o->height = 144;
	o->duration = 250;
	o->rate = 25;
	o->out_name = "demo.out";
	o->codec = V4L2_PIX_FMT_H264;
	o->bitrate = 1000;
}

int parse_args(struct options *opts, int argc, char **argv)
{
	int c;

	set_options_default(opts);

	while ((c = getopt(argc, argv, "i:m:o:c:d:r:s:b:")) != -1) {
		switch (c) {
		case 'i':
			opts->in_name = optarg;
			break;
		case 'm':
			opts->mfc_name = optarg;
			break;
		case 'o':
			opts->out_name = optarg;
			break;
		case 'c':
			opts->codec = get_codec(optarg);
			if (opts->codec == 0) {
				err("Unknown codec");
				return -1;
			}
			break;
		case 'd':
			opts->duration = atoi(optarg);
			break;
		case 'r':
			opts->rate = atoi(optarg);
			break;
		case 's': {
			char *sep = NULL;
			opts->width = strtol(optarg, &sep, 10);
			if (!sep || *sep != 'x') {
				err("Bad size, should be like 320x200");
				return -1;
			}
			opts->height = atoi(++sep);
			break;
		case 'b':
			opts->bitrate = atoi(optarg);
			break;
		}
		default:
			return -1;
		}
	}

	if (opts->mfc_name == NULL) {
		err("Please provide MFC device");
		return -1;
	}

	return 0;
}

