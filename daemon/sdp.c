#include <glib.h>
#include <netinet/in.h> 
#include <netinet/ip.h> 
#include <arpa/inet.h>

#include "sdp.h"
#include "call.h"
#include "log.h"
#include "str.h"
#include "call.h"

struct network_address {
	str network_type;
	str address_type;
	str address;
	struct in6_addr parsed;
};

struct sdp_origin {
	str username;
	str session_id;
	str version;
	struct network_address address;
	int parsed:1;
};

struct sdp_connection {
	struct network_address address;
	int parsed:1;
};

struct sdp_attributes {
	GQueue list;
	GHashTable *hash;
};

struct sdp_session {
	str s;
	struct sdp_origin origin;
	struct sdp_connection connection;
	struct sdp_attributes attributes;
	GQueue media_streams;
};

struct sdp_media {
	str s;
	str media_type;
	str port;
	str transport;
	/* ... format list */

	long int port_num;
	int port_count;

	struct sdp_connection connection;
	struct sdp_attributes attributes;
};

struct sdp_attribute {
	str full_line,	/* including a= and \r\n */
	    line_value,	/* without a= and without \r\n */
	    name,	/* just "rtpmap" */
	    value,	/* just "8 PCMA/8000" */
	    key,	/* "rtpmap:8" */
	    param;	/* "PCMA/8000" */
};




static const char ice_chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";



/* hack hack */
static inline int inet_pton_str(int af, str *src, void *dst) {
	char *s = src->s;
	char p;
	int ret;
	p = s[src->len];
	s[src->len] = '\0';
	ret = inet_pton(af, src->s, dst);
	s[src->len] = p;
	return ret;
}

static int parse_address(struct network_address *address) {
	struct in_addr in4;

	if (address->network_type.len != 2)
		return -1;
	if (memcmp(address->network_type.s, "IN", 2)
			&& memcmp(address->network_type.s, "in", 2))
		return -1;
	if (address->address_type.len != 3)
		return -1;
	if (!memcmp(address->address_type.s, "IP4", 3)
			|| !memcmp(address->address_type.s, "ip4", 3)) {
		if (inet_pton_str(AF_INET, &address->address, &in4) != 1)
			return -1;
		in4_to_6(&address->parsed, in4.s_addr);
	}
	else if (!memcmp(address->address_type.s, "IP6", 3)
			|| !memcmp(address->address_type.s, "ip6", 3)) {
		if (inet_pton_str(AF_INET6, &address->address, &address->parsed) != 1)
			return -1;
	}
	else
		return -1;

	return 0;
}

static inline int extract_token(char **sp, char *end, str *out) {
	char *space;

	out->s = *sp;
	space = memchr(*sp, ' ', end - *sp);
	if (space == *sp || end == *sp)
		return -1;

	if (!space) {
		out->len = end - *sp;
		*sp = end;
	}
	else {
		out->len = space - *sp;
		*sp = space + 1;
	}
	return 0;
	
}
#define EXTRACT_TOKEN(field) if (extract_token(&start, end, &output->field)) return -1
#define EXTRACT_NETWORK_ADDRESS(field) \
	EXTRACT_TOKEN(field.network_type); \
	EXTRACT_TOKEN(field.address_type); \
	EXTRACT_TOKEN(field.address); \
	if (parse_address(&output->address)) return -1

static int parse_origin(char *start, char *end, struct sdp_origin *output) {
	if (output->parsed)
		return -1;

	EXTRACT_TOKEN(username);
	EXTRACT_TOKEN(session_id);
	EXTRACT_TOKEN(version);
	EXTRACT_NETWORK_ADDRESS(address);

	output->parsed = 1;
	return 0;
}

static int parse_connection(char *start, char *end, struct sdp_connection *output) {
	if (output->parsed)
		return -1;

	EXTRACT_NETWORK_ADDRESS(address);

	output->parsed = 1;
	return 0;
}

