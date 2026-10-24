// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GATT server for the LS1021A-IoT: board temperatures, voltages, currents,
 * the fan, the switch ports and the CPLD LED0 over Bluetooth LE.
 *
 * Based on tools/btgatt-server.c from BlueZ, Copyright (C) 2014 Google Inc.
 *
 * Services:
 *  - Environmental Sensing (0x181a): the three temperatures (0x2a6e, sint16
 *    in 0.01 C), read only
 *  - Spartan (7370a000-...), every value as UTF-8 text with a Presentation
 *    Format descriptor, so any client shows it as it is:
 *      01 LED0, write 0/1/2 (or off/on/blink)
 *      02 a summary of everything, read only (Read Blob past the MTU)
 *      10-12 temperatures: LS1021A die (ADT7461 remote), ADT7461, LM75A
 *      20-21 12 V input and 1.0 V rail: V, A and W from the INA220
 *      30 fan, on while the ADT7461 THERM output is asserted
 *      40-43 switch ports ETH2-ETH5, write 0/1 (or off/on) to power the
 *            PHY down or up
 *    10-43 notify, when the value changes
 *
 * It owns the LE ATT channel, so bluetoothd must not be running. It
 * advertises as connectable, serves one central and exits when it
 * disconnects; the init script respawns it. With -e, it runs a program
 * as "<program> connected <address>" and "<program> disconnected".
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/hci.h"
#include "bluetooth/hci_lib.h"
#include "bluetooth/l2cap.h"
#include "bluetooth/uuid.h"

#include "src/shared/mainloop.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/timeout.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"

#define ATT_CID			4

#define UUID_GAP		0x1800
#define UUID_GATT		0x1801
#define UUID_ESS		0x181a
#define UUID_TEMPERATURE	0x2a6e
#define UUID_USER_DESC		0x2901
#define UUID_PRES_FORMAT	0x2904

/* Characteristic Presentation Format */
#define FORMAT_SINT16		0x0e
#define FORMAT_UTF8		0x19
#define UNIT_NONE		0x2700
#define UNIT_CELSIUS		0x272f

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#endif

/* 7370a0xx-4c53-3130-3231-412d496f5400: "sp" "LS1021A-IoT" */
#define SPARTAN_UUID(x)	"7370a0" x "-4c53-3130-3231-412d496f5400"

#define LED_PATH	"/sys/class/leds/ls1021aiot:green:led0"
#define NOTIFY_MS	1000
#define PING_MS		5000

static const char *dev_name = "spartan";
static bool verbose;
static int kick_dd = -1;
static const char *event_prog;
static bool connected;

/*
 * The KW41Z hangs on ATT PDUs longer than the default ATT_MTU of 23, so
 * values fit a notification (20 bytes) and longer texts are read in
 * parts with Read Blob.
 */
#define TEXT_MAX	21	/* a value */
#define LABEL_MAX	64	/* "label: value", what clients get */

struct notifier {
	uint16_t handle;
	bool enabled;
	int arg;
	const char *label;
	bool (*read)(int arg, char *buf, size_t *len);
	char last[LABEL_MAX];
	size_t last_len;
};

enum {
	N_TEMP,				/* 3 temperatures */
	N_RAIL = N_TEMP + 3,		/* 2 INA220 */
	N_FAN = N_RAIL + 2,
	N_PORT,				/* 4 switch ports */
	N_MAX = N_PORT + 4
};

struct server {
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_server *gatt;
	struct notifier notify[N_MAX];
	char summary[512];
	size_t summary_len;
	uint16_t svc_chngd_handle;
	bool svc_chngd_sent;
	unsigned int timeout_id;
	unsigned int ping_id;
};

/*
 * Event program. A disconnection ends this run, and procd then kills every
 * process left in the cgroup of the service: wait for the program, which
 * waits for its sound. Otherwise run it in the background.
 */
