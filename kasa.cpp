#include <stdint.h>
#include <stdio.h>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <err.h>
#include <netdb.h>
#include <sysexits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "third_party/nlohmann/json.hpp"

using json = nlohmann::json;

static const size_t kBufSize = 4096;

static void xor171_enc(unsigned char buf[], size_t n) {
	int key = 171;
	for (unsigned char *p = buf, *e = buf + n; p != e; ++p) {
		*p = (key ^= *p);
	}
}

static void xor171_dec(unsigned char buf[], size_t n) {
	int key = 171;
	for (unsigned char *p = buf, *e = buf + n; p != e; ++p) {
		*p ^= key;
		key ^= *p;
	}
}

static void read_fully(int fd, void *buf, size_t n) {
	while (n) {
		ssize_t r = read(fd, buf, n);
		if (r < 0) {
			err(EX_IOERR, "read");
		}
		buf = static_cast<unsigned char *>(buf) + r;
		n -= static_cast<size_t>(r);
	}
}

static void writev_fully(int fd, struct iovec iov[], int iovcnt) {
	for (struct iovec *end = iov + iovcnt; iov != end;) {
		ssize_t w = writev(fd, iov, static_cast<int>(end - iov));
		if (w < 0) {
			err(EX_IOERR, "write");
		}
		for (; w > 0; ++iov) {
			if (iov->iov_len > static_cast<size_t>(w)) {
				iov->iov_base = static_cast<unsigned char *>(iov->iov_base) + w;
				iov->iov_len -= static_cast<size_t>(w);
				break;
			}
			w -= static_cast<ssize_t>(iov->iov_len);
			iov->iov_base = static_cast<unsigned char *>(iov->iov_base) + iov->iov_len;
			iov->iov_len = 0;
		}
	}
}

/** Send one request string, receive one response string. Uses XOR-171 and 4-byte length prefix. */
static std::string send_recv(int sock, const std::string &request) {
	if (request.size() > kBufSize) {
		errx(EX_DATAERR, "request too long");
	}
	std::vector<unsigned char> enc(request.begin(), request.end());
	xor171_enc(enc.data(), enc.size());
	uint32_t len_be = htonl(static_cast<uint32_t>(enc.size()));
	struct iovec iov[2] = {
		{ &len_be, sizeof(len_be) },
		{ enc.data(), enc.size() }
	};
	writev_fully(sock, iov, 2);
	read_fully(sock, &len_be, sizeof(len_be));
	size_t n = ntohl(len_be);
	if (n > kBufSize) {
		errx(EX_PROTOCOL, "response too long");
	}
	std::vector<unsigned char> resp(n);
	read_fully(sock, resp.data(), n);
	xor171_dec(resp.data(), n);
	return std::string(resp.begin(), resp.end());
}

/**
 * Kasa auto-discovery via UDP broadcast (legacy protocol, port 9999).
 * Sends XOR-171-encoded get_sysinfo to 255.255.255.255:9999; returns the IP
 * of the first device that responds, or empty string on timeout.
 */
