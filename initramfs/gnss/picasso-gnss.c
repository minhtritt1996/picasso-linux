/* Persistent QMI LOC (service 16) GNSS client over AF_QIPCRTR for picasso.
 *
 * The modem's QMI LOC service drops its client and halts tracking whenever the
 * owning QRTR socket closes, so a single long-lived process must allocate the
 * socket, configure the engine, start periodic fixes, and consume indications.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#define QMI_SERVICE_LOC          16
#define QMI_FLAG_REQUEST         0x00
#define QMI_FLAG_RESPONSE        0x02
#define QMI_FLAG_INDICATION      0x04

#define QMI_LOC_REGISTER_EVENTS  0x0021
#define QMI_LOC_START            0x0022
#define QMI_LOC_STOP             0x0023
#define QMI_LOC_POSITION_REPORT  0x0024
#define QMI_LOC_GNSS_SV_INFO     0x0025
#define QMI_LOC_NMEA             0x0026
#define QMI_LOC_ENGINE_STATE     0x002B
#define QMI_LOC_FIX_SESSION      0x002C
#define QMI_LOC_INJECT_UTC_TIME  0x0038
#define QMI_LOC_SET_ENGINE_LOCK  0x003A
#define QMI_LOC_SET_NMEA_TYPES   0x003E
#define QMI_LOC_SET_OP_MODE      0x004A

/* Register POSITION_REPORT | GNSS_SV_INFO | NMEA | ENGINE_STATE | FIX_SESSION */
#define LOC_EVENT_MASK           0x00000187ULL
#define LOC_LOCK_NONE            1U
#define LOC_OP_MODE_STANDALONE   4U
/* Standard NMEA sentences (0xFFFF sets vendor bits that suppress NMEA output). */
#define LOC_NMEA_TYPES_STD       0x0000007FU
#define LOC_RECURRENCE_PERIODIC  1U
#define LOC_INTERMEDIATE_ENABLE  1U
#define LOC_SESSION_ID           1U
#define LOC_MIN_INTERVAL_MS      1000U

#define NMEA_MAX_BYTES           (128U * 1024U)
#define DEFAULT_NMEA_PATH        "/run/gnss.nmea"
#define DEFAULT_STATUS_PATH      "/run/gnss.status"

struct qmi_hdr {
	uint8_t flags;
	uint16_t txn;
	uint16_t msg_id;
	uint16_t tlv_len;
} __attribute__((packed));

struct gnss_state {
	uint32_t engine_state;
	uint32_t session_state;
	uint32_t pos_status;
	int have_pos;
	double lat;
	double lon;
	float alt_msl;
	float horiz_unc;
	uint64_t utc_ms;
	uint32_t sv_count;
	uint64_t nmea_count;
	char last_nmea[160];
};

static volatile sig_atomic_t stop_requested;
static const char *nmea_path = DEFAULT_NMEA_PATH;
static const char *status_path = DEFAULT_STATUS_PATH;
static struct gnss_state g_state;
static uint64_t g_last_ind_ms;
static uint64_t g_restart_at_ms;
static int g_need_restart;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
	uint64_t lo = get_le32(p);
	uint64_t hi = get_le32(p + 4);
	return lo | (hi << 32);
}

static float get_f32(const uint8_t *p)
{
	uint32_t v = get_le32(p);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

static double get_f64(const uint8_t *p)
{
	uint64_t v = get_le64(p);
	double d;
	memcpy(&d, &v, sizeof(d));
	return d;
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)(v & 0xffffffffU));
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static size_t tlv_add(uint8_t *buf, size_t cap, size_t off, uint8_t type,
		      const void *val, uint16_t len)
{
	if (off + 3 + len > cap)
		return off;
	buf[off] = type;
	put_le16(buf + off + 1, len);
	if (len > 0 && val)
		memcpy(buf + off + 3, val, len);
	return off + 3 + len;
}

static int tlv_find(const uint8_t *tlvs, size_t tlv_len, uint8_t wanted,
		    const uint8_t **out_val, uint16_t *out_len)
{
	size_t i = 0;

	while (i + 3 <= tlv_len) {
		uint8_t t = tlvs[i];
		uint16_t l = get_le16(tlvs + i + 1);

		i += 3;
		if (i + l > tlv_len)
			break;
		if (t == wanted) {
			*out_val = tlvs + i;
			*out_len = l;
			return 0;
		}
		i += l;
	}
	return -1;
}