static void event(const char *what, const char *arg)
{
	bool last = !strcmp(what, "disconnected");
	pid_t pid;

	if (!event_prog)
		return;
	if (last) {
		if (!connected)
			return;
		connected = false;
	}

	pid = fork();
	if (pid == 0) {
		/* fork twice so that init reaps a program left running */
		if (last || fork() == 0) {
			/* do not keep the HCI and L2CAP sockets open */
			for (int fd = 3; fd < 1024; fd++)
				close(fd);
			execl(event_prog, event_prog, what, arg, (char *) NULL);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* hwmon */

static bool hwmon_find(const char *name, const char *dev, char *path,
		       size_t len)
{
	char buf[PATH_MAX], link[PATH_MAX];
	struct dirent *de;
	bool found = false;
	DIR *d;

	d = opendir("/sys/class/hwmon");
	if (!d)
		return false;

	while (!found && (de = readdir(d))) {
		FILE *f;
		ssize_t n;

		if (de->d_name[0] == '.')
			continue;

		snprintf(buf, sizeof(buf), "/sys/class/hwmon/%s/name",
			 de->d_name);
		f = fopen(buf, "r");
		if (!f)
			continue;
		if (fgets(link, sizeof(link), f) &&
		    !strncmp(link, name, strlen(name))) {
			snprintf(buf, sizeof(buf), "/sys/class/hwmon/%s/device",
				 de->d_name);
			n = readlink(buf, link, sizeof(link) - 1);
			if (n > 0) {
				link[n] = '\0';
				if (!dev || strstr(link, dev)) {
					snprintf(path, len, "/sys/class/hwmon/%s",
						 de->d_name);
					found = true;
				}
			}
		}
		fclose(f);
	}

	closedir(d);
	return found;
}

static bool read_long(const char *path, long *val)
{
	FILE *f = fopen(path, "r");
	bool ok;

	if (!f)
		return false;
	ok = fscanf(f, "%ld", val) == 1;
	fclose(f);
	return ok;
}

static bool write_str(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	bool ok;

	if (!f)
		return false;
	ok = fputs(val, f) >= 0;
	return fclose(f) == 0 && ok;
}

static bool read_hwmon(const char *name, const char *dev, const char *attr,
		       long *val)
{
	char path[PATH_MAX];
	size_t n;

	if (!hwmon_find(name, dev, path, sizeof(path)))
		return false;
	n = strlen(path);
	snprintf(path + n, sizeof(path) - n, "/%s", attr);
	return read_long(path, val);
}

/*
 * Values as UTF-8 text, so any GATT client shows them as they are, at
 * most TEXT_MAX - 1 bytes.
 */
#define DEG	"\xc2\xb0" "C"

static const struct {
	const char *name, *attr, *label, *desc;
} temps[] = {
	{ "adt7461", "temp2_input", "Die LS1021A", "Temperatura do die LS1021A" },
	{ "adt7461", "temp1_input", "Placa ADT7461",
	  "Temperatura da placa (ADT7461)" },
	{ "lm75", "temp1_input", "Placa LM75A", "Temperatura da placa (LM75A)" },
};

static const struct {
	const char *dev, *vfmt, *label, *desc;
} rails[] = {
	{ "0-0045", "%.2fV", "Entrada 12 V", "Entrada 12 V (INA220 0x45)" },
	{ "0-0044", "%.3fV", "Trilho 1,0 V", "Trilho 1,0 V (INA220 0x44)" },
};

/*
 * The switch ports in front panel order: the DSA network interface of
 * each LAN jack
 */
static const struct {
	const char *ifname;
	const char *jack;
} ports[] = {
	{ "lan2", "ETH2" }, { "lan3", "ETH3" }, { "lan4", "ETH4" },
	{ "lan5", "ETH5" },
};

static bool temp_text(int i, char *buf, size_t *len)
{
	long mc;

	if (!read_hwmon(temps[i].name, NULL, temps[i].attr, &mc))
		return false;
	*len = snprintf(buf, TEXT_MAX, "%.1f " DEG, mc / 1000.0);
	return true;
}

/* GATT Temperature format, sint16 in 0.01 C, for the standard service */
static bool temp_raw(int i, uint8_t *buf)
{
	long mc;

	if (!read_hwmon(temps[i].name, NULL, temps[i].attr, &mc))
		return false;
	put_le16((int16_t) (mc / 10), buf);
	return true;
}

static bool rail_text(int i, char *buf, size_t *len)
{
	long mv, ma, uw;
	int n;

	if (!read_hwmon("ina2", rails[i].dev, "in1_input", &mv) ||
	    !read_hwmon("ina2", rails[i].dev, "curr1_input", &ma) ||
	    !read_hwmon("ina2", rails[i].dev, "power1_input", &uw))
		return false;
	n = snprintf(buf, TEXT_MAX, rails[i].vfmt, mv / 1000.0);
	n += snprintf(buf + n, TEXT_MAX - n, " %.3fA %.2fW", ma / 1000.0,
		      uw / 1e6);
	*len = n < TEXT_MAX ? n : TEXT_MAX - 1;
	return true;
}

/*
 * The THERM output of the ADT7461 switches the fan through the CPLD: its
 * remote channel alarm is the fan state, with the hysteresis of THERM.
 */
static bool fan_text(int i, char *buf, size_t *len)
{
	long on, limit_on, limit_off;

	if (!read_hwmon("adt7461", NULL, "temp2_crit_alarm", &on))
		return false;
	if (read_hwmon("adt7461", NULL, "temp2_crit", &limit_on) &&
	    read_hwmon("adt7461", NULL, "temp2_crit_hyst", &limit_off))
		*len = snprintf(buf, TEXT_MAX, "%s (%ld/%ld" DEG ")",
				on ? "ligado" : "desligado", limit_on / 1000,
				limit_off / 1000);
	else
		*len = snprintf(buf, TEXT_MAX, "%s",
				on ? "ligado" : "desligado");
	return true;
}

static bool net_attr(int i, const char *attr, long *val)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ports[i].ifname,
		 attr);
	return read_long(path, val);
}

/*
 * A port that is down has its PHY powered down; carrier and speed only
 * read while the interface is up.
 */
static bool port_text(int i, char *buf, size_t *len)
{
	long flags, carrier, speed;
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/net/%s/flags", ports[i].ifname);
	f = fopen(path, "r");
	if (!f)
		return false;
	if (fscanf(f, "%li", &flags) != 1) {
		fclose(f);
		return false;
	}
	fclose(f);

	if (!(flags & 1))	/* IFF_UP */
		*len = snprintf(buf, TEXT_MAX, "desligada");
	else if (!net_attr(i, "carrier", &carrier) || !carrier ||
		 !net_attr(i, "speed", &speed) || speed <= 0)
		*len = snprintf(buf, TEXT_MAX, "ligada, sem link");
	else
		*len = snprintf(buf, TEXT_MAX, "ligada, %ld Mb/s", speed);
	return true;
}

static bool port_set(int i, bool on)
{
	char cmd[80];

	snprintf(cmd, sizeof(cmd), "ip link set dev %s %s", ports[i].ifname,
		 on ? "up" : "down");
	return system(cmd) == 0;
}

static bool led_text(int i, char *buf, size_t *len)
{
	char trigger[256];
	FILE *f;
	long b;

	f = fopen(LED_PATH "/trigger", "r");
	if (f) {
		if (fgets(trigger, sizeof(trigger), f) &&
		    strstr(trigger, "[timer]")) {
			fclose(f);
			*len = snprintf(buf, TEXT_MAX, "piscando");
			return true;
		}
		fclose(f);
	}
	if (!read_long(LED_PATH "/brightness", &b))
		return false;
	*len = snprintf(buf, TEXT_MAX, "%s", b ? "ligado" : "desligado");
	return true;
}

/* everything in one text, read with Read Blob when it exceeds the MTU */
static void build_summary(struct server *server)
{
	char *s = server->summary, v[TEXT_MAX];
	size_t size = sizeof(server->summary), n = 0, len;
	unsigned int i;

#define ADD(...) do { \
		if (n < size) \
			n += snprintf(s + n, size - n, __VA_ARGS__); \
	} while (0)
#define VAL(f, i) ((f)(i, v, &len) ? v : "indispon\xc3\xadvel")

	for (i = 0; i < ARRAY_SIZE(temps); i++)
		ADD("%s: %s\n", temps[i].desc, VAL(temp_text, i));
	for (i = 0; i < ARRAY_SIZE(rails); i++)
		ADD("%s: %s\n", rails[i].desc, VAL(rail_text, i));
	ADD("Ventilador: %s\n", VAL(fan_text, 0));
	for (i = 0; i < ARRAY_SIZE(ports); i++)
		ADD("Porta %s: %s\n", ports[i].jack, VAL(port_text, i));
	ADD("LED0: %s\n", VAL(led_text, 0));

#undef VAL
#undef ADD
	server->summary_len = n < size ? n : size - 1;
}

