#!/usr/bin/env bash
# ft_ping comparison test suite.
#
# Each test runs the real ping (reference: inetutils-2.0, see the subject) and
# ft_ping with the same arguments, then compares exit status and output.
#
# Usage:   tests/run_tests.sh [path/to/ft_ping]
# Env:     REAL_PING=/path/to/inetutils/ping   (default: ping)
#          VERBOSE=1                           show both outputs for every test
#          RESULTS_DIR=dir                     where the result files go (default: tests)
#
# Every test's raw output is also written to two files with the same layout,
# so they can be compared side by side (e.g. diff, vimdiff):
#   $RESULTS_DIR/ping_results.txt      reference ping
#   $RESULTS_DIR/ft_ping_results.txt   ft_ping
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
RESULTS_DIR="${RESULTS_DIR:-$(dirname "$0")}"
REAL_RESULTS="$RESULTS_DIR/ping_results.txt"
FT_RESULTS="$RESULTS_DIR/ft_ping_results.txt"

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

# ------------------------------------------------------------ result files
: >"$REAL_RESULTS" && : >"$FT_RESULTS" || exit 1
# Under sudo the files would belong to root: give them back to the user.
if [ -n "${SUDO_UID:-}" ]; then
	chown "$SUDO_UID:$SUDO_GID" "$REAL_RESULTS" "$FT_RESULTS"
fi

# log_result <file> <program> <description> <status> <output file> <args...>
log_result() {
	local file="$1" prog="$2" desc="$3" st="$4" out="$5"
	shift 5
	{
		printf '==== %s\n$ %s %s\n' "$desc" "$prog" "$*"
		cat "$out"
		printf -- '---- exit status: %s\n\n' "$st"
	} >>"$file"
}

# --------------------------------------------------------------- normalize
# Remove from the output what the subject says may differ, or what changes on every run.
normalize() {
	sed -E \
		-e 's/^(ft_)?ping: /PROG: /' \
		-e "s/'(ft_)?ping --(help|usage)'/'PROG --\\2'/g" \
		-e 's/^Usage: (ft_)?ping/Usage: PROG/' \
		-e 's/\b(ft_)?ping \[OPTION/PROG [OPTION/' \
		-e 's/time=[0-9]+(\.[0-9]+)? ms/time=X ms/g' \
		-e '/^(round-trip|rtt) /d' \
		-e 's/id 0x[0-9a-fA-F]+ = [0-9]+/id 0xID = ID/' \
		-e 's/(icmp_seq|seq)=[0-9]+/\1=N/g' \
		-e 's/\t[0-9]+ ms( = 0x[0-9a-f]+)?$/\tN ms/' \
		"$1" |
	awk '
		# -v dumps the IP header of the packet that caused an ICMP error.
		# Its Id and checksum change on every packet: mask them.
		prev_hex { $3 = "ID"; $6 = "CKS"; prev_hex = 0 }
		/^IP Hdr Dump:/ { prev_hex = 1 }
		prev_hdr { $5 = "ID"; $10 = "CKS"; prev_hdr = 0 }
		/^Vr HL TOS/ { prev_hdr = 1 }
		# ICMP dump: "ICMP: type 8, code 0, size 64, id 0x1234, seq 0x0000"
		/^ICMP: type/ { gsub(/id 0x[0-9a-fA-F]+/, "id 0xID"); gsub(/seq 0x[0-9a-fA-F]+/, "seq 0xN") }
		{ print }
	' |
	if [ "${MASK_COUNTS:-0}" = 1 ]; then
		# Flood mode: how many packets fit in the time depends on the machine.
		sed -E -e 's/[.\x08]+---/---/' -e 's/^[0-9]+ packets transmitted, [0-9]+ packets received, [0-9]+%/N packets transmitted, N packets received, N%/'
	else
		cat
	fi
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

	log_result "$REAL_RESULTS" ping "$desc" "$real_st" "$real_out" "$@"
	log_result "$FT_RESULTS" ft_ping "$desc" "$ft_st" "$ft_out" "$@"

	normalize "$real_out" >"$real_out.n"
	normalize "$ft_out" >"$ft_out.n"

	if [ "${STATUS_ONLY:-0}" = 1 ]; then
		# Output differs on purpose (see the call site): only compare the status.
		cmp -s "$real_out.n" "$ft_out.n" || cp "$real_out.n" "$ft_out.n"
	fi

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

# Timeouts end in .5 so Ctrl+C never lands exactly when a packet is due
# (ping sends one per second), which would make the packet counts flaky.

# ============================================================ mandatory
section "Mandatory"
compare "IPv4 address"                       2.5 127.0.0.1
compare "hostname"                           2.5 localhost
compare "no host"                            2.5
compare "unknown host"                       2.5 nonexistent.invalid
# ft_ping's help lists only the options it has (the subject's), so the text
# differs from inetutils on purpose: only the exit status is compared.
STATUS_ONLY=1 compare "-? (help)"            2.5 -?
compare "-v (TTL exceeded error)"            3.5 -v --ttl=1 8.8.8.8

# ================================================================ bonus
section "Bonus: one test per option"
MASK_COUNTS=1 compare "-f"                   2.5 -f -w 1 127.0.0.1
compare "-l"                                 2.5 -l 3 127.0.0.1
compare "-n"                                 2.5 -n localhost
compare "-w"                                 3.5 -w 2 127.0.0.1
compare "-p"                                 2.5 -p ff 127.0.0.1
compare "-r"                                 2.5 -r 127.0.0.1
compare "-s"                                 2.5 -s 100 127.0.0.1
compare "-T"                                 2.5 -T 16 127.0.0.1
compare "--ttl"                              2.5 --ttl=64 127.0.0.1
compare "--ip-timestamp"                     2.5 --ip-timestamp=tsonly 127.0.0.1

# ======================================================= not bonus (subject)
section "Not bonus"
# Same: ft_ping prints its own version line and a usage line for its options.
STATUS_ONLY=1 compare "-V"                   2.5 -V
STATUS_ONLY=1 compare "--usage"              2.5 --usage
compare "--echo"                             2.5 --echo 127.0.0.1

# ------------------------------------------------------------------ summary
printf '\n%sResult:%s %s%d passed%s, %s%d failed%s\n' \
	"$BOLD" "$RESET" "$GREEN" "$PASS" "$RESET" "$RED" "$FAIL" "$RESET"
echo "Raw outputs: $REAL_RESULTS and $FT_RESULTS"
if [ "$FAIL" -gt 0 ]; then
	echo "Failed tests:"
	printf '  - %s\n' "${FAILED_TESTS[@]}"
	exit 1
fi
