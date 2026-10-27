#!/bin/sh -e
# Give an ish executable the entitlement Hypervisor.framework asks for, so
# ISH_HVF=1 can run guest code in a VM (kernel/hvf.h). Ad hoc signature,
# for running it on this Mac.
#   tools/hvf-sign.sh build/ish
ent=$(mktemp -t hvf).plist
cat > "$ent" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
PLIST
for exe in "$@"; do
    codesign -s - -f --entitlements "$ent" "$exe"
done
rm -f "$ent"
