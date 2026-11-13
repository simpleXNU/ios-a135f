#!/bin/sh
# Host side of the phone's USB Ethernet (kernel a135f_usb=2, patches 0075-0077).
# tools/a135f-usbcon.sh [LOGFILE] [WIFI_INTERFACE]
# Example: ./a135f-usbcon.sh my_log.txt wlan0
#
# Waits for the phone's CDC-ECM interface (host MAC 02:00:5e:77:ee:01),
# gives it 192.168.7.1/24 (the phone is 192.168.7.2), shares Wi-Fi internet,
# then prints the live kernel console.

set -eu
mac=02:00:5e:77:ee:01

# Default Wi-Fi interface if not provided as second argument
# Change 'wlan0' to your actual Wi-Fi interface name if different
WIFI_IFACE=${2:-wlan0}

echo "waiting for the phone's USB Ethernet ($mac)..." >&2
until ip -o link | grep -qi "$mac"; do sleep 1; done

ifname=$(ip -o link | grep -i "$mac" | awk -F': ' '{print $2}' | cut -d@ -f1)
nmcli dev set "$ifname" managed no 2>/dev/null || true

sudo ip addr replace 192.168.7.1/24 dev "$ifname"
sudo ip link set "$ifname" up

# --- Internet Sharing Configuration ---
echo "Enabling internet sharing from $WIFI_IFACE to $ifname..." >&2
# Enable IP Forwarding in the kernel
sudo sysctl -w net.ipv4.ip_forward=1 > /dev/null

# Set up NAT / Masquerade
sudo iptables -t nat -A POSTROUTING -o "$WIFI_IFACE" -j MASQUERADE
sudo iptables -A FORWARD -i "$WIFI_IFACE" -o "$ifname" -m state --state RELATED,ESTABLISHED -j ACCEPT
sudo iptables -A FORWARD -i "$ifname" -o "$WIFI_IFACE" -j ACCEPT
# --------------------------------------

echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140" >&2

if [ ${1:-} ] && [ "$1" != "" ]; then
    nc -ukl 5140 | tee -a "$1"
else
    nc -ukl 5140
fi