static uint64_t now_utc_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static uint64_t now_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void write_status_file(void)
{
	char tmp[256];
	FILE *f;

	if (!status_path || !status_path[0])
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", status_path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f,
		"engine_state=%u\n"
		"session_state=%u\n"
		"position_status=%u\n"
		"have_fix=%d\n"
		"latitude=%.7f\n"
		"longitude=%.7f\n"
		"altitude_msl_m=%.2f\n"
		"horiz_unc_m=%.2f\n"
		"sv_count=%u\n"
		"utc_ms=%llu\n"
		"nmea_lines=%llu\n"
		"last_nmea=%s\n",
		g_state.engine_state,
		g_state.session_state,
		g_state.pos_status,
		(g_state.have_pos && g_state.pos_status == 0) ? 1 : 0,
		g_state.lat,
		g_state.lon,
		g_state.alt_msl,
		g_state.horiz_unc,
		g_state.sv_count,
		(unsigned long long)g_state.utc_ms,
		(unsigned long long)g_state.nmea_count,
		g_state.last_nmea);
	fclose(f);
	(void)rename(tmp, status_path);
}

static void append_nmea_line(const char *line, size_t len)
{
	struct stat st;
	FILE *f;

	while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
		len--;
	if (len == 0)
		return;

	g_state.nmea_count++;
	{
		size_t copy = len < sizeof(g_state.last_nmea) - 1 ?
			      len : sizeof(g_state.last_nmea) - 1;
		memcpy(g_state.last_nmea, line, copy);
		g_state.last_nmea[copy] = '\0';
	}

	if (nmea_path && nmea_path[0]) {
		if (stat(nmea_path, &st) == 0 && (size_t)st.st_size >= NMEA_MAX_BYTES) {
			char old_path[256];
			snprintf(old_path, sizeof(old_path), "%s.1", nmea_path);
			(void)rename(nmea_path, old_path);
		}
		f = fopen(nmea_path, "a");
		if (f) {
			fwrite(line, 1, len, f);
			fputc('\n', f);
			fclose(f);
		}
	}

	/* Print the first batch and periodic GGA/RMC/GSV lines to journald/stdout. */
	if (g_state.nmea_count <= 8 ||
	    strncmp(g_state.last_nmea, "$GPGGA", 6) == 0 ||
	    strncmp(g_state.last_nmea, "$GNGGA", 6) == 0 ||
	    strncmp(g_state.last_nmea, "$GPGSV", 6) == 0) {
		printf("nmea: %.*s\n", (int)len, line);
		fflush(stdout);
	}
}

static void handle_position_report(const uint8_t *tlvs, size_t tlv_len)
{
	const uint8_t *v;
	uint16_t l;
	uint32_t prev_status = g_state.pos_status;

	if (tlv_find(tlvs, tlv_len, 0x01, &v, &l) == 0 && l >= 4)
		g_state.pos_status = get_le32(v);
	if (tlv_find(tlvs, tlv_len, 0x10, &v, &l) == 0 && l == 8) {
		g_state.lat = get_f64(v);
		g_state.have_pos = 1;
	}
	if (tlv_find(tlvs, tlv_len, 0x11, &v, &l) == 0 && l == 8)
		g_state.lon = get_f64(v);
	if (tlv_find(tlvs, tlv_len, 0x12, &v, &l) == 0 && l == 4)
		g_state.horiz_unc = get_f32(v);
	if (tlv_find(tlvs, tlv_len, 0x1B, &v, &l) == 0 && l == 4)
		g_state.alt_msl = get_f32(v);
	if (tlv_find(tlvs, tlv_len, 0x25, &v, &l) == 0 && l == 8)
		g_state.utc_ms = get_le64(v);

	if (g_state.pos_status == 0 && prev_status != 0) {
		printf("fix: lat=%.7f lon=%.7f alt=%.1fm unc=%.1fm\n",
		       g_state.lat, g_state.lon, g_state.alt_msl, g_state.horiz_unc);
		fflush(stdout);
	} else if (g_state.pos_status >= 2 && prev_status != g_state.pos_status &&
		   !stop_requested) {
		/* Cold-start timeout or failure indoors: schedule a session restart. */
		printf("session ended (pos_status=%u), scheduling restart\n",
		       g_state.pos_status);
		fflush(stdout);
		g_need_restart = 1;
		g_restart_at_ms = now_mono_ms() + 2000ULL;
	}
	write_status_file();
}

