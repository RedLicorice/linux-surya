#!/bin/sh
# Install the surya tuned profiles and the tuned-ppd mapping. Run as root on the phone, from any
# directory. Locations can be overridden from the environment:
#   TUNED_PROFILE_DIR (default /etc/tuned/profiles)   TUNED_PPD_CONF (default /etc/tuned/ppd.conf)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
profiles=${TUNED_PROFILE_DIR:-/etc/tuned/profiles}
ppd=${TUNED_PPD_CONF:-/etc/tuned/ppd.conf}

for p in "$here"/surya-*/; do
    name=$(basename "$p")
    install -D -m 0644 "$p/tuned.conf" "$profiles/$name/tuned.conf"
done
# keep the packaged mapping once, so it can be put back
[ -f "$ppd.dist" ] || cp -a "$ppd" "$ppd.dist"
install -m 0644 "$here/ppd.conf" "$ppd"
systemctl restart tuned tuned-ppd
tuned-adm active