/*
 * The KW41Z HCI firmware only takes ACL data from the host for a short
 * while after it sent something itself: a response 2 ms late or an
 * unsolicited notification hangs it. HCI commands always get through, so
 * send one and wait for its Command Complete right before every ACL
 * packet that is not an immediate reply.
 */
static void kick(void)
{
	bdaddr_t ba;

	if (kick_dd >= 0 && hci_read_bd_addr(kick_dd, &ba, 1000) < 0) {
		fprintf(stderr, "controller not responding\n");
		event("disconnected", NULL);
		exit(2);
	}
}

/* GATT callbacks */

static void att_disconnect_cb(int err, void *user_data)
{
	printf("disconnected: %s\n", strerror(err));
	event("disconnected", NULL);
	mainloop_quit();
}

static void debug_cb(const char *str, void *user_data)
{
	printf("%s%s\n", (const char *) user_data, str);
}

static void name_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
			 uint16_t offset, uint8_t opcode, struct bt_att *att,
			 void *user_data)
{
	size_t len = strlen(dev_name);

	if (offset > len) {
		gatt_db_attribute_read_result(attrib, id,
					      BT_ATT_ERROR_INVALID_OFFSET,
					      NULL, 0);
		return;
	}
	gatt_db_attribute_read_result(attrib, id, 0,
				      (const uint8_t *) dev_name + offset,
				      len - offset);
}

/*
 * Clients such as nRF Connect on iOS do not show the User Description, so
 * every value carries its label.
 */
static bool labeled_text(struct notifier *n, char *buf, size_t *len)
{
	char v[TEXT_MAX];
	size_t vlen;
	int ret;

	if (!n->read(n->arg, v, &vlen))
		return false;
	ret = snprintf(buf, LABEL_MAX, "%s: %.*s", n->label, (int) vlen, v);
	*len = ret < LABEL_MAX ? ret : LABEL_MAX - 1;
	return true;
}