static void handle_sv_info(const uint8_t *tlvs, size_t tlv_len)
{
	const uint8_t *v;
	uint16_t l;
	uint32_t prev = g_state.sv_count;

	if (tlv_find(tlvs, tlv_len, 0x10, &v, &l) == 0 && l >= 1)
		g_state.sv_count = v[0];
	else
		g_state.sv_count = 0;

	if (g_state.sv_count != prev) {
		if (g_state.sv_count > 0 && l >= 1 + 28) {
			uint32_t sys = get_le32(v + 1 + 4);
			uint16_t svid = get_le16(v + 1 + 8);
			float snr = get_f32(v + 1 + 24);
			const char *sname = sys == 1 ? "GPS" :
					    sys == 2 ? "GLO" :
					    sys == 3 ? "SBAS" :
					    sys == 4 ? "BDS" :
					    sys == 5 ? "GAL" :
					    sys == 6 ? "QZSS" : "GNSS";
			printf("sv_info: count=%u [%s:%u snr=%.1f]\n",
			       g_state.sv_count, sname, svid, snr);
		} else {
			printf("sv_info: count=%u\n", g_state.sv_count);
		}
		fflush(stdout);
	}
	write_status_file();
}

static void handle_indication(uint16_t msg_id, const uint8_t *tlvs, size_t tlv_len)
{
	const uint8_t *v;
	uint16_t l;

	g_last_ind_ms = now_mono_ms();

	switch (msg_id) {
	case QMI_LOC_NMEA:
		if (tlv_find(tlvs, tlv_len, 0x01, &v, &l) == 0 && l > 0) {
			append_nmea_line((const char *)v, l);
			write_status_file();
		}
		break;
	case QMI_LOC_POSITION_REPORT:
		handle_position_report(tlvs, tlv_len);
		break;
	case QMI_LOC_GNSS_SV_INFO:
		handle_sv_info(tlvs, tlv_len);
		break;
	case QMI_LOC_ENGINE_STATE:
		if (tlv_find(tlvs, tlv_len, 0x01, &v, &l) == 0 && l >= 4) {
			g_state.engine_state = get_le32(v);
			printf("engine_state: %s (%u)\n",
			       g_state.engine_state == 1 ? "ON" :
			       g_state.engine_state == 2 ? "OFF" : "UNKNOWN",
			       g_state.engine_state);
			fflush(stdout);
			if (g_state.engine_state == 2 && !stop_requested) {
				g_need_restart = 1;
				g_restart_at_ms = now_mono_ms() + 2000ULL;
			}
			write_status_file();
		}
		break;
	case QMI_LOC_FIX_SESSION:
		if (tlv_find(tlvs, tlv_len, 0x01, &v, &l) == 0 && l >= 4) {
			g_state.session_state = get_le32(v);
			printf("session_state: %s (%u)\n",
			       g_state.session_state == 1 ? "STARTED" :
			       g_state.session_state == 2 ? "ENDED" : "UNKNOWN",
			       g_state.session_state);
			fflush(stdout);
			if (g_state.session_state == 2 && !stop_requested) {
				g_need_restart = 1;
				g_restart_at_ms = now_mono_ms() + 2000ULL;
			}
			write_status_file();
		}
		break;
	default:
		break;
	}
}

