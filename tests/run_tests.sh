#!/usr/bin/env bash
# ft_ping comparison test suite.
#
# Each test runs the real ping (reference: inetutils-2.0, see the subject) and
# ft_ping with the same arguments, then compares exit status and output.
#
# Usage:   tests/run_tests.sh [path/to/ft_ping]
# Env:     REAL_PING=/path/to/inetutils/ping   (default: ping)
#          VERBOSE=1                           show both outputs for every test
#
# Must run as root (or with CAP_NET_RAW on both binaries) because both use raw ICMP sockets.
#
# Before comparing, outputs are normalized for the differences the subject allows:
#   - the program name ("ping:" / "ft_ping:" -> "PROG:")
#   - round-trip times (time=1.234 ms -> time=X ms)
#   - the RTT summary line ("round-trip ..." / "rtt ...") is dropped
#   - ICMP ids and IP header fields that change between runs (id, checksum)
# The subject also tolerates +/- 30 ms on packet reception, so times are never compared.

FT_PING="${1:-./ft_ping}"
REAL_PING="${REAL_PING:-ping}"
VERBOSE="${VERBOSE:-0}"

PASS=0
FAIL=0
FAILED_TESTS=()
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ -t 1 ]; then
	GREEN=$'\e[32m'; RED=$'\e[31m'; YELLOW=$'\e[33m'; BOLD=$'\e[1m'; RESET=$'\e[0m'
else
	GREEN=; RED=; YELLOW=; BOLD=; RESET=
fi

# ------------------------------------------------------------ sanity checks
if [ ! -x "$FT_PING" ]; then
	echo "${RED}error:${RESET} $FT_PING not found or not executable (run 'make' first)" >&2
	exit 1
fi
if ! command -v "$REAL_PING" >/dev/null 2>&1; then
	echo "${RED}error:${RESET} reference ping '$REAL_PING' not found (set REAL_PING=...)" >&2
	exit 1
fi
if ! "$REAL_PING" -V 2>&1 | grep -qi inetutils; then
	echo "${YELLOW}warning:${RESET} '$REAL_PING' is not GNU inetutils ping; the subject's reference is inetutils-2.0." >&2
	echo "         Install it (Debian: apt install inetutils-ping) or set REAL_PING=..." >&2
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "${YELLOW}warning:${RESET} not running as root; tests that send packets will probably fail." >&2
fi

# --------------------------------------------------------------- normalize
# Remove from the output what the subject says may differ, or what changes on every run.
normalize() {
	sed -E \
		-e 's/^(ft_)?ping: /PROG: /' \
		-e 's/(Try|Usage:) ['\''"]?(ft_)?ping/\1 PROG/' \
		-e 's/\b(ft_)?ping \[OPTION/PROG [OPTION/' \
		-e 's/time=[0-9]+(\.[0-9]+)? ms/time=X ms/g' \
		-e '/^(round-trip|rtt) /d' \
		-e 's/id 0x[0-9a-fA-F]+ = [0-9]+/id 0xID = ID/' \
		-e 's/(icmp_seq|seq)=[0-9]+/\1=N/g' \
		"$1" |
	awk '
		# inetutils -v dumps the IP header after "Vr HL TOS ..."; mask Id and Cks.
		prev_hdr { $6 = "ID"; $11 = "CKS"; prev_hdr = 0 }
		/^Vr HL TOS/ { prev_hdr = 1 }
		# ICMP dump: "ICMP: type 8, code 0, size 64, id 0x1234, seq 0x0000"
		/^ICMP: type/ { gsub(/id 0x[0-9a-fA-F]+/, "id 0xID"); gsub(/seq 0x[0-9a-fA-F]+/, "seq 0xN") }
		{ print }
	'
}

