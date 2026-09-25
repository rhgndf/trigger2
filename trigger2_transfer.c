// SPDX-License-Identifier: GPL-2.0-only

#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>

#include "trigger2.h"
#include "trigger2_registers.h"

#define TRIGGER2_RGB_BLOCK_PIXELS	1024
#define TRIGGER2_CODEC_SCRATCH_SIZE	(3 * TRIGGER2_RGB_BLOCK_PIXELS)

void trigger2_stop_io(struct trigger2_device *trigger2)
{
	/* Drain ordered EP02 frame work before a mode uses the same endpoint. */
	WRITE_ONCE(trigger2->display_enabled, false);
	atomic_inc(&trigger2->io_generation);
	flush_workqueue(trigger2->transfer_wq);
}

static void trigger2_bulk_complete(struct urb *urb)
{
	struct trigger2_bulk_chunk *chunk = urb->context;

	complete(&chunk->complete);
}

static int trigger2_wait_chunk(struct trigger2_bulk_chunk *chunk)
{
	if (!wait_for_completion_timeout(&chunk->complete,
					 msecs_to_jiffies(TRIGGER2_BULK_TIMEOUT_MS)))
		return -ETIMEDOUT;
	if (chunk->urb->status)
		return chunk->urb->status;
	if (chunk->urb->actual_length != chunk->urb->transfer_buffer_length)
		return -EIO;
	return 0;
}

static int trigger2_send_bulk(struct trigger2_transfer *transfer,
			      const u8 *data, size_t len)
{
	unsigned int submitted = 0, completed = 0;
	struct trigger2_bulk_chunk *chunk;
	size_t offset = 0, count;
	int ret;

	/* The device expects EP02 payloads in requests of at most 20,480 bytes. */
	/* NULL data produces the zero-filled mode bitmap without staging it. */
	while (offset < len) {
		if (submitted - completed == TRIGGER2_BULK_URBS) {
			ret = trigger2_wait_chunk(&transfer->chunks[
						completed % TRIGGER2_BULK_URBS]);
			if (ret)
				goto cancel;
			completed++;
		}

		chunk = &transfer->chunks[submitted % TRIGGER2_BULK_URBS];
		count = min_t(size_t, len - offset, TRIGGER2_BULK_CHUNK_SIZE);
		if (data)
			memcpy(chunk->data, data + offset, count);
		else
			memset(chunk->data, 0, count);
		reinit_completion(&chunk->complete);
		chunk->urb->transfer_buffer_length = count;
		usb_anchor_urb(chunk->urb, &transfer->submitted);
		ret = usb_submit_urb(chunk->urb, GFP_KERNEL);
		if (ret) {
			usb_unanchor_urb(chunk->urb);
			goto cancel;
		}
		submitted++;
		offset += count;
	}

	while (completed < submitted) {
		ret = trigger2_wait_chunk(&transfer->chunks[
					completed % TRIGGER2_BULK_URBS]);
		if (ret)
			goto cancel;
		completed++;
	}
	return 0;

cancel:
	usb_kill_anchored_urbs(&transfer->submitted);
	return ret;
}

static void trigger2_transfer_work(struct work_struct *work)
{
	struct trigger2_transfer *transfer =
		container_of(work, struct trigger2_transfer, transfer_work);
	struct trigger2_device *trigger2 = transfer->trigger2;
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	int idx, ret, actual;

	if (!drm_dev_enter(&trigger2->drm, &idx))
		goto complete;

	if (!READ_ONCE(trigger2->display_enabled) ||
	    transfer->generation != atomic_read(&trigger2->io_generation))
		goto exit;

	/* The ordered workqueue keeps frame descriptors ahead of their payloads. */
	ret = usb_bulk_msg(udev, trigger2->bulk_pipe, transfer->header,
			   TRIGGER2_FRAME_HEADER_LEN, &actual,
			   TRIGGER2_BULK_TIMEOUT_MS);
	if (!ret && actual != TRIGGER2_FRAME_HEADER_LEN)
		ret = -EIO;
	if (!ret)
		ret = trigger2_send_bulk(transfer, transfer->buf.data,
					 transfer->frame_len);
	if (ret)
		drm_err_ratelimited(&trigger2->drm, "USB frame failed: %d\n", ret);
exit:
	drm_dev_exit(idx);
complete:
	complete(&transfer->frame_complete);
}

