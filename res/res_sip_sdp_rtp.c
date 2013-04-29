/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
 * Kevin Harwell <kharwell@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \author Joshua Colp <jcolp@digium.com>
 *
 * \brief SIP SDP media stream handling
 */

/*** MODULEINFO
	<depend>pjproject</depend>
	<depend>res_sip</depend>
	<depend>res_sip_session</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjmedia.h>
#include <pjlib.h>

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/module.h"
#include "asterisk/rtp_engine.h"
#include "asterisk/netsock2.h"
#include "asterisk/channel.h"
#include "asterisk/causes.h"
#include "asterisk/sched.h"
#include "asterisk/acl.h"

#include "asterisk/res_sip.h"
#include "asterisk/res_sip_session.h"

/*! \brief Scheduler for RTCP purposes */
static struct ast_sched_context *sched;

/*! \brief Address for IPv4 RTP */
static struct ast_sockaddr address_ipv4;

/*! \brief Address for IPv6 RTP */
static struct ast_sockaddr address_ipv6;

static const char STR_AUDIO[] = "audio";
static const int FD_AUDIO = 0;

static const char STR_VIDEO[] = "video";
static const int FD_VIDEO = 2;

/*! \brief Retrieves an ast_format_type based on the given stream_type */
static enum ast_format_type stream_to_media_type(const char *stream_type)
{
	if (!strcasecmp(stream_type, STR_AUDIO)) {
		return AST_FORMAT_TYPE_AUDIO;
	} else if (!strcasecmp(stream_type, STR_VIDEO)) {
		return AST_FORMAT_TYPE_VIDEO;
	}

	return 0;
}

/*! \brief Get the starting descriptor for a media type */
static int media_type_to_fdno(enum ast_format_type media_type)
{
	switch (media_type) {
	case AST_FORMAT_TYPE_AUDIO: return FD_AUDIO;
	case AST_FORMAT_TYPE_VIDEO: return FD_VIDEO;
	case AST_FORMAT_TYPE_TEXT:
	case AST_FORMAT_TYPE_IMAGE: break;
	}
	return -1;
}

/*! \brief Remove all other cap types but the one given */
static void format_cap_only_type(struct ast_format_cap *caps, enum ast_format_type media_type)
{
	int i = AST_FORMAT_INC;
	while (i <= AST_FORMAT_TYPE_TEXT) {
		if (i != media_type) {
			ast_format_cap_remove_bytype(caps, i);
		}
		i += AST_FORMAT_INC;
	}
}

/*! \brief Internal function which creates an RTP instance */
static int create_rtp(struct ast_sip_session *session, struct ast_sip_session_media *session_media, unsigned int ipv6)
{
	struct ast_rtp_engine_ice *ice;

	if (!(session_media->rtp = ast_rtp_instance_new("asterisk", sched, ipv6 ? &address_ipv6 : &address_ipv4, NULL))) {
		return -1;
	}

	ast_rtp_instance_set_prop(session_media->rtp, AST_RTP_PROPERTY_RTCP, 1);
	ast_rtp_instance_set_prop(session_media->rtp, AST_RTP_PROPERTY_NAT, session->endpoint->rtp_symmetric);

	ast_rtp_codecs_packetization_set(ast_rtp_instance_get_codecs(session_media->rtp),
					 session_media->rtp, &session->endpoint->prefs);

	if (!session->endpoint->ice_support && (ice = ast_rtp_instance_get_ice(session_media->rtp))) {
		ice->stop(session_media->rtp);
	}

	return 0;
}

