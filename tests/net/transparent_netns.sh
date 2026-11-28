#!/bin/sh
# Run as root. All firewall and route changes live in a fresh network namespace.
set -eu
exec unshare --net -- sh -eu -c '
    ip link set lo up
    iptables -t nat -A OUTPUT -o lo -p tcp --dport 29001 -j REDIRECT --to-ports 29002
    iptables -t nat -A OUTPUT -o lo -p tcp --dport 29005 -j REDIRECT --to-ports 29006
    ip6tables -t nat -A OUTPUT -o lo -p tcp --dport 29005 -j REDIRECT --to-ports 29006
    iptables -t mangle -A PREROUTING -i lo -p udp --dport 29003 -j TPROXY --on-port 29004
    PACE_TEST_TRANSPARENT=1 "$1" --test-case="netfilter original destinations"
' sh "$1"