static int parse_media(char *start, char *end, struct sdp_media *output) {
	char *ep;

	EXTRACT_TOKEN(media_type);
	EXTRACT_TOKEN(port);
	EXTRACT_TOKEN(transport);

	output->port_num = strtol(output->port.s, &ep, 10);
	if (ep == output->port.s)
		return -1;
	if (output->port_num <= 0 || output->port_num > 0xffff)
		return -1;

	if (*ep == '/') {
		output->port_count = atoi(ep + 1);
		if (output->port_count <= 0)
			return -1;
		if (output->port_count > 10) /* unsupported */
			return -1;
	}
	else
		output->port_count = 1;

	return 0;
}

static void attrs_init(struct sdp_attributes *a) {
	g_queue_init(&a->list);
	a->hash = g_hash_table_new(str_hash, str_equal);
}

int sdp_parse(str *body, GQueue *sessions) {
	char *b, *end, *value, *line_end, *next_line;
	struct sdp_session *session = NULL;
	struct sdp_media *media = NULL;
	const char *errstr;
	struct sdp_attributes *attrs;
	struct sdp_attribute *attr;
	str *adj_s;

	b = body->s;
	end = str_end(body);

	while (b && b < end - 1) {
		errstr = "Missing '=' sign";
		if (b[1] != '=')
			goto error;

		value = &b[2];
		line_end = memchr(value, '\n', end - value);
		if (!line_end) {
			/* assume missing LF at end of body */
			line_end = end;
			next_line = NULL;
		}
		else {
			next_line = line_end + 1;
			if (line_end[-1] == '\r')
				line_end--;
		}

		switch (b[0]) {
			case 'v':
				errstr = "Error in v= line";
				if (line_end != value + 1)
					goto error;
				if (value[0] != '0')
					goto error;

				session = g_slice_alloc0(sizeof(*session));
				g_queue_init(&session->media_streams);
				attrs_init(&session->attributes);
				g_queue_push_tail(sessions, session);
				media = NULL;
				session->s.s = b;

				break;

			case 'o':
				errstr = "o= line found within media section";
				if (media)
					goto error;
				errstr = "Error parsing o= line";
				if (parse_origin(value, line_end, &session->origin))
					goto error;

				break;

			case 'm':
				media = g_slice_alloc0(sizeof(*media));
				attrs_init(&media->attributes);
				errstr = "Error parsing m= line";
				if (parse_media(value, line_end, media))
					goto error;
				g_queue_push_tail(&session->media_streams, media);
				media->s.s = b;

				break;

			case 'c':
				errstr = "Error parsing c= line";
				if (parse_connection(value, line_end,
						media ? &media->connection : &session->connection))
					goto error;

				break;

			case 'a':
				attr = g_slice_alloc0(sizeof(*attr));

				attr->full_line.s = b;
				attr->full_line.len = next_line ? (next_line - b) : (line_end - b);

				attr->line_value.s = value;
				attr->line_value.len = line_end - value;

				attr->name = attr->line_value;
				str_chr_str(&attr->value, &attr->name, ':');
				if (attr->value.s) {
					attr->name.len -= attr->value.len;
					attr->value.s++;
					attr->value.len--;

					attr->key = attr->name;
					str_chr_str(&attr->param, &attr->value, ' ');
					if (attr->param.s) {
						attr->key.len += 1 +
							(attr->value.len - attr->param.len);

						attr->param.s++;
						attr->param.len--;

						if (!attr->param.len)
							attr->param.s = NULL;
					}
					else
						attr->key.len += 1 + attr->value.len;
				}

				attrs = media ? &media->attributes : &session->attributes;
				g_queue_push_tail(&attrs->list, attr);
				g_hash_table_insert(attrs->hash, &attr->name, attr);
				if (attr->key.s)
					g_hash_table_insert(attrs->hash, &attr->key, attr);

				break;

			case 's':
			case 'i':
			case 'u':
			case 'e':
			case 'p':
			case 'b':
			case 't':
			case 'r':
			case 'z':
			case 'k':
				break;

			default:
				errstr = "Unknown SDP line type found";
				goto error;
		}

		adj_s = media ? &media->s : &session->s;
		adj_s->len = (next_line ? : end) - adj_s->s;

		b = next_line;
	}

	return 0;

error:
	mylog(LOG_WARNING, "Error parsing SDP at offset %li: %s", b - body->s, errstr);
	sdp_free(sessions);
	return -1;
}