static void get_codecs(struct ast_sip_session *session, const struct pjmedia_sdp_media *stream, struct ast_rtp_codecs *codecs)
{
	pjmedia_sdp_attr *attr;
	pjmedia_sdp_rtpmap *rtpmap;
	pjmedia_sdp_fmtp fmtp;
	struct ast_format *format;
	int i, num = 0;
	char name[256];
	char media[20];
	char fmt_param[256];

	ast_rtp_codecs_payloads_initialize(codecs);

	/* Iterate through provided formats */
	for (i = 0; i < stream->desc.fmt_count; ++i) {
		/* The payload is kept as a string for things like t38 but for video it is always numerical */
		ast_rtp_codecs_payloads_set_m_type(codecs, NULL, pj_strtoul(&stream->desc.fmt[i]));
		/* Look for the optional rtpmap attribute */
		if (!(attr = pjmedia_sdp_media_find_attr2(stream, "rtpmap", &stream->desc.fmt[i]))) {
			continue;
		}

		/* Interpret the attribute as an rtpmap */
		if ((pjmedia_sdp_attr_to_rtpmap(session->inv_session->pool_prov, attr, &rtpmap)) != PJ_SUCCESS) {
			continue;
		}

		ast_copy_pj_str(name, &rtpmap->enc_name, sizeof(name));
		ast_copy_pj_str(media, (pj_str_t*)&stream->desc.media, sizeof(media));
		ast_rtp_codecs_payloads_set_rtpmap_type_rate(codecs, NULL, pj_strtoul(&stream->desc.fmt[i]),
							     media, name, 0, rtpmap->clock_rate);
		/* Look for an optional associated fmtp attribute */
		if (!(attr = pjmedia_sdp_media_find_attr2(stream, "fmtp", &rtpmap->pt))) {
			continue;
		}

		if ((pjmedia_sdp_attr_get_fmtp(attr, &fmtp)) == PJ_SUCCESS) {
			sscanf(pj_strbuf(&fmtp.fmt), "%d", &num);
			if ((format = ast_rtp_codecs_get_payload_format(codecs, num))) {
				ast_copy_pj_str(fmt_param, &fmtp.fmt_param, sizeof(fmt_param));
				ast_format_sdp_parse(format, fmt_param);
			}
		}
	}
}

static int set_caps(struct ast_sip_session *session, struct ast_sip_session_media *session_media,
		    const struct pjmedia_sdp_media *stream)
{
	RAII_VAR(struct ast_format_cap *, caps, NULL, ast_format_cap_destroy);
	RAII_VAR(struct ast_format_cap *, peer, NULL, ast_format_cap_destroy);
	RAII_VAR(struct ast_format_cap *, joint, NULL, ast_format_cap_destroy);
	enum ast_format_type media_type = stream_to_media_type(session_media->stream_type);
	struct ast_rtp_codecs codecs;
	struct ast_format fmt;
	int fmts = 0;
	int direct_media_enabled = !ast_sockaddr_isnull(&session_media->direct_media_addr) &&
		!ast_format_cap_is_empty(session->direct_media_cap);

	if (!(caps = ast_format_cap_alloc_nolock()) ||
	    !(peer = ast_format_cap_alloc_nolock())) {
		ast_log(LOG_ERROR, "Failed to allocate %s capabilities\n", session_media->stream_type);
		return -1;
	}

	/* get the endpoint capabilities */
	if (direct_media_enabled) {
		ast_format_cap_joint_copy(session->endpoint->codecs, session->direct_media_cap, caps);
	} else {
		ast_format_cap_copy(caps, session->endpoint->codecs);
	}
	format_cap_only_type(caps, media_type);

	/* get the capabilities on the peer */
	get_codecs(session, stream, &codecs);
	ast_rtp_codecs_payload_formats(&codecs, peer, &fmts);

	/* get the joint capabilities between peer and endpoint */
	if (!(joint = ast_format_cap_joint(caps, peer))) {
		char usbuf[64], thembuf[64];

		ast_rtp_codecs_payloads_destroy(&codecs);

		ast_getformatname_multiple(usbuf, sizeof(usbuf), caps);
		ast_getformatname_multiple(thembuf, sizeof(thembuf), peer);
		ast_log(LOG_WARNING, "No joint capabilities between our configuration(%s) and incoming SDP(%s)\n", usbuf, thembuf);
		return -1;
	}

	ast_rtp_codecs_payloads_copy(&codecs, ast_rtp_instance_get_codecs(session_media->rtp),
				     session_media->rtp);

	ast_format_cap_copy(caps, session->req_caps);
	ast_format_cap_remove_bytype(caps, media_type);
	ast_format_cap_append(caps, joint);
	ast_format_cap_append(session->req_caps, caps);

	if (session->channel) {
		ast_format_cap_copy(caps, ast_channel_nativeformats(session->channel));
		ast_format_cap_remove_bytype(caps, media_type);
		ast_format_cap_append(caps, joint);

		/* Apply the new formats to the channel, potentially changing read/write formats while doing so */
		ast_format_cap_append(ast_channel_nativeformats(session->channel), caps);
		ast_codec_choose(&session->endpoint->prefs, caps, 0, &fmt);
		ast_format_copy(ast_channel_rawwriteformat(session->channel), &fmt);
		ast_format_copy(ast_channel_rawreadformat(session->channel), &fmt);
		ast_set_read_format(session->channel, ast_channel_readformat(session->channel));
		ast_set_write_format(session->channel, ast_channel_writeformat(session->channel));
	}

	ast_rtp_codecs_payloads_destroy(&codecs);
	return 1;
}

