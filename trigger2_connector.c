// SPDX-License-Identifier: GPL-2.0-only

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "trigger2.h"

static int trigger2_read_edid(void *data, u8 *buf, unsigned int block,
			      size_t len)
{
	struct trigger2_device *trigger2 = data;
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	int idx, ret;

	if (!drm_dev_enter(&trigger2->drm, &idx))
		return -ENODEV;

	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      TRIGGER2_REQUEST_GET_EDID,
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      block, 0, buf, len, USB_CTRL_GET_TIMEOUT);
	drm_dev_exit(idx);

	if (ret < 0)
		return ret;
	if (ret != len)
		return -EIO;

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
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	u8 status[2];
	int idx, ret;

	if (!drm_dev_enter(&trigger2->drm, &idx))
		return connector_status_disconnected;

	ret = usb_control_msg_recv(udev, 0, TRIGGER2_REQUEST_GET_STATUS,
				   USB_DIR_IN | USB_TYPE_VENDOR |
					   USB_RECIP_DEVICE,
				   0xff, 0x3, status, sizeof(status),
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	drm_dev_exit(idx);

	if (ret)
		return connector_status_unknown;

	return status[1] == 1 ? connector_status_connected :
			     connector_status_disconnected;
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
