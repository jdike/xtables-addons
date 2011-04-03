/* Copyright 2007-2010 Jozsef Kadlecsik (kadlec@blackhole.kfki.hu)
 *
 * This program is free software; you can redistribute it and/or modify   
 * it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation.
 */
#include <assert.h>				/* assert */
#include <errno.h>				/* errno */
#include <limits.h>				/* ULLONG_MAX */
#include <netdb.h>				/* getservbyname, getaddrinfo */
#include <stdlib.h>				/* strtoull, etc. */
#include <sys/types.h>				/* getaddrinfo */
#include <sys/socket.h>				/* getaddrinfo, AF_ */
#include <net/ethernet.h>			/* ETH_ALEN */
#include <netinet/in.h>				/* IPPROTO_ */

#include <libipset/debug.h>			/* D() */
#include <libipset/data.h>			/* IPSET_OPT_* */
#include <libipset/icmp.h>			/* name_to_icmp */
#include <libipset/icmpv6.h>			/* name_to_icmpv6 */
#include <libipset/pfxlen.h>			/* prefixlen_netmask_map */
#include <libipset/session.h>			/* ipset_err */
#include <libipset/types.h>			/* ipset_type_get */
#include <libipset/utils.h>			/* string utilities */
#include <libipset/parse.h>			/* prototypes */

#ifndef ULLONG_MAX
#define ULLONG_MAX	18446744073709551615ULL
#endif

/* Parse input data */

#define cidr_separator(str)	ipset_strchr(str, IPSET_CIDR_SEPARATOR)
#define range_separator(str)	ipset_strchr(str, IPSET_RANGE_SEPARATOR)
#define elem_separator(str)	ipset_strchr(str, IPSET_ELEM_SEPARATOR)
#define name_separator(str)	ipset_strchr(str, IPSET_NAME_SEPARATOR)
#define proto_separator(str)	ipset_strchr(str, IPSET_PROTO_SEPARATOR)

#define syntax_err(fmt, args...) \
	ipset_err(session, "Syntax error: " fmt , ## args)

static char *
ipset_strchr(const char *str, const char *sep)
{
	char *match;
	
	assert(str);
	assert(sep);
	
	for (; *sep != '\0'; sep++)
		if ((match = strchr(str, sep[0])) != NULL
		    && str[0] != sep[0]
		    && str[strlen(str)-1] != sep[0])
			return match;
	
	return NULL;
}

/* 
 * Parser functions, shamelessly taken from iptables.c, ip6tables.c 
 * and parser.c from libnetfilter_conntrack.
 */

/*
 * Parse numbers
 */
static int
string_to_number_ll(struct ipset_session *session,
		    const char *str, 
		    unsigned long long min,
		    unsigned long long max,
		    unsigned long long *ret)
{
	unsigned long long number = 0;
	char *end;

	/* Handle hex, octal, etc. */
	errno = 0;
	number = strtoull(str, &end, 0);
	if (*end == '\0' && end != str && errno != ERANGE) {
		/* we parsed a number, let's see if we want this */
		if (min <= number && (!max || number <= max)) {
			*ret = number;
			return 0;
		} else
			errno = ERANGE;
	}
	if (errno == ERANGE && max)
		return syntax_err("'%s' is out of range %llu-%llu",
				  str, min, max);
	else if (errno == ERANGE)
		return syntax_err("'%s' is out of range %llu-%llu",
				  str, min, ULLONG_MAX);
	else
		return syntax_err("'%s' is invalid as number", str);
}

static int
string_to_u8(struct ipset_session *session,
	     const char *str, uint8_t *ret)
{
	int err;
	unsigned long long num = 0;

	err = string_to_number_ll(session, str, 0, 255, &num);
	*ret = num;

	return err;
}

static int
string_to_cidr(struct ipset_session *session,
	       const char *str, uint8_t min, uint8_t max, uint8_t *ret)
{
	int err = string_to_u8(session, str, ret);
	
	if (!err && (*ret < min || *ret > max))
		return syntax_err("'%s' is out of range %u-%u",
				  str, min, max);

	return err;
}

static int
string_to_u16(struct ipset_session *session,
	      const char *str, uint16_t *ret)
{
	int err;
	unsigned long long num = 0;

	err = string_to_number_ll(session, str, 0, USHRT_MAX, &num);
	*ret = num;

	return err;
}

static int
string_to_u32(struct ipset_session *session,
	      const char *str, uint32_t *ret)
{
	int err;
	unsigned long long num = 0;

	err = string_to_number_ll(session, str, 0, UINT_MAX, &num);
	*ret = num;

	return err;
}

/**
 * ipset_parse_ether - parse ethernet address
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an ethernet address. The parsed ethernet
 * address is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_ether(struct ipset_session *session,
		  enum ipset_opt opt, const char *str)
{
	unsigned int i = 0;
	unsigned char ether[ETH_ALEN];
	
	assert(session);
	assert(opt == IPSET_OPT_ETHER);
	assert(str);

	if (strlen(str) != ETH_ALEN * 3 - 1)
		goto error;

	for (i = 0; i < ETH_ALEN; i++) {
		long number;
		char *end;

		number = strtol(str + i * 3, &end, 16);

		if (end == str + i * 3 + 2
		    && (*end == ':' || *end == '\0')
		    && number >= 0 && number <= 255)
			ether[i] = number;
		else
			goto error;
	}
	return ipset_session_data_set(session, opt, ether);

error:
	return syntax_err("cannot parse '%s' as ethernet address", str);
}

/*
 * Parse TCP service names or port numbers
 */
static int
parse_portname(struct ipset_session *session, const char *str,
	       uint16_t *port, const char *proto)
{
	struct servent *service;

	if ((service = getservbyname(str, proto)) != NULL) {
		*port = ntohs((uint16_t) service->s_port);
		return 0;
	}
	
	return syntax_err("cannot parse '%s' as a %s port", str, proto);
}

/**
 * ipset_parse_single_port - parse a single port number or name
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 * @proto: protocol
 *
 * Parse string as a single port number or name. The parsed port
 * number is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_port(struct ipset_session *session,
		 enum ipset_opt opt, const char *str,
		 const char *proto)
{
	uint16_t port;
	int err;

	assert(session);
	assert(opt == IPSET_OPT_PORT || opt == IPSET_OPT_PORT_TO);
	assert(str);

	if ((err = string_to_u16(session, str, &port)) == 0
	    || (err = parse_portname(session, str, &port, proto)) == 0)
		err = ipset_session_data_set(session, opt, &port);

	if (!err)
		/* No error, so reset false error messages! */
		ipset_session_report_reset(session);

	return err;
}