static void free_attributes(struct sdp_attributes *a) {
	struct sdp_attribute *attr;

	g_hash_table_destroy(a->hash);
	while ((attr = g_queue_pop_head(&a->list))) {
		g_slice_free1(sizeof(*attr), attr);
	}
}

void sdp_free(GQueue *sessions) {
	struct sdp_session *session;
	struct sdp_media *media;

	while ((session = g_queue_pop_head(sessions))) {
		while ((media = g_queue_pop_head(&session->media_streams))) {
			free_attributes(&media->attributes);
			g_slice_free1(sizeof(*media), media);
		}
		free_attributes(&session->attributes);
		g_slice_free1(sizeof(*session), session);
	}
}

static int fill_stream(struct stream_input *si, struct sdp_media *media, struct sdp_session *session, int offset) {
	if (media->connection.parsed)
		si->stream.ip46 = media->connection.address.parsed;
	else if (session->connection.parsed)
		si->stream.ip46 = session->connection.address.parsed;
	else
		return -1;

	/* we ignore the media type */
	si->stream.port = (media->port_num + (offset * 2)) & 0xffff;

	return 0;
}

int sdp_streams(const GQueue *sessions, GQueue *streams, GHashTable *streamhash) {
	struct sdp_session *session;
	struct sdp_media *media;
	struct stream_input *si;
	GList *l, *k;
	const char *errstr;
	int i, num;

	num = 0;
	for (l = sessions->head; l; l = l->next) {
		session = l->data;

		for (k = session->media_streams.head; k; k = k->next) {
			media = k->data;

			for (i = 0; i < media->port_count; i++) {
				si = g_slice_alloc0(sizeof(*si));

				errstr = "No address info found for stream";
				if (fill_stream(si, media, session, i))
					goto error;

				if (i == 0 && g_hash_table_contains(streamhash, si)) {
					g_slice_free1(sizeof(*si), si);
					continue;
				}

				si->stream.num = ++num;
				si->consecutive_num = (i == 0) ? media->port_count : 1;

				g_hash_table_insert(streamhash, si, si);
				g_queue_push_tail(streams, si);
			}
		}
	}

	return 0;

error:
	mylog(LOG_WARNING, "Failed to extract streams from SDP: %s", errstr);
	return -1;
}

struct sdp_chopper *sdp_chopper_new(str *input) {
	struct sdp_chopper *c = g_slice_alloc0(sizeof(*c));
	c->input = input;
	c->chunk = g_string_chunk_new(512);
	c->iov = g_array_new(0, 0, sizeof(struct iovec));
	return c;
}

static void chopper_append(struct sdp_chopper *c, const char *s, int len) {
	struct iovec *iov;

	g_array_set_size(c->iov, ++c->iov_num);
	iov = &g_array_index(c->iov, struct iovec, c->iov_num - 1);
	iov->iov_base = (void *) s;
	iov->iov_len = len;
	c->str_len += len;
}
static inline void chopper_append_c(struct sdp_chopper *c, const char *s) {
	chopper_append(c, s, strlen(s));
}
static inline void chopper_append_str(struct sdp_chopper *c, const str *s) {
	chopper_append(c, s->s, s->len);
}

static void chopper_append_dup(struct sdp_chopper *c, const char *s, int len) {
	return chopper_append(c, g_string_chunk_insert_len(c->chunk, s, len), len);
}

static void chopper_append_printf(struct sdp_chopper *c, const char *fmt, ...) __attribute__((format(printf,2,3)));

static void chopper_append_printf(struct sdp_chopper *c, const char *fmt, ...) {
	char buf[32];
	int l;
	va_list va;

	va_start(va, fmt);
	l = vsnprintf(buf, sizeof(buf) - 1, fmt, va);
	va_end(va);
	chopper_append(c, g_string_chunk_insert_len(c->chunk, buf, l), l);
}

static int copy_up_to_ptr(struct sdp_chopper *chop, const char *b) {
	int offset, len;

	offset = b - chop->input->s;
	assert(offset >= 0);
	assert(offset <= chop->input->len);

	len = offset - chop->position;
	if (len < 0) {
		mylog(LOG_WARNING, "Malformed SDP, cannot rewrite");
		return -1;
	}
	chopper_append(chop, chop->input->s + chop->position, len);
	chop->position += len;
	return 0;
}

