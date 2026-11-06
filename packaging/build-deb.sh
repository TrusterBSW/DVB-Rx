#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Debian packages (the receiver is compiled here):
#   ./packaging/build-deb.sh [out dir]   ->  dvbrx_<VERSION><suffix>_<arch>.deb, dvbrx-web_<VERSION><suffix>_all.deb
# <suffix>: +deb12, +deb13, +ubuntu24.04... from /etc/os-release (DEB_SUFFIX= to choose; empty: none).
# One package serves Debian 12 / 13, Proxmox VE 8 / 9 and Ubuntu 24.04 when it is built on the oldest of
# them (Debian 12): the radio libraries (libuhd, libbladeRF, libhackrf) are loaded at run time whatever
# their version, and dvbrx is built three times (x86-64-v2, v3 with AVX2, v4 with AVX-512), the
# /usr/bin/dvbrx launcher running the one this processor can. packaging/build-all-debs.sh does it all in
# containers.
# Needs: build-essential libfftw3-dev dpkg-dev, and the headers of the radio libraries (libuhd-dev
# libbladerf-dev libhackrf-dev; a missing one drops its driver).
set -eu
cd "$(dirname "$0")/.."
if [ -z "${DEB_SUFFIX+x}" ]; then
    DEB_SUFFIX=$(. /etc/os-release; case "$ID" in debian) echo "+deb${VERSION_ID%%.*}" ;;
                                                  ubuntu) echo "+ubuntu$VERSION_ID" ;; *) echo "+$ID$VERSION_ID" ;; esac)