static pjmedia_sdp_attr* generate_rtpmap_attr(pjmedia_sdp_media *media, pj_pool_t *pool, int rtp_code,
					      int asterisk_format, struct ast_format *format, int code)
{
	pjmedia_sdp_rtpmap rtpmap;
	pjmedia_sdp_attr *attr = NULL;
	char tmp[64];

	snprintf(tmp, sizeof(tmp), "%d", rtp_code);
	pj_strdup2(pool, &media->desc.fmt[media->desc.fmt_count++], tmp);
	rtpmap.pt = media->desc.fmt[media->desc.fmt_count - 1];
	rtpmap.clock_rate = ast_rtp_lookup_sample_rate2(asterisk_format, format, code);
	pj_strdup2(pool, &rtpmap.enc_name, ast_rtp_lookup_mime_subtype2(asterisk_format, format, code, 0));
	rtpmap.param.slen = 0;

	pjmedia_sdp_rtpmap_to_attr(pool, &rtpmap, &attr);

	return attr;
}

static pjmedia_sdp_attr* generate_fmtp_attr(pj_pool_t *pool, struct ast_format *format, int rtp_code)
{
	struct ast_str *fmtp0 = ast_str_alloca(256);
	pj_str_t fmtp1;
	pjmedia_sdp_attr *attr = NULL;
	char *tmp;

	ast_format_sdp_generate(format, rtp_code, &fmtp0);
	if (ast_str_strlen(fmtp0)) {
		tmp = ast_str_buffer(fmtp0) + ast_str_strlen(fmtp0) - 1;
		/* remove any carriage return line feeds */
		while (*tmp == '\r' || *tmp == '\n') --tmp;
		*++tmp = '\0';
		/* ast...generate gives us everything, just need value */
		tmp = strchr(ast_str_buffer(fmtp0), ':');
		if (tmp && tmp + 1) {
			fmtp1 = pj_str(tmp + 1);
		} else {
			fmtp1 = pj_str(ast_str_buffer(fmtp0));
		}
		attr = pjmedia_sdp_attr_create(pool, "fmtp", &fmtp1);
	}
	return attr;
}

/*! \brief Function which adds ICE attributes to a media stream */
static void add_ice_to_stream(struct ast_sip_session *session, struct ast_sip_session_media *session_media, pj_pool_t *pool, pjmedia_sdp_media *media)
{
	struct ast_rtp_engine_ice *ice;
	struct ao2_container *candidates;
	const char *username, *password;
	pj_str_t stmp;
	pjmedia_sdp_attr *attr;
	struct ao2_iterator it_candidates;
	struct ast_rtp_engine_ice_candidate *candidate;

	if (!session->endpoint->ice_support || !(ice = ast_rtp_instance_get_ice(session_media->rtp)) ||
		!(candidates = ice->get_local_candidates(session_media->rtp))) {
		return;
	}

	if ((username = ice->get_ufrag(session_media->rtp))) {
		attr = pjmedia_sdp_attr_create(pool, "ice-ufrag", pj_cstr(&stmp, username));
		media->attr[media->attr_count++] = attr;
	}

	if ((password = ice->get_password(session_media->rtp))) {
		attr = pjmedia_sdp_attr_create(pool, "ice-pwd", pj_cstr(&stmp, password));
		media->attr[media->attr_count++] = attr;
	}

	it_candidates = ao2_iterator_init(candidates, 0);
	for (; (candidate = ao2_iterator_next(&it_candidates)); ao2_ref(candidate, -1)) {
		struct ast_str *attr_candidate = ast_str_create(128);

		ast_str_set(&attr_candidate, -1, "%s %d %s %d %s ", candidate->foundation, candidate->id, candidate->transport,
					candidate->priority, ast_sockaddr_stringify_host(&candidate->address));
		ast_str_append(&attr_candidate, -1, "%s typ ", ast_sockaddr_stringify_port(&candidate->address));

		switch (candidate->type) {
			case AST_RTP_ICE_CANDIDATE_TYPE_HOST:
				ast_str_append(&attr_candidate, -1, "host");
				break;
			case AST_RTP_ICE_CANDIDATE_TYPE_SRFLX:
				ast_str_append(&attr_candidate, -1, "srflx");
				break;
			case AST_RTP_ICE_CANDIDATE_TYPE_RELAYED:
				ast_str_append(&attr_candidate, -1, "relay");
				break;
		}

		if (!ast_sockaddr_isnull(&candidate->relay_address)) {
			ast_str_append(&attr_candidate, -1, " raddr %s rport ", ast_sockaddr_stringify_host(&candidate->relay_address));
			ast_str_append(&attr_candidate, -1, " %s", ast_sockaddr_stringify_port(&candidate->relay_address));
		}

		attr = pjmedia_sdp_attr_create(pool, "candidate", pj_cstr(&stmp, ast_str_buffer(attr_candidate)));
		media->attr[media->attr_count++] = attr;

		ast_free(attr_candidate);
	}

	ao2_iterator_destroy(&it_candidates);
}

