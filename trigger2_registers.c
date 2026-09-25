// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/string.h>

#include "trigger2.h"
#include "trigger2_registers.h"

/*
 * All command/reply operations require cmd_lock.  In particular, nothing may
 * consume IN81 between a register/EDID request on OUT03 and its reply.
 */
int trigger2_command_locked(struct trigger2_device *trigger2, u8 endpoint,
			    const void *buf, size_t len)
{
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	unsigned int pipe;
	int actual, ret;

	lockdep_assert_held(&trigger2->cmd_lock);

	if (!buf || !len)
		return -EINVAL;
	if (len > TRIGGER2_CMD_BUF_LEN)
		return -EMSGSIZE;

	if (endpoint == 2)
		pipe = trigger2->bulk_pipe;
	else if (endpoint == 3)
		pipe = trigger2->cmd_pipe;
	else if (endpoint == 4)
		pipe = trigger2->aux_pipe;
	else
		return -EINVAL;

	/* Callers may pass stack data; cmd_buf is persistent DMA-safe storage. */
	if (buf != trigger2->cmd_buf)
		memcpy(trigger2->cmd_buf, buf, len);

	ret = usb_bulk_msg(udev, pipe, trigger2->cmd_buf, len, &actual,
			   TRIGGER2_BULK_TIMEOUT_MS);
	if (ret) {
		dev_err(&trigger2->intf->dev, "OUT%02x command failed: %d\n",
			endpoint, ret);
		return ret;
	}
	if (actual != len) {
		dev_err(&trigger2->intf->dev,
			"OUT%02x command short: %d of %zu bytes\n",
			endpoint, actual, len);
		return -EIO;
	}

	return 0;
}

int trigger2_reply_locked(struct trigger2_device *trigger2, void *buf,
			  size_t len)
{
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	int actual, ret;

	lockdep_assert_held(&trigger2->cmd_lock);

	if (!buf || !len)
		return -EINVAL;
	if (len > TRIGGER2_REPLY_BUF_LEN)
		return -EMSGSIZE;

	ret = usb_bulk_msg(udev, trigger2->reply_pipe, trigger2->reply_buf,
			   len, &actual, TRIGGER2_BULK_TIMEOUT_MS);
	if (ret) {
		dev_err(&trigger2->intf->dev, "IN81 reply failed: %d\n", ret);
	} else if (actual != len) {
		dev_err(&trigger2->intf->dev,
			"IN81 reply short: %d of %zu bytes\n", actual, len);
		ret = -EIO;
	} else if (buf != trigger2->reply_buf) {
		memcpy(buf, trigger2->reply_buf, len);
	}

	return ret;
}

int trigger2_reg_read_locked(struct trigger2_device *trigger2, u16 reg,
			     u8 *val)
{
	const u8 cmd[] = { TRIGGER2_CMD_REG_READ, (u8)reg, (u8)(reg >> 8) };
	int ret;

	if (!val)
		return -EINVAL;

	ret = trigger2_command_locked(trigger2, 3, cmd, sizeof(cmd));
	if (ret)
		return ret;

	return trigger2_reply_locked(trigger2, val, 1);
}

int trigger2_reg_write_locked(struct trigger2_device *trigger2, u16 reg,
			      u8 val)
{
	const u8 cmd[] = { TRIGGER2_CMD_REG_WRITE, 0x00, 0x01,
			   (u8)(reg >> 8), (u8)reg, val };

	return trigger2_command_locked(trigger2, 3, cmd, sizeof(cmd));
}

int trigger2_edid_read_locked(struct trigger2_device *trigger2, u8 data[512])
{
	const u8 cmd[] = { TRIGGER2_CMD_EDID, 0x80, 0x00, 0xa0,
			   0x00, 0x80, 0x00, 0x00 };
	int ret;

	if (!data)
		return -EINVAL;

	ret = trigger2_command_locked(trigger2, 3, cmd, sizeof(cmd));
	if (ret)
		return ret;

	return trigger2_reply_locked(trigger2, data, TRIGGER2_REPLY_BUF_LEN);
}

struct trigger2_boot_reg {
	u16 reg;
	u8 value;
};

