# Mini Guide to Writing udev Rules

## Where rules live

- Create or symlink active rules in `/etc/udev/rules.d/`.
  - They are processed in lexicographic order.
- Prefix custom `uaccess` rules with 71 or 72.
  - They must be processed before `73-seat-late.rules`, but after `70-uaccess.rules`.

## Useful commands

- Obtain device information:
  - `lsusb` (USB)
  - `for i in /dev/hidraw* ; do udevadm info -a -n "$i" | grep \"0005: ; done` (Bluetooth)
  - `udevadm info -q path -n /dev/hidrawX` (Bluetooth)
  - `evtest` (evdev)
  - `hid-recorder` (hidraw)
- Reload udev:
  ```bash
  sudo udevadm control --reload-rules
  sudo udevadm trigger
  ```
- Test how udev interprets a device:
  ```bash
  udevadm info --attribute-walk --path=/sys/class/hidraw/hidraw0
  ```
- Monitor events in real time:
  ```bash
  udevadm monitor --environment --udev
  ```
- Check logs:
  ```bash
  journalctl -u systemd-udevd
  ```

## Basic structure

A udev rule is a line with match conditions and actions:

```
MATCH_KEYS=="value", MATCH_KEYS=="value", ACTIONS
```

- Match keys: `KERNEL`, `SUBSYSTEM`, `ATTRS{idVendor}`, `ATTRS{idProduct}`, `DRIVERS`, `KERNELS`
- Actions: `MODE="0660"`, `TAG+="uaccess"`, `OPTIONS+="static_node=..."`

`TAG+="uaccess"` grants access to the active user session via systemd‑logind.

## Virtual output devices

These rules are needed to create virtual output devices.
```udev
KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"
KERNEL=="uhid", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uhid"
```

## General device access

These rules are useful during development when you want broad access to all input and HID devices.
Otherwise, writing device-specific rules is recommended.

```udev
SUBSYSTEM=="input", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="hidraw", MODE="0660", TAG+="uaccess"
```

## Specific device access

Rules vary by bus (USB, Bluetooth) and type (evdev, hidraw).

* For evdev devices, either USB or Bluetooth, match by vendor and product IDs using `ATTRS`.

  ```udev
  SUBSYSTEM=="input", ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", MODE="0660", TAG+="uaccess"
  ```

* For hidraw devices, using USB, match by vendor and product IDs with `ATTRS`.

  ```udev
  KERNEL=="hidraw*", SUBSYSTEM=="hidraw", \
    ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", \
    MODE="0660", TAG+="uaccess"
  ```

* For hidraw devices, using Bluetooth, match the VID/PID with `KERNELS` because Bluetooth HID devices don't expose `ATTRS`.

  ```udev
  KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="0005:1234:5678.*", MODE="0660", TAG+="uaccess"
  ```

* For libusb devices, match by vendor and product IDs with `ATTRS`.

  ```udev
  SUBSYSTEM=="usb", ATTR{idVendor}=="1234", ATTR{idProduct}=="5678", MODE="0660", TAG+="uaccess"
  ```
