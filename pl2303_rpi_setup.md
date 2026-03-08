# Raspberry Pi PL2303 Raw Gadget Setup

## 1. Setup script

Create the script:

``` bash
nano setup_pl2303_rpi.sh
```

Paste:

``` bash
#!/usr/bin/env bash
set -e

echo "Installing dependencies..."
sudo apt update
sudo apt install -y git build-essential linux-headers-$(uname -r)

echo "Cloning raw-gadget..."
cd ~
if [ ! -d raw-gadget ]; then
    git clone https://github.com/xairy/raw-gadget
fi

echo "Enabling dwc2 gadget mode..."

if ! grep -q "dtoverlay=dwc2" /boot/firmware/config.txt; then
    echo "dtoverlay=dwc2" | sudo tee -a /boot/firmware/config.txt
fi

if ! grep -q "modules-load=dwc2" /boot/firmware/cmdline.txt; then
    sudo sed -i 's/rootwait/rootwait modules-load=dwc2/' /boot/firmware/cmdline.txt
fi

echo "Setup complete. Please reboot the Pi."
```

Make executable:

``` bash
chmod +x setup_pl2303_rpi.sh
sudo ./setup_pl2303_rpi.sh
sudo reboot
```

------------------------------------------------------------------------

## 2. After reboot

Check Raw Gadget support:

``` bash
zgrep CONFIG_USB_RAW_GADGET /proc/config.gz
```

If it is a module:

``` bash
sudo modprobe raw_gadget
ls /dev/raw-gadget
```

------------------------------------------------------------------------

## 3. If Raw Gadget module is missing

Build manually:

``` bash
cd ~/raw-gadget/raw_gadget
make
sudo insmod raw_gadget.ko
ls /dev/raw-gadget
```

------------------------------------------------------------------------

## 4. Place your emulator source

    ~/raw-gadget/examples/pl2303.c

------------------------------------------------------------------------

## 5. Build emulator

``` bash
cd ~/raw-gadget
make -C examples
```

Binary:

    ~/raw-gadget/examples/pl2303

------------------------------------------------------------------------

## 6. Find the USB Device Controller

``` bash
ls /sys/class/udc
```

Example:

    20980000.usb

------------------------------------------------------------------------

## 7. Start emulator

``` bash
sudo ~/raw-gadget/examples/pl2303 dwc2 $(ls /sys/class/udc | head -n1)
```

Example output:

    PTY backend ready: /dev/pts/5
    PTY symlink ready: /dev/r2usbc -> /dev/pts/5
    event: connect
    event: reset

------------------------------------------------------------------------

## 8. Use stable PTY path

    /dev/r2usbc

------------------------------------------------------------------------

## 9. Test communication

Read:

``` bash
cat /dev/r2usbc
```

Send:

``` bash
echo "hello host" > /dev/r2usbc
```

------------------------------------------------------------------------

## 10. Optional systemd service

Create:

``` bash
sudo nano /etc/systemd/system/pl2303.service
```

``` ini
[Unit]
Description=PL2303 Raw Gadget Emulator
After=network.target

[Service]
Type=simple
ExecStartPre=/sbin/modprobe raw_gadget
ExecStart=/bin/bash -c '/home/yoshi/raw-gadget/examples/pl2303 dwc2 $(ls /sys/class/udc | head -n1)'
Restart=always

[Install]
WantedBy=multi-user.target
```

Enable:

``` bash
sudo systemctl daemon-reload
sudo systemctl enable pl2303
sudo systemctl start pl2303
```

Logs:

``` bash
journalctl -u pl2303 -f
```

------------------------------------------------------------------------

## Architecture

    Embedded device (USB host)
            │
            │ USB
            ▼
    Raspberry Pi gadget port
            │
            ▼
    Raw Gadget
            │
            ▼
    PL2303 emulator
            │
            ▼
    PTY → /dev/r2usbc
            │
            ▼
    Your service
