#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "recli.h"

static ssize_t parse_boolean(const char *buffer, const char **error)
{
	if (strcmp(buffer, "on") == 0) return 1;
	if (strcmp(buffer, "off") == 0) return 1;

	if (strcmp(buffer, "1") == 0) return 1;
	if (strcmp(buffer, "0") == 0) return 1;

	*error = "Invalid value for boolean";
	return 0;
}

static ssize_t parse_integer(const char *buffer, const char **error)
{
	long part;
	char *end;
	
	part = strtol(buffer, &end, 10);

	if ((part == LONG_MIN) || (part == LONG_MAX)) {
		*error = "Integer value is out of bounds";
		return 0;
	}

	if (*end) {
		*error = "Unexpected text after decimal integer";
		return 0;
	}

	return 1;
}

static ssize_t parse_ipprefix(const char *buffer, UNUSED const char **error)
{
	char c;
	int num, parts[5];

	num = sscanf(buffer, "%d.%d.%d.%d/%d%c", &parts[0], &parts[1],
		     &parts[2], &parts[3], &parts[4], &c);
	if (num != 5) {
		return 0;
	}
	for (num = 0; num < 4; num++) {
		if (parts[num] < 0) return 0;
		if (parts[num] > 255) return 0;
	}

	if ((parts[4] < 0) || (parts[4] > 32)) return 0;

	return 1;
}

static ssize_t parse_ipv4addr(const char *buffer, UNUSED const char **error)
{
	char c;
	int num, parts[4];

	num = sscanf(buffer, "%d.%d.%d.%d%c", &parts[0], &parts[1],
		     &parts[2], &parts[3], &c);
	if (num != 4) {
		return 0;
	}
	for (num = 0; num < 4; num++) {
		if (parts[num] < 0) return 0;
		if (parts[num] > 255) return 0;
	}

	return 1;
}

/*
 *	@todo - make this better
 */
static ssize_t parse_ipv6addr(const char *buffer, const char **error)
{
	char const *p;

	for (p = buffer; *p; p++) {
		if (*p == ':') continue;
		if ((*p >= '0') && (*p <= '9')) continue;
		if ((*p >= 'a') && (*p <= 'f')) continue;
		if ((*p >= 'A') && (*p <= 'F')) continue;

		*error = "Invalid character in IPv6 address";
		return 0;
	}

	return 1;
}


static ssize_t parse_ipaddr(const char *buffer, const char **error)
{
	if (parse_ipv4addr(buffer, error) == 1) return 1;

	if (parse_ipv6addr(buffer, error) == 1) return 1;

	*error = "Invalid syntax for IP address";
	return 0;
}


static ssize_t parse_macaddr(const char *buffer, UNUSED const char **error)
{
	char c;
	int num, parts[6];

	num = sscanf(buffer, "%x:%x:%x:%x:%x:%x%c",
		     &parts[0], &parts[1], &parts[2],
		       &parts[3], &parts[4], &parts[5], &c);
	if (num != 6) {
		return 0;
	}
	for (num = 0; num < 6; num++) {
		if (parts[num] < 0) return 0;
		if (parts[num] > 255) return 0;
	}

	return 1;
}


static ssize_t parse_label(const char *buffer, const char **error)
{
	char const *p;

	if (*buffer == '.') {
		*error = "Too many '.'";
		return 0;
	}

	for (p = buffer; *p && (*p != '.'); p++) {
		if ((p - buffer) > 63) {
			*error = "Label is too long";
			return 0;
		}

		if (*p == '-') continue;
		if ((*p >= '0') && (*p <= '9')) continue;
		if ((*p >= 'a') && (*p <= 'z')) continue;
		if ((*p >= 'Z') && (*p <= 'Z')) continue;

		*error = "Invalid character in host name";
		return 0;
	}

	return p - buffer;
}


/*
 *	Each element of the hostname must be from 1 to 63 characters
 *	long and the entire hostname, including the dots, can be at
 *	most 253 characters long. Valid characters for hostnames are
 *	ASCII(7) letters from a to z, the digits from 0 to 9, and the
 *	hyphen (-). A hostname may not start with a hyphen.
 */
static ssize_t parse_hostname(const char *buffer, const char **error)
{
	ssize_t label, total;
	char const *p;

	if (*buffer == '-') {
		*error = "Host names cannot begin with '-'";
		return 0;
	}

	/*
	 *	Bare '.' is allowed.
	 */
	if ((*buffer == '.') && !buffer[1]) return 1;

	/*
	 *	Enforce limits on labels and total length.
	 */
	total = 0;
	p = buffer;
	while (*p) {
		label = parse_label(p, error);
		if (!label) return 0;

		p += label;
		if (!*p) break;

		p++;		/* MUST be a '.' */
		total += label + 1;

		if (total > 253) {
			*error = "Host name is too long";
			return 0;
		}
	}

	return 1;
}


static ssize_t parse_string(const char *buffer, UNUSED const char **error)
{
	if ((*buffer == '"') || (*buffer == '\'') || (*buffer == '`')) {
		if (strquotelen(buffer) > 0) return 1;
		return 0;
	}

	return 1;
}

static ssize_t parse_dqstring(const char *buffer, UNUSED const char **error)
{
	if (*buffer != '"') return 0;

	return parse_string(buffer, error);
}

static ssize_t parse_sqstring(const char *buffer, UNUSED const char **error)
{
	if (*buffer != '\'') return 0;

	return parse_string(buffer, error);
}

static ssize_t parse_bqstring(const char *buffer, UNUSED const char **error)
{
	if (*buffer != '`') return 0;

	return parse_string(buffer, error);
}

recli_datatype_t recli_datatypes[] = {
	{ "BOOLEAN", parse_boolean },
	{ "HOSTNAME", parse_hostname },
	{ "INTEGER", parse_integer },
	{ "IPADDR", parse_ipaddr },
	{ "IPPREFIX", parse_ipprefix },
	{ "IPV4ADDR", parse_ipv4addr },
	{ "IPV6ADDR", parse_ipv6addr },
	{ "MACADDR", parse_macaddr },
	{ "STRING", parse_string },
	{ "DQSTRING", parse_dqstring },
	{ "SQSTRING", parse_sqstring },
	{ "BQSTRING", parse_bqstring },

	{ NULL, NULL }
};

int recli_datatypes_init(void)
{
	int i;
	
	for (i = 0; recli_datatypes[i].name != NULL; i++) {
		if (!syntax_parse_add(recli_datatypes[i].name,
				      recli_datatypes[i].parse)) {
			return -1;
		}
	}

	return 0;
}