static const u8 info[] = { TRIGGER2_CMD_BOOT_INFO, 0x00, 0x02, 0x00 };
static const u8 identity[] = {
	TRIGGER2_CMD_BOOT_ID, 0x80, 0x00, 0xae, 0x00, 0x00, 0x01, 0x00
};
static const u8 config_a[] = {
	TRIGGER2_CMD_BOOT_CONFIG, 0x04, 0x04, 0x00,
	0x00, 0x06, 0x1a, 0x80
};
static const u8 config_b[] = {
	TRIGGER2_CMD_BOOT_CONFIG, 0x03, 0x01, 0x00, 0x00
};
static const u8 config_c[] = {
	TRIGGER2_CMD_BOOT_CONFIG, 0x08, 0x01, 0x00, 0x02
};
static const u8 board_pairs[] = {
	TRIGGER2_CMD_REG_PAIRS, 0x0c, 0x00,
	0xa4, 0x39, 0xa5, 0x00, 0xa6, 0x00, 0xa7, 0x00,
	0xa3, 0x65, 0xa3, 0x64
};
static const u8 bitmap[] = {
	TRIGGER2_CMD_BITMAP, 0x00, 0x00, 0x00, 0x00,
	0x20, 0x00, 0x01, 0x00, 0x20, 0x00, 0x01,
	0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x60,
	0x00, 0x00
};
static const struct trigger2_boot_reg reset[] = {
	{ TRIGGER2_REG_FE57, 0xa0 },
	{ TRIGGER2_REG_FE57, 0x20 },
	{ TRIGGER2_REG_FE70, 0x80 },
	{ TRIGGER2_REG_FE70, 0x00 },
	{ TRIGGER2_REG_FE36, 0x20 },
	{ TRIGGER2_REG_FE36, 0x00 },
	{ TRIGGER2_REG_FC6F, 0x00 },
};
static const struct trigger2_boot_reg channel_setup[] = {
	{ TRIGGER2_REG_FC6A, 0x12 }, { TRIGGER2_REG_FC6B, 0x22 },
	{ TRIGGER2_REG_FC6A, 0x13 }, { TRIGGER2_REG_FC6B, 0x22 },
	{ TRIGGER2_REG_FC6A, 0x11 }, { TRIGGER2_REG_FC6B, 0x22 },
	{ TRIGGER2_REG_FC6A, 0x10 }, { TRIGGER2_REG_FC6B, 0x22 },
	{ TRIGGER2_REG_FBFF, 0x81 },
};
static const struct trigger2_boot_reg pre_bitmap[] = {
	{ TRIGGER2_REG_FCB0, 0x20 },
	{ TRIGGER2_REG_FC4B, 0x0e },
	{ TRIGGER2_REG_FBF2, 0x04 },
	{ TRIGGER2_REG_FCA2, 0x10 },
	{ TRIGGER2_REG_FBF4, 0x03 },
	{ TRIGGER2_REG_FCF0, 0x01 },
	{ TRIGGER2_REG_FCF1, 0x1c },
	{ TRIGGER2_REG_FCF2, 0x01 },
	{ TRIGGER2_REG_FC4B, 0x02 },
};
static const struct trigger2_boot_reg channel_reset[] = {
	{ TRIGGER2_REG_CHANNEL_RESET, 0x00 },
	{ TRIGGER2_REG_CHANNEL_70, 0x00 },
	{ TRIGGER2_REG_CHANNEL_71, 0x00 },
	{ TRIGGER2_REG_CHANNEL_72, 0x00 },
	{ TRIGGER2_REG_CHANNEL_74, 0x00 },
	{ TRIGGER2_REG_CHANNEL_75, 0x00 },
	{ TRIGGER2_REG_CHANNEL_76, 0x00 },
	{ TRIGGER2_REG_FEA8, 0x00 },
	{ TRIGGER2_REG_FEA9, 0x00 },
	{ TRIGGER2_REG_FEAA, 0x00 },
};

static int trigger2_boot_writes_locked(struct trigger2_device *trigger2,
				       const struct trigger2_boot_reg *writes,
				       size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = trigger2_reg_write_locked(trigger2, writes[i].reg,
						writes[i].value);
		if (ret)
			return ret;
	}

	return 0;
}

static int trigger2_boot_read_locked(struct trigger2_device *trigger2, u16 reg)
{
	u8 value;

	/* These reads are part of the handshake; observed values vary by boot. */
	return trigger2_reg_read_locked(trigger2, reg, &value);
}