fi
UPSTREAM=$(cat VERSION)
VERSION=$UPSTREAM$DEB_SUFFIX
OUT=$(realpath -m "${1:-.}")
ARCH=$(dpkg --print-architecture)
[ "$(sed -n 's/^VERSION = "\([^"]*\)".*/\1/p' apps/dvbrx-web/dvbrxweb/__init__.py)" = "$UPSTREAM" ] ||
    { echo "apps/dvbrx-web/dvbrxweb/__init__.py: VERSION is not $UPSTREAM" >&2; exit 1; }
ROOT=$(mktemp -d)
WEB=$(mktemp -d)
trap 'rm -rf "$ROOT" "$WEB"' EXIT
DESCRIBE=$(git describe --always --dirty 2>/dev/null || echo 'the source tree')

changelog() {   # PACKAGE DIR
    { printf '%s (%s) unstable; urgency=medium\n\n  * Built from %s.\n\n' "$1" "$VERSION" "$DESCRIBE"
      printf ' -- BILLY Rémi <byhoster.association@gmail.com>  %s\n' "$(date -R)"; } | gzip -9n > "$2/changelog.Debian.gz"
}
copyright() {   # PACKAGE DIR [kernel]
    { printf '%s\n' 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/' "Upstream-Name: $1" \
        'Source: https://github.com/TrusterBSW/DVB-Rx' '' 'Files: *' 'Copyright: 2026 BILLY Rémi' 'License: GPL-3+' \
        ' This program is free software: you can redistribute it and/or modify it under the terms of the GNU' \
        ' General Public License as published by the Free Software Foundation, either version 3 of the' \
        ' License, or (at your option) any later version.' ' .' \
        ' On Debian systems, the complete text of the GNU General Public License version 3 can be found in' \
        ' /usr/share/common-licenses/GPL-3.'
      if [ "${3:-}" = kernel ]; then
        printf '%s\n' '' "Files: usr/src/dvbrx-$UPSTREAM/*" 'Copyright: 2026 BILLY Rémi' 'License: GPL-2' \
            ' This program is free software; you can redistribute it and/or modify it under the terms of the GNU' \
            ' General Public License version 2 as published by the Free Software Foundation.' ' .' \
            ' On Debian systems, the complete text of the GNU General Public License version 2 can be found in' \
            ' /usr/share/common-licenses/GPL-2.'
      fi; } > "$2/copyright"
}
manpages() {    # ROOT PAGE...   (English in man/, French in man/fr/)
    r=$1; shift
    for p in "$@"; do
        for m in "man/$p" "man/fr/$p"; do
            [ -e "$m" ] || continue
            sec=${p##*.}
            case "$m" in man/fr/*) dir=$r/usr/share/man/fr/man$sec ;; *) dir=$r/usr/share/man/man$sec ;; esac
            install -d "$dir"
            gzip -9n < "$m" > "$dir/$p.gz"
        done
    done
}

# --- dvbrx -------------------------------------------------------------------------------------------
lib=$ROOT/usr/lib/dvbrx
src=$ROOT/usr/src/dvbrx-$UPSTREAM
doc=$ROOT/usr/share/doc/dvbrx
install -d "$lib" "$src" "$doc/examples" "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/usr/lib/systemd/system" \
           "$ROOT/usr/lib/udev/rules.d" "$ROOT/usr/lib/modules-load.d" "$ROOT/etc/modprobe.d" "$ROOT/DEBIAN"
for v in v2 v3 v4; do
    make -C libdvbrx clean >/dev/null
    make -C libdvbrx build/dvbrx MARCH=x86-64-$v >/dev/null
    install -m 755 libdvbrx/build/dvbrx "$lib/dvbrx-x86-64-$v"
done
strip --strip-unneeded "$lib"/dvbrx-x86-64-*
install -m 755 packaging/dvbrx "$ROOT/usr/bin/"
install -m 755 packaging/dvbrx-irq-pin "$ROOT/usr/sbin/"
sed -e 's|/usr/local/bin/dvbrx|/usr/bin/dvbrx|' -e 's|^Documentation=.*|Documentation=man:dvbrx(8)|' \
    packaging/dvbrxd@.service > "$ROOT/usr/lib/systemd/system/dvbrxd@.service"
install -m 644 packaging/60-dvbrx.rules "$ROOT/usr/lib/udev/rules.d/"
echo dvbrx > "$ROOT/usr/lib/modules-load.d/dvbrx.conf"
printf '# number of /dev/dvb adapters (1 to 8), one radio and one dvbrxd@N service each\noptions dvbrx adapters=1\n' \
    > "$ROOT/etc/modprobe.d/dvbrx.conf"
install -m 644 kernel/dvbrx.c kernel/dvbrx_ioctl.h kernel/Kbuild kernel/Makefile "$src/"
sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$UPSTREAM\"/" kernel/dkms.conf > "$src/dkms.conf"
install -m 644 packaging/dvbrxd.conf.example "$doc/examples/dvbrxd.conf"
sed 's|/usr/local/sbin/|/usr/sbin/|' packaging/dvbrxd-cpu.conf.example > "$doc/examples/dvbrxd-cpu.conf"
install -m 644 README.md README.fr.md doc/lnb-protocol.md doc/dvbrx-web-api.md "$doc/"
manpages "$ROOT" dvbrx.8
changelog dvbrx "$doc"
copyright dvbrx "$doc" kernel

# shared library dependencies of the receiver; the radio libraries are loaded at run time: recommended
DEP="libc6"
if command -v dpkg-shlibdeps >/dev/null; then
    tmp=$(mktemp -d)
    mkdir -p "$tmp/debian"
    printf 'Source: dvbrx\n\nPackage: dvbrx\nArchitecture: any\n' > "$tmp/debian/control"
    DEP=$(cd "$tmp" && dpkg-shlibdeps -O "$lib/dvbrx-x86-64-v2" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
    rm -rf "$tmp"
fi
# headers for DKMS (apt takes the first one this system has), then the radio libraries
REC="proxmox-default-headers | linux-headers-amd64 | linux-headers-generic, libuhd4.8.0 | libuhd4.6.0 | libuhd4.3.0, libbladerf2, libhackrf0"

for s in postinst prerm postrm; do
    sed "s/@DKMS_VER@/$UPSTREAM/" "packaging/debian/dvbrx/$s" > "$ROOT/DEBIAN/$s"
    chmod 755 "$ROOT/DEBIAN/$s"
done
install -m 644 packaging/debian/dvbrx/conffiles "$ROOT/DEBIAN/"
SIZE=$(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)
sed -e "s/@VERSION@/$VERSION/" -e "s/@SIZE@/$SIZE/" -e "s/@ARCH@/$ARCH/" -e "s/@DEPENDS@/$DEP/" \
    -e "s/@RECOMMENDS@/$REC/" packaging/debian/dvbrx/control.in > "$ROOT/DEBIAN/control"

mkdir -p "$OUT"
deb="$OUT/dvbrx_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$deb" >/dev/null
echo "$deb"

# --- dvbrx-web ---------------------------------------------------------------------------------------
app=$WEB/usr/lib/dvbrx-web
doc=$WEB/usr/share/doc/dvbrx-web
install -d "$app/dvbrxweb" "$app/static" "$doc" "$WEB/usr/bin" "$WEB/usr/lib/systemd/system" "$WEB/etc/dvbrxd" \
           "$WEB/DEBIAN"
install -m 755 apps/dvbrx-web/dvbrx-web "$app/"
install -m 644 apps/dvbrx-web/dvbrxweb/*.py "$app/dvbrxweb/"
install -m 644 apps/dvbrx-web/static/* "$app/static/"
ln -s ../lib/dvbrx-web/dvbrx-web "$WEB/usr/bin/dvbrx-web"
sed -e 's|/usr/local/bin/dvbrx-web|/usr/bin/dvbrx-web|' -e 's|^Documentation=.*|Documentation=man:dvbrx-web(8)|' \
    packaging/dvbrx-web.service > "$WEB/usr/lib/systemd/system/dvbrx-web.service"
install -m 644 packaging/dvbrx-web.conf.example "$WEB/etc/dvbrxd/web.conf"
install -m 644 apps/dvbrx-web/README.md "$doc/"
manpages "$WEB" dvbrx-web.8
changelog dvbrx-web "$doc"
copyright dvbrx-web "$doc"
for s in postinst prerm postrm; do install -m 755 "packaging/debian/dvbrx-web/$s" "$WEB/DEBIAN/"; done
install -m 644 packaging/debian/dvbrx-web/conffiles "$WEB/DEBIAN/"
SIZE=$(du -sk --exclude=DEBIAN "$WEB" | cut -f1)
sed -e "s/@VERSION@/$VERSION/" -e "s/@SIZE@/$SIZE/" packaging/debian/dvbrx-web/control.in > "$WEB/DEBIAN/control"
deb="$OUT/dvbrx-web_${VERSION}_all.deb"
dpkg-deb --root-owner-group -Zxz --build "$WEB" "$deb" >/dev/null
echo "$deb"
