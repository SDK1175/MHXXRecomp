#!/bin/sh
# Installs (or updates) the hub server on an Ubuntu VM (an Oracle Cloud Always Free one: docs/hub-server.md):
# builds server/ from this repository, runs it as a systemd service on port 39510 (TCP and UDP) and opens that
# port in the VM's own firewall (Oracle's Ubuntu images reject everything else). Run it again to update.
#   curl -fsSL https://raw.githubusercontent.com/SDK1175/MHXXRecomp/mhxx/server/setup-ubuntu.sh | sudo sh
set -eu
PORT=39510
REPO=https://github.com/SDK1175/MHXXRecomp
BRANCH=mhxx
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq golang-go git iptables-persistent >/dev/null

echo "== build"
SRC=/opt/mhxx-hubserver-src
if [ -d "$SRC/.git" ]; then git -C "$SRC" fetch -q --depth 1 origin "$BRANCH" && git -C "$SRC" reset -q --hard FETCH_HEAD
else rm -rf "$SRC" && git clone -q --depth 1 -b "$BRANCH" "$REPO" "$SRC"; fi
(cd "$SRC/server" && GOFLAGS=-mod=mod go build -o /usr/local/bin/mhxx-hubserver .)

echo "== service"
cat > /etc/systemd/system/mhxx-hubserver.service <<UNIT
[Unit]
Description=MHXX Recomp hub server
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/mhxx-hubserver -addr :$PORT
DynamicUser=yes
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable -q mhxx-hubserver
systemctl restart mhxx-hubserver

echo "== firewall (the VM's own; Oracle's Security List is set in the console)"
for proto in tcp udp; do
    iptables -C INPUT -p $proto --dport $PORT -j ACCEPT 2>/dev/null || iptables -I INPUT 5 -p $proto --dport $PORT -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p $proto --dport $PORT -j ACCEPT
done
netfilter-persistent save >/dev/null 2>&1 || true

sleep 1
systemctl --no-pager --lines=3 status mhxx-hubserver | sed -n '1,3p;/hub server on/p'
echo "done: the hub server listens on port $PORT (TCP and UDP)"