/*! \brief Function which processes ICE attributes in an audio stream */
static void process_ice_attributes(struct ast_sip_session *session, struct ast_sip_session_media *session_media,
				   const struct pjmedia_sdp_session *remote, const struct pjmedia_sdp_media *remote_stream)
{
	struct ast_rtp_engine_ice *ice;
	const pjmedia_sdp_attr *attr;
	char attr_value[256];
	unsigned int attr_i;

	/* If ICE support is not enabled or available exit early */
	if (!session->endpoint->ice_support || !(ice = ast_rtp_instance_get_ice(session_media->rtp))) {
		return;
	}

	if ((attr = pjmedia_sdp_media_find_attr2(remote_stream, "ice-ufrag", NULL))) {
		ast_copy_pj_str(attr_value, (pj_str_t*)&attr->value, sizeof(attr_value));
		ice->set_authentication(session_media->rtp, attr_value, NULL);
	}

	if ((attr = pjmedia_sdp_media_find_attr2(remote_stream, "ice-pwd", NULL))) {
		ast_copy_pj_str(attr_value, (pj_str_t*)&attr->value, sizeof(attr_value));
		ice->set_authentication(session_media->rtp, NULL, attr_value);
	}

	if (pjmedia_sdp_media_find_attr2(remote_stream, "ice-lite", NULL)) {
		ice->ice_lite(session_media->rtp);
	}

	/* Find all of the candidates */
	for (attr_i = 0; attr_i < remote_stream->attr_count; ++attr_i) {
		char foundation[32], transport[32], address[PJ_INET6_ADDRSTRLEN + 1], cand_type[6], relay_address[PJ_INET6_ADDRSTRLEN + 1] = "";
		int port, relay_port = 0;
		struct ast_rtp_engine_ice_candidate candidate = { 0, };

		attr = remote_stream->attr[attr_i];

		/* If this is not a candidate line skip it */
		if (pj_strcmp2(&attr->name, "candidate")) {
			continue;
		}

		ast_copy_pj_str(attr_value, (pj_str_t*)&attr->value, sizeof(attr_value));

		if (sscanf(attr_value, "%31s %30u %31s %30u %46s %30u typ %5s %*s %23s %*s %30u", foundation, &candidate.id, transport,
			&candidate.priority, address, &port, cand_type, relay_address, &relay_port) < 7) {
			/* Candidate did not parse properly */
			continue;
		}

		candidate.foundation = foundation;
		candidate.transport = transport;

		ast_sockaddr_parse(&candidate.address, address, PARSE_PORT_FORBID);
		ast_sockaddr_set_port(&candidate.address, port);

		if (!strcasecmp(cand_type, "host")) {
			candidate.type = AST_RTP_ICE_CANDIDATE_TYPE_HOST;
		} else if (!strcasecmp(cand_type, "srflx")) {
			candidate.type = AST_RTP_ICE_CANDIDATE_TYPE_SRFLX;
		} else if (!strcasecmp(cand_type, "relay")) {
			candidate.type = AST_RTP_ICE_CANDIDATE_TYPE_RELAYED;
		} else {
			continue;
		}

		if (!ast_strlen_zero(relay_address)) {
			ast_sockaddr_parse(&candidate.relay_address, relay_address, PARSE_PORT_FORBID);
		}

		if (relay_port) {
			ast_sockaddr_set_port(&candidate.relay_address, relay_port);
		}

		ice->add_remote_candidate(session_media->rtp, &candidate);
	}

	ice->start(session_media->rtp);
}

