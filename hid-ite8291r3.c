// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <dt-bindings/leds/common.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/leds.h>
#include <linux/limits.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <uapi/linux/uleds.h>

/* ========================================================================== */

#define ITE8291R3_NUM_ROWS 6
#define ITE8291R3_NUM_COLS 21
#define ITE8291R3_MAX_BRIGHTNESS 50

#define ITE8291R3_ROW_COLOR_OFFSET 2
#define ITE8291R3_ROW_RED_OFFSET   (ITE8291R3_ROW_COLOR_OFFSET + 2 * ITE8291R3_NUM_COLS)
#define ITE8291R3_ROW_GREEN_OFFSET (ITE8291R3_ROW_COLOR_OFFSET + 1 * ITE8291R3_NUM_COLS)
#define ITE8291R3_ROW_BLUE_OFFSET  (ITE8291R3_ROW_COLOR_OFFSET + 0 * ITE8291R3_NUM_COLS)

#define ITE8291R3_HID_REPORT_LENGTH 9

#define ITE8291R3_SET_EFFECT          8
#define ITE8291R3_SET_BRIGHTNESS      9
#define ITE8291R3_SET_PALETTE_COLOR  20
#define ITE8291R3_SET_ROW_INDEX      22
#define ITE8291R3_GET_FW_VERSION    128
#define ITE8291R3_GET_EFFECT        136

#define ITE8291R3_REP_BRIGHTNESS_OFFSET 5

#define ITE8291R3_FW_VERSION_LENGTH 4

/* ========================================================================== */

struct ite8291r3_priv {
	struct hid_device *hdev;
	struct led_classdev led;

	struct {
		struct timer_list put_timer;
		bool gotten;
	} intf;

	struct {
		uint32_t color;
		uint8_t brightness;
	} state;

	struct mutex lock; /* protects the whole object */

	char name[LED_MAX_NAME_SIZE];
	u8 transfer_buf[ITE8291R3_HID_REPORT_LENGTH];
	u8 row_color_buf[2 + 3 * ITE8291R3_NUM_COLS];
};

/* ========================================================================== */

static void intf_put_timeout(struct timer_list *timer)
{
	struct ite8291r3_priv *p = container_of(timer, struct ite8291r3_priv, intf.put_timer);
	struct usb_interface *intf = to_usb_interface(p->hdev->dev.parent);

	if (!mutex_trylock(&p->lock))
		return;

	if (p->intf.gotten) {
		usb_autopm_put_interface(intf);
		p->intf.gotten = false;

		hid_dbg(p->hdev, "usb interface put\n");
	}

	mutex_unlock(&p->lock);
}

/* p->lock must be held */
static int intf_get(struct ite8291r3_priv *p)
{
	struct usb_interface *intf = to_usb_interface(p->hdev->dev.parent);
	int err;

	lockdep_assert_held(&p->lock);

	if (p->intf.gotten)
		return 0;

	err = usb_autopm_get_interface(intf);
	if (err)
		return err;

	p->intf.gotten = true;
	hid_dbg(p->hdev, "usb interface gotten\n");

	return 0;
}

/* p->lock must be held */
static void intf_put(struct ite8291r3_priv *p)
{
	lockdep_assert_held(&p->lock);
	mod_timer(&p->intf.put_timer, jiffies + msecs_to_jiffies(5000));
}

static int lock_and_get(struct ite8291r3_priv *p)
{
	int err;

	err = mutex_lock_interruptible(&p->lock);
	if (err)
		return err;

	err = intf_get(p);
	if (err)
		mutex_unlock(&p->lock);

	return err;
}

/* p->lock must be held */
static void put_and_unlock(struct ite8291r3_priv *p)
{
	lockdep_assert_held(&p->lock);

	intf_put(p);
	mutex_unlock(&p->lock);
}
/* ========================================================================== */

