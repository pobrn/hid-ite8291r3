# What is it?
This a Linux kernel driver for the ITE 8291 (rev 0.03) RGB keyboard backlight controller. It is based on the [`ite8291r3-ctl`](https://github.com/pobrn/ite8291r3-ctl) userspace program. It provides more seamless integration with the rest of the system because it exposes the controller as a LED device. That means that in many desktop environments, you will be able to directly control the brightness on a graphical user interface.

# Disclaimer
**This software is in early stages of developement. Futhermore, to quote GPL: everything is provided as is. There is no warranty for the program, to the extent permitted by applicable law.**

**This software is licensed under the GNU General Public License v2.0**

# Compatibility
The following devices have been reported to work:

| idVendor | idProduct | bcdDevice |                vendor                |      product      |
|----------|-----------|-----------|--------------------------------------|-------------------|
| 048d     | 6004      | 0.03      | Integrated Technology Express, Inc.  | ITE Device(8291)  |
| 048d     | ce00      | 0.03      | Integrated Technology Express, Inc.  | ITE Device(8291)  |

The `048d:6005` USB device is also in the list of supported devices of this driver, however, it has not been tested, the vendor and product identifiers have been found on [linux-hardware.org](https://linux-hardware.org). If you have such device, and the driver works for you, please report it.

You can use `lsusb` to determine if a compatible device is found on your system. If you believe your device should be supported, but it is not, please open an issue.

# Dependencies
### Required
* Your kernel has been compiled with `CONFIG_HID` and `CONFIG_LEDS_CLASS` (it probably was)
* Linux headers for you current kernel

# How to install
## Downloading
If you have `git` installed:
```
git clone https://github.com/pobrn/hid-ite8291r3
```

If you don't, then you can download it [here](https://github.com/pobrn/hid-ite8291r3/archive/master.zip).

## Installing
### Linux headers
On Debian and its [many](https://www.debian.org/derivatives/) [derivatives](https://wiki.ubuntu.com/DerivativeTeam/Derivatives) (Ubuntu, Pop OS, Linux Mint, ...) , run
```
sudo apt install linux-headers-$(uname -r)
```
to install the necessary header files.

On Arch Linux and its derivatives (Manjaro, ...), run
```
sudo pacman -Syu linux-headers
```
(or something similar)

### DKMS
DKMS should be in your distributions repositories. `sudo apt install dkms`, `sudo pacman -Syu dkms` should work depending on your distribution.

### The module
Run
```
sudo make dkmsinstall
```
to install the module with DKMS. Or run
```
sudo make dkmsuninstall
```
to uninstall the module.

The module should automatically load at boot after this. If you want to load it immediately, run `sudo modprobe hid-ite8291r3`.

# How to use
After the module is loaded, and new entry should appear in `/sys/class/leds/` with `:kbd_backlight` suffix. After installation with DKMS, it is recommended to reboot so that every system daemon (e.g. `upower`) can discover the new LED device.

## Changing the color
As of yet, you cannot set effects with this module, only the color may be changed. The LED device has a `color` attribute, which you can write to in order to change the color:
```
# echo aabbcc > /sys/class/leds/<name>:kbd_backlight/color
```
This sets the red component to 0xaa (170), the green to 0xbb (187), and the blue to 0xcc (204).

Assuming you have a single keyboard backlight LED device, you can use the following udev rule to set the color (to white - in the example) upon boot (when the device is created):
```
ACTION=="add", SUBSYSTEM=="leds", DEVPATH=="*:kbd_backlight", TEST=="color", ATTR{color}="ffffff" 
```
Create `/etc/udev/rules.d/99-ite8291.rules` (if it does not exist yet) and simply add the previous line.

Furthermore, the `systemd-backlight` service should take care of restoring the brightness to the last set value.
