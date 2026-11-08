#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs dvbrx from this source checkout, under /usr/local (the .deb, see install.sh at the top of
# the repository, is the usual way): the dvbrx receiver (dvbrxd@N service), the dvbrx kernel module
# (DKMS), the udev rule, the systemd service and an unprivileged "dvbrxd" user.
# usage: sudo packaging/install-service.sh [adapters]
#        sudo packaging/install-service.sh --web   (only dvbrx-web, the web supervision page;
#                                                   the dvbrx-web service is installed, not enabled)
set -e
cd "$(dirname "$0")/.."
VERSION=$(cat VERSION)

if [ "$1" = "--web" ]; then
    getent passwd dvbrxd >/dev/null ||
        useradd --system --home-dir /var/lib/dvbrxd --create-home --shell /usr/sbin/nologin \
            --groups video,dialout,plugdev dvbrxd
    python3 -c 'import numpy' || { echo "dvbrx-web needs python3-numpy" >&2; exit 1; }
    rm -rf /usr/local/lib/dvbrx-web
    install -d /usr/local/lib/dvbrx-web/dvbrxweb /usr/local/lib/dvbrx-web/static
    install -m 0755 apps/dvbrx-web/dvbrx-web /usr/local/lib/dvbrx-web/dvbrx-web
    install -m 0644 apps/dvbrx-web/dvbrxweb/*.py /usr/local/lib/dvbrx-web/dvbrxweb/
    install -m 0644 apps/dvbrx-web/static/* /usr/local/lib/dvbrx-web/static/
    ln -sf ../lib/dvbrx-web/dvbrx-web /usr/local/bin/dvbrx-web
    install -D -m 0644 apps/dvbrx-web/README.md /usr/local/share/doc/dvbrx/dvbrx-web.md
    install -D -m 0644 man/dvbrx-web.8 /usr/local/share/man/man8/dvbrx-web.8
    install -D -m 0644 man/fr/dvbrx-web.8 /usr/local/share/man/fr/man8/dvbrx-web.8
    install -D -m 0644 doc/dvbrx-web-api.md /usr/local/share/doc/dvbrx/dvbrx-web-api.md
    install -d /etc/dvbrxd
    [ -f /etc/dvbrxd/web.conf ] || install -m 0644 packaging/dvbrx-web.conf.example /etc/dvbrxd/web.conf
    install -d -o dvbrxd -g video -m 0750 /var/lib/dvbrx-web
    install -D -m 0644 packaging/dvbrx-web.service /etc/systemd/system/dvbrx-web.service
    systemctl daemon-reload
    echo "dvbrx-web installed (service not enabled); edit /etc/dvbrxd/web.conf, then:"
    echo "  systemctl enable --now dvbrx-web    # http://<host>:8099/"
    exit 0
fi

ADAPTERS=${1:-1}

make -C libdvbrx build/dvbrx
install -D -m 0755 libdvbrx/build/dvbrx /usr/local/bin/dvbrx
ln -sf dvbrx /usr/local/bin/dvbrxd   # former name
install -D -m 0755 packaging/dvbrx-irq-pin /usr/local/sbin/dvbrx-irq-pin
install -D -m 0755 packaging/dvbrx-module /usr/local/sbin/dvbrx-module
install -D -m 0644 packaging/dvbrxd-cpu.conf.example /usr/local/share/doc/dvbrx/dvbrxd-cpu.conf.example
install -D -m 0644 README.md /usr/local/share/doc/dvbrx/README.md
install -D -m 0644 README.fr.md /usr/local/share/doc/dvbrx/README.fr.md
install -D -m 0644 doc/lnb-protocol.md /usr/local/share/doc/dvbrx/lnb-protocol.md
install -D -m 0644 doc/dvbrx-web-api.md /usr/local/share/doc/dvbrx/dvbrx-web-api.md
install -D -m 0644 man/dvbrx.8 /usr/local/share/man/man8/dvbrx.8
install -D -m 0644 man/fr/dvbrx.8 /usr/local/share/man/fr/man8/dvbrx.8

# kernel module, rebuilt by DKMS for new kernels
rm -rf /usr/src/dvbrx-$VERSION
mkdir -p /usr/src/dvbrx-$VERSION
cp kernel/dvbrx.c kernel/dvbrx_ioctl.h kernel/Kbuild kernel/Makefile kernel/dkms.conf \
    /usr/src/dvbrx-$VERSION/
sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VERSION\"/" /usr/src/dvbrx-$VERSION/dkms.conf
dkms remove -m dvbrx -v $VERSION --all >/dev/null 2>&1 || true
dkms install -m dvbrx -v $VERSION
rm -f /etc/modules-load.d/dvbrx.conf   # loaded by the dvbrxd@N services (dvbrx-module)
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

# the module is loaded by the first dvbrxd@N service: unloaded now if none runs (new build, new
# number of adapters), else at the next stop of the last one
systemctl list-units --no-legend --state=active 'dvbrxd@*' | grep -q . || rmmod dvbrx 2>/dev/null || true
systemctl try-restart 'dvbrxd@*.service' || true
echo "installed; edit /etc/dvbrxd/N.conf, then: systemctl enable --now dvbrxd@0"
