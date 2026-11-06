#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs dvbrx from its Debian package, on Debian 12 / 13, Proxmox VE 8 / 9 or Ubuntu 24.04 and later:
#
#   curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Rx/main/install.sh | sudo sh
#   curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Rx/main/install.sh | sudo sh -s -- --web
#   sudo ./install.sh --deb dist/dvbrx_0.1.0_amd64.deb        (a package built here, see packaging/)
#
# Options:
#   --web          also install dvbrx-web, the web supervision page (installed, not enabled)
#   --adapters N   N /dev/dvb adapters (default: as configured, 1 on a first install), one radio each
#   --deb FILE     install this .deb instead of downloading the latest one (repeatable: dvbrx, dvbrx-web)
#   --force        go on with a release that is not one of the above
#
# It installs the headers of the running kernel (the kernel module is built by DKMS), then the package;
# apt brings the radio libraries. It does not start any service: write /etc/dvbrxd/0.conf first, then
# "sudo systemctl enable --now dvbrxd@0".
set -eu

BASE=${DVBRX_BASE_URL:-https://github.com/TrusterBSW/DVB-Rx/raw/main/releases}
DEBS="" WEB=0 ADAPTERS="" FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
    --deb) [ -n "${2:-}" ] || { echo "install.sh: --deb needs a file" >&2; exit 2; }
           DEBS="$DEBS $(realpath "$2")"; shift ;;
    --web) WEB=1 ;;
    --adapters) ADAPTERS=${2:?--adapters needs a number}; shift ;;
    --force) FORCE=1 ;;
    -h | --help) sed -n '3,18s/^# \{0,1\}//p' "$0" 2>/dev/null || echo "see the head of install.sh"; exit 0 ;;
    *) echo "install.sh: unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run it as root (sudo sh, or sudo ./install.sh)"
command -v apt-get >/dev/null || die "apt-get not found: Debian, Ubuntu or Proxmox VE needed"
[ "$(dpkg --print-architecture)" = amd64 ] || die "the package is built for amd64 (x86-64), not $(dpkg --print-architecture)"
case "$ADAPTERS" in "" | [1-8]) ;; *) die "--adapters: 1 to 8" ;; esac

# the release
. /etc/os-release
ok=0
case "$ID" in
debian) case "${VERSION_ID:-}" in 12 | 13) ok=1 ;; esac ;;
ubuntu) [ "${VERSION_ID%%.*}" -ge 24 ] 2>/dev/null && ok=1 ;;
esac
what="$PRETTY_NAME"
PVE=0
if [ -e /etc/pve ] || command -v pveversion >/dev/null 2>&1; then
    PVE=1
    what="Proxmox VE $(pveversion 2>/dev/null | sed -n 's|^pve-manager/\([0-9.]*\).*|\1|p') on $PRETTY_NAME"
fi
if [ "$ok" = 1 ]; then
    say "system: $what"
elif [ "$FORCE" = 1 ]; then
    say "system: $what (not tested, --force)"
else
    die "$what is not supported (Debian 12/13, Proxmox VE 8/9, Ubuntu 24.04 or later); --force to try anyway"
fi
grep -q -w sse4_2 /proc/cpuinfo || die "this processor lacks SSE4.2 (x86-64-v2): the receiver would not run"

# the packages
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fetch() {   # URL FILE
    if command -v curl >/dev/null; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null; then wget -q "$1" -O "$2"
    else apt-get install -y -qq curl >/dev/null && curl -fsSL "$1" -o "$2"; fi
}
if [ -n "$DEBS" ]; then
    for d in $DEBS; do
        [ -f "$d" ] || die "$d: no such file"
        say "package: $d"
    done
else
    fetch "$BASE/LATEST" "$TMP/LATEST" || die "cannot download $BASE/LATEST"
    fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" || die "cannot download $BASE/SHA256SUMS"
    for name in $(tr -d '\r' < "$TMP/LATEST"); do
        case "$name" in
        dvbrx_*_amd64.deb) ;;
        dvbrx-web_*_all.deb) [ "$WEB" = 1 ] || continue ;;
        *) die "unexpected LATEST entry: $name" ;;
        esac
        say "downloading $name"
        fetch "$BASE/$name" "$TMP/$name" || die "cannot download $BASE/$name"
        (cd "$TMP" && grep " $name\$" SHA256SUMS | sha256sum -c --quiet -) || die "$name: checksum mismatch"
        chmod 644 "$TMP/$name"
        DEBS="$DEBS $TMP/$name"
    done