static void apply_packetization(struct ast_sip_session *session, struct ast_sip_session_media *session_media,
			 const struct pjmedia_sdp_media *remote_stream)
{
	pjmedia_sdp_attr *attr;
	pj_str_t value;
	unsigned long framing;
	int codec;
	struct ast_codec_pref *pref = &ast_rtp_instance_get_codecs(session_media->rtp)->pref;

	/* Apply packetization if available and configured to do so */
	if (!session->endpoint->use_ptime || !(attr = pjmedia_sdp_media_find_attr2(remote_stream, "ptime", NULL))) {
		return;
	}

	value = attr->value;
	framing = pj_strtoul(pj_strltrim(&value));

	for (codec = 0; codec < AST_RTP_MAX_PT; codec++) {
		struct ast_rtp_payload_type format = ast_rtp_codecs_payload_lookup(ast_rtp_instance_get_codecs(
											   session_media->rtp), codec);

		if (!format.asterisk_format) {
			continue;
		}

		ast_codec_pref_setsize(pref, &format.format, framing);
	}

	ast_rtp_codecs_packetization_set(ast_rtp_instance_get_codecs(session_media->rtp),
					 session_media->rtp, pref);
}

/*! \brief Function which negotiates an incoming media stream */
static int negotiate_incoming_sdp_stream(struct ast_sip_session *session, struct ast_sip_session_media *session_media,
					 const struct pjmedia_sdp_session *sdp, const struct pjmedia_sdp_media *stream)
{
	char host[NI_MAXHOST];
	RAII_VAR(struct ast_sockaddr *, addrs, NULL, ast_free_ptr);
	enum ast_format_type media_type = stream_to_media_type(session_media->stream_type);

	/* If no type formats have been configured reject this stream */
	if (!ast_format_cap_has_type(session->endpoint->codecs, media_type)) {
		return 0;
	}

	ast_copy_pj_str(host, stream->conn ? &stream->conn->addr : &sdp->conn->addr, sizeof(host));

	/* Ensure that the address provided is valid */
	if (ast_sockaddr_resolve(&addrs, host, PARSE_PORT_FORBID, AST_AF_UNSPEC) <= 0) {
		/* The provided host was actually invalid so we error out this negotiation */
		return -1;
	}

	/* Using the connection information create an appropriate RTP instance */
	if (!session_media->rtp && create_rtp(session, session_media, ast_sockaddr_is_ipv6(addrs))) {
		return -1;
	}

	return set_caps(session, session_media, stream);
}

/*! \brief Function which creates an outgoing stream */
static int create_outgoing_sdp_stream(struct ast_sip_session *session, struct ast_sip_session_media *session_media,
				      struct pjmedia_sdp_session *sdp)
{
	pj_pool_t *pool = session->inv_session->pool_prov;
	static const pj_str_t STR_IN = { "IN", 2 };
	static const pj_str_t STR_IP4 = { "IP4", 3};
	static const pj_str_t STR_IP6 = { "IP6", 3};
	static const pj_str_t STR_RTP_AVP = { "RTP/AVP", 7 };
	static const pj_str_t STR_SENDRECV = { "sendrecv", 8 };
	pjmedia_sdp_media *media;
	char hostip[PJ_INET6_ADDRSTRLEN+2];
	struct ast_sockaddr addr;
	char tmp[512];
	pj_str_t stmp;
	pjmedia_sdp_attr *attr;
	int index = 0, min_packet_size = 0, noncodec = (session->endpoint->dtmf == AST_SIP_DTMF_RFC_4733) ? AST_RTP_DTMF : 0;
	int rtp_code;
	struct ast_format format;
	struct ast_format compat_format;
	RAII_VAR(struct ast_format_cap *, caps, NULL, ast_format_cap_destroy);
	enum ast_format_type media_type = stream_to_media_type(session_media->stream_type);

	int direct_media_enabled = !ast_sockaddr_isnull(&session_media->direct_media_addr) &&
		!ast_format_cap_is_empty(session->direct_media_cap);

	if (!ast_format_cap_has_type(session->endpoint->codecs, media_type)) {
		/* If no type formats are configured don't add a stream */
		return 0;
	} else if (!session_media->rtp && create_rtp(session, session_media, session->endpoint->rtp_ipv6)) {
		return -1;
	}

	if (!(media = pj_pool_zalloc(pool, sizeof(struct pjmedia_sdp_media))) ||
		!(media->conn = pj_pool_zalloc(pool, sizeof(struct pjmedia_sdp_conn)))) {
		return -1;
	}

	/* TODO: This should eventually support SRTP */
	media->desc.media = pj_str(session_media->stream_type);
	media->desc.transport = STR_RTP_AVP;