static void text_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
			 uint16_t offset, uint8_t opcode, struct bt_att *att,
			 void *user_data)
{
	struct notifier *n = user_data;
	char buf[LABEL_MAX];
	size_t len;

	if (!labeled_text(n, buf, &len)) {
		kick();
		gatt_db_attribute_read_result(attrib, id,
					      BT_ATT_ERROR_UNLIKELY, NULL, 0);
		return;
	}
	if (offset > len) {
		kick();
		gatt_db_attribute_read_result(attrib, id,
					      BT_ATT_ERROR_INVALID_OFFSET,
					      NULL, 0);
		return;
	}
	kick();
	gatt_db_attribute_read_result(attrib, id, 0,
				      (const uint8_t *) buf + offset,
				      len - offset);
}

static void temp_raw_read_cb(struct gatt_db_attribute *attrib,
			     unsigned int id, uint16_t offset, uint8_t opcode,
			     struct bt_att *att, void *user_data)
{
	uint8_t buf[2];

	if (!temp_raw((intptr_t) user_data, buf)) {
		kick();
		gatt_db_attribute_read_result(attrib, id,
					      BT_ATT_ERROR_UNLIKELY, NULL, 0);
		return;
	}
	kick();
	gatt_db_attribute_read_result(attrib, id, 0, buf, sizeof(buf));
}

/* a new text at offset 0, the same one for the Read Blob that follow */
static void summary_read_cb(struct gatt_db_attribute *attrib,
			    unsigned int id, uint16_t offset, uint8_t opcode,
			    struct bt_att *att, void *user_data)
{
	struct server *server = user_data;

	if (!offset || !server->summary_len)
		build_summary(server);
	if (offset > server->summary_len) {
		kick();
		gatt_db_attribute_read_result(attrib, id,
					      BT_ATT_ERROR_INVALID_OFFSET,
					      NULL, 0);
		return;
	}
	kick();
	gatt_db_attribute_read_result(attrib, id, 0,
				(const uint8_t *) server->summary + offset,
				server->summary_len - offset);
}

/* only changed values are sent */
static bool notify_cb(void *user_data)
{
	struct server *server = user_data;
	size_t len, max = bt_att_get_mtu(server->att) - 3;
	char buf[LABEL_MAX];
	int i;

	for (i = 0; i < N_MAX; i++) {
		struct notifier *n = &server->notify[i];

		if (!n->enabled || !labeled_text(n, buf, &len))
			continue;
		/* the value alone when the label does not fit */
		if (len > max && !n->read(n->arg, buf, &len))
			continue;
		if (len == n->last_len && !memcmp(buf, n->last, len))
			continue;
		memcpy(n->last, buf, len);
		n->last_len = len;
		kick();
		bt_gatt_server_send_notification(server->gatt, n->handle,
						 (const uint8_t *) buf, len,
						 false);
	}
	return true;
}

static void update_timer(struct server *server)
{
	bool any = false;
	int i;

	for (i = 0; i < N_MAX; i++)
		any |= server->notify[i].enabled;

	if (any && !server->timeout_id)
		server->timeout_id = timeout_add(NOTIFY_MS, notify_cb, server,
						 NULL);
	else if (!any && server->timeout_id) {
		timeout_remove(server->timeout_id);
		server->timeout_id = 0;
	}
}

struct ccc {
	struct server *server;
	struct notifier *n;
};

static void ccc_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
			uint16_t offset, uint8_t opcode, struct bt_att *att,
			void *user_data)
{
	struct ccc *ccc = user_data;
	uint8_t value[2] = { ccc->n->enabled ? 0x01 : 0x00, 0x00 };

	gatt_db_attribute_read_result(attrib, id, 0, value, 2);
}

static void ccc_write_cb(struct gatt_db_attribute *attrib, unsigned int id,
			 uint16_t offset, const uint8_t *value, size_t len,
			 uint8_t opcode, struct bt_att *att, void *user_data)
{
	struct ccc *ccc = user_data;
	uint8_t ecode = 0;

	if (!value || len != 2)
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
	else if (offset)
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
	else if (value[0] > 1)
		ecode = 0x80;
	else {
		ccc->n->enabled = value[0] == 0x01;
		/* send the current value on the next tick */
		ccc->n->last_len = 0;
		update_timer(ccc->server);
	}

	gatt_db_attribute_write_result(attrib, id, ecode);
}

/*
 * Clients keep the database of a device they saw before, so they would
 * not see a new one. When a client subscribes to Service Changed, tell it
 * once per connection that the whole database may have changed.
 */
static void svc_chngd_ccc_read_cb(struct gatt_db_attribute *attrib,
				  unsigned int id, uint16_t offset,
				  uint8_t opcode, struct bt_att *att,
				  void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2] = { server->svc_chngd_sent ? 0x02 : 0x00, 0x00 };

	gatt_db_attribute_read_result(attrib, id, 0, value, 2);
}