/* p->lock must be held, interface must be gotten */
static int ite8291r3_receive(struct ite8291r3_priv *p)
{
	int err;

	lockdep_assert_held(&p->lock);

	memset(p->transfer_buf, 0, sizeof(p->transfer_buf));

	err = hid_hw_raw_request(p->hdev, 0,
				 p->transfer_buf, sizeof(p->transfer_buf),
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);

	hid_dbg(p->hdev, "received: err = %d, buf = %*ph\n",
		err, (int) sizeof(p->transfer_buf), p->transfer_buf);

	if (err < 0) {
		hid_err(p->hdev, "%s: get feature report failed: %d\n",
			__func__, err);
	} else if (err != (int) sizeof(p->transfer_buf)) {
		hid_err(p->hdev, "%s: not enough data returned: got %d bytes, expected %d\n",
			__func__, err, (int) sizeof(p->transfer_buf));
		err = -ENODATA;
	}

	return err;
}

/* p->lock must be held, interface must be gotten */
static int ite8291r3_send(struct ite8291r3_priv *p)
{
	int err;

	lockdep_assert_held(&p->lock);

	err = hid_hw_raw_request(p->hdev, 0,
				 p->transfer_buf, sizeof(p->transfer_buf),
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	hid_dbg(p->hdev, "sent: err = %d, buf = %*ph\n",
		err, (int) sizeof(p->transfer_buf), p->transfer_buf);

	if (err < 0) {
		hid_err(p->hdev, "%s: set feature report failed: %d\n",
			__func__, err);
	} else if (err != (int) sizeof(p->transfer_buf)) {
		hid_err(p->hdev, "%s: not enough data accepted: sent %d bytes, expected to send %d\n",
			__func__, err, (int) sizeof(p->transfer_buf));
		err = -ENOSPC;
	}

	return err;
}

/* ========================================================================== */

/* p->lock must be held, interface must be gotten */
static int ite8291r3_get_brightness(struct ite8291r3_priv *p)
{
	int err;

	lockdep_assert_held(&p->lock);

	memset(p->transfer_buf, 0, sizeof(p->transfer_buf));
	p->transfer_buf[1] = ITE8291R3_GET_EFFECT;

	err = ite8291r3_send(p);
	if (err < 0)
		goto out;

	err = ite8291r3_receive(p);
	if (err < 0)
		goto out;

	err = p->transfer_buf[ITE8291R3_REP_BRIGHTNESS_OFFSET];

out:
	return err;
}

/* p->lock must be held, interface must be gotten */
static int ite8291r3_set_brightness(struct ite8291r3_priv *p, uint8_t brightness)
{
	int err;

	lockdep_assert_held(&p->lock);

	memset(p->transfer_buf, 0, sizeof(p->transfer_buf));
	p->transfer_buf[1] = ITE8291R3_SET_BRIGHTNESS;
	p->transfer_buf[2] = 0x02;
	p->transfer_buf[3] = brightness;

	err = ite8291r3_send(p);
	if (err >= 0)
		err = 0;

	return err;
}

static int ite8291r3_get_firmware_version(struct ite8291r3_priv *p, uint8_t fw_ver[static 4])
{
	int err;

	err = lock_and_get(p);
	if (err)
		return err;

	memset(p->transfer_buf, 0, sizeof(p->transfer_buf));
	p->transfer_buf[1] = ITE8291R3_GET_FW_VERSION;

	err = ite8291r3_send(p);
	if (err < 0)
		goto out_put_intf;

	err = ite8291r3_receive(p);
	if (err < 0)
		goto out_put_intf;

	if (err < ITE8291R3_FW_VERSION_LENGTH + 2) {
		err = -ENODATA;
		goto out_put_intf;
	}

	memcpy(fw_ver, &p->transfer_buf[2], 4);
	err = 0;

out_put_intf:
	put_and_unlock(p);
	return err;
}

/*
static int ite8291r3_turn_off(struct ite8291r3_priv *p)
{
	int err;

	err = lock_and_get(p);
	if (err)
		return err;

	memset(p->transfer_buf, 0, sizeof(p->transfer_buf));
	p->transfer_buf[1] = ITE8291R3_SET_EFFECT;
	p->transfer_buf[2] = 0x01;

	err = ite8291r3_send(p);
	if (err < 0)
		goto out_put_intf;

	err = 0;

	put_and_unlock(p);
	return err;
}
*/

/* ========================================================================== */

static enum led_brightness ite8291r3_led_cdev_get_brightness(struct led_classdev *led_cdev)
{
	struct ite8291r3_priv *p = container_of(led_cdev, struct ite8291r3_priv, led);
	int err;

	err = lock_and_get(p);
	if (err)
		return err;

	err = ite8291r3_get_brightness(p);

	put_and_unlock(p);

	return err;
}

static int ite8291r3_led_cdev_set_brightness(struct led_classdev *led_cdev,
					     enum led_brightness value)
{
	struct ite8291r3_priv *p = container_of(led_cdev, struct ite8291r3_priv, led);
	int err;

	if (led_cdev->flags & LED_UNREGISTERING)
		return 0;

	err = lock_and_get(p);
	if (err)
		return err;

	err = ite8291r3_set_brightness(p, value);

	put_and_unlock(p);

	return err;
}

/* ========================================================================== */

static int ite8291r3_set_color(struct ite8291r3_priv *p, uint32_t color)
{
	uint8_t red = (color >> 16) & 0xFF,
		green = (color >> 8) & 0xFF,
		blue = color & 0xFF;
	unsigned int row, col;
	int err, brightness;

	err = lock_and_get(p);
	if (err)
		return err;

	err = brightness = ite8291r3_get_brightness(p);
	if (err < 0)
		goto out_put_intf;

	memset(p->transfer_buf, 0, sizeof(p->transfer_buf));
	p->transfer_buf[1] = ITE8291R3_SET_EFFECT;
	p->transfer_buf[2] = 0x02;
	p->transfer_buf[3] = 0x33;
	p->transfer_buf[5] = brightness;

	err = ite8291r3_send(p);
	if (err < 0)
		goto out_put_intf;

	memset(p->row_color_buf, 0, sizeof(p->row_color_buf));
	for (col = 0; col < ITE8291R3_NUM_COLS; col++) {
		p->row_color_buf[ITE8291R3_ROW_RED_OFFSET   + col] = red;
		p->row_color_buf[ITE8291R3_ROW_GREEN_OFFSET + col] = green;
		p->row_color_buf[ITE8291R3_ROW_BLUE_OFFSET  + col] = blue;
	}

	for (row = 0; row < ITE8291R3_NUM_ROWS; row++) {
		memset(p->transfer_buf, 0, sizeof(p->transfer_buf));
		p->transfer_buf[1] = ITE8291R3_SET_ROW_INDEX;
		p->transfer_buf[2] = 0x00;
		p->transfer_buf[3] = row;

		err = ite8291r3_send(p);
		if (err < 0)
			goto out_put_intf;

		err = hid_hw_output_report(p->hdev, p->row_color_buf, sizeof(p->row_color_buf));
		if (err < 0)
			goto out_put_intf;
	}

	p->state.color = color;
	err = 0;

out_put_intf:
	put_and_unlock(p);

	return err;
}

/* ========================================================================== */

static ssize_t color_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *p = container_of(led_cdev, struct ite8291r3_priv, led);
	uint32_t color;
	int err;

	err = mutex_lock_interruptible(&p->lock);
	if (err)
		return err;

	color = p->state.color;

	mutex_unlock(&p->lock);

	if (color == U32_MAX)
		return -ENODATA;

	return sprintf(buf, "%06x\n", color);
}