	/* Add connection level details */
	if (direct_media_enabled) {
		ast_copy_string(hostip, ast_sockaddr_stringify_fmt(&session_media->direct_media_addr, AST_SOCKADDR_STR_ADDR), sizeof(hostip));
	} else if (ast_strlen_zero(session->endpoint->external_media_address)) {
		pj_sockaddr localaddr;

		if (pj_gethostip(session->endpoint->rtp_ipv6 ? pj_AF_INET6() : pj_AF_INET(), &localaddr)) {
			return -1;
		}
		pj_sockaddr_print(&localaddr, hostip, sizeof(hostip), 2);
	} else {
		ast_copy_string(hostip, session->endpoint->external_media_address, sizeof(hostip));
	}

	media->conn->net_type = STR_IN;
	media->conn->addr_type = session->endpoint->rtp_ipv6 ? STR_IP6 : STR_IP4;
	pj_strdup2(pool, &media->conn->addr, hostip);
	ast_rtp_instance_get_local_address(session_media->rtp, &addr);
	media->desc.port = direct_media_enabled ? ast_sockaddr_port(&session_media->direct_media_addr) : (pj_uint16_t) ast_sockaddr_port(&addr);
	media->desc.port_count = 1;

	/* Add ICE attributes and candidates */
	add_ice_to_stream(session, session_media, pool, media);

	if (!(caps = ast_format_cap_alloc_nolock())) {
		ast_log(LOG_ERROR, "Failed to allocate %s capabilities\n", session_media->stream_type);
		return -1;
	}

	if (direct_media_enabled) {
		ast_format_cap_joint_copy(session->endpoint->codecs, session->direct_media_cap, caps);
	} else if (ast_format_cap_is_empty(session->req_caps)) {
		ast_format_cap_copy(caps, session->endpoint->codecs);
	} else {
		ast_format_cap_joint_copy(session->endpoint->codecs, session->req_caps, caps);
	}

	for (index = 0; ast_codec_pref_index(&session->endpoint->prefs, index, &format); ++index) {
		struct ast_codec_pref *pref = &ast_rtp_instance_get_codecs(session_media->rtp)->pref;

		if (AST_FORMAT_GET_TYPE(format.id) != media_type) {
			continue;
		}

		if (!ast_format_cap_get_compatible_format(caps, &format, &compat_format)) {
			continue;
		}

		if ((rtp_code = ast_rtp_codecs_payload_code(ast_rtp_instance_get_codecs(session_media->rtp), 1, &compat_format, 0)) == -1) {
			return -1;
		}

		if (!(attr = generate_rtpmap_attr(media, pool, rtp_code, 1, &compat_format, 0))) {
			continue;
		}

		media->attr[media->attr_count++] = attr;

		if ((attr = generate_fmtp_attr(pool, &compat_format, rtp_code))) {
			media->attr[media->attr_count++] = attr;
		}

		if (pref && media_type != AST_FORMAT_TYPE_VIDEO) {
			struct ast_format_list fmt = ast_codec_pref_getsize(pref, &compat_format);
			if (fmt.cur_ms && ((fmt.cur_ms < min_packet_size) || !min_packet_size)) {
				min_packet_size = fmt.cur_ms;
			}
		}
	}

	/* Add non-codec formats */
	if (media_type != AST_FORMAT_TYPE_VIDEO) {
		for (index = 1LL; index <= AST_RTP_MAX; index <<= 1) {
			if (!(noncodec & index) || (rtp_code = ast_rtp_codecs_payload_code(ast_rtp_instance_get_codecs(session_media->rtp),
											   0, NULL, index)) == -1) {
				continue;
			}

			if (!(attr = generate_rtpmap_attr(media, pool, rtp_code, 0, NULL, index))) {
				continue;
			}

			media->attr[media->attr_count++] = attr;

			if (index == AST_RTP_DTMF) {
				snprintf(tmp, sizeof(tmp), "%d 0-16", rtp_code);
				attr = pjmedia_sdp_attr_create(pool, "fmtp", pj_cstr(&stmp, tmp));
				media->attr[media->attr_count++] = attr;
			}
		}
	}

	/* If ptime is set add it as an attribute */
	if (min_packet_size) {
		snprintf(tmp, sizeof(tmp), "%d", min_packet_size);
		attr = pjmedia_sdp_attr_create(pool, "ptime", pj_cstr(&stmp, tmp));
		media->attr[media->attr_count++] = attr;
	}

	/* Add the sendrecv attribute - we purposely don't keep track because pjmedia-sdp will automatically change our offer for us */
	attr = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_attr);
	attr->name = STR_SENDRECV;
	media->attr[media->attr_count++] = attr;

	/* Add the media stream to the SDP */
	sdp->media[sdp->media_count++] = media;

	return 1;
}