/**
 * ipset_parse_tcpudp_port - parse TCP/UDP port name, number, or range of them
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 * @proto: TCP|UDP
 *
 * Parse string as a TCP/UDP port name or number or range of them
 * separated by a dash. The parsed port numbers are stored
 * in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_tcpudp_port(struct ipset_session *session,
			enum ipset_opt opt, const char *str, const char *proto)
{
	char *a, *saved, *tmp;
	int err = 0;

	assert(session);
	assert(opt == IPSET_OPT_PORT);
	assert(str);

	saved = tmp = strdup(str);
	if (tmp == NULL)
		return ipset_err(session,
				 "Cannot allocate memory to duplicate %s.",
				 str);

	a = range_separator(tmp);
	if (a != NULL) {
		/* port-port */
		*a++ = '\0';
		err = ipset_parse_port(session, IPSET_OPT_PORT_TO, a, proto);
		if (err)
			goto error;
	}
	err = ipset_parse_port(session, opt, tmp, proto);

error:
	free(saved);
	return err;
}

/**
 * ipset_parse_tcp_port - parse TCP port name, number, or range of them
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a TCP port name or number or range of them
 * separated by a dash. The parsed port numbers are stored
 * in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_tcp_port(struct ipset_session *session,
		     enum ipset_opt opt, const char *str)
{
	return ipset_parse_tcpudp_port(session, opt, str, "TCP");
}

/**
 * ipset_parse_single_tcp_port - parse TCP port name or number
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a single TCP port name or number.
 * The parsed port number is stored
 * in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_single_tcp_port(struct ipset_session *session,
		     enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_PORT || opt == IPSET_OPT_PORT_TO);
	assert(str);

	return ipset_parse_port(session, opt, str, "TCP");
}

/**
 * ipset_parse_proto - parse protocol name
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a protocol name.
 * The parsed protocol is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_proto(struct ipset_session *session,
		  enum ipset_opt opt, const char *str)
{
	const struct protoent *protoent;
	uint8_t proto = 0;

	assert(session);
	assert(opt == IPSET_OPT_PROTO);
	assert(str);
	
	protoent = getprotobyname(strcasecmp(str, "icmpv6") == 0
				  ? "ipv6-icmp" : str);
	if (protoent == NULL)
		return syntax_err("cannot parse '%s' "
				  "as a protocol name", str);
	proto = protoent->p_proto;
	if (!proto)
		return syntax_err("Unsupported protocol '%s'", str);
	
	return ipset_session_data_set(session, opt, &proto);
}

/* Parse ICMP and ICMPv6 type/code */
static int
parse_icmp_typecode(struct ipset_session *session,
		    enum ipset_opt opt, const char *str,
		    const char *family)
{
	uint16_t typecode;
	uint8_t type, code;
	char *a, *saved, *tmp;
	int err;
		
	saved = tmp = strdup(str);
	if (tmp == NULL)
		return ipset_err(session,
				 "Cannot allocate memory to duplicate %s.",
				 str);
	a = cidr_separator(tmp);
	if (a == NULL) {
		free(saved);
		return ipset_err(session,
				 "Cannot parse %s as an %s type/code.", str, family);
	}
	*a++ = '\0';
	if ((err = string_to_u8(session, a, &type)) != 0
	    || (err = string_to_u8(session, tmp, &code)) != 0)
	    	goto error;
		
	typecode = (type << 8) | code;
	err = ipset_session_data_set(session, opt, &typecode);

error:
	free(saved);
	return err;
}

