#ifndef _DATATYPES_H
#define _DATATYPES_H

extern int syntax_parse_integer(const char *buffer);
extern int syntax_parse_ipaddr(const char *buffer);
extern int syntax_parse_ipv6addr(const char *buffer);
extern int syntax_parse_macaddr(const char *buffer);
extern int syntax_parse_string(const char *buffer);
extern int syntax_parse_boolean(const char *buffer);

#endif	/* _DATATYPES_H */
