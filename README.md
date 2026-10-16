# ft_ping

`ft_ping` sends IPv4 ICMP echo requests to a host, prints replies as they arrive,
and reports packet loss and round-trip time statistics when stopped. It is
written in C and uses a raw ICMP socket.

## Build and run

Build the program with:

```sh
make
```

Run it with a hostname or IPv4 address:

```sh
./ft_ping example.com
./ft_ping --deadline=5 --size=120 192.0.2.1
```

Opening a raw socket usually requires root privileges or the appropriate
capability. Press **Ctrl-C** to stop a regular run and print its statistics.
Use `./ft_ping --help` to see the supported options.

## Main data structures

### `t_options`

Stores the command-line settings: output mode, timing values, payload size and
pattern, TTL/TOS values, and the destination host. The `have_*` fields record
whether optional settings such as `--ttl` were explicitly supplied.

### `t_ping`

Holds the state for one ping session: destination address and resolved name,
socket descriptor, sequence and identifier values, packet counters, timing
statistics, and a copy of the parsed options.

## Function guide

### Signal and time helpers

| Function | Purpose |
| --- | --- |
| `on_sigint` | Handles Ctrl-C by clearing `g_running`. The main ping loop sees this flag and exits cleanly so it can print the final statistics. |
| `elapsed_ms` | Returns the time between two `timeval` values in milliseconds. Used to calculate reply times and total run time. |
| `elapsed_sec` | Converts the result of `elapsed_ms` to seconds for interval, timeout, and deadline calculations. |

### Command-line parsing

| Function | Purpose |
| --- | --- |
| `parse_unsigned` | Reads a non-negative integer option, checks that the whole argument is numeric, and enforces a caller-provided maximum. |
| `parse_double` | Reads a positive floating-point value, used for options measured in seconds. |
| `usage` | Prints the help text to the stream passed by the caller. |
| `parse_pattern` | Validates an even-length hexadecimal payload pattern and stores its bytes in `t_options`. |
| `option_value` | Reads the value of a long option, accepting either `--option=value` or `--option value`. It reports whether the option matched and whether a value was missing. |
| `read_option_value` | Extends `option_value` to support short options such as `-s 120` and attached values such as `-s120`. |
| `parse_options` | Sets defaults, recognizes help/version and ping options, validates their values, and stores the single destination host. On failure it supplies a human-readable error for `main`. |

### Address and packet handling

| Function | Purpose |
| --- | --- |
| `icmp_checksum` | Computes the Internet checksum over the ICMP packet before it is sent. |
| `resolve_host` | Resolves the supplied destination as IPv4, stores its numeric address, and looks up a display name unless numeric output was requested. |
| `open_socket` | Opens the raw ICMP socket and applies requested socket settings such as route bypass, TTL, TOS, and the IP timestamp option. |
| `send_echo` | Builds an ICMP echo-request packet with the current identifier and sequence number, adds the timestamp/payload pattern, calculates the checksum, and sends it to the destination. |
| `recv_reply` | Reads a packet from the socket, validates the IP and ICMP headers, ignores packets that are not replies for this process, calculates RTT, updates counters/statistics, and prints the reply. With `-v`, it also displays other ICMP packets it sees. |

### Session control and output

| Function | Purpose |
| --- | --- |
| `print_stats` | Prints the number of transmitted and received packets, packet loss, total run time, and RTT summary when replies were received. |
| `ping_loop` | Runs the session: sends preload and regular requests, waits for replies with `select`, respects the configured timeout/deadline, and prints final statistics when the loop ends. |
| `main` | Coordinates the program: parses options, resolves the host, opens the socket, prints the initial status line, installs the Ctrl-C handler, starts the loop, and closes the socket. |

## Subject testing checklist

Use this checklist to prepare for the subject evaluation. Run packet tests in a
Linux VM with permission to open raw ICMP sockets. The subject specifies Debian
7.0 or later and a Linux kernel newer than 3.14 for evaluation. Use a short
deadline for tests that would otherwise keep running; press **Ctrl-C** when a
test calls for stopping it manually.

### Submission and build requirements