static int apply_negotiated_sdp_stream(struct ast_sip_session *session, struct ast_sip_session_media *session_media,
				       const struct pjmedia_sdp_session *local, const struct pjmedia_sdp_media *local_stream,
				       const struct pjmedia_sdp_session *remote, const struct pjmedia_sdp_media *remote_stream)
{
	RAII_VAR(struct ast_sockaddr *, addrs, NULL, ast_free_ptr);
	enum ast_format_type media_type = stream_to_media_type(session_media->stream_type);
	char host[NI_MAXHOST];
	int fdno;

	if (!session->channel) {
		return 1;
	}

	/* Create an RTP instance if need be */
	if (!session_media->rtp && create_rtp(session, session_media, session->endpoint->rtp_ipv6)) {
		return -1;
	}

	ast_copy_pj_str(host, remote_stream->conn ? &remote_stream->conn->addr : &remote->conn->addr, sizeof(host));

	/* Ensure that the address provided is valid */
	if (ast_sockaddr_resolve(&addrs, host, PARSE_PORT_FORBID, AST_AF_UNSPEC) <= 0) {
		/* The provided host was actually invalid so we error out this negotiation */
		return -1;
	}

	/* Apply connection information to the RTP instance */
	ast_sockaddr_set_port(addrs, remote_stream->desc.port);
	ast_rtp_instance_set_remote_address(session_media->rtp, addrs);

	if (set_caps(session, session_media, local_stream) < 1) {
		return -1;
	}

	if (media_type == AST_FORMAT_TYPE_AUDIO) {
		apply_packetization(session, session_media, remote_stream);
	}

	if ((fdno = media_type_to_fdno(media_type)) < 0) {
		return -1;
	}
	ast_channel_set_fd(session->channel, fdno, ast_rtp_instance_fd(session_media->rtp, 0));
	ast_channel_set_fd(session->channel, fdno + 1, ast_rtp_instance_fd(session_media->rtp, 1));

	/* If ICE support is enabled find all the needed attributes */
	process_ice_attributes(session, session_media, remote, remote_stream);

	/* audio stream handles music on hold */
	if (media_type != AST_FORMAT_TYPE_AUDIO) {
		return 1;
	}

	/* Music on hold for audio streams only */
	if (session_media->held &&
	    (!ast_sockaddr_isnull(addrs) ||
	     !pjmedia_sdp_media_find_attr2(remote_stream, "sendonly", NULL))) {
		/* The remote side has taken us off hold */
		ast_queue_control(session->channel, AST_CONTROL_UNHOLD);
		ast_queue_frame(session->channel, &ast_null_frame);
		session_media->held = 0;
	} else if (ast_sockaddr_isnull(addrs) ||
		   ast_sockaddr_is_any(addrs) ||
		   pjmedia_sdp_media_find_attr2(remote_stream, "sendonly", NULL)) {
		/* The remote side has put us on hold */
		ast_queue_control_data(session->channel, AST_CONTROL_HOLD, S_OR(session->endpoint->mohsuggest, NULL),
				       !ast_strlen_zero(session->endpoint->mohsuggest) ? strlen(session->endpoint->mohsuggest) + 1 : 0);
		ast_rtp_instance_stop(session_media->rtp);
		ast_queue_frame(session->channel, &ast_null_frame);
		session_media->held = 1;
	} else {
		/* The remote side has not changed state, but make sure the instance is active */
		ast_rtp_instance_activate(session_media->rtp);
	}

	return 1;
}

/*! \brief Function which updates the media stream with external media address, if applicable */
static void change_outgoing_sdp_stream_media_address(pjsip_tx_data *tdata, struct pjmedia_sdp_media *stream, struct ast_sip_transport *transport)
{
	char host[NI_MAXHOST];
	struct ast_sockaddr addr = { { 0, } };

	ast_copy_pj_str(host, &stream->conn->addr, sizeof(host));
	ast_sockaddr_parse(&addr, host, PARSE_PORT_FORBID);

	/* Is the address within the SDP inside the same network? */
	if (ast_apply_ha(transport->localnet, &addr) == AST_SENSE_ALLOW) {
		return;
	}

	pj_strdup2(tdata->pool, &stream->conn->addr, transport->external_media_address);
}

/*! \brief Function which destroys the RTP instance when session ends */
static void stream_destroy(struct ast_sip_session_media *session_media)
{
	if (session_media->rtp) {
		ast_rtp_instance_stop(session_media->rtp);
		ast_rtp_instance_destroy(session_media->rtp);
	}
}

