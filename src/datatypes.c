#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recli.h"

static int parse_boolean(const char *buffer)
{
	if (strcmp(buffer, "on") == 0) return 1;
	if (strcmp(buffer, "off") == 0) return 1;

	if (strcmp(buffer, "1") == 0) return 1;
	if (strcmp(buffer, "0") == 0) return 1;


	return 0;
}

static int parse_integer(const char *buffer)
{
	int part;
	char *end;
	
	part = strtol(buffer, &end, 10);
	if (*end) return 0;

	return 1;
}

static int parse_ipaddr(const char *buffer)
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
 *	This is broken.
 */
static int parse_ipv6addr(const char *buffer)
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

static int parse_macaddr(const char *buffer)
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

static int parse_string(const char *buffer)
{
	if ((*buffer == '"') || (*buffer == '\'') || (*buffer == '`')) {
		if (strquotelen(buffer) > 0) return 1;
		return 0;
	}

	return 1;
}

static int parse_dqstring(const char *buffer)
{
	if (*buffer != '"') return 0;

	return parse_string(buffer);
}

static int parse_sqstring(const char *buffer)
{
	if (*buffer != '\'') return 0;

	return parse_string(buffer);
}

static int parse_bqstring(const char *buffer)
{
	if (*buffer != '`') return 0;

	return parse_string(buffer);
}

recli_datatype_t recli_datatypes[] = {
	{ "BOOLEAN", parse_boolean },
	{ "INTEGER", parse_integer },
	{ "IPADDR", parse_ipaddr },
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