static ssize_t color_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *p = container_of(led_cdev, struct ite8291r3_priv, led);
	unsigned long value;
	int err;

	if (kstrtoul(buf, 16, &value))
		return -EINVAL;

	if (value > 0xFFFFFF)
		return -EINVAL;

	err = ite8291r3_set_color(p, value);
	if (err)
		return err;

	return count;
}

static DEVICE_ATTR_RW(color);

static struct attribute *ite8291r3_led_attrs[] = {
	&dev_attr_color.attr,
	NULL
};

ATTRIBUTE_GROUPS(ite8291r3_led);

/* ========================================================================== */

static int ite8291r3_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ite8291r3_priv *p;
	uint8_t fw_ver[4];
	int err;

	hid_info(hdev, "probing\n");

	if (usb_dev->descriptor.bcdDevice != 0x0003) {
		hid_warn(hdev, "unsupported bcdDevice (%#06x)",
			 (unsigned int) usb_dev->descriptor.bcdDevice);
		return -ENODEV;
	}

	hid_info(hdev, "usb interface: %d\n",
		 (int) intf->cur_altsetting->desc.bInterfaceNumber);

	err = hid_parse(hdev);
	if (err) {
		hid_warn(hdev, "hid_parse() failed: %d\n", err);
		goto out;
	}

	err = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (err) {
		hid_warn(hdev, "hid_hw_start() failed: %d\n", err);
		goto out;
	}

	err = hid_hw_open(hdev);
	if (err) {
		hid_warn(hdev, "hid_hw_open() failed: %d\n", err);
		goto out_stop;
	}

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		err = -ENOMEM;
		goto out_close;
	}

	p->hdev = hdev;
	p->state.color = U32_MAX;
	p->state.brightness = U8_MAX;

	mutex_init(&p->lock);
	timer_setup(&p->intf.put_timer, intf_put_timeout, 0);

	err = ite8291r3_get_firmware_version(p, fw_ver);
	if (err) {
		hid_err(hdev, "failed to query firmware version: %d\n", err);
		goto out_free_priv;
	}

	hid_info(hdev, "firmware version: %*ph\n", (int) sizeof(fw_ver), fw_ver);

	snprintf(p->name, sizeof(p->name),
		 "usb-%d-%d-%d-%d::" LED_FUNCTION_KBD_BACKLIGHT,
		 usb_dev->bus->busnum, usb_dev->portnum, usb_dev->devnum,
		 intf->cur_altsetting->desc.bInterfaceNumber);

	p->led.name                    = p->name;
	p->led.max_brightness          = ITE8291R3_MAX_BRIGHTNESS;
	p->led.brightness_get          = ite8291r3_led_cdev_get_brightness;
	p->led.brightness_set_blocking = ite8291r3_led_cdev_set_brightness;
	p->led.flags                   = LED_BRIGHT_HW_CHANGED | LED_CORE_SUSPENDRESUME;
	p->led.groups                  = ite8291r3_led_groups;

	err = led_classdev_register(&hdev->dev, &p->led);
	if (err) {
		hid_warn(hdev, "failed to register led: %d\n", err);
		goto out_free_priv;
	}

	hid_set_drvdata(hdev, p);

	return 0;