static int qrtr_lookup_loc(uint32_t *out_node, uint32_t *out_port)
{
	struct sockaddr_qrtr local, ctrl;
	struct qrtr_ctrl_pkt req, resp;
	socklen_t slen = sizeof(local);
	struct pollfd pfd;
	int fd, ret = -1;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	memset(&local, 0, sizeof(local));
	if (getsockname(fd, (struct sockaddr *)&local, &slen) < 0)
		local.sq_node = 1;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.sq_family = AF_QIPCRTR;
	ctrl.sq_node = local.sq_node;
	ctrl.sq_port = QRTR_PORT_CTRL;

	memset(&req, 0, sizeof(req));
	req.cmd = QRTR_TYPE_NEW_LOOKUP;
	req.server.service = QMI_SERVICE_LOC;
	req.server.instance = 0;

	if (sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0)
		goto out;

	pfd.fd = fd;
	pfd.events = POLLIN;
	while (poll(&pfd, 1, 1500) > 0) {
		ssize_t n = recv(fd, &resp, sizeof(resp), 0);

		if (n < (ssize_t)sizeof(resp))
			break;
		if (resp.cmd != QRTR_TYPE_NEW_SERVER)
			continue;
		if (resp.server.node == 0 && resp.server.port == 0)
			break;
		if (resp.server.service == QMI_SERVICE_LOC) {
			*out_node = resp.server.node;
			*out_port = resp.server.port;
			ret = 0;
			break;
		}
	}
out:
	close(fd);
	return ret;
}

static int qmi_send_and_wait(int fd, uint16_t *txn_seq, uint16_t msg_id,
			     const uint8_t *tlvs, size_t tlv_len,
			     int wait_ind, int timeout_ms)
{
	uint8_t pkt[128];
	struct qmi_hdr hdr;
	uint16_t txn = ++(*txn_seq);
	int got_resp = 0, got_ind = !wait_ind;
	uint64_t deadline = now_mono_ms() + (uint64_t)timeout_ms;

	if (sizeof(hdr) + tlv_len > sizeof(pkt))
		return -1;

	hdr.flags = QMI_FLAG_REQUEST;
	hdr.txn = txn;
	hdr.msg_id = msg_id;
	hdr.tlv_len = (uint16_t)tlv_len;
	memcpy(pkt, &hdr, sizeof(hdr));
	if (tlv_len > 0 && tlvs)
		memcpy(pkt + sizeof(hdr), tlvs, tlv_len);

	if (send(fd, pkt, sizeof(hdr) + tlv_len, 0) < 0)
		return -1;

	while (!got_resp || !got_ind) {
		uint64_t now = now_mono_ms();
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int rem = (now < deadline) ? (int)(deadline - now) : 0;
		uint8_t rx[4096];
		ssize_t n;

		if (rem <= 0 || poll(&pfd, 1, rem) <= 0)
			return -1;
		n = recv(fd, rx, sizeof(rx), 0);
		if (n < (ssize_t)sizeof(hdr))
			continue;
		memcpy(&hdr, rx, sizeof(hdr));
		if ((size_t)n < sizeof(hdr) + hdr.tlv_len)
			continue;

		const uint8_t *rx_tlvs = rx + sizeof(hdr);
		const uint8_t *v;
		uint16_t l;

		if (hdr.flags == QMI_FLAG_RESPONSE &&
		    hdr.msg_id == msg_id && hdr.txn == txn) {
			if (tlv_find(rx_tlvs, hdr.tlv_len, 0x02, &v, &l) == 0 && l >= 4) {
				uint16_t res = get_le16(v);
				uint16_t err = get_le16(v + 2);
				if (res != 0) {
					fprintf(stderr, "qmi msg 0x%04x resp error (%u, %u)\n",
						msg_id, res, err);
					return -1;
				}
			}
			got_resp = 1;
		} else if (hdr.flags == QMI_FLAG_INDICATION) {
			if (wait_ind && hdr.msg_id == msg_id) {
				if (tlv_find(rx_tlvs, hdr.tlv_len, 0x01, &v, &l) == 0 && l >= 4) {
					uint32_t st = get_le32(v);
					if (st != 0) {
						fprintf(stderr, "qmi msg 0x%04x ind status=%u\n",
							msg_id, st);
						return -1;
					}
				}
				got_ind = 1;
			} else {
				handle_indication(hdr.msg_id, rx_tlvs, hdr.tlv_len);
			}
		}
	}
	return 0;
}

