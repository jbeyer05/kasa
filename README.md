# Kasa Smart

This **UNOFFICIAL** repository contains information about TP-Link's Kasa Smart API and a C++ CLI tool for interacting with Kasa devices via the local control protocol (not via the cloud service).

* [Kasa API Documentation](API.md)
* [Known Firmware URLs](FIRMWARE.md)

GitHub issues and pull requests are very welcome to help refine and expand this knowledge base.


## Compiling

The tool is C++ and uses a vendored [nlohmann/json](https://github.com/nlohmann/json) header. It should compile and run on just about any POSIX-compliant system. Linux and macOS are known to be supported.
Windows users are probably out of luck — sorry, not sorry.

```
$ make
```

Or with the C++ compiler directly (include the vendored JSON path):

```
$ c++ -O3 -Wall -Wextra -I third_party -o kasa kasa.cpp
```

## Usage

The tool supports two modes, selected automatically:

- **Probe mode** — When you run `./kasa <device-addr>` with **no piped input** (stdin is a TTY), the tool connects to the device, fetches system info, discovers child outlets (e.g. on a power strip), then for each outlet requests realtime power data (`get_realtime` with per-child context), prints a one-line summary (slot, alias, current_ma, voltage_mv, power_mw, total_wh), and sleeps **1 second** between each child request to avoid overloading the device.

- **Pipe mode** — When you run `echo '...' | ./kasa <device-addr>`, each line from stdin is sent as one request and each response is printed as one line to stdout. Existing scripts and one-off commands behave as before.

You can pass a host name or IP address, or **`discover`** / **`auto`** to auto-discover a Kasa device on the LAN (UDP broadcast on port 9999; the first responding device is used).

Examples:

```
$ ./kasa discover
$ ./kasa 192.168.1.100
Slot 1 (Plug 1): current_ma=0 voltage_mv=122001 power_mw=0 total_wh=0
Slot 2 (Plug 2): ...
```

```
$ echo '{"system":{"get_sysinfo":{}}}' | ./kasa <device-addr>
```

The tool establishes a TCP connection on port 9999 to the Kasa device specified on the command line by host name, IP address, or via auto-discovery.
In pipe mode, each line that is written to the tool's standard input is sent as a separate request to the connected Kasa device,
and the tool writes each response received from the device as a separate line to its standard output.

**Tip:** You can pipe the output of the tool into [jq](https://stedolan.github.io/jq/) to extract the information that interests you.

### Example in Bash

```bash
#!/bin/bash

if [[ ! -v 1 ]] ; then
	echo 'Toggles the relay state of a Kasa plug/switch.'
	echo
	echo "usage: ${0} <device-addr> [...]"
	exit 1
fi >&2

kasa_call() {
	local - ; set -o pipefail
	echo "${2:?}" | "${KASA:-./kasa}" "${1:?}" |
		jq -r "if [ .[][].err_code != 0 ] | any then halt_error else ${3:-empty} end"
}

for each ; do
	{ read -r alias ; read -r relay_state ; } < <(
		kasa_call "${each}" '{"system":{"get_sysinfo":{}}}' \
			'.system.get_sysinfo | ( .alias, .relay_state )')
	case "${relay_state}" in
		0)
			echo "${alias} is switched off; switching on."
			kasa_call "${each}" '{"system":{"set_relay_state":{"state":1}}}'
			;;
		1)
			echo "${alias} is switched on; switching off."
			kasa_call "${each}" '{"system":{"set_relay_state":{"state":0}}}'
			;;
		*)
			exit "${?}"
			;;
	esac
done
```

## Disclaimer

The information and code contained in this repository is provided “as is,” without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, and non-infringement.
In no event shall the authors or copyright holders be liable for any claim, damages, or other liability, whether in an action of contract, tort, or otherwise, arising from, out of, or in connection with the information and code contained in this repository or the use or other dealings therewith.

The information contained in this repository may be incorrect or incomplete. Any use of what you find here is undertaken solely at your own risk.

Kasa is a [registered trademark](https://uspto.report/TM/87867268) of the TP-Link Research America Corporation.
TP-Link have not endorsed or contributed to this repository and are not affiliated with it in any way.
