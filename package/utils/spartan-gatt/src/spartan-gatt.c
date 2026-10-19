// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GATT server for the LS1021A-IoT: board temperatures, input power and the
 * CPLD LED0 over Bluetooth LE.
 *
 * Based on tools/btgatt-server.c from BlueZ, Copyright (C) 2014 Google Inc.
 *
 * Services:
 *  - Environmental Sensing (0x181a): two Temperature (0x2a6e, sint16 in
 *    0.01 C) characteristics, the LS1021A die (ADT7461 remote channel) and
 *    the board (LM75A), read and notify
 *  - Spartan (7370a000-...): LED0 (uint8 0/1, read and write) and the 12 V
 *    input power (uint16 in mW, INA220 at 0x45, read and notify)
 *
 * It owns the LE ATT channel, so bluetoothd must not be running. It
 * advertises as connectable, serves one central and exits when it
 * disconnects; the init script respawns it.
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
#include <sys/ioctl.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "lib/uuid.h"

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

/* 7370a0xx-4c53-3130-3231-412d496f5400: "sp" "LS1021A-IoT" */
#define SPARTAN_UUID(x)	"7370a0" x "-4c53-3130-3231-412d496f5400"

#define LED_PATH	"/sys/class/leds/ls1021aiot:green:led0"
#define NOTIFY_MS	1000
#define PING_MS		5000

static const char *dev_name = "spartan";
static bool verbose;
static int kick_dd = -1;

struct notifier {
	uint16_t handle;
	bool enabled;
	bool (*read)(uint8_t *buf, size_t *len);
};

enum { N_DIE, N_BOARD, N_POWER, N_MAX };

struct server {
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_server *gatt;
	struct notifier notify[N_MAX];
	unsigned int timeout_id;
	unsigned int ping_id;
};

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

/* millidegrees to the GATT Temperature format, sint16 in 0.01 C */
static bool temp_value(const char *name, const char *attr, uint8_t *buf,
		       size_t *len)
{
	long mc;

	if (!read_hwmon(name, NULL, attr, &mc))
		return false;
	put_le16((int16_t) (mc / 10), buf);
	*len = 2;
	return true;
}

static bool die_temp(uint8_t *buf, size_t *len)
{
	return temp_value("adt7461", "temp2_input", buf, len);
}

static bool board_temp(uint8_t *buf, size_t *len)
{
	return temp_value("lm75", "temp1_input", buf, len);
}

static bool input_power(uint8_t *buf, size_t *len)
{
	long uw;

	if (!read_hwmon("ina2", "0-0045", "power1_input", &uw))
		return false;
	put_le16((uint16_t) (uw / 1000), buf);
	*len = 2;
	return true;
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
		exit(2);
	}
}

/* GATT callbacks */

static void att_disconnect_cb(int err, void *user_data)
{
	printf("disconnected: %s\n", strerror(err));
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

static void value_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
			  uint16_t offset, uint8_t opcode, struct bt_att *att,
			  void *user_data)
{
	struct notifier *n = user_data;
	uint8_t buf[8];
	size_t len;

	if (!n->read(buf, &len)) {
		kick();
		gatt_db_attribute_read_result(attrib, id,
					      BT_ATT_ERROR_UNLIKELY, NULL, 0);
		return;
	}
	kick();
	gatt_db_attribute_read_result(attrib, id, 0, buf, len);
}

static bool notify_cb(void *user_data)
{
	struct server *server = user_data;
	uint8_t buf[8];
	size_t len;
	int i;

	for (i = 0; i < N_MAX; i++) {
		struct notifier *n = &server->notify[i];

		if (n->enabled && n->read(buf, &len)) {
			kick();
			bt_gatt_server_send_notification(server->gatt,
							 n->handle, buf, len,
							 false);
		}
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
		update_timer(ccc->server);
	}

	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void led_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
			uint16_t offset, uint8_t opcode, struct bt_att *att,
			void *user_data)
{
	uint8_t value;
	long b;

	if (!read_long(LED_PATH "/brightness", &b)) {
		kick();
		gatt_db_attribute_read_result(attrib, id,
					      BT_ATT_ERROR_UNLIKELY, NULL, 0);
		return;
	}
	value = b ? 1 : 0;
	kick();
	gatt_db_attribute_read_result(attrib, id, 0, &value, 1);
}

