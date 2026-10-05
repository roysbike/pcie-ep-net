#!/bin/sh
# Put the proprietary MIOP stack back. Run on the Blade after a reboot
# that did not load openmiop. Unloading a live vendor MIOP stack has
# wedged this CPU; do not rmmod those modules.
#
#   systemctl disable openmiop.service
#   systemctl enable load-miop.service
#   reboot
#
# Then on the Cluster Box: echo 1 > /sys/bus/pci/rescan
set -eu
systemctl disable openmiop.service 2>/dev/null || true
systemctl enable load-miop.service
echo "vendor MIOP will load on the next boot"