static void svc_chngd_ccc_write_cb(struct gatt_db_attribute *attrib,
				   unsigned int id, uint16_t offset,
				   const uint8_t *value, size_t len,
				   uint8_t opcode, struct bt_att *att,
				   void *user_data)
{
	struct server *server = user_data;
	uint8_t range[4];

	if (!value || len != 2) {
		gatt_db_attribute_write_result(attrib, id,
				BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN);
		return;
	}
	gatt_db_attribute_write_result(attrib, id, 0);

	if (!(value[0] & 0x02) || server->svc_chngd_sent)
		return;
	server->svc_chngd_sent = true;
	put_le16(0x0001, range);
	put_le16(0xffff, range + 2);
	kick();
	bt_gatt_server_send_indication(server->gatt, server->svc_chngd_handle,
				       range, sizeof(range), NULL, NULL,
				       NULL);
}

/*
 * Commands are a byte or text: 0/1/2, or a word. Returns the number,
 * -1 when it is not one of them.
 */
static int parse_cmd(const uint8_t *value, size_t len)
{
	static const struct {
		const char *word;
		int cmd;
	} words[] = {
		{ "0", 0 }, { "off", 0 }, { "desligar", 0 }, { "desligado", 0 },
		{ "desligada", 0 },
		{ "1", 1 }, { "on", 1 }, { "ligar", 1 }, { "ligado", 1 },
		{ "ligada", 1 },
		{ "2", 2 }, { "blink", 2 }, { "piscar", 2 },
	};
	char s[16];
	unsigned int i;

	if (len == 1 && value[0] <= 2)
		return value[0];
	if (!len || len >= sizeof(s))
		return -1;
	memcpy(s, value, len);
	s[len] = '\0';
	/* clients often append a line end */
	while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
		       s[len - 1] == ' '))
		s[--len] = '\0';
	for (i = 0; i < ARRAY_SIZE(words); i++)
		if (!strcasecmp(s, words[i].word))
			return words[i].cmd;
	return -1;
}

/* 0 off, 1 on, 2 back to blinking (timer trigger) */
static void led_write_cb(struct gatt_db_attribute *attrib, unsigned int id,
			 uint16_t offset, const uint8_t *value, size_t len,
			 uint8_t opcode, struct bt_att *att, void *user_data)
{
	uint8_t ecode = 0;
	int cmd;
	bool ok;

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}
	cmd = value ? parse_cmd(value, len) : -1;
	if (cmd < 0) {
		ecode = 0x80;
		goto done;
	}

	if (cmd == 2)
		ok = write_str(LED_PATH "/trigger", "timer");
	else
		ok = write_str(LED_PATH "/trigger", "none") &&
		     write_str(LED_PATH "/brightness", cmd ? "1" : "0");

	if (!ok)
		ecode = BT_ATT_ERROR_UNLIKELY;
	else
		printf("LED0 <- %d\n", cmd);

done:
	kick();
	gatt_db_attribute_write_result(attrib, id, ecode);
}

/* 0 powers the port PHY down, 1 up again */
static void port_write_cb(struct gatt_db_attribute *attrib, unsigned int id,
			  uint16_t offset, const uint8_t *value, size_t len,
			  uint8_t opcode, struct bt_att *att, void *user_data)
{
	struct notifier *n = user_data;
	uint8_t ecode = 0;
	int cmd;

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}
	cmd = value ? parse_cmd(value, len) : -1;
	if (cmd != 0 && cmd != 1) {
		ecode = 0x80;
		goto done;
	}

	if (!port_set(n->arg, cmd))
		ecode = BT_ATT_ERROR_UNLIKELY;
	else
		printf("port %s <- %s\n", ports[n->arg].jack,
		       cmd ? "on" : "off");

done:
	kick();
	gatt_db_attribute_write_result(attrib, id, ecode);
}

/*
 * Check that the controller still answers; kick() exits if it does not, so
 * the init script resets the KW41Z through the CPLD.
 */
static bool ping_cb(void *user_data)
{
	kick();
	return true;
}

/* database */

static void confirm_write(struct gatt_db_attribute *attr, int err,
			  void *user_data)
{
	if (err) {
		fprintf(stderr, "failed to cache attribute: %d\n", err);
		exit(1);
	}
}

static void set_value(struct gatt_db_attribute *attr, const void *val,
		      size_t len)
{
	gatt_db_attribute_write(attr, 0, val, len, BT_ATT_OP_WRITE_REQ, NULL,
				confirm_write, NULL);
}

static void add_desc(struct gatt_db_attribute *service, uint16_t uuid16,
		     const void *val, size_t len)
{
	struct gatt_db_attribute *attr;
	bt_uuid_t uuid;

	bt_uuid16_create(&uuid, uuid16);
	attr = gatt_db_service_add_descriptor(service, &uuid, BT_ATT_PERM_READ,
					      NULL, NULL, NULL);
	set_value(attr, val, len);
}