static int copy_up_to(struct sdp_chopper *chop, str *where) {
	return copy_up_to_ptr(chop, where->s);
}

static int copy_up_to_end_of(struct sdp_chopper *chop, str *where) {
	return copy_up_to_ptr(chop, where->s + where->len);
}

static void copy_remainder(struct sdp_chopper *chop) {
	copy_up_to_ptr(chop, chop->input->s + chop->input->len);
}

static int skip_over(struct sdp_chopper *chop, str *where) {
	int offset, len;

	offset = (where->s - chop->input->s) + where->len;
	assert(offset >= 0);
	assert(offset <= chop->input->len);

	len = offset - chop->position;
	if (len < 0) {
		mylog(LOG_WARNING, "Malformed SDP, cannot rewrite");
		return -1;
	}
	chop->position += len;
	return 0;
}

static int replace_media_port(struct sdp_chopper *chop, struct sdp_media *media, GList *m, int off) {
	struct callstream *cs;
	struct streamrelay *sr;
	str *port = &media->port;
	int cons;

	if (!m) {
		mylog(LOG_ERROR, "BUG! Ran out of streams");
		return -1;
	}

	cs = m->data;
	sr = &cs->peers[off].rtps[0];

	if (copy_up_to(chop, port))
		return -1;

	chopper_append_printf(chop, "%hu", sr->fd.localport);

	if (skip_over(chop, port))
		return -1;

	if (media->port_count == 1)
		return 0;

	for (cons = 1; cons < media->port_count; cons++) {
		m = m->next;
		if (!m)
			goto warn;
		cs = m->data;
		if (cs->peers[off].rtps[0].fd.localport != sr->fd.localport + cons * 2) {
warn:
			mylog(LOG_WARN, "Failed to handle consecutive ports");
			break;
		}
	}

	chopper_append_printf(chop, "/%i", cons);

	return 0;
}

static int insert_ice_address(struct sdp_chopper *chop, GList *m, int off, struct sdp_ng_flags *flags, int streamoff) {
	struct callstream *cs;
	struct peer *peer;
	struct streamrelay *sr;
	char buf[64];
	int len;

	cs = m->data;
	peer = &cs->peers[off];
	sr = &peer->rtps[streamoff];

	if (!flags->trust_address && flags->received_from_family.len == 3 && flags->received_from_address.len)
		chopper_append_str(chop, &flags->received_from_address);
	else {
		call_stream_address(buf, peer, SAF_ICE, &len);
		chopper_append_dup(chop, buf, len);
	}

	chopper_append_printf(chop, " %hu", sr->fd.localport);

	return 0;
}

static int replace_network_address(struct sdp_chopper *chop, struct network_address *address, GList *m, int off, struct sdp_ng_flags *flags) {
	struct callstream *cs;
	struct peer *peer;
	char buf[64];
	int len;

	if (!m) {
		mylog(LOG_ERROR, "BUG! Ran out of streams");
		return -1;
	}

	cs = m->data;
	peer = &cs->peers[off];

	if (copy_up_to(chop, &address->address_type))
		return -1;

	if (!flags->trust_address && flags->received_from_family.len == 3 && flags->received_from_address.len) {
		chopper_append_str(chop, &flags->received_from_family);
		chopper_append_c(chop, " ");
		chopper_append_str(chop, &flags->received_from_address);
	}
	else {
		call_stream_address(buf, peer, SAF_NG, &len);
		chopper_append_dup(chop, buf, len);
	}

	if (skip_over(chop, &address->address))
		return -1;

	return 0;
}

void sdp_chopper_destroy(struct sdp_chopper *chop) {
	g_string_chunk_free(chop->chunk);
	g_array_free(chop->iov, 1);
	g_slice_free1(sizeof(*chop), chop);
}