/**
 * ipset_parse_icmp - parse an ICMP name or type/code
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an ICMP name or type/code numbers.
 * The parsed ICMP type/code is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_icmp(struct ipset_session *session,
		 enum ipset_opt opt, const char *str)
{
	uint16_t typecode;

	assert(session);
	assert(opt == IPSET_OPT_PORT);
	assert(str);

	if (name_to_icmp(str, &typecode) < 0)
		return parse_icmp_typecode(session, opt, str, "ICMP");

	return ipset_session_data_set(session, opt, &typecode);
}

/**
 * ipset_parse_icmpv6 - parse an ICMPv6 name or type/code
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an ICMPv6 name or type/code numbers.
 * The parsed ICMPv6 type/code is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_icmpv6(struct ipset_session *session,
		   enum ipset_opt opt, const char *str)
{
	uint16_t typecode;

	assert(session);
	assert(opt == IPSET_OPT_PORT);
	assert(str);

	if (name_to_icmpv6(str, &typecode) < 0)
		return parse_icmp_typecode(session, opt, str, "ICMPv6");

	return ipset_session_data_set(session, opt, &typecode);
}

/**
 * ipset_parse_proto_port - parse (optional) protocol and a single port
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a protocol and port, separated by a colon.
 * The protocol part is optional.
 * The parsed protocol and port numbers are stored in the data
 * blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_proto_port(struct ipset_session *session,
		       enum ipset_opt opt, const char *str)
{
	struct ipset_data *data;
	char *a, *saved, *tmp;
	const char *proto;
	uint8_t p = IPPROTO_TCP;
	int err = 0;

	assert(session);
	assert(opt == IPSET_OPT_PORT);
	assert(str);

	data = ipset_session_data(session);
	saved = tmp = strdup(str);
	if (tmp == NULL)
		return ipset_err(session,
				 "Cannot allocate memory to duplicate %s.",
				 str);

	a = proto_separator(tmp);
	if (a != NULL) {
		uint8_t family = ipset_data_family(data);

		/* proto:port */
		*a++ = '\0';
		err = ipset_parse_proto(session, IPSET_OPT_PROTO, tmp);
		if (err)
			goto error;
		
		p = *(const uint8_t *) ipset_data_get(data, IPSET_OPT_PROTO);
		switch (p) {
		case IPPROTO_TCP:
			proto = tmp;
			tmp = a;
			goto parse_port;
		case IPPROTO_UDP:
			proto = tmp;
			tmp = a;
			goto parse_port;
		case IPPROTO_ICMP:
			if (family != AF_INET) {
				syntax_err("Protocol ICMP can be used with family INET only");
				goto error;
			}
			err = ipset_parse_icmp(session, opt, a);
			break;
		case IPPROTO_ICMPV6:
			if (family != AF_INET6) {
				syntax_err("Protocol ICMPv6 can be used with family INET6 only");
				goto error;
			}
			err = ipset_parse_icmpv6(session, opt, a);
			break;
		default:
			if (!STREQ(a, "0")) {
				syntax_err("Protocol %s can be used with pseudo port value 0 only.");
				goto error;
			}
			ipset_data_flags_set(data, IPSET_FLAG(opt));
		}
		goto error;
	} else {
		proto = "TCP";	
		err = ipset_data_set(data, IPSET_OPT_PROTO, &p);
		if (err)
			goto error;
	}
parse_port:
	err = ipset_parse_tcpudp_port(session, opt, tmp, proto);

error:
	free(saved);
	return err;
}