void trigger2_free_bulk_buffer(struct trigger2_transfer_buf *buf)
{
	if (!buf->data)
		return;
	vfree(buf->data);
	buf->data = NULL;
	buf->len = 0;
}

int trigger2_alloc_bulk_buffer(struct trigger2_transfer_buf *buf, size_t len)
{
	/* Codec scratch follows the payload and is never sent over USB. */
	buf->data = vmalloc(size_add(len, TRIGGER2_CODEC_SCRATCH_SIZE));
	if (!buf->data)
		return -ENOMEM;
	buf->len = len;
	return 0;
}

static int trigger2_init_transfer(struct trigger2_device *trigger2,
				  struct trigger2_transfer *transfer)
{
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	struct trigger2_bulk_chunk *chunk;
	unsigned int i;

	init_completion(&transfer->frame_complete);
	complete(&transfer->frame_complete);
	INIT_WORK(&transfer->transfer_work, trigger2_transfer_work);
	transfer->trigger2 = trigger2;

	for (i = 0; i < TRIGGER2_BULK_URBS; i++) {
		chunk = &transfer->chunks[i];
		chunk->data = kmalloc(TRIGGER2_BULK_CHUNK_SIZE, GFP_KERNEL);
		if (!chunk->data)
			return -ENOMEM;
		chunk->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!chunk->urb)
			return -ENOMEM;
		init_completion(&chunk->complete);
		usb_fill_bulk_urb(chunk->urb, udev, trigger2->bulk_pipe,
				  chunk->data, TRIGGER2_BULK_CHUNK_SIZE,
				  trigger2_bulk_complete, chunk);
	}
	return 0;
}

int trigger2_transfer_mode_init(struct trigger2_device *trigger2,
				const struct drm_display_mode *mode)
{
	struct trigger2_transfer *transfer = &trigger2->transfers[0];
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	u32 width = mode->hdisplay, height = mode->vdisplay;
	u64 pixels = (u64)width * height;
	u64 bound = (u64)trigger2->frame_base +
		    9 * (u64)width * ALIGN(height, 16);
	size_t bitmap_len = pixels / 8;
	u8 *setup = transfer->header;
	int actual, ret;

	/* Quarter dimensions and bitmap bytes must be exact on the wire. */
	if (!width || !height || ((width | height) & 3) ||
	    width > U16_MAX || height > U16_MAX - 15 ||
	    pixels > (U32_MAX - 4) / 32 ||
	    3 * pixels > 0xffffff || bound > U32_MAX ||
	    bound != trigger2->frame_end || bitmap_len > transfer->buf.len)
		return -EINVAL;

	memset(setup, 0, 21);
	setup[0] = TRIGGER2_CMD_BITMAP;
	put_unaligned_le32(trigger2->frame_base, setup + 1);
	put_unaligned_le16(width / 4, setup + 5);
	put_unaligned_le16(height / 4, setup + 7);
	put_unaligned_le16(width, setup + 9);
	put_unaligned_le16(height, setup + 11);
	setup[14] = setup[16] = 0x10;
	put_unaligned_le32(32 * pixels + 4, setup + 17);

	ret = usb_bulk_msg(udev, trigger2->bulk_pipe, setup, 21,
			   &actual, TRIGGER2_BULK_TIMEOUT_MS);
	if (ret)
		return ret;
	if (actual != 21)
		return -EIO;

	ret = trigger2_send_bulk(transfer, NULL, bitmap_len);
	if (ret)
		drm_err(&trigger2->drm, "USB mode bitmap failed: %d\n", ret);
	return ret;
}