static int trigger2_boot_reply_locked(struct trigger2_device *trigger2,
				      const u8 *cmd, size_t len)
{
	int ret;

	ret = trigger2_command_locked(trigger2, 3, cmd, len);
	if (ret)
		return ret;

	return trigger2_reply_locked(trigger2, trigger2->reply_buf,
				     TRIGGER2_REPLY_BUF_LEN);
}

int trigger2_boot_locked(struct trigger2_device *trigger2)
{
	u8 pairs[3 + 10 * 4];
	unsigned int i;
	int ret;

	lockdep_assert_held(&trigger2->cmd_lock);

	/* Captured cold re-enumeration, packets 365–509. */
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FC01);
	if (ret)
		return ret;
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FEB0);
	if (ret)
		return ret;
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FEB1);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FEB0, 0x43);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FEB1, 0x03);
	if (ret)
		return ret;
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FEB0);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FEB0, 0x40);
	if (ret)
		return ret;
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FEB1);
	if (ret)
		return ret;
	ret = trigger2_boot_reply_locked(trigger2, info, sizeof(info));
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FC6F, 0x00);
	if (ret)
		return ret;
	ret = trigger2_boot_reply_locked(trigger2, identity, sizeof(identity));
	if (ret)
		return ret;

	ret = trigger2_boot_writes_locked(trigger2, reset, ARRAY_SIZE(reset));
	if (ret)
		return ret;
	ret = trigger2_command_locked(trigger2, 3, config_a, sizeof(config_a));
	if (ret)
		return ret;
	ret = trigger2_command_locked(trigger2, 3, config_b, sizeof(config_b));
	if (ret)
		return ret;
	ret = trigger2_command_locked(trigger2, 3, config_c, sizeof(config_c));
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FEE0, 0x05);
	if (ret)
		return ret;

	/* EP03 register-pair count is little-endian bytes, not pair count. */
	pairs[0] = TRIGGER2_CMD_REG_PAIRS;
	pairs[1] = sizeof(pairs) - 3;
	pairs[2] = 0x00;
	for (i = 0; i < 10; i++) {
		pairs[3 + 4 * i] = TRIGGER2_REG_FC6A & 0xff;
		pairs[4 + 4 * i] = i;
		pairs[5 + 4 * i] = TRIGGER2_REG_FC6B & 0xff;
		pairs[6 + 4 * i] = 0x51;
	}
	ret = trigger2_command_locked(trigger2, 3, pairs, sizeof(pairs));
	if (ret)
		return ret;
	ret = trigger2_boot_writes_locked(trigger2, channel_setup,
					  ARRAY_SIZE(channel_setup));
	if (ret)
		return ret;

	/* Packet 443 is exactly 0x0a followed by 768 zero bytes. */
	memset(trigger2->cmd_buf, 0, TRIGGER2_CMD_BUF_LEN);
	trigger2->cmd_buf[0] = TRIGGER2_CMD_BOOT_ZERO;
	ret = trigger2_command_locked(trigger2, 3, trigger2->cmd_buf,
				      TRIGGER2_CMD_BUF_LEN);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FC6F, 0x00);
	if (ret)
		return ret;
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FCA3);
	if (ret)
		return ret;
	ret = trigger2_boot_writes_locked(trigger2, pre_bitmap,
					  ARRAY_SIZE(pre_bitmap));
	if (ret)
		return ret;

	/* Packets 469/471 initialize a small bitmap, not a display frame. */
	ret = trigger2_command_locked(trigger2, 2, bitmap, sizeof(bitmap));
	if (ret)
		return ret;
	memset(trigger2->cmd_buf, 0, 96);
	ret = trigger2_command_locked(trigger2, 2, trigger2->cmd_buf, 96);
	if (ret)
		return ret;
	ret = trigger2_command_locked(trigger2, 3, board_pairs,
				      sizeof(board_pairs));
	if (ret)
		return ret;
	ret = trigger2_boot_writes_locked(trigger2, channel_reset,
					  ARRAY_SIZE(channel_reset));
	if (ret)
		return ret;

	/* Startup EDID may be all 0xff in its first 128 bytes; consume it. */
	ret = trigger2_edid_read_locked(trigger2, trigger2->reply_buf);
	if (ret)
		return ret;
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FEB0);
	if (ret)
		return ret;
	ret = trigger2_boot_read_locked(trigger2, TRIGGER2_REG_FEB1);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FEB0, 0x40);
	if (ret)
		return ret;
	return trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FEB1, 0xff);
}
