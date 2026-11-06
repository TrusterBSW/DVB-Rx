#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the .deb (dvbrx and dvbrx-web) for Debian 12 and 13, Proxmox VE 8 and 9 (same userland) and
# Ubuntu 24.04 in a clean container (docker or podman), then installs them with install.sh in a fresh
# container of each release and checks them:
#   packaging/build-all-debs.sh [out dir]          (default dist/)
# Built on the oldest release (Debian 12): its glibc holds on the newer ones, and the radio libraries are
# loaded at run time whatever their soname (libuhd 4.3, 4.6, 4.8). The kernel module is built by DKMS
# against each release's kernel headers.
# BUILD_IMAGE (debian:12), TEST_IMAGES ("debian:12 debian:13 ubuntu:24.04") to change; DKMS_TEST=0 skips
# the module build (it downloads the kernel headers).
set -eu
cd "$(dirname "$0")/.."
OUT=$(realpath -m "${1:-dist}")
BUILD_IMAGE=${BUILD_IMAGE:-debian:12}
TEST_IMAGES=${TEST_IMAGES:-debian:12 debian:13 ubuntu:24.04}
DKMS_TEST=${DKMS_TEST:-1}
DOCKER=$(command -v docker || command -v podman) || { echo "docker or podman needed" >&2; exit 1; }
VERSION=$(cat VERSION)
mkdir -p "$OUT"

# 1. the packages, on the oldest release
echo "== $BUILD_IMAGE: packages"
$DOCKER run --rm -v "$PWD:/src:ro" -v "$OUT:/out" -e OWNER="$(id -u):$(id -g)" "$BUILD_IMAGE" sh -ec '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential libfftw3-dev libuhd-dev libbladerf-dev \
        libhackrf-dev dpkg-dev git ca-certificates >/dev/null
    cp -a /src /build && cd /build
    git config --global --add safe.directory /build
    rm -rf libdvbrx/build
    for deb in $(DEB_SUFFIX= ./packaging/build-deb.sh /out); do
        chown "$OWNER" "$deb"
        echo "$deb: $(dpkg-deb --field "$deb" Depends)"
    done'
DEB=$OUT/dvbrx_${VERSION}_amd64.deb
WEBDEB=$OUT/dvbrx-web_${VERSION}_all.deb

# 2. installed in a fresh container of each release
for img in $TEST_IMAGES; do
    echo "== $img: install and check $(basename "$DEB") $(basename "$WEBDEB")"
    $DOCKER run --rm -v "$OUT:/out:ro" -v "$PWD/install.sh:/install.sh:ro" -e DEB="/out/$(basename "$DEB")" \
        -e WEBDEB="/out/$(basename "$WEBDEB")" -e DVBRX_VERSION="$VERSION" -e DKMS_TEST="$DKMS_TEST" "$img" sh -ec '
        export DEBIAN_FRONTEND=noninteractive
        rm -f /etc/dpkg/dpkg.cfg.d/excludes /etc/dpkg/dpkg.cfg.d/docker   # images without man pages
        apt-get update -qq
        apt-get install -y -qq man-db lintian >/dev/null
        sh /install.sh --deb "$DEB" --deb "$WEBDEB" --web > /tmp/install.log 2>&1 || { tail -30 /tmp/install.log; exit 1; }
        grep "==>" /tmp/install.log | sed "s/\x1b\[[0-9;]*m//g"
        dvbrx --version
        dvbrx -h >/dev/null
        dvbrx-web --version
        for f in /usr/lib/dvbrx/dvbrx-x86-64-*; do
            if ldd "$f" | grep "not found"; then echo "$f: library missing"; exit 1; fi
        done
        # the builds this processor runs (the launcher picks the best one)
        flags=" $(grep -m1 "^flags" /proc/cpuinfo | cut -d: -f2) "
        for v in v2 v3 v4; do
            case "$v:$flags" in v3:*" avx2 "*|v2:*|v4:*" avx512bw "*) ;; *) echo "build $v: not run (processor)"; continue ;; esac
            DVBRX_ARCH=$v dvbrx --version >/dev/null && echo "build $v: runs"
        done
        # each radio library loads (no radio here: the error must come from the device, not from loading)
        for drv in uhd bladerf hackrf; do
            msg=$(timeout 60 dvbrx -u "$drv:" -f 1e9 -s 1e6 -o /dev/null 2>&1 | grep -v "^\[INFO\]") || true
            if printf "%s\n" "$msg" | grep -q -e "library not found" -e " has no "; then
                echo "radio $drv: NOT LOADED: $msg"; exit 1
            fi
            echo "radio $drv: loads ($(printf "%s\n" "$msg" | grep -m1 . || echo "device answers"))"
        done
        if [ "$DKMS_TEST" = 1 ]; then
            . /etc/os-release
            case "$ID" in ubuntu) h=linux-headers-generic ;; *) h=linux-headers-amd64 ;; esac
            apt-get install -y -qq "$h" >/dev/null
            k=$(ls /lib/modules | sort -V | tail -1)
            if dkms build -m dvbrx -v "$DVBRX_VERSION" -k "$k" >/tmp/dkms.log 2>&1; then
                echo "kernel module: builds against $k"
            else
                tail -20 /tmp/dkms.log; cat /var/lib/dkms/dvbrx/"$DVBRX_VERSION"/build/make.log 2>/dev/null | tail -20
                echo "kernel module: DOES NOT BUILD against $k"; exit 1
            fi
        fi
        man=$(command -v man.REAL || command -v man)    # minimized Ubuntu images: man is a stub
        for p in dvbrx.8 dvbrx-web.8; do
            sec=${p##*.}
            $man -w "$sec" "${p%.*}" 2>/dev/null | grep -q "/man$sec/$p" || { echo "missing man page $p"; exit 1; }
            $man -L fr -w "$sec" "${p%.*}" 2>/dev/null | grep -q "/fr/man$sec/$p" || { echo "missing French man page $p"; exit 1; }
        done
        echo "man pages: $(ls /usr/share/man/man*/dvbrx* | wc -l) in English, $(ls /usr/share/man/fr/man*/dvbrx* | wc -l) in French"
        out=$(lintian --no-tag-display-limit "$DEB" "$WEBDEB" 2>&1 || true)
        if printf "%s\n" "$out" | grep "^E:"; then exit 1; fi
        echo "lintian: no error"'
done
ls -l "$DEB" "$WEBDEB"