out_free_priv:
	del_timer_sync(&p->intf.put_timer);
	mutex_destroy(&p->lock);
	kfree(p);
out_close:
	hid_hw_close(hdev);
out_stop:
	hid_hw_stop(hdev);
out:
	return err;
}

static void ite8291r3_remove(struct hid_device *hdev)
{
	struct ite8291r3_priv *p = hid_get_drvdata(hdev);

	hid_info(hdev, "removing\n");

	led_classdev_unregister(&p->led);
	del_timer_sync(&p->intf.put_timer);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);

	mutex_destroy(&p->lock);
	kfree(p);
}

/* ========================================================================== */

#define USB_VENDOR_ID_ITE 0x048d

static const struct hid_device_id ite8291r3_device_ids[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, 0x6004) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, 0xce00) },
	{ },
};
MODULE_DEVICE_TABLE(hid, ite8291r3_device_ids);

static struct hid_driver ite8291r3_driver = {
	.name = KBUILD_MODNAME,
	.id_table = ite8291r3_device_ids,
	.probe = ite8291r3_probe,
	.remove = ite8291r3_remove,
};

/* ========================================================================== */

module_hid_driver(ite8291r3_driver);

/* ========================================================================== */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Barnabás Pőcze <pobrn@protonmail.com>");
MODULE_DESCRIPTION("ITE8291 (rev 0.03) keyboard backlight controller driver");