static void add_user_desc(struct gatt_db_attribute *service, const char *desc)
{
	add_desc(service, UUID_USER_DESC, desc, strlen(desc));
}

/* Characteristic Presentation Format: format, exponent, unit, namespace */
static void add_format(struct gatt_db_attribute *service, uint8_t format,
		       int8_t exponent, uint16_t unit)
{
	uint8_t fmt[7] = { format, (uint8_t) exponent, unit & 0xff, unit >> 8,
			   0x01, 0x00, 0x00 };

	add_desc(service, UUID_PRES_FORMAT, fmt, sizeof(fmt));
}

/* a UTF-8 text characteristic, read and notify, writable with write_cb */
static void add_text_chrc(struct server *server,
			  struct gatt_db_attribute *service, const char *uuid_str,
			  int idx, int arg, const char *label,
			  const char *desc, bool (*read)(int, char *, size_t *),
			  gatt_db_write_t write_cb)
{
	struct notifier *n = &server->notify[idx];
	struct gatt_db_attribute *chrc;
	uint32_t perm = BT_ATT_PERM_READ;
	uint8_t props = BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY;
	struct ccc *ccc;
	bt_uuid_t uuid;

	if (write_cb) {
		perm |= BT_ATT_PERM_WRITE;
		props |= BT_GATT_CHRC_PROP_WRITE;
	}

	n->read = read;
	n->arg = arg;
	n->label = label;
	bt_string_to_uuid(&uuid, uuid_str);
	chrc = gatt_db_service_add_characteristic(service, &uuid, perm, props,
						  text_read_cb, write_cb, n);
	n->handle = gatt_db_attribute_get_handle(chrc);

	ccc = new0(struct ccc, 1);
	ccc->server = server;
	ccc->n = n;
	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
				       BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				       ccc_read_cb, ccc_write_cb, ccc);

	add_user_desc(service, desc);
	add_format(service, FORMAT_UTF8, 0, UNIT_NONE);
}

static void populate_db(struct server *server)
{
	struct gatt_db_attribute *service, *attr;
	struct notifier *led = new0(struct notifier, 1);
	uint16_t appearance;
	char uuid_str[40], desc[48];
	bt_uuid_t uuid;
	unsigned int i;

	bt_uuid16_create(&uuid, UUID_GAP);
	service = gatt_db_add_service(server->db, &uuid, true, 6);
	bt_uuid16_create(&uuid, GATT_CHARAC_DEVICE_NAME);
	gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ,
					   BT_GATT_CHRC_PROP_READ,
					   name_read_cb, NULL, server);
	bt_uuid16_create(&uuid, GATT_CHARAC_APPEARANCE);
	attr = gatt_db_service_add_characteristic(service, &uuid,
						  BT_ATT_PERM_READ,
						  BT_GATT_CHRC_PROP_READ,
						  NULL, NULL, NULL);
	put_le16(0x0540, &appearance);	/* Generic Sensor */
	set_value(attr, &appearance, sizeof(appearance));
	gatt_db_service_set_active(service, true);

	bt_uuid16_create(&uuid, UUID_GATT);
	service = gatt_db_add_service(server->db, &uuid, true, 4);
	bt_uuid16_create(&uuid, GATT_CHARAC_SERVICE_CHANGED);
	attr = gatt_db_service_add_characteristic(service, &uuid,
						  BT_ATT_PERM_NONE,
						  BT_GATT_CHRC_PROP_INDICATE,
						  NULL, NULL, NULL);
	server->svc_chngd_handle = gatt_db_attribute_get_handle(attr);
	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
				       BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				       svc_chngd_ccc_read_cb,
				       svc_chngd_ccc_write_cb, server);
	gatt_db_service_set_active(service, true);

	/* the standard temperatures, for clients that know the service */
	bt_uuid16_create(&uuid, UUID_ESS);
	service = gatt_db_add_service(server->db, &uuid, true,
				      1 + 4 * ARRAY_SIZE(temps));
	for (i = 0; i < ARRAY_SIZE(temps); i++) {
		bt_uuid16_create(&uuid, UUID_TEMPERATURE);
		gatt_db_service_add_characteristic(service, &uuid,
					BT_ATT_PERM_READ, BT_GATT_CHRC_PROP_READ,
					temp_raw_read_cb, NULL,
					(void *) (intptr_t) i);
		add_user_desc(service, temps[i].desc);
		add_format(service, FORMAT_SINT16, -2, UNIT_CELSIUS);
	}
	gatt_db_service_set_active(service, true);

	/* the whole board as text */
	bt_string_to_uuid(&uuid, SPARTAN_UUID("00"));
	service = gatt_db_add_service(server->db, &uuid, true,
				      1 + 4 + 4 + 5 * N_MAX);

	bt_string_to_uuid(&uuid, SPARTAN_UUID("01"));
	led->read = led_text;
	led->label = "LED0";
	gatt_db_service_add_characteristic(service, &uuid,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_WRITE,
				text_read_cb, led_write_cb, led);
	add_user_desc(service, "LED0: escreva 0 (desliga), 1 (liga) ou 2 (pisca)");
	add_format(service, FORMAT_UTF8, 0, UNIT_NONE);

	bt_string_to_uuid(&uuid, SPARTAN_UUID("02"));
	gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ,
					   BT_GATT_CHRC_PROP_READ,
					   summary_read_cb, NULL, server);
	add_user_desc(service, "Resumo da placa");
	add_format(service, FORMAT_UTF8, 0, UNIT_NONE);

