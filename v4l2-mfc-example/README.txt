V4L2 Codec decoding example application
by Kamil Debski <k.debski@samsung.com>

===========
* License *
===========
Copyright 2012 Samsung Electronics Co., Ltd.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

=============
* About MFC *
=============

MFC stands for Multi Format Codec. It is a hardware module available in the
Exynos family of SoCs. It is capable of decoding a range of compressed
streams with resolution up to 1080p at 30 fps. This includes H264, H264,
MPEG1, MPEG2, MPEG4 and VC1. It also supports encoding in H264 and H263 format
up to 1080p and MPEG up to D1 resolution.

==============
* About FIMC *
==============

Although this is the example application for MFC it also uses the FIMC hardware
module.

=======================
* About the interface *
=======================

The interface used by the MFC 5.1 driver is the Video4Linux2 framework.
It acts as a memory to memory device. It supports up to 16 contexts, each of
them can be setup to do encoding or decoding.

=========================
* About the application *
=========================

The purpose of this applications is to present and explain the usage of hardware
video codecs with the V4L2 interface. Also the application demonstrates how
should the stream be parsed and cut to successfully decode it with MFC.

The application was written to make is easy to understand which steps are
necessary to setup and conduct video decoding.


=================================
* Decoding in a few short words *
=================================

The main reference is the code which will guide you through the necessary steps.
However here is a short summary.

The header of the stream determines the resolution of the decompressed frames
thus it has to be processed before the buffers are allocated. The setup of
decoding is two-fold: first the OUTPUT queue is setup and it used to process the
header. After the header is processed the resolution of the CAPTURE buffers is
known and the application can safely setup the CAPTURE queue. To read the
parameters of the decoded movie G_FMT ioctl call is used.
Also the V4L2_CID_MIN_BUFFERS_FOR_CAPTURE control value is useful. It tells the
minimum number of CAPTURE buffers that have to be queued in the queue to enable
processing by the hardware. It is related to the number of frames that have
to be kept as reference frames in the decoder. It depends on the coded used and
its settings. The application can choose to allocate N numbers more to ensure
that these N buffers can be accessed by the application while the MFC keeps
processing new frames.

After the decoding has been setup it the application has to supply stream
buffers and processed the decoded frames. It is convenient to use three threads:
one for stream parser, one for handling the decoded frame and on for processing
the decoded frames.

Finishing streaming - when the end of stream is detected it is necessary to
extract and process all the remaining frames that have been kept as reference.
To do this the application should queue an empty (bytesused = 0) buffer on
OUTPUT after the end of stream has been detected and dequeue all remaining
CAPTURE buffers have been dequeued. The first empty buffer signalises MFC that
the end of stream has been reached and initializes decoding ending procedure.

===========================
* Running the application *
===========================

The application takes a few necessary arguments. Obviously you have to specify
which file to play and the used codec. Also you need to provide information on
which devices to use for processing.

Options:
-c <codec> - The codec of the encoded stream
	     Available codecs: mpeg4, h264
-d <device>  - Frame buffer device (e.g. /dev/fb0)
-f <device> - FIMC device (e.g. /dev/video4)
-i <file> - Input file name
-m <device> - MFC device (e.g. /dev/video8)
-V - synchronise to vsync

For example the following command:

./v4l2_decode -f /dev/video4 -m /dev/video8 -d /dev/fb0 -c mpeg4 -i shrek.m4v

would play the shrek.m4v video clip using /dev/video4 FIMC, /dev/video8 MFC
and /dev/fb0 frame buffer to display the movie. The -c option specifies the
mpeg4 codec.

To determine which devices to use you can try the following commands.
The number next to /dev/video may depend on your kernel configuration.

For MFC:
dmesg | grep -e s5p-mfc.*decoder.*/dev/video
which outputs:
[    2.160683] s5p-mfc s5p-mfc: decoder registered as /dev/video8

For FIMC:
dmesg | grep -e fimc...m2m
which outputs:
[    2.108768] s5p-fimc-md: Registered exynos4-fimc.0.m2m as /dev/video0
[    2.120782] s5p-fimc-md: Registered exynos4-fimc.1.m2m as /dev/video2
[    2.133962] s5p-fimc-md: Registered exynos4-fimc.2.m2m as /dev/video4
[    2.147145] s5p-fimc-md: Registered exynos4-fimc.3.m2m as /dev/video6

