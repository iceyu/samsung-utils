/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * FIMC operations header file
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

#ifndef INCLUDE_FIMC_H
#define INCLUDE_FIMC_H

#include "common.h"

/* Open the FIMC device */
int	fimc_open(struct instance *i, char *name);
/* Close the FIMC device */
void	fimc_close(struct instance *i);
/* Set format in FIMC */
int	fimc_sfmt(struct instance *i, int width, int height,
		enum v4l2_buf_type type, unsigned long pix_fmt, int num_planes,
		struct v4l2_plane_pix_format planes[]);
/* Setup OUTPUT queue of FIMC basing on the configuration of MFC */
int	fimc_setup_output_from_mfc(struct instance *i);
/* Setup CAPTURE queue of FIMC basing on the configuration of the frame buffer */
int	fimc_setup_capture_from_fb(struct instance *i);
/* Control streaming status */
int	fimc_stream(struct instance *i, enum v4l2_buf_type type, int status);
/* Convenience function for queueing buffers from MFC */
int	fimc_dec_queue_buf_out_from_mfc(struct instance *i, int n);
/* Convenience function for queueing buffers from  frame buffer*/
int	fimc_dec_queue_buf_cap_from_fb(struct instance *i, int n);
/* Dequeue buffer */
int	fimc_dec_dequeue_buf(struct instance *i, int *n, int nplanes, int type);
/* Dequeue buffer from the CAPTURE queue. The argument *n is set to the index of
 * the dequeued buffer */
int	fimc_dec_dequeue_buf_cap(struct instance *i, int *n);
/* Dequeue buffer from the OUTPUT queue. The argument *n is set to the index of
 * the dequeued buffer. */
int	fimc_dec_dequeue_buf_out(struct instance *i, int *n);
/* Setup crop in FIMC */
int	fimc_set_crop(struct instance *i, int type, int width, int height,
							int left, int top);



#endif /* INCLUDE_FIMC_H */

