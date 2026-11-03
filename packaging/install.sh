#!/bin/sh
# Installs dvbrx: the dvbrx receiver (dvbrxd@N service), the dvbrx kernel module (DKMS),
# the udev rule, the systemd service and an unprivileged "dvbrxd" user.
# usage: sudo packaging/install.sh [adapters]
set -e
cd "$(dirname "$0")/.."
ADAPTERS=${1:-1}
VERSION=0.1

make -C libdvbrx build/dvbrx
install -D -m 0755 libdvbrx/build/dvbrx /usr/local/bin/dvbrx
ln -sf dvbrx /usr/local/bin/dvbrxd   # former name
install -D -m 0755 packaging/dvbrx-irq-pin /usr/local/sbin/dvbrx-irq-pin
install -D -m 0644 packaging/dvbrxd-cpu.conf.example /usr/local/share/doc/dvbrx/dvbrxd-cpu.conf.example
install -D -m 0644 README.md /usr/local/share/doc/dvbrx/README.md
install -D -m 0644 doc/lnb-protocol.md /usr/local/share/doc/dvbrx/lnb-protocol.md

# kernel module, rebuilt by DKMS for new kernels
rm -rf /usr/src/dvbrx-$VERSION
mkdir -p /usr/src/dvbrx-$VERSION
cp kernel/dvbrx.c kernel/dvbrx_ioctl.h kernel/Kbuild kernel/Makefile kernel/dkms.conf \
    /usr/src/dvbrx-$VERSION/
dkms remove -m dvbrx -v $VERSION --all >/dev/null 2>&1 || true
dkms install -m dvbrx -v $VERSION
echo dvbrx > /etc/modules-load.d/dvbrx.conf
echo "options dvbrx adapters=$ADAPTERS" > /etc/modprobe.d/dvbrx.conf

install -D -m 0644 packaging/60-dvbrx.rules /etc/udev/rules.d/60-dvbrx.rules
udevadm control --reload

getent passwd dvbrxd >/dev/null ||
    useradd --system --home-dir /var/lib/dvbrxd --create-home --shell /usr/sbin/nologin \
        --groups video,dialout,plugdev dvbrxd
install -d -o dvbrxd -g video /var/lib/dvbrxd
install -d /etc/dvbrxd
i=0
while [ $i -lt "$ADAPTERS" ]; do
    [ -f /etc/dvbrxd/$i.conf ] || install -m 0644 packaging/dvbrxd.conf.example /etc/dvbrxd/$i.conf
    i=$((i + 1))
done
install -D -m 0644 packaging/dvbrxd@.service /etc/systemd/system/dvbrxd@.service
systemctl daemon-reload

modprobe dvbrx
udevadm trigger --subsystem-match=misc --action=add
echo "installed; edit /etc/dvbrxd/N.conf, then: systemctl enable --now dvbrxd@0"
