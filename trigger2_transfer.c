// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/highmem.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>

#include "trigger2.h"

void trigger2_stop_io(struct trigger2_device *trigger2)
{
	WRITE_ONCE(trigger2->display_enabled, false);
	flush_workqueue(trigger2->transfer_wq);
	cancel_delayed_work_sync(&trigger2->keepalive_work);
}

static void trigger2_bulk_timeout(struct timer_list *t)
{
	struct trigger2_transfer *transfer =
		timer_container_of(transfer, t, timer);

	usb_sg_cancel(&transfer->sgr);
}

static void trigger2_transfer_work(struct work_struct *work)
{
	struct trigger2_transfer *transfer =
		container_of(work, struct trigger2_transfer, transfer_work);
	struct trigger2_device *trigger2 = transfer->trigger2;
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	int idx, ret;

	if (!drm_dev_enter(&trigger2->drm, &idx))
		goto complete;

	/* Submit bulk transfer with a five-second timeout. */
	ret = usb_sg_init(&transfer->sgr, udev, trigger2->bulk_pipe, 0,
			  transfer->buf.sgt.sgl,
			  transfer->buf.sgt.nents, transfer->frame_len,
			  GFP_KERNEL);
	if (ret) {
		drm_err_ratelimited(&trigger2->drm,
				    "failed to initialize USB transfer: %d\n",
				    ret);
		goto exit;
	}

	mod_timer(&transfer->timer,
		  jiffies + msecs_to_jiffies(TRIGGER2_BULK_TIMEOUT_MS));
	usb_sg_wait(&transfer->sgr);
	timer_delete_sync(&transfer->timer);

	if (transfer->sgr.status)
		drm_err_ratelimited(&trigger2->drm,
				    "USB transfer failed: %d\n",
				    transfer->sgr.status);
	else if (transfer->sgr.bytes != transfer->frame_len)
		drm_err_ratelimited(&trigger2->drm,
				    "short USB transfer: %zu/%zu bytes\n",
				    transfer->sgr.bytes, transfer->frame_len);
	else if (READ_ONCE(trigger2->display_enabled))
		/* Keepalive must only be sent after a frame has been sent */
		queue_delayed_work(trigger2->transfer_wq,
				   &trigger2->keepalive_work,
				   msecs_to_jiffies(TRIGGER2_KEEPALIVE_INTERVAL_MS));

exit:
	drm_dev_exit(idx);
complete:
	complete(&transfer->frame_complete);
}

static void trigger2_keepalive_work(struct work_struct *work)
{
	struct trigger2_device *trigger2 =
		container_of(to_delayed_work(work), struct trigger2_device,
			     keepalive_work);
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	u8 response;
	int idx, ret;

	if (!READ_ONCE(trigger2->display_enabled))
		return;

	if (!drm_dev_enter(&trigger2->drm, &idx))
		return;

	ret = usb_control_msg_recv(udev, 0, TRIGGER2_REQUEST_KEEPALIVE,
				   USB_DIR_IN | USB_TYPE_VENDOR |
					   USB_RECIP_DEVICE,
				   0x0002, 0x0000, &response,
				   sizeof(response), USB_CTRL_GET_TIMEOUT,
				   GFP_KERNEL);
	if (ret)
		drm_err_ratelimited(&trigger2->drm,
				    "keepalive request failed: %d\n", ret);

	if (READ_ONCE(trigger2->display_enabled))
		mod_delayed_work(trigger2->transfer_wq,
				 &trigger2->keepalive_work,
				 msecs_to_jiffies(TRIGGER2_KEEPALIVE_INTERVAL_MS));

	drm_dev_exit(idx);
}

void trigger2_free_bulk_buffer(struct trigger2_transfer_buf *buf)
{
	if (!buf->data)
		return;
	sg_free_table(&buf->sgt);
	vfree(buf->data);
	buf->data = NULL;
	buf->len = 0;
}


int trigger2_alloc_bulk_buffer(struct trigger2_transfer_buf *buf,
				      size_t len)
{
	unsigned int num_pages;
	int ret, i;
	struct page **pages;
	u8 *data;
	void *ptr;

	/* Large transfer buffer requires vmalloc and a scatterlist. */
	data = vmalloc_32(len);
	if (!data)
		return -ENOMEM;

	num_pages = DIV_ROUND_UP(len, PAGE_SIZE);
	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_vfree;
	}
	for (i = 0, ptr = data; i < num_pages; i++, ptr += PAGE_SIZE)
		pages[i] = vmalloc_to_page(ptr);
	ret = sg_alloc_table_from_pages(&buf->sgt, pages,
					num_pages, 0, len, GFP_KERNEL);
	kfree(pages);
	if (ret)
		goto err_vfree;

	buf->data = data;
	buf->len = len;

	return 0;
err_vfree:
	vfree(data);
	return ret;
}

static void trigger2_init_transfer(struct trigger2_device *trigger2,
				   struct trigger2_transfer *transfer)
{
	init_completion(&transfer->frame_complete);
	complete(&transfer->frame_complete);
	timer_setup(&transfer->timer, trigger2_bulk_timeout, 0);
	INIT_WORK(&transfer->transfer_work, trigger2_transfer_work);
	transfer->trigger2 = trigger2;
}