/*! \brief SDP handler for 'audio' media stream */
static struct ast_sip_session_sdp_handler audio_sdp_handler = {
	.id = STR_AUDIO,
	.negotiate_incoming_sdp_stream = negotiate_incoming_sdp_stream,
	.create_outgoing_sdp_stream = create_outgoing_sdp_stream,
	.apply_negotiated_sdp_stream = apply_negotiated_sdp_stream,
	.change_outgoing_sdp_stream_media_address = change_outgoing_sdp_stream_media_address,
	.stream_destroy = stream_destroy,
};

/*! \brief SDP handler for 'video' media stream */
static struct ast_sip_session_sdp_handler video_sdp_handler = {
	.id = STR_VIDEO,
	.negotiate_incoming_sdp_stream = negotiate_incoming_sdp_stream,
	.create_outgoing_sdp_stream = create_outgoing_sdp_stream,
	.apply_negotiated_sdp_stream = apply_negotiated_sdp_stream,
	.change_outgoing_sdp_stream_media_address = change_outgoing_sdp_stream_media_address,
	.stream_destroy = stream_destroy,
};

static int video_info_incoming_request(struct ast_sip_session *session, struct pjsip_rx_data *rdata)
{
	struct pjsip_transaction *tsx = pjsip_rdata_get_tsx(rdata);
	pjsip_tx_data *tdata;

	if (pj_strcmp2(&rdata->msg_info.msg->body->content_type.type, "application") ||
	    pj_strcmp2(&rdata->msg_info.msg->body->content_type.subtype, "media_control+xml")) {

		return 0;
	}

	ast_queue_control(session->channel, AST_CONTROL_VIDUPDATE);

	if (pjsip_dlg_create_response(session->inv_session->dlg, rdata, 200, NULL, &tdata) == PJ_SUCCESS) {
		pjsip_dlg_send_response(session->inv_session->dlg, tsx, tdata);
	}

	return 0;
}

static struct ast_sip_session_supplement video_info_supplement = {
	.method = "INFO",
	.incoming_request = video_info_incoming_request,
};

/*! \brief Unloads the sdp RTP/AVP module from Asterisk */
static int unload_module(void)
{
	ast_sip_session_unregister_supplement(&video_info_supplement);
	ast_sip_session_unregister_sdp_handler(&video_sdp_handler, STR_VIDEO);
	ast_sip_session_unregister_sdp_handler(&audio_sdp_handler, STR_AUDIO);

	if (sched) {
		ast_sched_context_destroy(sched);
	}

	return 0;
}

/*!
 * \brief Load the module
 *
 * Module loading including tests for configuration or dependencies.
 * This function can return AST_MODULE_LOAD_FAILURE, AST_MODULE_LOAD_DECLINE,
 * or AST_MODULE_LOAD_SUCCESS. If a dependency or environment variable fails
 * tests return AST_MODULE_LOAD_FAILURE. If the module can not load the
 * configuration file or other non-critical problem return
 * AST_MODULE_LOAD_DECLINE. On success return AST_MODULE_LOAD_SUCCESS.
 */
static int load_module(void)
{
	ast_sockaddr_parse(&address_ipv4, "0.0.0.0", 0);
	ast_sockaddr_parse(&address_ipv6, "::", 0);

	if (!(sched = ast_sched_context_create())) {
		ast_log(LOG_ERROR, "Unable to create scheduler context.\n");
		goto end;
	}

	if (ast_sched_start_thread(sched)) {
		ast_log(LOG_ERROR, "Unable to create scheduler context thread.\n");
		goto end;
	}

	if (ast_sip_session_register_sdp_handler(&audio_sdp_handler, STR_AUDIO)) {
		ast_log(LOG_ERROR, "Unable to register SDP handler for %s stream type\n", STR_AUDIO);
		goto end;
	}

	if (ast_sip_session_register_sdp_handler(&video_sdp_handler, STR_VIDEO)) {
		ast_log(LOG_ERROR, "Unable to register SDP handler for %s stream type\n", STR_VIDEO);
		goto end;
	}

	ast_sip_session_register_supplement(&video_info_supplement);

	return AST_MODULE_LOAD_SUCCESS;
end:
	unload_module();

	return AST_MODULE_LOAD_FAILURE;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "SIP SDP RTP/AVP stream handler",
		.load = load_module,
		.unload = unload_module,
		.load_pri = AST_MODPRI_CHANNEL_DRIVER,
	);
