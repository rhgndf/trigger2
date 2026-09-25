/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TRIGGER2_H__
#define __TRIGGER2_H__

#include <linux/align.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#define DRIVER_NAME		"trigger2"
#define DRIVER_DESC		"MCT Trigger II USB display adapter"

#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0

#define TRIGGER2_NUM_TRANSFERS	2
#define TRIGGER2_CMD_BUF_LEN	769
#define TRIGGER2_REPLY_BUF_LEN	512
#define TRIGGER2_FRAME_HEADER_LEN	36
#define TRIGGER2_BULK_TIMEOUT_MS	5000
#define TRIGGER2_BULK_CHUNK_SIZE	(20 * 1024)
#define TRIGGER2_BULK_URBS	32
#define TRIGGER2_RGB_BLOCK_PIXELS	1024

struct trigger2_transfer_buf {
	void *data;
	size_t len;
};

struct trigger2_bulk_chunk {
	struct urb *urb;
	struct completion complete;
};

struct trigger2_transfer {
	struct trigger2_device *trigger2;

	struct trigger2_transfer_buf buf;
	size_t frame_len;
	int generation;
	struct drm_rect transfer_rect;

	u8 *header;

	struct work_struct transfer_work;
	struct completion frame_complete;
};

struct trigger2_device {
	struct drm_device drm;
	struct usb_interface *intf;
	unsigned int bulk_pipe;
	struct mutex cmd_lock;
	u8 *cmd_buf;
	u8 *reply_buf;
	u32 frame_base;
	u32 frame_end;

	struct drm_connector connector;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;

	int current_transfer;
	struct drm_rect pending_rect;
	struct workqueue_struct *transfer_wq;
	bool display_enabled;
	atomic_t io_generation;
	bool mode_programmed;

	struct trigger2_bulk_chunk chunks[TRIGGER2_BULK_URBS];
	struct trigger2_transfer transfers[TRIGGER2_NUM_TRANSFERS];
};

#define to_trigger2(x) container_of(x, struct trigger2_device, drm)

/* Keep complete 1024-pixel RGB blocks, including clipped right-edge damage. */
static inline unsigned int trigger2_frame_row_align(unsigned int width)
{
	unsigned int rows = TRIGGER2_RGB_BLOCK_PIXELS >>
		__ffs(width | TRIGGER2_RGB_BLOCK_PIXELS);

	/* The low set bit gives gcd(width, block size); retain 16-row banks. */
	return max(16U, rows);
}

static inline unsigned int trigger2_padded_height(unsigned int width,
						  unsigned int height)
{
	return ALIGN(height, trigger2_frame_row_align(width));
}

struct trigger2_reg_write {
	u16 reg;
	u8 value;
};

int trigger2_command_locked(struct trigger2_device *trigger2, u8 endpoint,
			    const void *data, size_t len);
int trigger2_reg_read_locked(struct trigger2_device *trigger2,
			     u16 reg, u8 *value);
int trigger2_reg_write_locked(struct trigger2_device *trigger2,
			      u16 reg, u8 value);
int trigger2_write_regs_locked(struct trigger2_device *trigger2,
			       const struct trigger2_reg_write *writes, size_t count);
int trigger2_edid_read_locked(struct trigger2_device *trigger2, u8 data[512]);
int trigger2_boot_locked(struct trigger2_device *trigger2);

int trigger2_alloc_bulk_buffer(struct trigger2_transfer_buf *buf, size_t len);
void trigger2_free_bulk_buffer(struct trigger2_transfer_buf *buf);
int trigger2_transfer_init(struct trigger2_device *trigger2);
void trigger2_stop_io(struct trigger2_device *trigger2);
int trigger2_transfer_mode_init(struct trigger2_device *trigger2,
			       const struct drm_display_mode *mode);
int trigger2_transfer_blank_frame(struct trigger2_device *trigger2,
				  u16 width, u16 height);
void trigger2_plane_atomic_update(struct drm_plane *plane,
				  struct drm_atomic_commit *state);

int trigger2_modeset_init(struct trigger2_device *trigger2);

int trigger2_connector_init(struct trigger2_device *trigger2,
			    int connector_type);
#endif /* __TRIGGER2_H__ */
