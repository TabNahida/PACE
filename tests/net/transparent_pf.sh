#!/bin/sh
# Run as root with the path to pace_network_tests.
set -eu
binary=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
test -x "$binary"

rules() {
    cat <<'EOF'
rdr pass on lo0 inet proto tcp from 127.0.0.1 to 127.0.0.1 port 29001 -> 127.0.0.1 port 29002
rdr pass on lo0 inet proto tcp from 127.0.0.1 to 127.0.0.1 port 29005 -> 127.0.0.1 port 29006
rdr pass on lo0 inet6 proto tcp from ::1 to ::1 port 29005 -> ::1 port 29006
EOF
}

case $(uname -s) in
FreeBSD)
    # Each VNET has its own interfaces, PF states and ruleset.
    kldload -n pf
    name=pace_pf_$$
    jail -c name="$name" path=/ host.hostname="$name" vnet persist
    trap 'jail -r "$name"' EXIT
    trap 'exit 1' HUP INT TERM
    jexec "$name" ifconfig lo0 inet 127.0.0.1/8 up
    jexec "$name" ifconfig lo0 inet6 ::1/128
    rules | jexec "$name" pfctl -f -
    jexec "$name" pfctl -e
    jexec "$name" env PACE_TEST_PF_REDIRECT=1 "$binary" --test-case="PF original destination after redirection"
    ;;
Darwin)
    # Use the standard Apple wildcard rdr anchor and release only our enable reference.
    pfctl -sn | grep -F 'rdr-anchor "com.apple/*"' >/dev/null
    anchor=com.apple/pace-p3-$$
    reference=$(pfctl -E 2>&1)
    token=$(printf '%s\n' "$reference" | awk '/Token *:/ { print $NF }')
    test -n "$token"
    cleanup() {
        pfctl -a "$anchor" -F nat
        pfctl -X "$token"
    }
    trap cleanup EXIT
    trap 'exit 1' HUP INT TERM
    rules | pfctl -a "$anchor" -f -
    PACE_TEST_PF_REDIRECT=1 "$binary" --test-case="PF original destination after redirection"
    ;;
*) exit 2 ;;
esac