static std::string discover_kasa_device(void) {
	const char *discovery_msg = "{\"system\":{\"get_sysinfo\":{}}}";
	const unsigned int discovery_timeout_sec = 5;
	std::vector<unsigned char> payload(discovery_msg, discovery_msg + std::strlen(discovery_msg));
	xor171_enc(payload.data(), payload.size());

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return "";
	}
	int on = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
		close(fd);
		return "";
	}
	struct timeval tv = {};
	tv.tv_sec = discovery_timeout_sec;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		close(fd);
		return "";
	}
	struct sockaddr_in broadcast = {};
	broadcast.sin_family = AF_INET;
	broadcast.sin_port = htons(9999);
	if (inet_pton(AF_INET, "255.255.255.255", &broadcast.sin_addr) != 1) {
		close(fd);
		return "";
	}
	if (sendto(fd, payload.data(), payload.size(), 0,
	    reinterpret_cast<struct sockaddr *>(&broadcast), sizeof(broadcast)) < 0) {
		close(fd);
		return "";
	}
	unsigned char buf[kBufSize];
	struct sockaddr_in from = {};
	socklen_t fromlen = sizeof(from);
	ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
	    reinterpret_cast<struct sockaddr *>(&from), &fromlen);
	close(fd);
	if (n <= 0 || static_cast<size_t>(n) > kBufSize) {
		return "";
	}
	xor171_dec(buf, static_cast<size_t>(n));
	char ip[INET_ADDRSTRLEN];
	if (inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)) == nullptr) {
		return "";
	}
	std::string ip_str(ip);
	/* Debug: print discovered IP and metadata from get_sysinfo response */
	fprintf(stderr, "Discovered device at %s\n", ip_str.c_str());
	try {
		json j = json::parse(std::string(reinterpret_cast<const char *>(buf), static_cast<size_t>(n)));
		if (j.contains("system") && j["system"].contains("get_sysinfo")) {
			const json &info = j["system"]["get_sysinfo"];
			if (info.contains("alias") && info["alias"].is_string()) {
				fprintf(stderr, "  alias: %s\n", info["alias"].get<std::string>().c_str());
			}
			if (info.contains("model") && info["model"].is_string()) {
				fprintf(stderr, "  model: %s\n", info["model"].get<std::string>().c_str());
			}
			if (info.contains("deviceId") && info["deviceId"].is_string()) {
				fprintf(stderr, "  deviceId: %s\n", info["deviceId"].get<std::string>().c_str());
			}
			if (info.contains("hw_ver") && info["hw_ver"].is_string()) {
				fprintf(stderr, "  hw_ver: %s\n", info["hw_ver"].get<std::string>().c_str());
			}
			if (info.contains("sw_ver") && info["sw_ver"].is_string()) {
				fprintf(stderr, "  sw_ver: %s\n", info["sw_ver"].get<std::string>().c_str());
			}
			if (info.contains("mic_type") && info["mic_type"].is_string()) {
				fprintf(stderr, "  mic_type: %s\n", info["mic_type"].get<std::string>().c_str());
			}
			if (info.contains("mac") && info["mac"].is_string()) {
				fprintf(stderr, "  mac: %s\n", info["mac"].get<std::string>().c_str());
			}
			if (info.contains("feature") && info["feature"].is_string()) {
				fprintf(stderr, "  feature: %s\n", info["feature"].get<std::string>().c_str());
			}
			if (info.contains("child_num") && info["child_num"].is_number_integer()) {
				fprintf(stderr, "  child_num: %d\n", info["child_num"].get<int>());
			}
		}
	} catch (...) {
		/* non-fatal: discovery succeeded, metadata print failed */
	}
	return ip_str;
}

/** Probe mode: get_sysinfo once for child IDs, then loop get_realtime per child with 5s delay forever. */
static void run_probe_mode(int sock, const char *prog) {
	const std::string get_sysinfo_req = "{\"system\":{\"get_sysinfo\":{}}}";
	std::string raw = send_recv(sock, get_sysinfo_req);
	json j;
	try {
		j = json::parse(raw);
	} catch (const json::parse_error &e) {
		errx(EX_DATAERR, "get_sysinfo: invalid JSON: %s", e.what());
	}
	json *sysinfo = nullptr;
	try {
		if (j.contains("system") && j["system"].contains("get_sysinfo")) {
			sysinfo = &j["system"]["get_sysinfo"];
		}
	} catch (...) {}
	if (!sysinfo || !sysinfo->is_object()) {
		errx(EX_PROTOCOL, "get_sysinfo: missing or invalid system.get_sysinfo");
	}
	json *children = nullptr;
	if (sysinfo->contains("children") && (*sysinfo)["children"].is_array()) {
		children = &(*sysinfo)["children"];
	}
	if (!children || children->empty()) {
		fprintf(stderr, "%s: no child outlets (single plug or unsupported device)\n", prog);
		return;
	}
	std::vector<std::pair<std::string, std::string>> child_list;
	for (auto &c : *children) {
		if (!c.is_object() || !c.contains("id") || !c["id"].is_string()) {
			continue;
		}
		std::string id = c["id"].get<std::string>();
		std::string alias;
		if (c.contains("alias") && c["alias"].is_string()) {
			alias = c["alias"].get<std::string>();
		}
		child_list.push_back({id, alias});
	}
	if (child_list.empty()) {
		fprintf(stderr, "%s: no valid child ids in get_sysinfo\n", prog);
		return;
	}
	for (;;) {
		for (size_t i = 0; i < child_list.size(); ++i) {
			const std::string &child_id = child_list[i].first;
			const std::string &alias = child_list[i].second;
			json req = {
				{"context", {{"child_ids", json::array({child_id})}}},
				{"emeter", {{"get_realtime", json::object()}}}
			};
			std::string req_str = req.dump();
			std::string resp_str = send_recv(sock, req_str);
			json rj;
			try {
				rj = json::parse(resp_str);
			} catch (const json::parse_error &e) {
				fprintf(stderr, "get_realtime: invalid JSON for child %s: %s\n", child_id.c_str(), e.what());
				if (i + 1 < child_list.size()) {
					sleep(1);
				}
				continue;
			}
			int slot_id = -1;
			int current_ma = 0, voltage_mv = 0, power_mw = 0, total_wh = 0, err_code = 0;
			if (rj.contains("emeter") && rj["emeter"].contains("get_realtime")) {
				json &rt = rj["emeter"]["get_realtime"];
				if (rt.contains("err_code") && rt["err_code"].is_number_integer()) {
					err_code = rt["err_code"].get<int>();
				}
				if (rt.contains("slot_id") && rt["slot_id"].is_number_integer()) {
					slot_id = rt["slot_id"].get<int>();
				}
				if (rt.contains("current_ma") && rt["current_ma"].is_number_integer()) {
					current_ma = rt["current_ma"].get<int>();
				}
				if (rt.contains("voltage_mv") && rt["voltage_mv"].is_number_integer()) {
					voltage_mv = rt["voltage_mv"].get<int>();
				}
				if (rt.contains("power_mw") && rt["power_mw"].is_number_integer()) {
					power_mw = rt["power_mw"].get<int>();
				}
				if (rt.contains("total_wh") && rt["total_wh"].is_number_integer()) {
					total_wh = rt["total_wh"].get<int>();
				}
			}
			const char *alias_str = alias.empty() ? "" : alias.c_str();
			time_t now = time(nullptr);
			struct tm *tm = localtime(&now);
			char tbuf[32];
			strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
			/* Fixed-width numeric fields for column alignment: current_ma=5, voltage_mv=6, power_mw=6, total_wh=6 */
			if (slot_id >= 0) {
				if (alias_str[0]) {
					printf("[%s] Slot %2d (%-8s): current_ma=%5d voltage_mv=%6d power_mw=%6d total_wh=%6d", tbuf, slot_id, alias_str, current_ma, voltage_mv, power_mw, total_wh);
				} else {
					printf("[%s] Slot %2d (%-8s): current_ma=%5d voltage_mv=%6d power_mw=%6d total_wh=%6d", tbuf, slot_id, "", current_ma, voltage_mv, power_mw, total_wh);
				}
			} else {
				printf("[%s] Child %-12s (%-8s): current_ma=%5d voltage_mv=%6d power_mw=%6d total_wh=%6d", tbuf, child_id.c_str(), alias_str, current_ma, voltage_mv, power_mw, total_wh);
			}
			if (err_code != 0) {
				printf(" err_code=%d", err_code);
			}
			printf("\n");
			if (i + 1 < child_list.size()) {
				sleep(1);
			}
		}
	}
}

