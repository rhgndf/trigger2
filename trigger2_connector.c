// SPDX-License-Identifier: GPL-2.0-only

#include <linux/string.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "trigger2.h"

static bool trigger2_edid_checksum_ok(const u8 *block)
{
	u8 checksum = 0;
	unsigned int i;

	for (i = 0; i < EDID_LENGTH; i++)
		checksum += block[i];

	return checksum == 0;
}

static int trigger2_fetch_edid(struct trigger2_device *trigger2,
				u8 reply[TRIGGER2_REPLY_BUF_LEN])
{
	int idx, ret;

	if (!drm_dev_enter(&trigger2->drm, &idx))
		return -ENODEV;

	mutex_lock(&trigger2->cmd_lock);
	ret = trigger2_edid_read_locked(trigger2, reply);
	mutex_unlock(&trigger2->cmd_lock);
	drm_dev_exit(idx);
	return ret;
}

static int trigger2_read_edid(void *data, u8 *buf, unsigned int block,
			      size_t len)
{
	struct trigger2_device *trigger2 = data;
	u8 reply[TRIGGER2_REPLY_BUF_LEN];
	int ret;

	if (len != EDID_LENGTH || block >= sizeof(reply) / EDID_LENGTH)
		return -EINVAL;

	ret = trigger2_fetch_edid(trigger2, reply);
	if (ret)
		return ret;

	/* IN81 is always 512 bytes, even when there is no usable EDID. */
	if (drm_edid_header_is_valid(reply) != 8 ||
	    !trigger2_edid_checksum_ok(reply))
		return -EBADMSG;
	if (reply[126] >= sizeof(reply) / EDID_LENGTH)
		return -EOVERFLOW;
	if (block > reply[126])
		return -EINVAL;
	if (block && !trigger2_edid_checksum_ok(reply + block * EDID_LENGTH))
		return -EBADMSG;

	memcpy(buf, reply + block * EDID_LENGTH, EDID_LENGTH);
	return 0;
}

static int trigger2_connector_get_modes(struct drm_connector *connector)
{
	struct trigger2_device *trigger2 = to_trigger2(connector->dev);
	const struct drm_edid *edid;
	int count;

	edid = drm_edid_read_custom(connector, trigger2_read_edid, trigger2);
	drm_edid_connector_update(connector, edid);
	count = drm_edid_connector_add_modes(connector);
	if (!count)
		count = drm_add_modes_noedid(connector, 1920, 1200);
	drm_edid_free(edid);

	return count;
}

static enum drm_connector_status
trigger2_detect(struct drm_connector *connector, bool force)
{
	struct trigger2_device *trigger2 = to_trigger2(connector->dev);
	u8 reply[TRIGGER2_REPLY_BUF_LEN];
	int ret;

	ret = trigger2_fetch_edid(trigger2, reply);
	if (ret)
		return connector_status_unknown;
	if (drm_edid_header_is_valid(reply) == 8 &&
	    trigger2_edid_checksum_ok(reply))
		return connector_status_connected;
	/* This adapter returns an all-ff base block while DDC/HPD is absent. */
	if (!memchr_inv(reply, 0xff, EDID_LENGTH))
		return connector_status_disconnected;
	return connector_status_unknown;
}

static const struct drm_connector_helper_funcs trigger2_connector_helper_funcs = {
	.get_modes = trigger2_connector_get_modes,
};

static const struct drm_connector_funcs trigger2_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.detect = trigger2_detect,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int trigger2_connector_init(struct trigger2_device *trigger2,
			    int connector_type)
{
	int ret;

	drm_connector_helper_add(&trigger2->connector,
				 &trigger2_connector_helper_funcs);
	ret = drm_connector_init(&trigger2->drm, &trigger2->connector,
				 &trigger2_connector_funcs, connector_type);
	trigger2->connector.polled =
		DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;
	return ret;
}