fi
case "$DEBS" in *dvbrx_*) ;; *) die "no dvbrx package" ;; esac

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq

# headers of the running kernel, for the module (DKMS)
k=$(uname -r)
if [ -d "/lib/modules/$k/build" ]; then
    say "kernel headers: $k already there"
else
    if [ "$PVE" = 1 ]; then
        hdr="proxmox-headers-$k pve-headers-$k proxmox-default-headers"
    else
        hdr="linux-headers-$k"
    fi
    got=""
    for h in $hdr; do
        if apt-cache policy "$h" 2>/dev/null | grep -q 'Candidate: [0-9]'; then
            apt-get install -y -qq "$h" >/dev/null && got=$h && break
        fi
    done
    if [ -n "$got" ]; then
        say "kernel headers: $got"
    else
        say "kernel headers for $k not found in the repositories: the module will not build (install them, then: dkms autoinstall)"
    fi
fi

# installed by apt (dependencies, and the radio libraries recommended by the package)
say "installing (apt brings dkms, FFTW and the radio libraries)"
# configuration files already there (an upgrade, or a former install from the sources) are kept
# shellcheck disable=SC2086
apt-get install -y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold $DEBS

if [ -n "$ADAPTERS" ]; then
    printf '# number of /dev/dvb adapters (1 to 8), one radio and one dvbrxd@N service each\noptions dvbrx adapters=%s\n' \
        "$ADAPTERS" > /etc/modprobe.d/dvbrx.conf
    i=0
    while [ "$i" -lt "$ADAPTERS" ]; do
        [ -f "/etc/dvbrxd/$i.conf" ] || install -m 0644 /usr/share/doc/dvbrx/examples/dvbrxd.conf "/etc/dvbrxd/$i.conf"
        i=$((i + 1))
    done
    if ! systemctl list-units --state=active --no-legend 'dvbrxd@*' 2>/dev/null | grep -q .; then
        rmmod dvbrx 2>/dev/null || true
    fi
    modprobe dvbrx 2>/dev/null || true
    say "adapters: $ADAPTERS (/etc/modprobe.d/dvbrx.conf)"
fi

# what is there
say "$(dvbrx --version)"
if [ -e /sys/module/dvbrx ]; then
    say "kernel module: loaded, $(ls -d /dev/dvb/adapter* 2>/dev/null | wc -l) adapter(s) in /dev/dvb"
else
    say "kernel module: not loaded (dkms status; modprobe dvbrx)"
fi
libs=""
ldconfig -p | grep -q 'libuhd\.so' && libs="$libs uhd"
ldconfig -p | grep -q 'libbladeRF\.so' && libs="$libs bladerf"
ldconfig -p | grep -q 'libhackrf\.so' && libs="$libs hackrf"
say "radio drivers usable here:${libs:- none}"
radios=""
if [ -d /sys/bus/usb/devices ]; then
    for d in /sys/bus/usb/devices/*; do
        [ -r "$d/idVendor" ] || continue
        case "$(cat "$d/idVendor"):$(cat "$d/idProduct")" in
        2500:002[0-2]) radios="$radios USRP-B2xx" ;;
        2cf0:5250) radios="$radios bladeRF-2.0" ;;
        1d50:6089) radios="$radios HackRF-One" ;;
        esac
    done
fi
say "radios plugged in:${radios:- none}"
cat <<'EOF'

Next:
  sudoedit /etc/dvbrxd/0.conf              the radio of adapter 0, e.g. DVBRXD_ARGS="-u type=b200 -G auto"
  sudo systemctl enable --now dvbrxd@0     /dev/dvb/adapter0, now and at every boot
  journalctl -u dvbrxd@0 -f                the receiver's log
  man dvbrx                                options, radios, several radios, USB bandwidth
EOF
[ "$WEB" = 1 ] && cat <<'EOF'
  sudo systemctl enable --now dvbrx-web    the web page, http://<host>:8099/ (man dvbrx-web)
EOF
exit 0