static void trigger2_frame_header(u8 *header, u32 addr, u16 width, u16 height,
				  u32 frame_end, u32 raw_len, u32 encoded_len)
{
	memset(header, 0, TRIGGER2_FRAME_HEADER_LEN);
	header[0] = TRIGGER2_CMD_FRAME;
	put_unaligned_le32(addr, header + 2);
	put_unaligned_le32(addr, header + 6);
	put_unaligned_le16(width, header + 10);
	put_unaligned_le16(height, header + 12);
	put_unaligned_le16(width, header + 14);
	put_unaligned_le16(height, header + 16);
	header[19] = header[21] = 0x40;
	put_unaligned_le32(frame_end, header + 23);
	header[27] = raw_len;
	header[28] = raw_len >> 8;
	header[29] = raw_len >> 16;
	header[30] = encoded_len;
	header[31] = encoded_len >> 8;
	header[32] = encoded_len >> 16;
	header[33] = 0x30;
	header[34] = 0x05;
	header[35] = 0x28;
}

int trigger2_transfer_blank_frame(struct trigger2_device *trigger2,
				  u16 width, u16 height)
{
	struct trigger2_transfer *transfer = &trigger2->transfers[0];
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	u8 *out = transfer->buf.data;
	size_t pixels = (size_t)width * height, pos, written = 0;
	size_t n, rem, run;
	unsigned int c;
	int actual, ret;

	if (3 * pixels > transfer->buf.len || 3 * pixels > 0xffffff)
		return -EINVAL;

	for (pos = 0; pos < pixels; pos += n) {
		n = min_t(size_t, TRIGGER2_RGB_BLOCK_PIXELS, pixels - pos);
		for (c = 0; c < 3; c++)
			for (rem = n; rem; rem -= run) {
				run = min_t(size_t, rem, 251);
				out[written++] = 0x30;
				out[written++] = run - 1;
				out[written++] = 0;
			}
	}
	trigger2_frame_header(transfer->header, trigger2->frame_base, width,
			      height, trigger2->frame_end, 3 * pixels, written);
	ret = usb_bulk_msg(udev, trigger2->bulk_pipe, transfer->header,
			   TRIGGER2_FRAME_HEADER_LEN, &actual,
			   TRIGGER2_BULK_TIMEOUT_MS);
	if (ret)
		return ret;
	if (actual != TRIGGER2_FRAME_HEADER_LEN)
		return -EIO;

	return trigger2_send_bulk(transfer, transfer->buf.data, written);
}

static void trigger2_clear_rect(struct drm_rect *rect)
{
	rect->x1 = INT_MAX;
	rect->y1 = INT_MAX;
	rect->x2 = 0;
	rect->y2 = 0;
}

static void trigger2_merge_rect(struct drm_rect *r1, const struct drm_rect *r2)
{
	r1->x1 = min(r1->x1, r2->x1);
	r1->y1 = min(r1->y1, r2->y1);
	r1->x2 = max(r1->x2, r2->x2);
	r1->y2 = max(r1->y2, r2->y2);
}

/*
 * EP02 pixels are planar RGB in successive 1024-pixel raster blocks.  The
 * observed encoder substitutes 0x31 for pixel component 0x30: literal 0x30
 * cannot be distinguished from the run marker in captured traffic.
 */
