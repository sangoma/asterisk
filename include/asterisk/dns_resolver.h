/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2015, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
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
 * \brief DNS Resolver API
 * \author Joshua Colp <jcolp@digium.com>
 */

#ifndef _ASTERISK_DNS_RESOLVER_H
#define _ASTERISK_DNS_RESOLVER_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! \brief DNS resolver implementation */
struct ast_dns_resolver {
    /*! \brief The name of the resolver implementation */
    const char *name;
    /*! \brief Priority for this resolver if multiple exist */
    unsigned int priority;
    /*! \brief Perform resolution of a DNS query */
    int (*resolve)(struct ast_dns_query *query);
    /*! \brief Cancel resolution of a DNS query */
    int (*cancel)(struct ast_dns_query *query);
    /*! \brief Linked list information */
    AST_RWLIST_ENTRY(ast_dns_resolver) next;
};

/*!
 * \brief Set resolver specific data on a query
 *
 * \param query The DNS query
 * \param data The resolver specific data
 *
 * \note Unlike user specific data this does not have to be ao2 allocated
 */
void ast_dns_resolver_set_data(struct ast_dns_query *query, void *data);

/*!
 * \brief Retrieve resolver specific data
 *
 * \param query The DNS query
 *
 * \return the resolver specific data
 */
void *ast_dns_resolver_get_data(const struct ast_dns_query *query);

/*!
 * \brief Set result information for a DNS query
 *
 * \param query The DNS query
 * \param nxdomain Whether the domain was not found
 * \param result Whether the result is secured or not
 * \param bogus Whether the result is bogus or not
 * \param canonical The canonical name
 *
 * \retval 0 success
 * \retval -1 failure
 */
int ast_dns_resolver_set_result(struct ast_dns_query *query, unsigned int nxdomain, unsigned int secure, unsigned int bogus,
	const char *canonical);

/*!
 * \brief Add a DNS record to the result of a DNS query
 *
 * \param query The DNS query
 * \param rr_type Resource record type
 * \param rr_class Resource record class
 * \param ttl TTL of the record
 * \param data The raw DNS record
 * \param size The size of the raw DNS record
 *
 * \retval 0 success
 * \retval -1 failure
 */
int ast_dns_resolver_add_record(struct ast_dns_query *query, int rr_type, int rr_class, int ttl, const char *data, const size_t size);

/*!
 * \brief Mark a DNS query as having been completed
 *
 * \param query The DNS query
 *
 * \note Once this is invoked the resolver data on the query will be removed
 */
void ast_dns_resolver_completed(const struct ast_dns_query *query);

/*!
 * \brief Register a DNS resolver
 *
 * \param resolver A DNS resolver implementation
 *
 * \retval 0 success
 * \retval -1 failure
 */
int ast_dns_resolver_register(struct ast_dns_resolver *resolver);

/*!
 * \brief Unregister a DNS resolver
 *
 * \param resolver A DNS resolver implementation
 */
void ast_dns_resolver_unregister(struct ast_dns_resolver *resolver);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ASTERISK_DNS_RESOLVER_H */
