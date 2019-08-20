/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2019 WireGuard LLC. All Rights Reserved.
 */

#define _DEFAULT_SOURCE

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>

#include "common.h"
#include "dbg.h"

static bool parse_ip_cidr(struct wg_combined_ip *ip, char *value)
{
	uintmax_t res;
	char *endptr;
	char *sep;

	if (value[0] == '\0') {
		memset(ip, 0, ip->family == AF_INET ? 4 : 16);
		ip->cidr = 0;
		return true;
	}

	sep = strchr(value, '/');
	if (!sep)
		return false;

	*sep = '\0';
	if (inet_pton(ip->family, value, ip) != 1)
		return false;

	res = strtoumax(sep + 1, &endptr, 10);
	if (res > UINT8_MAX || *endptr != '\0' || sep + 1 == endptr)
		return false;

	// TODO: validate cidr range depending on ip->family
	ip->cidr = (uint8_t)res;

	return true;
}

static struct wg_dynamic_attr *parse_value(enum wg_dynamic_key key, char *value)
{
	struct wg_dynamic_attr *attr;
	size_t len;
	char *endptr;
	uintmax_t uresult;
	union {
		uint32_t uint32;
		char errmsg[72];
		struct wg_combined_ip ip;
	} data = { 0 };

	switch (key) {
	case WGKEY_IPV4:
		len = sizeof data.ip;
		data.ip.family = AF_INET;
		if (!parse_ip_cidr(&data.ip, value))
			return NULL;

		break;
	case WGKEY_IPV6:
		len = sizeof data.ip;
		data.ip.family = AF_INET6;
		if (!parse_ip_cidr(&data.ip, value))
			return NULL;

		break;
	case WGKEY_LEASESTART:
	case WGKEY_LEASETIME:
	case WGKEY_ERRNO:
		len = sizeof data.uint32;
		uresult = strtoumax(value, &endptr, 10);
		if (uresult > UINT32_MAX || *endptr != '\0')
			return NULL;
		data.uint32 = (uint32_t)uresult;

		break;
	case WGKEY_ERRMSG:
		strncpy(data.errmsg, value, sizeof data.errmsg - 1);
		data.errmsg[sizeof data.errmsg - 1] = '\0';
		len = MIN(sizeof data.errmsg,
			  strlen(value) + 1); /* Copying the NUL byte too. */

		break;
	default:
		debug("Invalid key %d, aborting\n", key);
		BUG();
	}

	attr = malloc(sizeof(struct wg_dynamic_attr) + len);
	if (!attr)
		fatal("malloc()");

	attr->len = len;
	attr->key = key;
	attr->next = NULL;
	memcpy(&attr->value, &data, len);

	return attr;
}

static enum wg_dynamic_key parse_key(char *key)
{
	for (enum wg_dynamic_key e = 1; e < ARRAY_SIZE(WG_DYNAMIC_KEY); ++e)
		if (!strcmp(key, WG_DYNAMIC_KEY[e]))
			return e;

	return WGKEY_UNKNOWN;
}

/* Consumes one full line from buf, or up to MAX_LINESIZE-1 bytes if no newline
 * character was found.
 * If req != NULL then we expect to parse a command and will set cmd and version
 * of req accordingly, while *attr will be set to NULL.
 * Otherwise we expect to parse a normal key=value pair, that will be stored
 * in a newly allocated wg_dynamic_attr, pointed to by *attr.
 *
 * Return values:
 *   > 0 : Amount of bytes consumed (<= MAX_LINESIZE)
 *   < 0 : Error
 *   = 0 : End of message
 */
static ssize_t parse_line(unsigned char *buf, size_t len,
			  struct wg_dynamic_attr **attr,
			  struct wg_dynamic_request *req)
{
	unsigned char *line_end, *key_end;
	enum wg_dynamic_key key;
	ssize_t line_len;
	char *endptr;
	uintmax_t res;

	line_end = memchr(buf, '\n', len > MAX_LINESIZE ? MAX_LINESIZE : len);
	if (!line_end) {
		if (len >= MAX_LINESIZE)
			return -E2BIG;

		*attr = malloc(sizeof(struct wg_dynamic_attr) + len);
		if (!*attr)
			fatal("malloc()");

		(*attr)->key = WGKEY_INCOMPLETE;
		(*attr)->len = len;
		(*attr)->next = NULL;
		memcpy((*attr)->value, buf, len);

		return len;
	}

	if (line_end == buf)
		return 0; /* \n\n - end of message */

	*line_end = '\0';
	line_len = line_end - buf + 1;

	key_end = memchr(buf, '=', line_len - 1);
	if (!key_end)
		return -EINVAL;

	*key_end = '\0';
	key = parse_key((char *)buf);
	if (key == WGKEY_UNKNOWN)
		return -ENOENT;

	if (req) {
		if (key >= WGKEY_ENDCMD)
			return -ENOENT;

		*attr = NULL;
		res = strtoumax((char *)key_end + 1, &endptr, 10);

		if (res > UINT32_MAX || *endptr != '\0')
			return -EINVAL;

		req->cmd = key;
		req->version = (uint32_t)res;

		if (req->version != 1)
			return -EPROTONOSUPPORT;
	} else {
		if (key <= WGKEY_ENDCMD)
			return -ENOENT;

		*attr = parse_value(key, (char *)key_end + 1);
		if (!*attr)
			return -EINVAL;
	}

	return line_len;
}