static size_t trigger2_encode_frame(struct trigger2_transfer *transfer,
				    const struct drm_plane_state *state,
				    const struct drm_rect *src,
				    const struct drm_rect *dst,
				    const struct drm_rect *rect,
				    const struct iosys_map *map)
{
	u8 (*channels)[TRIGGER2_RGB_BLOCK_PIXELS] =
		(void *)((u8 *)transfer->buf.data + transfer->buf.len);
	u8 *out = transfer->buf.data;
	size_t total = (size_t)drm_rect_width(rect) * drm_rect_height(rect);
	size_t pos, written = 0;
	int x = rect->x1, y = rect->y1;
	unsigned int i, c, n, run;
	u32 pixel;
	u8 value;

	for (pos = 0; pos < total; pos += n) {
		n = min_t(size_t, TRIGGER2_RGB_BLOCK_PIXELS, total - pos);
		for (i = 0; i < n; i++) {
			pixel = 0;
			if (x >= dst->x1 && x < dst->x2 &&
			    y >= dst->y1 && y < dst->y2) {
				int sx = x - dst->x1 + src->x1;
				int sy = y - dst->y1 + src->y1;

				if (sx >= 0 && sy >= 0 &&
				    sx < state->fb->width &&
				    sy < state->fb->height)
					pixel = iosys_map_rd(map,
						(size_t)sy * state->fb->pitches[0] +
						(size_t)sx * sizeof(u32), u32);
			}
			channels[0][i] = (pixel >> 16) & 0xff;
			channels[1][i] = (pixel >> 8) & 0xff;
			channels[2][i] = pixel & 0xff;
			for (c = 0; c < 3; c++)
				if (channels[c][i] == 0x30)
					channels[c][i] = 0x31;
			if (++x == rect->x2) {
				x = rect->x1;
				y++;
			}
		}

		for (c = 0; c < 3; c++) {
			for (i = 0; i < n; i += run) {
				value = channels[c][i];
				run = 1;
				while (run < 251 && i + run < n &&
				       channels[c][i + run] == value)
					run++;
				if (run >= 3) {
					out[written++] = 0x30;
					out[written++] = run - 1;
					out[written++] = value;
				} else {
					out[written++] = value;
					if (run == 2)
						out[written++] = value;
				}
			}
		}
	}
	return written;
}

void trigger2_plane_atomic_update(struct drm_plane *plane,
				  trigger2_atomic_state *atomic_state)
{
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(atomic_state, plane);
	struct drm_plane_state *state =
		drm_atomic_get_new_plane_state(atomic_state, plane);
	struct trigger2_device *trigger2 = to_trigger2(plane->dev);
	struct trigger2_transfer *current_transfer, *previous_transfer;
	const struct iosys_map *map;
	const struct drm_display_mode *mode;
	struct drm_rect current_rect, damage_rect, src_rect, dst_rect;
	u64 addr, end;
	size_t raw_len, frame_len;
	int width, height, padded_height;
	int idx, ret, generation;

	if (!drm_atomic_helper_damage_merged(old_state, state, &current_rect))
		return;

	if (!drm_dev_enter(plane->dev, &idx))
		return;
	generation = atomic_read(&trigger2->io_generation);
	if (!READ_ONCE(trigger2->display_enabled) ||
	    generation != atomic_read(&trigger2->io_generation))
		goto exit;

	current_transfer =
		&trigger2->transfers[trigger2->current_transfer];
	previous_transfer = &trigger2->transfers[1 - trigger2->current_transfer];
	src_rect = drm_plane_state_src(state);
	dst_rect = drm_plane_state_dest(state);
	mode = &drm_atomic_get_new_crtc_state(atomic_state,
					      state->crtc)->mode;

	/* Match drm_atomic_helper_damage_iter_init() rounding. */
	src_rect.x1 >>= 16;
	src_rect.y1 >>= 16;
	src_rect.x2 = (src_rect.x2 >> 16) + !!(src_rect.x2 & 0xffff);
	src_rect.y2 = (src_rect.y2 >> 16) + !!(src_rect.y2 & 0xffff);

	/* Requeue damage from a queued frame replaced by this newer update. */
	if (cancel_work(&previous_transfer->transfer_work)) {
		complete(&previous_transfer->frame_complete);
		trigger2_merge_rect(&current_rect, &previous_transfer->transfer_rect);
		current_transfer = previous_transfer;
		trigger2->current_transfer = !trigger2->current_transfer;
	}

	trigger2_merge_rect(&current_rect, &trigger2->pending_rect);
	trigger2_clear_rect(&trigger2->pending_rect);
	if (!drm_rect_intersect(&current_rect, &src_rect))
		goto exit;
	damage_rect = current_rect;

	padded_height = ALIGN(mode->vdisplay, 16);
	/*
	 * Partial frames use screen coordinates and full-frame stride. Send
	 * a full frame if the plane is offset or cropped, so its uncovered
	 * screen pixels (and any old position) are cleared as well.
	 */
	if (src_rect.x1 || src_rect.y1 ||
	    src_rect.x2 != mode->hdisplay ||
	    src_rect.y2 != mode->vdisplay ||
	    dst_rect.x1 || dst_rect.y1 ||
	    dst_rect.x2 != mode->hdisplay ||
	    dst_rect.y2 != mode->vdisplay) {
		current_rect = DRM_RECT_INIT(0, 0, mode->hdisplay,
					     padded_height);
	} else {
		current_rect.x1 = round_down(current_rect.x1, 64);
		current_rect.x2 = min_t(int, round_up(current_rect.x2, 64),
					mode->hdisplay);
		current_rect.y1 = round_down(current_rect.y1, 16);
		current_rect.y2 = min_t(int, round_up(current_rect.y2, 16),
					padded_height);
	}

	width = drm_rect_width(&current_rect);
	height = drm_rect_height(&current_rect);
	if (!width || !height || width > U16_MAX || height > U16_MAX)
		goto exit_save_pending;
	raw_len = array3_size(width, height, 3);
	addr = (u64)trigger2->frame_base +
	       3 * ((u64)current_rect.y1 * mode->hdisplay + current_rect.x1);
	end = addr + 3 * ((u64)(height - 1) * mode->hdisplay + width);
	if (raw_len > 0xffffff || raw_len > current_transfer->buf.len ||
	    addr > U32_MAX || end > trigger2->frame_end)
		goto exit_save_pending;

	if (!wait_for_completion_timeout(&current_transfer->frame_complete,
					 msecs_to_jiffies(20)))
		goto exit_save_pending;

	ret = drm_gem_fb_begin_cpu_access(state->fb, DMA_FROM_DEVICE);
	if (ret) {
		complete(&current_transfer->frame_complete);
		goto exit_save_pending;
	}
	map = &to_drm_shadow_plane_state(state)->data[0];
	frame_len = trigger2_encode_frame(current_transfer, state, &src_rect,
					  &dst_rect, &current_rect, map);
	drm_gem_fb_end_cpu_access(state->fb, DMA_FROM_DEVICE);
	if (!frame_len || frame_len > 0xffffff ||
	    frame_len > current_transfer->buf.len) {
		complete(&current_transfer->frame_complete);
		goto exit_save_pending;
	}

	trigger2_frame_header(current_transfer->header, addr, width, height,
			      trigger2->frame_end, raw_len, frame_len);
	current_transfer->frame_len = frame_len;
	current_transfer->transfer_rect = damage_rect;
	current_transfer->generation = generation;

	if (!queue_work(trigger2->transfer_wq, &current_transfer->transfer_work)) {
		complete(&current_transfer->frame_complete);
		goto exit_save_pending;
	}
	trigger2->current_transfer = !trigger2->current_transfer;
	goto exit;

exit_save_pending:
	trigger2->pending_rect = damage_rect;
exit:
	drm_dev_exit(idx);
}