static int remove_ice(struct sdp_chopper *chop, struct sdp_attributes *attrs) {
	struct sdp_attribute *attr;
	GList *l;

	for (l = attrs->list.head; l; l = l->next) {
		attr = l->data;

		switch (attr->name.len) {
			case 7:
				if (!str_cmp(&attr->name, "ice-pwd"))
					goto strip;
				break;
			case 8:
				if (!str_cmp(&attr->name, "ice-lite"))
					goto strip;
				break;
			case 9:
				if (!str_cmp(&attr->name, "candidate"))
					goto strip;
				if (!str_cmp(&attr->name, "ice-ufrag"))
					goto strip;
				break;
			case 11:
				if (!str_cmp(&attr->name, "ice-options"))
					goto strip;
				break;
			case 12:
				if (!str_cmp(&attr->name, "ice-mismatch"))
					goto strip;
				break;
			case 17:
				if (!str_cmp(&attr->name, "remote-candidates"))
					goto strip;
				break;
		}

		continue;

strip:
		if (copy_up_to(chop, &attr->full_line))
			return -1;
		if (skip_over(chop, &attr->full_line))
			return -1;
	}

	return 0;
}

static void create_random_string(struct call *call, str *s, int len) {
	char buf[30];
	char *p;

	assert(len < sizeof(buf));
	if (s->s)
		return;

	p = buf;
	while (len--)
		*p++ = ice_chars[random() % strlen(ice_chars)];

	call_str_cpy_len(call, s, buf, p - buf);
}

int sdp_replace(struct sdp_chopper *chop, GQueue *sessions, struct call *call,
		enum call_opmode opmode, struct sdp_ng_flags *flags, GHashTable *streamhash)
{
	struct sdp_session *session;
	struct sdp_media *media;
	GList *l, *k, *m;
	int off;
	struct stream_input si, *sip;

	off = opmode;
	m = call->callstreams->head;

	for (l = sessions->head; l; l = l->next) {
		session = l->data;

		if (session->origin.parsed && flags->replace_origin) {
			if (replace_network_address(chop, &session->origin.address, m, off, flags))
				goto error;
		}
		if (session->connection.parsed) {
			if (replace_network_address(chop, &session->connection.address, m, off, flags))
				goto error;
		}

		/* XXX convert to a "process attributes" kinda function */
		if (flags->ice_remove || flags->ice_force) {
			if (remove_ice(chop, &session->attributes))
				goto error;
		}

		if (flags->ice_force) {
			create_random_string(call, &call->ice_ufrag[0], 4);
			create_random_string(call, &call->ice_ufrag[1], 4);
			create_random_string(call, &call->ice_pwd, 20);

			copy_up_to_end_of(chop, &session->s);
			chopper_append_c(chop, "a=ice-lite\r\na=ice-ufrag:");
			chopper_append_str(chop, &call->ice_ufrag[off]);
			chopper_append_c(chop, "\r\na=ice-pwd:");
			chopper_append_str(chop, &call->ice_pwd);
			chopper_append_c(chop, "\r\n");
			/* XXX handle RTCP attributes */
		}

		for (k = session->media_streams.head; k; k = k->next) {
			media = k->data;

			if (fill_stream(&si, media, session, 0))
				goto error;

			sip = g_hash_table_lookup(streamhash, &si);
			if (!sip)
				goto error;
			if (!m)
				m = call->callstreams->head;
			while (m && ((struct callstream *) m->data)->num < sip->stream.num)
				m = m->next;
			while (m && ((struct callstream *) m->data)->num > sip->stream.num)
				m = m->prev;

			if (replace_media_port(chop, media, m, off))
				goto error;

			if (media->connection.parsed && flags->replace_sess_conn) {
				if (replace_network_address(chop, &media->connection.address, m, off, flags))
					goto error;
			}

			/* XXX convert to a "process attributes" kinda function */
			if (flags->ice_remove || flags->ice_force) {
				if (remove_ice(chop, &media->attributes))
					goto error;
			}

			if (flags->ice_force) {
				copy_up_to_end_of(chop, &media->s);
				/* prio = (2^24) * 126 + (2^8) * 65535 + (256 - componentID) */
				chopper_append_c(chop, "a=candidate:1 1 UDP 2130706431 ");
				insert_ice_address(chop, m, off, flags, 0);
				chopper_append_c(chop, " typ host\r\n");
				chopper_append_c(chop, "a=candidate:1 2 UDP 2130706430 ");
				insert_ice_address(chop, m, off, flags, 1);
				chopper_append_c(chop, " typ host\r\n");
			}
		}
	}

	copy_remainder(chop);
	return 0;

error:
	mylog(LOG_ERROR, "Error rewriting SDP");
	return -1;
}
