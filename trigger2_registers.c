// SPDX-License-Identifier: GPL-2.0-only

#include "trigger2.h"

int trigger2_read_register(struct trigger2_device *trigger2, u16 reg,
			   void *data, size_t len)
{
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);

	return usb_control_msg_recv(udev, 0, TRIGGER2_REQUEST_GET_REGISTER,
				    USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				    0, reg, data, len, USB_CTRL_GET_TIMEOUT,
				    GFP_KERNEL);
}

int trigger2_write_register(struct trigger2_device *trigger2, u16 reg,
			    const void *data, size_t len)
{
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);

	return usb_control_msg_send(udev, 0, TRIGGER2_REQUEST_SET_REGISTER,
				    USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				    0, reg, data, len, USB_CTRL_SET_TIMEOUT,
				    GFP_KERNEL);
}