#define UUID_STR(x) (snprintf(uuid_str, sizeof(uuid_str), \
				    SPARTAN_UUID("%02x"), (x)), uuid_str)

	for (i = 0; i < ARRAY_SIZE(temps); i++)
		add_text_chrc(server, service, UUID_STR(0x10 + i), N_TEMP + i,
			      i, temps[i].label, temps[i].desc, temp_text,
			      NULL);
	for (i = 0; i < ARRAY_SIZE(rails); i++)
		add_text_chrc(server, service, UUID_STR(0x20 + i), N_RAIL + i,
			      i, rails[i].label, rails[i].desc, rail_text,
			      NULL);
	add_text_chrc(server, service, UUID_STR(0x30), N_FAN, 0, "Ventilador",
		      "Ventilador", fan_text, NULL);
	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		snprintf(desc, sizeof(desc),
			 "Porta %s: escreva 0 (desliga) ou 1 (liga)",
			 ports[i].jack);
		add_text_chrc(server, service, UUID_STR(0x40 + i), N_PORT + i,
			      i, ports[i].jack, desc, port_text,
			      port_write_cb);
	}

#undef UUID_STR
	gatt_db_service_set_active(service, true);
}

static struct server *server_create(int fd)
{
	struct server *server = new0(struct server, 1);

	server->att = bt_att_new(fd, false);
	if (!server->att)
		goto fail;
	bt_att_set_close_on_unref(server->att, true);
	bt_att_register_disconnect(server->att, att_disconnect_cb, NULL, NULL);

	server->db = gatt_db_new();
	server->gatt = bt_gatt_server_new(server->db, server->att, 0, 0);
	if (!server->gatt)
		goto fail;

	if (verbose) {
		bt_att_set_debug(server->att, BT_ATT_DEBUG_HEXDUMP,
				 debug_cb, "att: ", NULL);
		bt_gatt_server_set_debug(server->gatt, debug_cb, "server: ",
					 NULL);
	}

	populate_db(server);

	if (kick_dd >= 0)
		server->ping_id = timeout_add(PING_MS, ping_cb, server, NULL);
	return server;

fail:
	fprintf(stderr, "failed to create the GATT server\n");
	gatt_db_unref(server->db);
	bt_att_unref(server->att);
	free(server);
	return NULL;
}

/* advertising */

/*
 * A central left over from a previous run keeps the controller from
 * advertising, and its ATT channel is gone: drop it.
 */
static void drop_connections(int dev_id)
{
	struct hci_conn_list_req *cl;
	struct hci_conn_info *ci;
	int dd, i;

	dd = hci_open_dev(dev_id);
	if (dd < 0)
		return;

	cl = malloc(sizeof(*cl) + 10 * sizeof(*ci));
	cl->dev_id = dev_id;
	cl->conn_num = 10;
	if (!ioctl(dd, HCIGETCONNLIST, cl)) {
		for (i = 0, ci = cl->conn_info; i < cl->conn_num; i++, ci++) {
			printf("dropping connection %u\n", ci->handle);
			hci_disconnect(dd, ci->handle,
				       HCI_OE_USER_ENDED_CONNECTION, 2000);
		}
	}

	free(cl);
	hci_close_dev(dd);
}

static int hci_cmd(int dd, uint16_t ocf, void *cp, uint8_t len)
{
	struct hci_request rq;
	uint8_t status;

	memset(&rq, 0, sizeof(rq));
	rq.ogf = OGF_LE_CTL;
	rq.ocf = ocf;
	rq.cparam = cp;
	rq.clen = len;
	rq.rparam = &status;
	rq.rlen = 1;

	if (hci_send_req(dd, &rq, 1000) < 0)
		return -1;
	return status ? -1 : 0;
}