/**
 * ipset_parse_family - parse INET|INET6 family names
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an INET|INET6 family name.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_family(struct ipset_session *session,
		   enum ipset_opt opt, const char *str)
{
	struct ipset_data *data;
	uint8_t family;
	
	assert(session);
	assert(opt == IPSET_OPT_FAMILY);
	assert(str);

	data = ipset_session_data(session);
	if (ipset_data_flags_test(data, IPSET_FLAG(IPSET_OPT_FAMILY)))
		syntax_err("protocol family may not be specified "
			   "multiple times");

	if (STREQ(str, "inet") || STREQ(str, "ipv4") || STREQ(str, "-4"))
		family = AF_INET;
	else if (STREQ(str, "inet6") || STREQ(str, "ipv6") || STREQ(str, "-6"))
		family = AF_INET6;
	else if (STREQ(str, "any") || STREQ(str, "unspec"))
		family = AF_UNSPEC;
	else
		return syntax_err("unknown INET family %s", str);
				
	return ipset_data_set(data, opt, &family);
}

/*
 * Parse IPv4/IPv6 addresses, networks and ranges.
 * We resolve hostnames but just the first IP address is used.
 */
 
static struct addrinfo *
call_getaddrinfo(struct ipset_session *session, const char *str,
		 uint8_t family)
{
	struct addrinfo hints;
        struct addrinfo *res;
	int err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_CANONNAME;
        hints.ai_family = family;
        hints.ai_socktype = SOCK_RAW;
        hints.ai_protocol = 0;
        hints.ai_next = NULL;

        if ((err = getaddrinfo(str, NULL, &hints, &res)) != 0) {
        	syntax_err("cannot resolve '%s' to an %s address: %s",
        		   str, family == AF_INET6 ? "IPv6" : "IPv4",
        		   gai_strerror(err));
        	return NULL;
	} else
		return res;
}

static int
get_addrinfo(struct ipset_session *session,
	     enum ipset_opt opt,
	     const char *str,
	     struct addrinfo **info,
	     uint8_t family)
{
        struct addrinfo *i;
	size_t addrlen = family == AF_INET ? sizeof(struct sockaddr_in)
					   : sizeof(struct sockaddr_in6);
        int found, err = 0;

	if ((*info = call_getaddrinfo(session, str, family)) == NULL) {
		syntax_err("cannot parse %s: resolving to %s address failed",
			   str, family == AF_INET ? "IPv4" : "IPv6");
		return EINVAL;
	}

	for (i = *info, found = 0; i != NULL; i = i->ai_next) {
		if (i->ai_family != family || i->ai_addrlen != addrlen)
			continue;
		if (found == 0) {
			if (family == AF_INET) {
				/* Workaround: direct cast increases required alignment on Sparc */
				const struct sockaddr_in *saddr = (void *)i->ai_addr;
				err = ipset_session_data_set(session, opt, &saddr->sin_addr);
			} else {
				/* Workaround: direct cast increases required alignment on Sparc */
				const struct sockaddr_in6 *saddr = (void *)i->ai_addr;
				err = ipset_session_data_set(session, opt, &saddr->sin6_addr);
			}
		} else if (found == 1) {
			ipset_warn(session,
				   "%s resolves to multiple addresses: "
				   "using only the first one returned "
				   "by the resolver",
				   str);
		}
		found++;
	}
	if (found == 0)
		return syntax_err("cannot parse %s: "
				  "%s address could not be resolved",
				  str, family == AF_INET ? "IPv4" : "IPv6");
	return err;
}

static int
parse_ipaddr(struct ipset_session *session,
	     enum ipset_opt opt, const char *str,
	     uint8_t family)
{
        uint8_t m = family == AF_INET ? 32 : 128;
        int aerr = EINVAL, err = 0, range = 0;
        char *saved = strdup(str);
        char *a, *tmp = saved;
        struct addrinfo *info;
        enum ipset_opt copt = opt == IPSET_OPT_IP ? IPSET_OPT_CIDR
        			: IPSET_OPT_CIDR2;

	if (tmp == NULL)
		return ipset_err(session,
				 "Cannot allocate memory to duplicate %s.",
				 str);
	if ((a = cidr_separator(tmp)) != NULL) {
		/* IP/mask */
		*a++ = '\0';

		if ((err = string_to_cidr(session, a, 0, m, &m)) != 0
		    || (err = ipset_session_data_set(session, copt, &m)) != 0)
			goto out;
	} else if ((a = range_separator(tmp)) != NULL) {
		/* IP-IP */
		*a++ = '\0';
		D("range %s", a);
		range++;
	}
	if ((aerr = get_addrinfo(session, opt, tmp, &info, family)) != 0
	    || !range)
		goto out;
	freeaddrinfo(info);
	aerr = get_addrinfo(session, IPSET_OPT_IP_TO, a, &info, family);

out:
	if (aerr != EINVAL)
		/* getaddrinfo not failed */
		freeaddrinfo(info);
	else if (aerr)
		err = -1;
	free(saved);
	return err;
} 

enum ipaddr_type {
	IPADDR_ANY,
	IPADDR_PLAIN,
	IPADDR_NET,
	IPADDR_RANGE,
};

