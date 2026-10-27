#!/bin/sh -e
# Give ish executables the entitlement Hypervisor.framework asks for, so
# guest code can run in a VM (kernel/hvf.h). Ad hoc signature, for running
# on this Mac; meson does it after linking the CLI (-s: then touch a stamp).
#   tools/hvf-sign.sh [-s stamp] build/ish...
stamp=
if [ "$1" = -s ]; then stamp=$2; shift 2; fi
ent=$(mktemp -t hvf).plist
cat > "$ent" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
PLIST
for exe in "$@"; do
    codesign -s - -f --entitlements "$ent" "$exe" 2> /dev/null
done
rm -f "$ent"
[ -z "$stamp" ] || touch "$stamp"