static int clock_is_synced(void)
{
	struct timex tx;

	memset(&tx, 0, sizeof(tx));
	if (adjtimex(&tx) < 0)
		return 0;
	return !(tx.status & STA_UNSYNC);
}

static int inject_utc_and_start(int fd, uint16_t *txn, int allow_utc_inject)
{
	uint8_t buf[64], u8;
	uint8_t u32[4], u64[8];
	size_t off;

	/* Only inject UTC when synced: a stale pre-NTP boot clock poisons SV search. */
	if (allow_utc_inject) {
		put_le64(u64, now_utc_ms());
		off = tlv_add(buf, sizeof(buf), 0, 0x01, u64, 8);
		put_le32(u32, 1000U);
		off = tlv_add(buf, sizeof(buf), off, 0x02, u32, 4);
		(void)qmi_send_and_wait(fd, txn, QMI_LOC_INJECT_UTC_TIME,
					buf, off, 1, 2000);
	}

	/* Start periodic tracking session 1 (1 Hz) */
	u8 = LOC_SESSION_ID;
	off = tlv_add(buf, sizeof(buf), 0, 0x01, &u8, 1);
	put_le32(u32, LOC_RECURRENCE_PERIODIC);
	off = tlv_add(buf, sizeof(buf), off, 0x10, u32, 4);
	put_le32(u32, LOC_INTERMEDIATE_ENABLE);
	off = tlv_add(buf, sizeof(buf), off, 0x12, u32, 4);
	put_le32(u32, LOC_MIN_INTERVAL_MS);
	off = tlv_add(buf, sizeof(buf), off, 0x13, u32, 4);
	if (qmi_send_and_wait(fd, txn, QMI_LOC_START, buf, off, 0, 3000) < 0)
		return -1;

	g_last_ind_ms = now_mono_ms();
	g_need_restart = 0;
	return 0;
}

static int configure_and_start(int fd, uint16_t *txn)
{
	uint8_t buf[64];
	uint8_t u32[4], u64[8];
	size_t off;

	/* 1. Register events */
	put_le64(u64, LOC_EVENT_MASK);
	off = tlv_add(buf, sizeof(buf), 0, 0x01, u64, 8);
	if (qmi_send_and_wait(fd, txn, QMI_LOC_REGISTER_EVENTS, buf, off, 0, 2000) < 0)
		return -1;

	/* 2. Unlock engine (modem boots with lock=3, which rejects START) */
	put_le32(u32, LOC_LOCK_NONE);
	off = tlv_add(buf, sizeof(buf), 0, 0x01, u32, 4);
	if (qmi_send_and_wait(fd, txn, QMI_LOC_SET_ENGINE_LOCK, buf, off, 1, 2000) < 0)
		return -1;

	/* 3. Set standalone operation mode */
	put_le32(u32, LOC_OP_MODE_STANDALONE);
	off = tlv_add(buf, sizeof(buf), 0, 0x01, u32, 4);
	if (qmi_send_and_wait(fd, txn, QMI_LOC_SET_OP_MODE, buf, off, 1, 2000) < 0)
		return -1;

	/* 4. Set standard NMEA sentence mask */
	put_le32(u32, LOC_NMEA_TYPES_STD);
	off = tlv_add(buf, sizeof(buf), 0, 0x01, u32, 4);
	if (qmi_send_and_wait(fd, txn, QMI_LOC_SET_NMEA_TYPES, buf, off, 1, 2000) < 0)
		return -1;

	return inject_utc_and_start(fd, txn, clock_is_synced());
}