static inline bool
cidr_hostaddr(const char *str, uint8_t family)
{
	char *a = cidr_separator(str);
	
	return family == AF_INET ? STREQ(a, "/32") : STREQ(a, "/128");
}

static int
parse_ip(struct ipset_session *session,
	 enum ipset_opt opt, const char *str, enum ipaddr_type addrtype)
{
	struct ipset_data *data = ipset_session_data(session);
	uint8_t family = ipset_data_family(data);

	if (family == AF_UNSPEC) {
		family = AF_INET;
		ipset_data_set(data, IPSET_OPT_FAMILY, &family);
	}

	switch (addrtype) {
	case IPADDR_PLAIN:
		if (range_separator(str)
		    || (cidr_separator(str) && !cidr_hostaddr(str, family)))
			return syntax_err("plain IP address must be supplied: %s",
					  str);
		break;
	case IPADDR_NET:
		if (!cidr_separator(str) || range_separator(str))
			return syntax_err("IP/netblock must be supplied: %s",
					  str);
		break;
	case IPADDR_RANGE:
		if (!range_separator(str) || cidr_separator(str))
			return syntax_err("IP-IP range must supplied: %s",
					  str);
		break;
	case IPADDR_ANY:
	default:
		break;
	}

	return parse_ipaddr(session, opt, str, family);
}

/**
 * ipset_parse_ip - parse IPv4|IPv6 address, range or netblock
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 address or address range
 * or netblock. Hostnames are resolved. If family is not set
 * yet in the data blob, INET is assumed.
 * The values are stored in the data blob of the session.
 *
 * FIXME: if the hostname resolves to multiple addresses,
 * the first one is used only.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_ip(struct ipset_session *session,
	       enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_IP || opt == IPSET_OPT_IP2);
	assert(str);

	return parse_ip(session, opt, str, IPADDR_ANY);
}

/**
 * ipset_parse_single_ip - parse a single IPv4|IPv6 address
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 address or hostname. If family 
 * is not set yet in the data blob, INET is assumed.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_single_ip(struct ipset_session *session,
		      enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_IP
	       || opt == IPSET_OPT_IP_TO
	       || opt == IPSET_OPT_IP2);
	assert(str);

	return parse_ip(session, opt, str, IPADDR_PLAIN);
}

/**
 * ipset_parse_net - parse IPv4|IPv6 address/cidr
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 address/cidr pattern. If family 
 * is not set yet in the data blob, INET is assumed.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_net(struct ipset_session *session,
		enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_IP || opt == IPSET_OPT_IP2);
	assert(str);

	return parse_ip(session, opt, str, IPADDR_NET);
}

/**
 * ipset_parse_range - parse IPv4|IPv6 ranges
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 range separated by a dash. If family
 * is not set yet in the data blob, INET is assumed.
 * The values are stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_range(struct ipset_session *session,
		  enum ipset_opt opt ASSERT_UNUSED, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_IP || opt == IPSET_OPT_IP2);
	assert(str);

	return parse_ip(session, IPSET_OPT_IP, str, IPADDR_RANGE);
}

/**
 * ipset_parse_netrange - parse IPv4|IPv6 address/cidr or range
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 address/cidr pattern or a range
 * of addresses separated by a dash. If family is not set yet in
 * the data blob, INET is assumed.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_netrange(struct ipset_session *session,
		     enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_IP || opt == IPSET_OPT_IP2);
	assert(str);

	if (!(range_separator(str) || cidr_separator(str)))
		return syntax_err("IP/cidr or IP-IP range must be specified: %s",
				  str);
	return parse_ip(session, opt, str, IPADDR_ANY);
}

/**
 * ipset_parse_iprange - parse IPv4|IPv6 address or range
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 address pattern or a range
 * of addresses separated by a dash. If family is not set yet in
 * the data blob, INET is assumed.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_iprange(struct ipset_session *session,
		    enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_IP || opt == IPSET_OPT_IP2);
	assert(str);

	if (cidr_separator(str))
		return syntax_err("IP address or IP-IP range must be specified: %s",
				  str);
	return parse_ip(session, opt, str, IPADDR_ANY);
}

/**
 * ipset_parse_ipnet - parse IPv4|IPv6 address or address/cidr pattern
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 address or address/cidr pattern.
 * If family is not set yet in the data blob, INET is assumed.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_ipnet(struct ipset_session *session,
		  enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_OPT_IP || opt == IPSET_OPT_IP2);
	assert(str);

	if (range_separator(str))
		return syntax_err("IP address or IP/cidr must be specified: %s",
				  str);
	return parse_ip(session, opt, str, IPADDR_ANY);
}

/**
 * ipset_parse_ip4_single6 - parse IPv4 address, range or netblock or IPv6 address
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4 address or address range
 * or netblock or and IPv6 address. Hostnames are resolved. If family
 * is not set yet in the data blob, INET is assumed.
 * The values are stored in the data blob of the session.
 *
 * FIXME: if the hostname resolves to multiple addresses,
 * the first one is used only.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_ip4_single6(struct ipset_session *session,
			enum ipset_opt opt, const char *str)
{
	struct ipset_data *data;
	uint8_t family;

	assert(session);
	assert(opt == IPSET_OPT_IP || opt == IPSET_OPT_IP2);
	assert(str);
	
	data = ipset_session_data(session);
	family = ipset_data_family(data);
	
	if (family == AF_UNSPEC) {
		family = AF_INET;
		ipset_data_set(data, IPSET_OPT_FAMILY, &family);
	}
	
	return family == AF_INET ? ipset_parse_ip(session, opt, str)
				 : ipset_parse_single_ip(session, opt, str);

}

/**
 * ipset_parse_iptimeout - parse IPv4|IPv6 address and timeout
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an IPv4|IPv6 address and timeout parameter.
 * If family is not set yet in the data blob, INET is assumed.
 * The value is stored in the data blob of the session.
 *
 * Compatibility parser.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_iptimeout(struct ipset_session *session,
		      enum ipset_opt opt, const char *str)
{
	char *tmp, *saved, *a;
	int err;

	assert(session);
	assert(opt == IPSET_OPT_IP);
	assert(str);

	/* IP,timeout */
	if (ipset_data_flags_test(ipset_session_data(session),
				  IPSET_FLAG(IPSET_OPT_TIMEOUT)))
		return syntax_err("mixed syntax, timeout already specified");
		 
	tmp = saved = strdup(str);	
	if (saved == NULL)
		return ipset_err(session,
				 "Cannot allocate memory to duplicate %s.",
				 str);

	a = elem_separator(tmp);
	if (a == NULL) {
		free(saved);
		return syntax_err("Missing separator from %s", str);
	}
	*a++ = '\0';
	err = parse_ip(session, opt, tmp, IPADDR_ANY);
	if (!err)
		err = ipset_parse_uint32(session, IPSET_OPT_TIMEOUT, a);

	free(saved);
	return err;
}

