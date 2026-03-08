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