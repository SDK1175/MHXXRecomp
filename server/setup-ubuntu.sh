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
# a mirror in the middle of its sync fails some index files (seen on mirror.kakao.com): try again, then go on with
# what came; the install below says it if a package is really missing
apt-get update -qq || { sleep 20; apt-get update -qq; } || echo "   (some package lists failed: going on)"
apt-get install -y -qq golang-go git iptables-persistent >/dev/null

echo "== build"
SRC=/opt/mhxx-hubserver-src
if [ -d "$SRC/.git" ]; then git -C "$SRC" fetch -q --depth 1 origin "$BRANCH" && git -C "$SRC" reset -q --hard FETCH_HEAD
else rm -rf "$SRC" && git clone -q --depth 1 -b "$BRANCH" "$REPO" "$SRC"; fi
(cd "$SRC/server" && CGO_ENABLED=0 GOFLAGS=-mod=mod go build -o /usr/local/bin/mhxx-hubserver .)

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
# CLOSE_OTHERS=1: a server with no firewall of its provider in front (iwinv's Lite zones): everything else in is
# dropped, SSH (22) and the hub's port kept, for IPv4 and IPv6
if [ "${CLOSE_OTHERS:-0}" = 1 ]; then
    for t in iptables ip6tables; do
        command -v $t >/dev/null || continue
        $t -C INPUT -i lo -j ACCEPT 2>/dev/null || $t -I INPUT 1 -i lo -j ACCEPT
        $t -C INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT 2>/dev/null ||
            $t -I INPUT 2 -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
        $t -C INPUT -p tcp --dport 22 -j ACCEPT 2>/dev/null || $t -A INPUT -p tcp --dport 22 -j ACCEPT
        for proto in tcp udp; do
            $t -C INPUT -p $proto --dport $PORT -j ACCEPT 2>/dev/null || $t -A INPUT -p $proto --dport $PORT -j ACCEPT
        done
    done
    iptables -C INPUT -p icmp -j ACCEPT 2>/dev/null || iptables -A INPUT -p icmp -j ACCEPT
    command -v ip6tables >/dev/null && { ip6tables -C INPUT -p ipv6-icmp -j ACCEPT 2>/dev/null || ip6tables -A INPUT -p ipv6-icmp -j ACCEPT; }
    iptables -P INPUT DROP
    command -v ip6tables >/dev/null && ip6tables -P INPUT DROP
    echo "   everything else in: dropped (SSH 22 and $PORT open)"
fi
netfilter-persistent save >/dev/null 2>&1 || true

sleep 1
systemctl --no-pager --lines=3 status mhxx-hubserver | sed -n '1,3p;/hub server on/p'
echo "done: the hub server listens on port $PORT (TCP and UDP)"