#define check_setname(str, saved)					\
do {									\
    if (strlen(str) > IPSET_MAXNAMELEN - 1) {				\
    	if (saved != NULL)						\
    		free(saved);						\
	return syntax_err("setname '%s' is longer than %u characters",	\
			  str, IPSET_MAXNAMELEN - 1);			\
    }									\
} while (0)


/**
 * ipset_parse_name_compat - parse setname as element
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a setname or a setname element to add to a set.
 * The pattern "setname,before|after,setname" is recognized and
 * parsed.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_name_compat(struct ipset_session *session,
			enum ipset_opt opt, const char *str)
{
	char *saved;
	char *a = NULL, *b = NULL, *tmp;
	int err, before = 0;
	const char *sep = IPSET_ELEM_SEPARATOR;
	struct ipset_data *data;

	assert(session);
	assert(opt == IPSET_OPT_NAME);
	assert(str);

	data = ipset_session_data(session);
	if (ipset_data_flags_test(data, IPSET_FLAG(IPSET_OPT_NAMEREF)))
		syntax_err("mixed syntax, before|after option already used");

	tmp = saved = strdup(str);	
	if (saved == NULL)
		return ipset_err(session,
				 "Cannot allocate memory to duplicate %s.",
				 str);
	if ((a = elem_separator(tmp)) != NULL) {
		/* setname,[before|after,setname */
		*a++ = '\0';
		if ((b = elem_separator(a)) != NULL)
			*b++ = '\0';
		if (b == NULL
		    || !(STREQ(a, "before") || STREQ(a, "after"))) {
			err = ipset_err(session, "you must specify elements "
					"as setname%s[before|after]%ssetname",
					sep, sep);
			goto out;
		}
		before = STREQ(a, "before");
	}
	check_setname(tmp, saved);
	if ((err = ipset_data_set(data, opt, tmp)) != 0 || b == NULL)
		goto out;

	check_setname(b, saved);
	if ((err = ipset_data_set(data,
				  IPSET_OPT_NAMEREF, b)) != 0)
		goto out;

	if (before)
		err = ipset_data_set(data, IPSET_OPT_BEFORE, &before);

out:
	free(saved);
	return err;
}