# compare <description> <timeout secs> <args...>
# Runs both pings with the same args. If they are still running after <timeout>
# seconds, both get SIGINT, like a user pressing Ctrl+C.
compare() {
	local desc="$1" tmo="$2"
	shift 2
	local real_out="$WORK/real" ft_out="$WORK/ft" real_st ft_st reason=""

	timeout --preserve-status -s INT -k 3 "$tmo" "$REAL_PING" "$@" >"$real_out" 2>&1
	real_st=$?
	timeout --preserve-status -s INT -k 3 "$tmo" "$FT_PING" "$@" >"$ft_out" 2>&1
	ft_st=$?

	normalize "$real_out" >"$real_out.n"
	normalize "$ft_out" >"$ft_out.n"

	if [ "$ft_st" -ge 128 ] && [ "$ft_st" -ne "$real_st" ]; then
		reason="ft_ping crashed or was killed (status $ft_st, signal $((ft_st - 128)))"
	elif [ "$ft_st" -ne "$real_st" ]; then
		reason="exit status differs: ping=$real_st ft_ping=$ft_st"
	elif ! cmp -s "$real_out.n" "$ft_out.n"; then
		reason="output differs (after normalization)"
	fi

	if [ -z "$reason" ]; then
		PASS=$((PASS + 1))
		printf '  %sPASS%s  %s\n' "$GREEN" "$RESET" "$desc"
	else
		FAIL=$((FAIL + 1))
		FAILED_TESTS+=("$desc")
		printf '  %sFAIL%s  %s\n        args: %s\n        %s\n' "$RED" "$RESET" "$desc" "$*" "$reason"
		diff -u --label "ping $*" --label "ft_ping $*" "$real_out.n" "$ft_out.n" | head -40 | sed 's/^/        /'
	fi
	if [ "$VERBOSE" = 1 ]; then
		echo "        --- ping (raw) ---";    sed 's/^/        | /' "$real_out"
		echo "        --- ft_ping (raw) ---"; sed 's/^/        | /' "$ft_out"
	fi
}

section() { printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$RESET"; }

echo "${BOLD}ft_ping vs ping${RESET}"
echo "  ft_ping: $FT_PING"
echo "  ping:    $(command -v "$REAL_PING")  ($("$REAL_PING" -V 2>&1 | head -1))"

# =================================================== 1. subject: mandatory
section "Subject: -? option"
compare "-? (help)"                          3 -?
compare "--help"                             3 --help
compare "-? with a host"                     3 -? 127.0.0.1

section "Subject: IPv4 address as parameter"
compare "loopback 127.0.0.1"                 3 127.0.0.1
compare "other loopback 127.0.0.2"           3 127.0.0.2
compare "remote IPv4 8.8.8.8"                4 8.8.8.8
compare "remote IPv4 1.1.1.1"                4 1.1.1.1

section "Subject: hostname / FQDN as parameter (no reverse DNS on replies)"
compare "localhost"                          3 localhost
compare "example.com"                        4 example.com
compare "google.com"                         4 google.com
compare "FQDN with trailing dot"             4 google.com.

section "Subject: -v option"
compare "-v loopback"                        3 -v 127.0.0.1
compare "-v remote"                          4 -v 8.8.8.8
compare "-v hostname"                        4 -v google.com
# The subject says changing the TTL can force a packet error, which -v must show
# without stopping the program.
compare "-v --ttl=1 (Time to live exceeded)" 5 -v --ttl=1 8.8.8.8
compare "-v --ttl=2"                         5 -v --ttl=2 8.8.8.8
compare "--ttl=1 without -v"                 5 --ttl=1 8.8.8.8

section "Subject: error handling"
compare "no argument"                        3
compare "unknown long option"                3 --bogus 127.0.0.1
compare "unknown short option"               3 -x 127.0.0.1
compare "unknown host"                       5 nonexistent.invalid
compare "empty host"                         3 ''
compare "malformed IPv4 256.1.1.1"           5 256.1.1.1
compare "malformed IPv4 1.2.3.4.5"           5 1.2.3.4.5
compare "very long hostname"                 5 "$(printf 'a%.0s' $(seq 1 300)).com"
compare "IPv6 address"                       3 ::1
compare "two hosts"                          4 127.0.0.1 127.0.0.2

# ================================================== 2. bonus flags (subject)
section "Bonus: -w / --deadline"
compare "-w 2"                               5 -w 2 127.0.0.1
compare "-w 3 remote"         6 -w 3 8.8.8.8
compare "-w 0"                               3 -w 0 127.0.0.1
compare "-w abc"                             3 -w abc 127.0.0.1

section "Bonus: -W / --linger"
compare "-W 1"                               4 -W 1 127.0.0.1
compare "-W 1 unreachable"                   5 -W 1 -w 3 192.0.2.1

section "Bonus: -s / --size"
compare "-s 0"                               3 -s 0 127.0.0.1
compare "-s 1"                               3 -s 1 127.0.0.1
compare "-s 8"                               3 -s 8 127.0.0.1
compare "-s 120"                             3 -s 120 127.0.0.1
compare "-s 1472"                            3 -s 1472 127.0.0.1
compare "-s 1473 remote (fragmented)"        4 -s 1473 8.8.8.8
compare "-s 65507"                           3 -s 65507 127.0.0.1
compare "-s 65508 (too big)"                 3 -s 65508 127.0.0.1
compare "-s -1"                              3 -s -1 127.0.0.1
compare "-s abc"                             3 -s abc 127.0.0.1
compare "-s without value"                   3 -s

section "Bonus: -p / --pattern"
compare "-p ff"                              3 -p ff 127.0.0.1
compare "-p deadbeef"                        3 -p deadbeef 127.0.0.1
compare "-p 16 bytes"                        3 -p 0123456789abcdef0123456789abcdef 127.0.0.1
compare "-p zz (not hex)"                    3 -p zz 127.0.0.1
compare "-v -p ff"                           3 -v -p ff 127.0.0.1

section "Bonus: -l / --preload"
compare "-l 3"                               3 -l 3 127.0.0.1
compare "-l 0"                               3 -l 0 127.0.0.1
compare "-l -1"                              3 -l -1 127.0.0.1

section "Bonus: -n / --numeric"
compare "-n localhost"                       3 -n localhost
compare "-n remote hostname"                 4 -n google.com

section "Bonus: --ttl"
compare "--ttl=64"                           3 --ttl=64 127.0.0.1
compare "--ttl 64 (separate value)"          3 --ttl 64 127.0.0.1
compare "--ttl=0"                            3 --ttl=0 127.0.0.1
compare "--ttl=256"                          3 --ttl=256 127.0.0.1

section "Bonus: -T / --tos"
compare "-T 16"                              3 -T 16 127.0.0.1
compare "-T 256"                             3 -T 256 127.0.0.1

section "Bonus: -r / --ignore-routing"
compare "-r loopback"                        3 -r 127.0.0.1
compare "-r remote (no direct route)"        4 -r 8.8.8.8

section "Bonus: --ip-timestamp"
compare "--ip-timestamp loopback"            3 --ip-timestamp 127.0.0.1

section "Bonus: -f / --flood"
compare "-f -w 1"                            4 -f -w 1 127.0.0.1

section "Not bonus but handled: -V, --usage, --echo"
compare "-V"                                 3 -V
compare "--usage"                            3 --usage
compare "--echo"                             3 --echo 127.0.0.1

# ========================================== 3. common ping test scenarios
section "Common: unreachable and special addresses"
compare "TEST-NET 192.0.2.1 (no reply)"      5 192.0.2.1
compare "-v TEST-NET 192.0.2.1"              5 -v 192.0.2.1
compare "private unused 10.255.255.1"        5 10.255.255.1
compare "0.0.0.0"                            3 0.0.0.0
compare "broadcast 255.255.255.255"          3 255.255.255.255
compare "short-form address 127.1"           3 127.1
compare "decimal address 2130706433"         3 2130706433
compare "hex address 0x7f000001"             3 0x7f000001

section "Common: argument order and combinations"
compare "options after the host"             3 127.0.0.1 -s 100
compare "--size=100 (= form)"                3 --size=100 127.0.0.1
compare "-s100 (glued value)"                3 -s100 127.0.0.1
compare "-vn (grouped short options)"        3 -vn 127.0.0.1
compare "-- before the host"                 3 -- 127.0.0.1
compare "-v -s 100 -p ab -w 2"               4 -v -s 100 -p ab -w 2 8.8.8.8

# ------------------------------------------------------------------ summary
printf '\n%sResult:%s %s%d passed%s, %s%d failed%s\n' \
	"$BOLD" "$RESET" "$GREEN" "$PASS" "$RESET" "$RED" "$FAIL" "$RESET"
if [ "$FAIL" -gt 0 ]; then
	echo "Failed tests:"
	printf '  - %s\n' "${FAILED_TESTS[@]}"
	exit 1
fi