void trigger2_transfer_fini(struct trigger2_device *trigger2)
{
	unsigned int i, j;

	for (i = 0; i < TRIGGER2_NUM_TRANSFERS; i++) {
		struct trigger2_transfer *transfer = &trigger2->transfers[i];

		usb_kill_anchored_urbs(&transfer->submitted);
		for (j = 0; j < TRIGGER2_BULK_URBS; j++) {
			struct trigger2_bulk_chunk *chunk = &transfer->chunks[j];

			usb_free_urb(chunk->urb);
			kfree(chunk->data);
			chunk->urb = NULL;
			chunk->data = NULL;
		}
	}
}

int trigger2_transfer_init(struct trigger2_device *trigger2)
{
	int i, ret;

	trigger2_clear_rect(&trigger2->pending_rect);
	atomic_set(&trigger2->io_generation, 0);
	for (i = 0; i < TRIGGER2_NUM_TRANSFERS; i++)
		init_usb_anchor(&trigger2->transfers[i].submitted);
	for (i = 0; i < TRIGGER2_NUM_TRANSFERS; i++) {
		ret = trigger2_init_transfer(trigger2, &trigger2->transfers[i]);
		if (ret) {
			trigger2_transfer_fini(trigger2);
			return ret;
		}
	}
	return 0;
}