/**
 * ipset_parse_setname - parse string as a setname
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a setname.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_setname(struct ipset_session *session,
		    enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(opt == IPSET_SETNAME
	       || opt == IPSET_OPT_NAME
	       || opt == IPSET_OPT_SETNAME2);
	assert(str);

	check_setname(str, NULL);

	return ipset_session_data_set(session, opt, str);
}

/**
 * ipset_parse_before - parse string as "before" reference setname
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a "before" reference setname for list:set
 * type of sets. The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_before(struct ipset_session *session,
		   enum ipset_opt opt, const char *str)
{
	struct ipset_data *data;

	assert(session);
	assert(opt == IPSET_OPT_NAMEREF);
	assert(str);

	data = ipset_session_data(session);
	if (ipset_data_flags_test(data, IPSET_FLAG(IPSET_OPT_NAMEREF)))
		syntax_err("mixed syntax, before|after option already used");

	check_setname(str, NULL);
	ipset_data_set(data, IPSET_OPT_BEFORE, str);

	return ipset_data_set(data, opt, str);
}

/**
 * ipset_parse_after - parse string as "after" reference setname
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a "after" reference setname for list:set
 * type of sets. The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_after(struct ipset_session *session,
		   enum ipset_opt opt, const char *str)
{
	struct ipset_data *data;

	assert(session);
	assert(opt == IPSET_OPT_NAMEREF);
	assert(str);

	data = ipset_session_data(session);
	if (ipset_data_flags_test(data, IPSET_FLAG(IPSET_OPT_NAMEREF)))
		syntax_err("mixed syntax, before|after option already used");

	check_setname(str, NULL);

	return ipset_data_set(data, opt, str);
}

/**
 * ipset_parse_uint32 - parse string as an unsigned integer
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an unsigned integer number.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_uint32(struct ipset_session *session,
		   enum ipset_opt opt, const char *str)
{
	uint32_t value;
	int err;
	
	assert(session);
	assert(str);

	if ((err = string_to_u32(session, str, &value)) == 0)
		return ipset_session_data_set(session, opt, &value);
	
	return err;
}

/**
 * ipset_parse_uint8 - parse string as an unsigned short integer
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as an unsigned short integer number.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_uint8(struct ipset_session *session,
		  enum ipset_opt opt, const char *str)
{
	uint8_t value;
	int err;
	
	assert(session);
	assert(str);

	if ((err = string_to_u8(session, str, &value)) == 0)
		return ipset_session_data_set(session, opt, &value);

	return err;
}

/**
 * ipset_parse_netmask - parse string as a CIDR netmask value
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a CIDR netmask value, depending on family type.
 * If family is not set yet, INET is assumed.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_netmask(struct ipset_session *session,
		    enum ipset_opt opt, const char *str)
{
	uint8_t family, cidr;
	struct ipset_data *data;
	int err = 0;
	
	assert(session);
	assert(opt == IPSET_OPT_NETMASK);
	assert(str);

	data = ipset_session_data(session);
	family = ipset_data_family(data);
	if (family == AF_UNSPEC) {
		family = AF_INET;
		ipset_data_set(data, IPSET_OPT_FAMILY, &family);
	}

	err = string_to_cidr(session, str,
			     family == AF_INET ? 1 : 4, 
			     family == AF_INET ? 31 : 124,
			     &cidr);

	if (err)
		return syntax_err("netmask is out of the inclusive range "
				  "of %u-%u",
				  family == AF_INET ? 1 : 4,
				  family == AF_INET ? 31 : 124);

	return ipset_data_set(data, opt, &cidr);
}

/**
 * ipset_parse_flag - "parse" option flags
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse option flags :-)
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_flag(struct ipset_session *session,
		 enum ipset_opt opt, const char *str UNUSED)
{
	assert(session);
	
	return ipset_session_data_set(session, opt, NULL);
}

/**
 * ipset_parse_type - parse ipset type name
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse ipset module type: supports both old and new formats.
 * The type name is looked up and the type found is stored
 * in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_typename(struct ipset_session *session,
		     enum ipset_opt opt ASSERT_UNUSED, const char *str)
{
	const struct ipset_type *type;
	const char *typename;

	assert(session);
	assert(opt == IPSET_OPT_TYPENAME);
	assert(str);

	if (strlen(str) > IPSET_MAXNAMELEN - 1)
		return syntax_err("typename '%s' is longer than %u characters",
				  str, IPSET_MAXNAMELEN - 1);

	/* Find the corresponding type */
	typename = ipset_typename_resolve(str);
	if (typename == NULL)
		return syntax_err("typename '%s' is unkown", str);
	ipset_session_data_set(session, IPSET_OPT_TYPENAME, typename);
	type = ipset_type_get(session, IPSET_CMD_CREATE);

	if (type == NULL)
		return -1;
	
	return ipset_session_data_set(session, IPSET_OPT_TYPE, type);
}