- [ ] Confirm the project is written in C and the executable is named `ft_ping`.
- [ ] Build with `make` and confirm it succeeds without warnings or errors.
- [ ] Check the usual Makefile rules: `all`, `clean`, `fclean`, and `re`.
- [ ] Run `make` twice and confirm the second run does not rebuild unchanged files.
- [ ] Confirm the program does not invoke the system `ping` command.
- [ ] Run it in the required Linux VM and confirm it can be used from the cluster computer, as required by the subject.

### Mandatory behavior

- [ ] `./ft_ping -?` displays help and exits successfully.
- [ ] `./ft_ping -v 127.0.0.1` starts a verbose IPv4 ping; check that the program can display ICMP packet errors without terminating unexpectedly.
- [ ] `./ft_ping 127.0.0.1` accepts an IPv4 address, sends echo requests, receives replies, and prints packet statistics when stopped.
- [ ] `./ft_ping example.com` accepts a hostname and resolves it to IPv4.
- [ ] Try a fully qualified domain name (FQDN). Confirm the program handles forward resolution and does not perform a new DNS lookup as part of processing each returned packet.
- [ ] Cause an ICMP error, for example by using a very low TTL where supported, and confirm `-v` reports the packet/error without crashing or stopping unexpectedly.
- [ ] Compare output and indentation with `ping` from inetutils 2.0 (`ping -V` identifies the reference version). The subject allows differences in the RTT line and reverse DNS resolution; other output indentation should match.
- [ ] Compare reply timing with the reference. The subject allows a tolerance of +/- 30 ms.
- [ ] Check invalid or missing arguments and unreachable destinations: errors should be handled cleanly, without a crash.

### Bonus options

The subject evaluates bonuses only after every mandatory requirement works
perfectly. Test each feature separately and confirm it behaves as described.

| Option | Suggested check |
| --- | --- |
| `-f` | Run a brief flood test (for example, `sudo ./ft_ping -f -w 1 127.0.0.1`). Confirm packets are sent rapidly and the process stops at the deadline. |
| `-l COUNT` | Run `./ft_ping -l 3 -w 2 127.0.0.1`. Confirm the initial preload packets are sent quickly before regular interval sending. |
| `-n` | Run `./ft_ping -n -w 2 example.com`. Confirm output uses numeric addresses without reverse-name display. |
| `-w SEC` | Run `./ft_ping -w 2 127.0.0.1`. Confirm the session ends after the deadline and prints statistics. |
| `-W SEC` | Run `./ft_ping -W 0.5 -w 2 127.0.0.1`. Confirm the reply wait timeout is accepted and the overall deadline still ends the session. |
| `-p HEX` | Run `./ft_ping -p deadbeef -s 64 -w 2 127.0.0.1`. Confirm a valid hexadecimal payload pattern is accepted; also check malformed or odd-length patterns are rejected cleanly. |
| `-r` | Run `./ft_ping -r -w 2 127.0.0.1`. Confirm the route-bypass option is accepted and applied where the OS supports it. |
| `-s SIZE` | Run `./ft_ping -s 120 -w 2 127.0.0.1`. Confirm the requested payload size is reflected in the output and replies. |
| `-T NUM` | Run `./ft_ping -T 16 -w 2 127.0.0.1`. Confirm the TOS value is accepted and applied where the OS supports it. |
| `--ttl NUM` | Run `./ft_ping --ttl 1 -v -w 2 127.0.0.1` or use a routed destination. Confirm the TTL is applied; use a routed destination to exercise a resulting TTL-expired ICMP error. |
| `--ip-timestamp` | Run `./ft_ping --ip-timestamp -w 2 127.0.0.1`. Confirm the IP timestamp option is requested where the OS/network supports it. |

### Other subject-mentioned options

The subject says these options do not count as bonus features. Check them if
they are included in the submitted program:

- [ ] `-V` prints version information.
- [ ] `--usage` prints a short usage message.
- [ ] `--echo` is accepted and sends an echo request.

When checking a numeric option, also try a missing, malformed, or out-of-range
value and confirm the program prints a useful error instead of crashing.

## Packet flow

1. `main` parses the arguments and resolves the destination.
2. `open_socket` prepares the raw ICMP socket.
3. `ping_loop` calls `send_echo` at the configured interval and waits for socket activity.
4. When a packet arrives, `recv_reply` validates it and records its round-trip time.
5. Ctrl-C or a configured deadline ends the loop, and `print_stats` summarizes the run.