int main(int argc, char **argv)
{
	unsigned long duration_sec = 0;
	uint32_t loc_node = 0, loc_port = 0;
	struct sockaddr_qrtr sa;
	struct sigaction act;
	uint64_t end_ms = 0;
	uint16_t txn = 0;
	int fd, i;

	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) &&
		    i + 1 < argc) {
			duration_sec = strtoul(argv[++i], NULL, 10);
		} else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--nmea-file") == 0) &&
			   i + 1 < argc) {
			nmea_path = argv[++i];
		} else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--status-file") == 0) &&
			   i + 1 < argc) {
			status_path = argv[++i];
		} else {
			fprintf(stderr, "usage: %s [-d sec] [-o nmea_file] [-s status_file]\n",
				argv[0]);
			return 2;
		}
	}

	memset(&act, 0, sizeof(act));
	act.sa_handler = on_signal;
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	while (!stop_requested) {
		if (qrtr_lookup_loc(&loc_node, &loc_port) == 0)
			break;
		fprintf(stderr, "waiting for QMI LOC service (16) on QRTR...\n");
		sleep(1);
	}
	if (stop_requested)
		return 0;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket(AF_QIPCRTR)");
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sq_family = AF_QIPCRTR;
	sa.sq_node = loc_node;
	sa.sq_port = loc_port;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("connect(QMI_LOC)");
		close(fd);
		return 1;
	}

	printf("connected to QMI LOC at qrtr %u:%u\n", loc_node, loc_port);
	fflush(stdout);

	if (configure_and_start(fd, &txn) < 0) {
		fprintf(stderr, "failed to configure/start QMI LOC session\n");
		close(fd);
		return 1;
	}

	if (duration_sec > 0)
		end_ms = now_mono_ms() + duration_sec * 1000ULL;

	{
		uint64_t prev_mono = now_mono_ms();
		uint64_t prev_utc = now_utc_ms();

		g_last_ind_ms = prev_mono;
		while (!stop_requested) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			int timeout_ms = 1000;
			uint8_t rx[4096];
			struct qmi_hdr hdr;
			uint64_t now = now_mono_ms();
			uint64_t utc = now_utc_ms();
			int64_t mono_d = (int64_t)(now - prev_mono);
			int64_t utc_d = (int64_t)(utc - prev_utc);
			int64_t drift = utc_d - mono_d;
			ssize_t n;

			prev_mono = now;
			prev_utc = utc;

			/* Re-inject UTC if NTP stepped the system clock by > 5 s */
			if (drift > 5000 || drift < -5000) {
				uint8_t tbuf[32], tu64[8], tu32[4];
				size_t toff;

				printf("clock step detected (%lld ms), re-injecting UTC\n",
				       (long long)drift);
				fflush(stdout);
				put_le64(tu64, utc);
				toff = tlv_add(tbuf, sizeof(tbuf), 0, 0x01, tu64, 8);
				put_le32(tu32, 1000U);
				toff = tlv_add(tbuf, sizeof(tbuf), toff, 0x02, tu32, 4);
				(void)qmi_send_and_wait(fd, &txn, QMI_LOC_INJECT_UTC_TIME,
							tbuf, toff, 1, 2000);
			}

			if (!g_need_restart && (now - g_last_ind_ms) >= 30000ULL) {
				printf("no GNSS indications for 30 s, restarting session\n");
				fflush(stdout);
				g_need_restart = 1;
				g_restart_at_ms = now;
			}
			if (g_need_restart && now >= g_restart_at_ms) {
				if (inject_utc_and_start(fd, &txn, clock_is_synced()) < 0)
					g_restart_at_ms = now + 5000ULL;
			}

			if (end_ms > 0) {
				if (now >= end_ms)
					break;
				if ((end_ms - now) < (uint64_t)timeout_ms)
					timeout_ms = (int)(end_ms - now);
			}

			if (poll(&pfd, 1, timeout_ms) <= 0)
				continue;

			n = recv(fd, rx, sizeof(rx), 0);
			if (n < (ssize_t)sizeof(hdr))
				continue;
			memcpy(&hdr, rx, sizeof(hdr));
			if ((size_t)n < sizeof(hdr) + hdr.tlv_len)
				continue;
			if (hdr.flags == QMI_FLAG_INDICATION)
				handle_indication(hdr.msg_id, rx + sizeof(hdr), hdr.tlv_len);
		}
	}

	/* Cleanly stop tracking before closing the QRTR socket. */
	{
		uint8_t buf[16], sid = LOC_SESSION_ID;
		size_t off = tlv_add(buf, sizeof(buf), 0, 0x01, &sid, 1);
		(void)qmi_send_and_wait(fd, &txn, QMI_LOC_STOP, buf, off, 0, 500);
		g_state.engine_state = 2;
		g_state.session_state = 2;
		write_status_file();
	}
	close(fd);
	return 0;
}