/**
 * ipset_parse_output - parse output format name
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse output format names and set session mode.
 * The value is stored in the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_output(struct ipset_session *session,
		   int opt UNUSED, const char *str)
{
	assert(session);
	assert(str);

	if (STREQ(str, "plain"))
		return ipset_session_output(session, IPSET_LIST_PLAIN);
	else if (STREQ(str, "xml"))
		return ipset_session_output(session, IPSET_LIST_XML);
	else if (STREQ(str, "save"))
		return ipset_session_output(session, IPSET_LIST_SAVE);

	return syntax_err("unkown output mode '%s'", str);
}

/**
 * ipset_parse_ignored - "parse" ignored option
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Ignore deprecated options. A single warning is generated
 * for every ignored opton.
 *
 * Returns 0.
 */
int
ipset_parse_ignored(struct ipset_session *session,
		    enum ipset_opt opt, const char *str)
{
	assert(session);
	assert(str);

	if (!ipset_data_ignored(ipset_session_data(session), opt))
		ipset_warn(session,
			   "Option %s is ignored. Please upgrade your syntax.", str);

	return 0;
}

/**
 * ipset_call_parser - call a parser function
 * @session: session structure
 * @parsefn: parser function
 * @optstr: option name
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Wrapper to call the parser functions so that ignored options
 * are handled properly.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_call_parser(struct ipset_session *session,
		  ipset_parsefn parse, const char *optstr,
		  enum ipset_opt opt, const char *str)
{
	if (ipset_data_flags_test(ipset_session_data(session),
				  IPSET_FLAG(opt)))
		syntax_err("%s already specified", optstr);

	return parse(session, opt, parse == ipset_parse_ignored
				   ? optstr : str);
}

#define parse_elem(s, t, d, str)					\
do { 									\
	if (!(t)->elem[d].parse)					\
		goto internal;						\
	ret = (t)->elem[d].parse(s, (t)->elem[d].opt, str);		\
	if (ret)							\
		goto out;						\
} while (0)

#define elem_syntax_err(fmt, args...)	\
do {					\
	free(saved);			\
	return syntax_err(fmt , ## args);\
} while (0)

/**
 * ipset_parse_elem - parse ADT elem, depending on settype
 * @session: session structure
 * @opt: option kind of the data
 * @str: string to parse
 *
 * Parse string as a (multipart) element according to the settype.
 * The value is stored in the data blob of the session.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_parse_elem(struct ipset_session *session,
		 enum ipset_opt optional, const char *str)
{
	const struct ipset_type *type;
	char *a = NULL, *b = NULL, *tmp, *saved;
	int ret;

	assert(session);
	assert(str);

	type = ipset_session_data_get(session, IPSET_OPT_TYPE);
	if (!type)
		return ipset_err(session,
				 "Internal error: set type is unknown!");

	saved = tmp = strdup(str);
	if (tmp == NULL)
		return ipset_err(session,
				 "Cannot allocate memory to duplicate %s.",
				 str);

	a = elem_separator(tmp);
	if (type->dimension > IPSET_DIM_ONE) {
		if (a != NULL) {
			/* elem,elem */
			*a++ = '\0';
		} else if (!optional)
			elem_syntax_err("Second element is missing from %s.",
					str);
	} else if (a != NULL) {
		if (type->compat_parse_elem) {
			ret = type->compat_parse_elem(session,
						      type->elem[IPSET_DIM_ONE].opt,
						      saved);
			goto out;
		}
		elem_syntax_err("Elem separator in %s, "
				"but settype %s supports none.",
				str, type->name);
	}

	if (a)
		b = elem_separator(a);
	if (type->dimension > IPSET_DIM_TWO) {
		if (b != NULL) {
			/* elem,elem,elem */
			*b++ = '\0';
		} else if (!optional)
			elem_syntax_err("Third element is missing from %s.",
					str);
	} else if (b != NULL)
		elem_syntax_err("Two elem separators in %s, "
				"but settype %s supports one.",
				str, type->name);
	if (b != NULL && elem_separator(b))
		elem_syntax_err("Three elem separators in %s, "
				"but settype %s supports two.",
				str, type->name);

	D("parse elem part one: %s", tmp);
	parse_elem(session, type, IPSET_DIM_ONE, tmp);

	if (type->dimension > IPSET_DIM_ONE && a != NULL) {
		D("parse elem part two: %s", a);
		parse_elem(session, type, IPSET_DIM_TWO, a);
	}
	if (type->dimension > IPSET_DIM_TWO && b != NULL)
		parse_elem(session, type, IPSET_DIM_THREE, b);

	goto out;

internal:
	ret = ipset_err(session,
			"Internal error: missing parser function for %s",
			type->name);
out:
	free(saved);
	return ret;
}