/* 0 off, 1 on, 2 back to blinking (timer trigger) */
static void led_write_cb(struct gatt_db_attribute *attrib, unsigned int id,
			 uint16_t offset, const uint8_t *value, size_t len,
			 uint8_t opcode, struct bt_att *att, void *user_data)
{
	uint8_t ecode = 0;
	bool ok;

	if (!value || len != 1) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}
	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	switch (value[0]) {
	case 0:
	case 1:
		ok = write_str(LED_PATH "/trigger", "none") &&
		     write_str(LED_PATH "/brightness", value[0] ? "1" : "0");
		break;
	case 2:
		ok = write_str(LED_PATH "/trigger", "timer");
		break;
	default:
		ecode = 0x80;
		goto done;
	}

	if (!ok)
		ecode = BT_ATT_ERROR_UNLIKELY;
	else
		printf("LED0 <- %u\n", value[0]);

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

static void add_user_desc(struct gatt_db_attribute *service, const char *desc)
{
	struct gatt_db_attribute *attr;
	bt_uuid_t uuid;

	bt_uuid16_create(&uuid, UUID_USER_DESC);
	attr = gatt_db_service_add_descriptor(service, &uuid, BT_ATT_PERM_READ,
					      NULL, NULL, NULL);
	set_value(attr, desc, strlen(desc));
}

static void add_notify_chrc(struct server *server,
			    struct gatt_db_attribute *service,
			    const bt_uuid_t *uuid, int idx, const char *desc,
			    bool (*read)(uint8_t *, size_t *))
{
	struct notifier *n = &server->notify[idx];
	struct gatt_db_attribute *chrc;
	struct ccc *ccc;
	bt_uuid_t ccc_uuid;

	n->read = read;
	chrc = gatt_db_service_add_characteristic(service, uuid,
				BT_ATT_PERM_READ,
				BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY,
				value_read_cb, NULL, n);
	n->handle = gatt_db_attribute_get_handle(chrc);

	ccc = new0(struct ccc, 1);
	ccc->server = server;
	ccc->n = n;
	bt_uuid16_create(&ccc_uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &ccc_uuid,
				       BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				       ccc_read_cb, ccc_write_cb, ccc);

	add_user_desc(service, desc);
}

static void populate_db(struct server *server)
{
	struct gatt_db_attribute *service, *attr;
	uint16_t appearance;
	bt_uuid_t uuid;

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
	service = gatt_db_add_service(server->db, &uuid, true, 1);
	gatt_db_service_set_active(service, true);

	bt_uuid16_create(&uuid, UUID_ESS);
	service = gatt_db_add_service(server->db, &uuid, true, 9);
	bt_uuid16_create(&uuid, UUID_TEMPERATURE);
	add_notify_chrc(server, service, &uuid, N_DIE, "LS1021A die",
			die_temp);
	add_notify_chrc(server, service, &uuid, N_BOARD, "Board (LM75A)",
			board_temp);
	gatt_db_service_set_active(service, true);

	bt_string_to_uuid(&uuid, SPARTAN_UUID("00"));
	service = gatt_db_add_service(server->db, &uuid, true, 9);
	bt_string_to_uuid(&uuid, SPARTAN_UUID("01"));
	gatt_db_service_add_characteristic(service, &uuid,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_WRITE,
				led_read_cb, led_write_cb, server);
	add_user_desc(service, "LED0: 0 off, 1 on, 2 blink");
	bt_string_to_uuid(&uuid, SPARTAN_UUID("02"));
	add_notify_chrc(server, service, &uuid, N_POWER, "12 V input power (mW)",
			input_power);
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
		bt_att_set_debug(server->att, debug_cb, "att: ", NULL);
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
	printf("Usage: spartan-gatt [-i hciX] [-n name] [-v]\n");
}

int main(int argc, char *argv[])
{
	int dev_id = 0, opt, lsk, fd, retries = 0;
	struct sockaddr_l2 addr;
	struct server *server;
	socklen_t optlen;
	bdaddr_t src;
	char ba[18];

	while ((opt = getopt(argc, argv, "hi:n:v")) != -1) {
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