static int advertise(int dev_id)
{
	le_set_advertising_parameters_cp params;
	le_set_advertising_data_cp data;
	uint8_t enable = 1, *p = data.data;
	size_t name_len = strlen(dev_name);
	int dd, err;

	dd = hci_open_dev(dev_id);
	if (dd < 0)
		return -1;

	memset(&data, 0, sizeof(data));
	*p++ = 2; *p++ = 0x01; *p++ = 0x06;		/* LE only, general */
	*p++ = 3; *p++ = 0x03;				/* 16-bit UUIDs */
	put_le16(UUID_ESS, p); p += 2;
	if (name_len > 31 - (p - data.data) - 2)
		name_len = 31 - (p - data.data) - 2;
	*p++ = name_len + 1; *p++ = 0x09;		/* complete name */
	memcpy(p, dev_name, name_len); p += name_len;
	data.length = p - data.data;

	memset(&params, 0, sizeof(params));
	params.min_interval = htobs(0x00a0);		/* 100 ms */
	params.max_interval = htobs(0x00a0);
	params.advtype = 0x00;				/* ADV_IND */
	params.chan_map = 0x07;

	/* parameters can't change while a previous run is still advertising */
	enable = 0;
	hci_cmd(dd, OCF_LE_SET_ADVERTISE_ENABLE, &enable, 1);

	/*
	 * Newer kernels turn on address resolution in the controller (LL
	 * privacy). With it on, the KW41Z keeps advertising but ignores every
	 * connection request: turn it off, the server has no resolving list.
	 */
	hci_cmd(dd, OCF_LE_SET_ADDRESS_RESOLUTION_ENABLE, &enable, 1);
	enable = 1;

	err = hci_cmd(dd, OCF_LE_SET_ADVERTISING_PARAMETERS, &params,
		      sizeof(params));
	if (!err)
		err = hci_cmd(dd, OCF_LE_SET_ADVERTISING_DATA, &data,
			      sizeof(data));
	if (!err)
		err = hci_cmd(dd, OCF_LE_SET_ADVERTISE_ENABLE, &enable, 1);

	hci_close_dev(dd);
	return err;
}

static int att_listen(const bdaddr_t *src)
{
	struct sockaddr_l2 addr;
	struct bt_security sec;
	int sk;

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		perror("socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	addr.l2_cid = htobs(ATT_CID);
	addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
	bacpy(&addr.l2_bdaddr, src);

	memset(&sec, 0, sizeof(sec));
	sec.level = BT_SECURITY_LOW;

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
	    setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) ||
	    listen(sk, 1) < 0) {
		perror("ATT socket");
		close(sk);
		return -1;
	}
	return sk;
}

static void usage(void)
{
	printf("Usage: spartan-gatt [-i hciX] [-n name] [-e program] [-v]\n"
	       "       spartan-gatt -s    print the summary and exit\n");
}

int main(int argc, char *argv[])
{
	int dev_id = 0, opt, lsk, fd, retries = 0;
	struct sockaddr_l2 addr;
	struct server *server;
	socklen_t optlen;
	bdaddr_t src;
	char ba[18];

	while ((opt = getopt(argc, argv, "he:i:n:sv")) != -1) {
		switch (opt) {
		case 'i':
			dev_id = hci_devid(optarg);
			if (dev_id < 0) {
				perror("adapter");
				return EXIT_FAILURE;
			}
			break;
		case 'n':
			dev_name = optarg;
			break;
		case 'e':
			event_prog = optarg;
			break;
		case 's': {
			static struct server server;

			build_summary(&server);
			fputs(server.summary, stdout);
			return EXIT_SUCCESS;
		}
		case 'v':
			verbose = true;
			break;
		default:
			usage();
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (hci_devba(dev_id, &src) < 0) {
		perror("adapter not available");
		return EXIT_FAILURE;
	}

	lsk = att_listen(&src);
	if (lsk < 0)
		return EXIT_FAILURE;

	setvbuf(stdout, NULL, _IOLBF, 0);

	/*
	 * The controller refuses to advertise while the previous central is
	 * still connected; the kernel only drops that link a few seconds
	 * after the last channel closes.
	 */
	drop_connections(dev_id);

	while (advertise(dev_id) < 0) {
		if (!retries++)
			fprintf(stderr, "advertising refused, retrying\n");
		if (retries == 10) {
			fprintf(stderr, "controller not responding\n");
			return 2;
		}
		sleep(1);
	}
	printf("advertising as \"%s\"\n", dev_name);

	/*
	 * The KW41Z can also hang while it only advertises: keep checking it
	 * while waiting for a central, and exit so it gets reset.
	 */
	kick_dd = hci_open_dev(dev_id);
	for (;;) {
		struct pollfd pfd = { .fd = lsk, .events = POLLIN };
		int ret = poll(&pfd, 1, PING_MS);

		if (ret > 0)
			break;
		if (ret < 0 && errno != EINTR) {
			perror("poll");
			return EXIT_FAILURE;
		}
		kick();
	}

	do {
		optlen = sizeof(addr);
		fd = accept(lsk, (struct sockaddr *) &addr, &optlen);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0) {
		perror("accept");
		return EXIT_FAILURE;
	}
	close(lsk);
	ba2str(&addr.l2_bdaddr, ba);
	printf("connected: %s\n", ba);
	connected = true;
	event("connected", ba);

	mainloop_init();
	server = server_create(fd);
	if (!server) {
		close(fd);
		return EXIT_FAILURE;
	}
	mainloop_run();

	/* one central per run: procd respawns us to advertise again */
	return EXIT_SUCCESS;
}