static int parse_request(struct wg_dynamic_request *req, unsigned char *buf,
			 size_t len)
{
	struct wg_dynamic_attr *attr;
	size_t offset = 0;
	ssize_t ret;

	if (memchr(buf, '\0', len))
		return -EINVAL; /* don't allow null bytes */

	if (req->last && req->last->key == WGKEY_INCOMPLETE) {
		len += req->last->len;

		memmove(buf + req->last->len, buf, len);
		memcpy(buf, req->last->value, req->last->len);
		free(req->last);

		if (req->first == req->last) {
			req->first = NULL;
			req->last = NULL;
		} else {
			attr = req->first;
			while (attr->next != req->last)
				attr = attr->next;

			attr->next = NULL;
			req->last = attr;
		}
	}

	while (len > 0) {
		ret = parse_line(buf + offset, len, &attr,
				 req->cmd == WGKEY_UNKNOWN ? req : NULL);
		if (ret <= 0)
			return ret; /* either error or message complete */

		len -= ret;
		offset += ret;
		if (!attr)
			continue;

		if (!req->first)
			req->first = attr;
		else
			req->last->next = attr;

		req->last = attr;
	}

	return 1;
}

bool handle_request(struct wg_dynamic_request *req,
		    bool (*success)(struct wg_dynamic_request *),
		    bool (*error)(struct wg_dynamic_request *, int))
{
	ssize_t bytes;
	int ret;
	unsigned char buf[RECV_BUFSIZE + MAX_LINESIZE];

	while (1) {
		bytes = read(req->fd, buf, RECV_BUFSIZE);
		if (bytes < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN)
				break;

			// TODO: handle EINTR

			debug("Reading from socket %d failed: %s\n", req->fd,
			      strerror(errno));
			return true;
		} else if (bytes == 0) {
			debug("Peer disconnected unexpectedly\n");
			return true;
		}

		ret = parse_request(req, buf, bytes);
		if (ret < 0)
			return error(req, -ret);
		else if (ret == 0)
			return success(req);
	}

	return false;
}

bool send_message(struct wg_dynamic_request *req, const void *buf, size_t len)
{
	size_t offset = 0;

	while (1) {
		ssize_t written = write(req->fd, buf + offset, len - offset);
		if (written < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN)
				break;

			// TODO: handle EINTR

			debug("Writing to socket %d failed: %s\n", req->fd,
			      strerror(errno));
			return true;
		}

		offset += written;
		if (offset == len)
			return true;
	}

	debug("Socket %d blocking on write with %lu bytes left, postponing\n",
	      req->fd, len - offset);

	if (!req->buf) {
		req->buflen = len - offset;
		req->buf = malloc(req->buflen);
		if (!req->buf)
			fatal("malloc()");

		memcpy(req->buf, buf + offset, req->buflen);
	} else {
		req->buflen = len - offset;
		memmove(req->buf, buf + offset, req->buflen);
	}

	return false;
}

void print_to_buf(char *buf, size_t bufsize, size_t *offset, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + *offset, bufsize - *offset, fmt, ap);
	va_end(ap);

	if (n < 0) {
		fatal("vsnprintf()");
	} else if (n + *offset >= bufsize) {
		debug("Outbuffer too small: %d + %zu >= %zu\n", n, *offset,
		      bufsize);
		BUG();
	}

	*offset += n;
}

uint32_t current_time()
{
	struct timespec tp;
	if (clock_gettime(CLOCK_REALTIME, &tp))
		fatal("clock_gettime(CLOCK_REALTIME)");
	return tp.tv_sec;
}

void close_connection(struct wg_dynamic_request *req)
{
	struct wg_dynamic_attr *prev, *cur = req->first;

	if (close(req->fd))
		debug("Failed to close socket\n");

	while (cur) {
		prev = cur;
		cur = cur->next;
		free(prev);
	}

	req->cmd = WGKEY_UNKNOWN;
	req->version = 0;
	req->fd = -1;
	free(req->buf);
	req->buf = NULL;
	req->buflen = 0;
	req->first = NULL;
	req->last = NULL;
}

bool is_link_local(unsigned char *addr)
{
	/* TODO: check if the remaining 54 bits are 0 */
	return IN6_IS_ADDR_LINKLOCAL(addr);
}

void iface_get_all_addrs(uint8_t family, mnl_cb_t data_cb, void *cb_data)
{
	struct mnl_socket *nl;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	/* TODO: rtln-addr-dump from libmnl uses rtgenmsg here? */
	struct ifaddrmsg *ifaddr;
	int ret;
	unsigned int seq, portid;

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (nl == NULL)
		fatal("mnl_socket_open");

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0)
		fatal("mnl_socket_bind");

	/* You'd think that we could just request addresses from a specific
	 * interface, via NLM_F_MATCH or something, but we can't. See also:
	 * https://marc.info/?l=linux-netdev&m=132508164508217
	 */
	seq = time(NULL);
	portid = mnl_socket_get_portid(nl);
	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_GETADDR;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = seq;
	ifaddr = mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifaddrmsg));
	ifaddr->ifa_family = family;

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		fatal("mnl_socket_sendto");

	do {
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret <= MNL_CB_STOP)
			break;
		ret = mnl_cb_run(buf, ret, seq, portid, data_cb, cb_data);
	} while (ret > 0);

	if (ret == -1)
		fatal("mnl_cb_run/mnl_socket_recvfrom");

	mnl_socket_close(nl);
}

int data_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	/* skip unsupported attribute in user-space */
	if (mnl_attr_type_valid(attr, IFA_MAX) < 0)
		return MNL_CB_OK;

	switch (type) {
	case IFA_ADDRESS:
		if (mnl_attr_validate(attr, MNL_TYPE_BINARY) < 0) {
			perror("mnl_attr_validate");
			return MNL_CB_ERROR;
		}
		break;
	}
	tb[type] = attr;
	return MNL_CB_OK;
}