static u8 trigger2_bulk_header_checksum(const struct trigger2_bulk_header *header)
{
	const u8 *data = (const u8 *)header;
	u16 checksum = 0;
	size_t i;

	for (i = 0; i < sizeof(struct trigger2_bulk_header) - 1; i++)
		checksum += data[i];
	checksum &= 0xff;
	checksum = 0x100 - checksum;
	return checksum & 0xff;
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

void trigger2_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_commit *atomic_state)
{
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(atomic_state, plane);
	struct drm_plane_state *state =
		drm_atomic_get_new_plane_state(atomic_state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(state);
	struct trigger2_device *trigger2 = to_trigger2(plane->dev);
	struct trigger2_transfer *current_transfer, *previous_transfer;
	struct trigger2_bulk_header *header;
	struct drm_rect current_rect, src_rect;
	struct iosys_map data_map;
	size_t frame_len, payload_len;
	int width, height;
	int idx, ret;

	if (!drm_atomic_helper_damage_merged(old_state, state, &current_rect))
		return;

	if (!drm_dev_enter(plane->dev, &idx))
		return;

	current_transfer =
		&trigger2->transfers[trigger2->current_transfer];
	previous_transfer = &trigger2->transfers[1 - trigger2->current_transfer];

	src_rect = drm_plane_state_src(state);

	/* Match drm_atomic_helper_damage_iter_init() rounding. */
	src_rect.x1 >>= 16;
	src_rect.y1 >>= 16;
	src_rect.x2 = (src_rect.x2 >> 16) + !!(src_rect.x2 & 0xffff);
	src_rect.y2 = (src_rect.y2 >> 16) + !!(src_rect.y2 & 0xffff);

	/* Latency reduction: requeue with the latest frame data. */
	if (cancel_work(&previous_transfer->transfer_work)) {
		complete(&previous_transfer->frame_complete);

		trigger2_merge_rect(&current_rect, &previous_transfer->transfer_rect);

		current_transfer = previous_transfer;
		trigger2->current_transfer = !trigger2->current_transfer;
	}

	/* Damage deferred by an earlier failed update. */
	trigger2_merge_rect(&current_rect, &trigger2->pending_rect);
	trigger2_clear_rect(&trigger2->pending_rect);

	/* Clip merged damage to the new resolution. */
	if (!drm_rect_intersect(&current_rect, &src_rect))
		goto exit;

	width = drm_rect_width(&current_rect);
	height = drm_rect_height(&current_rect);
	payload_len = array3_size(width, height, 3);
	frame_len = size_add(payload_len, sizeof(*header));

	/* Buffers are sized for the full mode in crtc atomic_check. */
	if (drm_WARN_ON_ONCE(plane->dev, frame_len > current_transfer->buf.len))
		goto exit;

	/*
	 * This should almost never wait because we have should have a
	 * pending transfer ready to be de-queued above in case the transfer
	 * hasn't finished, but do a bounded wait just in case it gets stuck
	 */
	if (!wait_for_completion_timeout(&current_transfer->frame_complete,
					 msecs_to_jiffies(20)))
		goto exit_save_pending;

	current_transfer->transfer_rect = current_rect;

	current_transfer->frame_len = frame_len;
	header = current_transfer->buf.data;
	header->magic = 0xfb;
	header->length = 0x14;
	/* flags 0: uncompressed 24-bit RGB888. */
	header->counter =
		cpu_to_le16((trigger2->frame_counter++) & 0xfff);
	header->horizontal_offset = cpu_to_le16(current_rect.x1);
	header->vertical_offset = cpu_to_le16(current_rect.y1);
	header->width = cpu_to_le16(width);
	header->height = cpu_to_le16(height);
	header->payload_length = cpu_to_le32((u32)payload_len);
	header->flags = 0x1;
	header->unknown1 = 0;
	header->unknown2 = 0;
	header->checksum = trigger2_bulk_header_checksum(header);

	iosys_map_set_vaddr(&data_map,
			    current_transfer->buf.data + sizeof(*header));

	ret = drm_gem_fb_begin_cpu_access(state->fb, DMA_FROM_DEVICE);
	if (ret < 0) {
		complete(&current_transfer->frame_complete);
		goto exit_save_pending;
	}

	drm_fb_xrgb8888_to_rgb888(&data_map, NULL,
				  &shadow_plane_state->data[0],
				  state->fb, &current_rect,
				  &shadow_plane_state->fmtcnv_state);

	drm_gem_fb_end_cpu_access(state->fb, DMA_FROM_DEVICE);

	flush_kernel_vmap_range(current_transfer->buf.data, frame_len);

	queue_work(trigger2->transfer_wq, &current_transfer->transfer_work);
	trigger2->current_transfer = !trigger2->current_transfer;
	goto exit;

	/* Retry the dropped damage on the next update. */
exit_save_pending:
	trigger2->pending_rect = current_rect;
exit:
	drm_dev_exit(idx);
}

void trigger2_transfer_init(struct trigger2_device *trigger2)
{
	trigger2_clear_rect(&trigger2->pending_rect);
	trigger2_init_transfer(trigger2, &trigger2->transfers[0]);
	trigger2_init_transfer(trigger2, &trigger2->transfers[1]);
	INIT_DELAYED_WORK(&trigger2->keepalive_work, trigger2_keepalive_work);
}
