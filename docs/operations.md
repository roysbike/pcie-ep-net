# Operating openmiop (protocol v4)

Debian blades and the Cluster Box BMC. Talos: see
`mixtile-talos/docs/openmiop.md`.

## Install

**Blade (Debian, vendor 6.1):**

```sh
scripts/blade-build.sh _out/debian            # builds on a blade, copies the .ko back
scp _out/debian/openmiop-ep.ko blade:/tmp/
ssh blade 'sudo cp /tmp/openmiop-ep.ko /usr/local/lib/openmiop-ep.ko'
# once per blade:
#   /usr/local/sbin/openmiop-ep-start  (scripts/openmiop-ep-start.sh)
#   /etc/systemd/system/openmiop.service, systemctl enable openmiop
#   /etc/openmiop.addr: 10.20.0.<last octet of the management address>/24
```

The module leaves the fabric on `rmmod`, reboot and poweroff (leave
handshake), so no stop script is needed.

**Cluster Box (OpenWrt):**

```sh
make -C userspace                              # openmiop-rc, omi-peek (mipsel)
ssh clusterbox 'cat > /tmp/openmiop-rc' < userspace/openmiop-rc
ssh clusterbox 'sudo cp /tmp/openmiop-rc /usr/bin/ && sudo chmod 755 /usr/bin/openmiop-rc'
ssh clusterbox 'cat > /tmp/openmiop.init' < scripts/openmiop.init
ssh clusterbox 'sudo cp /tmp/openmiop.init /etc/init.d/openmiop && sudo chmod 755 /etc/init.d/openmiop'
```

The BMC has no sftp server (use `ssh ... cat >`), and copying over an
existing file through its overlay drops the execute bit: always
`chmod 755` afterwards. The helper logs to `/tmp/openmiop-rc.log`.

## Addresses

| Member | omi0 |
| --- | --- |
| Cluster Box | 10.20.0.1/24 |
| blade with management address 192.168.70.N | 10.20.0.N/24 |

MTU 9000 everywhere (gateway included).

## Development reload

`scripts/dev-reload.sh path/to/openmiop-ep.ko [path/to/openmiop-rc]`
stops the helper, reloads both blades, optionally installs a new helper,
and starts it again. Reloading a blade with the helper running is also
safe (leave handshake), but stop it before a hard reset.

## Health

```sh
ip -s link show omi0
sudo ethtool -S omi0           # per-context counters, see below
sudo ethtool -i omi0           # firmware-version: "proto 4 node <index>"
dmesg | grep openmiop          # node index, peer N up/down, link reset, stalls
ssh clusterbox 'tail /tmp/openmiop-rc.log'
ssh clusterbox 'sudo /tmp/omi-peek 0000:03:00.0'   # one EP's header, control, table
scripts/matrix-test.sh         # every member to every member, normal and jumbo
scripts/bench.sh 3 10          # full benchmark report
```

Counters worth watching: `tx_dma_errors`, `rx_bad_len`, `rx_resyncs`
(all should stay 0), `ctl_peer_stalls` (a peer stopped consuming),
`ctl_link_resets` (the host reset the link), `ctl_peer_up/down`.

## Recovery

| Symptom | Action |
| --- | --- |
| A blade does not rejoin after reload/reboot | `tail /tmp/openmiop-rc.log` on the BMC; `omi-peek` the EP. "BAR0 has no address" triggers a switch re-enumeration automatically (once a minute); force one with `sudo touch /var/run/openmiop-reenumerate`. |
| Blade module stuck / blade wedged | stop the helper (`/etc/init.d/openmiop stop` on the BMC), then `sudo nodectl reset -n <slot>` (slot 3 = 03:00.0, slot 4 = 04:00.0). |
| Nothing answers behind the BMC root port (lspci shows only 00:00.0 or zeros) | the MT7620A/ASM2824 path is wedged; a BMC reboot recovers it. Blade power stays on across a BMC reboot. |
| EPs advertise a 1 GiB BAR after a BMC reboot | old driver without link-reset handling; reload the module (current versions restore the BAR size themselves). |

## Safety rules

* Stop the helper before hard-resetting a blade or toggling a switch
  port's link: a read in flight when a link drops can take the fabric
  down.
* Do not run two helpers.
* The gateway is a management path (~12 Mbit/s towards the BMC).