static void run_pipe_mode(int sock) {
	unsigned char buf[kBufSize];
	unsigned char *p = buf;
	for (;;) {
		int c = getchar();
		if (c == EOF) {
			if (p != buf) {
				errx(EX_IOERR, "incomplete line at EOF");
			}
			return;
		}
		if (c != '\n') {
			if (p == buf + sizeof(buf)) {
				errx(EX_DATAERR, "line too long");
			}
			*p++ = static_cast<unsigned char>(c);
		} else if (p != buf) {
			std::string line(reinterpret_cast<char *>(buf), static_cast<size_t>(p - buf));
			std::string resp = send_recv(sock, line);
			printf("%s\n", resp.c_str());
			p = buf;
		}
	}
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <host|discover|auto>\n", argc > 0 ? argv[0] : "kasa");
		return EX_USAGE;
	}

	const char *host = argv[1];
	std::string discovered;
	if (std::strcmp(argv[1], "discover") == 0 || std::strcmp(argv[1], "auto") == 0) {
		discovered = discover_kasa_device();
		if (discovered.empty()) {
			errx(EX_UNAVAILABLE, "no Kasa device found on LAN");
		}
		host = discovered.c_str();
	}

	struct addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *info = nullptr;
	struct addrinfo *pai = nullptr;
	int e;
	if ((e = getaddrinfo(host, nullptr, &hints, &info)) != 0) {
		if (e == EAI_SYSTEM) {
			err(EX_OSERR, "getaddrinfo: %s", host);
		}
		fprintf(stderr, "%s: %s: %s: %s\n", argv[0], "getaddrinfo", host, gai_strerror(e));
		return EX_NOHOST;
	}

	int sock = -1;
	for (pai = info; pai != nullptr; pai = pai->ai_next) {
		if ((sock = socket(pai->ai_family, pai->ai_socktype, pai->ai_protocol)) < 0) {
			err(EX_OSERR, "socket");
		}
		(reinterpret_cast<struct sockaddr_in *>(pai->ai_addr))->sin_port = htons(9999);
		if (connect(sock, pai->ai_addr, pai->ai_addrlen) == 0) {
			goto connected;
		}
		if (close(sock) < 0) {
			err(EX_OSERR, "close");
		}
	}
	err(EX_UNAVAILABLE, "connect");

connected:
	freeaddrinfo(info);

	struct timeval timeout = {};
	timeout.tv_sec = 15;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		err(EX_OSERR, "setsockopt");
	}

	if (isatty(STDIN_FILENO)) {
		run_probe_mode(sock, argv[0]);
	} else {
		run_pipe_mode(sock);
	}
	return EX_OK;
}
