/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2015, Mark Michelson
 *
 * Mark Michelson <mmichelson@digium.com>
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

/*** MODULEINFO
	<depend>TEST_FRAMEWORK</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <arpa/nameser.h>
#include <arpa/inet.h>

#include "asterisk/test.h"
#include "asterisk/module.h"
#include "asterisk/dns_core.h"
#include "asterisk/dns_resolver.h"
#include "asterisk/dns_internal.h"

/* Used when a stub is needed for certain tests */
static int stub_resolve(struct ast_dns_query *query)
{
	return 0;
}

/* Used when a stub is needed for certain tests */
static int stub_cancel(struct ast_dns_query *query)
{
	return 0;
}

AST_TEST_DEFINE(resolver_register_unregister)
{
	struct ast_dns_resolver cool_guy_resolver = {
		.name = "A snake that swallowed a deer",
		.priority = 19890504,
		.resolve = stub_resolve,
		.cancel = stub_cancel,
	};

	switch (cmd) {
	case TEST_INIT:
		info->name = "resolver_register_unregister";
		info->category = "/main/dns/";
		info->summary = "Test nominal resolver registration and unregistration";
		info->description =
			"The test performs the following steps:\n"
			"\t* Register a valid resolver.\n"
			"\t* Unregister the resolver.\n"
			"If either step fails, the test fails\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	if (ast_dns_resolver_register(&cool_guy_resolver)) {
		ast_test_status_update(test, "Unable to register a perfectly good resolver\n");
		return AST_TEST_FAIL;
	}

	ast_dns_resolver_unregister(&cool_guy_resolver);

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(resolver_register_off_nominal)
{
	struct ast_dns_resolver valid = {
		.name = "valid",
		.resolve = stub_resolve,
		.cancel = stub_cancel,
	};

	struct ast_dns_resolver incomplete1 = {
		.name = NULL,
		.resolve = stub_resolve,
		.cancel = stub_cancel,
	};

	struct ast_dns_resolver incomplete2 = {
		.name = "incomplete2",
		.resolve = NULL,
		.cancel = stub_cancel,
	};

	struct ast_dns_resolver incomplete3 = {
		.name = "incomplete3",
		.resolve = stub_resolve,
		.cancel = NULL,
	};

	switch (cmd) {
	case TEST_INIT:
		info->name = "resolver_register_off_nominal";
		info->category = "/main/dns/";
		info->summary = "Test off-nominal resolver registration";
		info->description =
			"Test off-nominal resolver registration:\n"
			"\t* Register a duplicate resolver\n"
			"\t* Register a resolver without a name\n"
			"\t* Register a resolver without a resolve() method\n"
			"\t* Register a resolver without a cancel() method\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	if (ast_dns_resolver_register(&valid)) {
		ast_test_status_update(test, "Failed to register valid resolver\n");
		return AST_TEST_FAIL;
	}

	if (!ast_dns_resolver_register(&valid)) {
		ast_test_status_update(test, "Successfully registered the same resolver multiple times\n");
		return AST_TEST_FAIL;
	}

	ast_dns_resolver_unregister(&valid);

	if (!ast_dns_resolver_register(NULL)) {
		ast_test_status_update(test, "Successfully registered a NULL resolver\n");
		return AST_TEST_FAIL;
	}

	if (!ast_dns_resolver_register(&incomplete1)) {
		ast_test_status_update(test, "Successfully registered a DNS resolver with no name\n");
		return AST_TEST_FAIL;
	}

	if (!ast_dns_resolver_register(&incomplete2)) {
		ast_test_status_update(test, "Successfully registered a DNS resolver with no resolve() method\n");
		return AST_TEST_FAIL;
	}

	if (!ast_dns_resolver_register(&incomplete3)) {
		ast_test_status_update(test, "Successfully registered a DNS resolver with no cancel() method\n");
		return AST_TEST_FAIL;
	}

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(resolver_unregister_off_nominal)
{
	struct ast_dns_resolver non_existent = {
		.name = "I do not exist",
		.priority = 20141004,
		.resolve = stub_resolve,
		.cancel = stub_cancel,
	};

	switch (cmd) {
	case TEST_INIT:
		info->name = "resolver_unregister_off_nominal";
		info->category = "/main/dns/";
		info->summary = "Test off-nominal DNS resolver unregister";
		info->description =
			"The test attempts the following:\n"
			"\t* Unregister a resolver that is not registered.\n"
			"\t* Unregister a NULL pointer.\n"
			"Because unregistering a resolver does not return an indicator of success, the best\n"
			"this test can do is verify that nothing blows up when this is attempted.\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	ast_dns_resolver_unregister(&non_existent);
	ast_dns_resolver_unregister(NULL);

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(resolver_data)
{
	struct ast_dns_query some_query;

	struct digits {
		int fingers;
		int toes;
	};
	
	struct digits average = {
		.fingers = 10,
		.toes = 10,
	};

	struct digits polydactyl = {
		.fingers = 12,
		.toes = 10,
	};

	struct digits *data_ptr;

	switch (cmd) {
	case TEST_INIT:
		info->name = "resolver_data";
		info->category = "/main/dns/";
		info->summary = "Test getting and setting data on a DNS resolver";
		info->description = "This test does the following:\n"
			"\t* Ensure that requesting resolver data results in a NULL return if no data has been set.\n"
			"\t* Ensure that setting resolver data does not result in an error.\n"
			"\t* Ensure that retrieving the set resolver data returns the data we expect\n"
			"\t* Ensure that setting new resolver data on the query does not result in an error\n"
			"\t* Ensure that retrieving the resolver data returns the new data that we set\n"
			"\t* Ensure that ast_dns_resolver_completed() removes resolver data from the query\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	memset(&some_query, 0, sizeof(some_query));

	/* Ensure that NULL is retrieved if we haven't set anything on the query */
	data_ptr = ast_dns_resolver_get_data(&some_query);
	if (data_ptr) {
		ast_test_status_update(test, "Retrieved non-NULL resolver data from query unexpectedly\n");
		return AST_TEST_FAIL;
	}

	ast_dns_resolver_set_data(&some_query, &average);

	/* Ensure that data can be set and retrieved */
	data_ptr = ast_dns_resolver_get_data(&some_query);
	if (!data_ptr) {
		ast_test_status_update(test, "Unable to retrieve resolver data from DNS query\n");
		return AST_TEST_FAIL;
	}

	if (data_ptr->fingers != average.fingers || data_ptr->toes != average.toes) {
		ast_test_status_update(test, "Unexpected resolver data retrieved from DNS query\n");
		return AST_TEST_FAIL;
	}

	/* Ensure that we can set new resolver data even if there already is resolver data on the query */
	ast_dns_resolver_set_data(&some_query, &polydactyl);

	data_ptr = ast_dns_resolver_get_data(&some_query);
	if (!data_ptr) {
		ast_test_status_update(test, "Unable to retrieve resolver data from DNS query\n");
		return AST_TEST_FAIL;
	}

	if (data_ptr->fingers != polydactyl.fingers || data_ptr->toes != polydactyl.toes) {
		ast_test_status_update(test, "Unexpected resolver data retrieved from DNS query\n");
		return AST_TEST_FAIL;
	}

	/* Ensure that ast_dns_resolver_completed() removes resolver data from the query */
	ast_dns_resolver_completed(&some_query);

	data_ptr = ast_dns_resolver_get_data(&some_query);
	if (data_ptr) {
		ast_test_status_update(test, "Query still has resolver data after query completed\n");
		return AST_TEST_FAIL;
	}

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(resolver_add_record)
{
	struct ast_dns_query some_query;
	static const char *LOCAL_ADDR = "127.0.0.1";
	static const size_t bufsize = sizeof(struct in_addr);
	char buf[bufsize];

	switch (cmd) {
	case TEST_INIT:
		info->name = "resolver_add_record";
		info->category = "/main/dns/";
		info->summary = "Test adding DNS records to a query";
		info->description =
			"This test performs the following:\n"
			"\t* Ensure a nominal A record can be added to a query\n"
			"\t* Ensure that an A record with invalid RR types cannot be added to a query\n"
			"\t* Ensure that an A record with invalid RR classes cannot be added to a query\n"
			"\t* Ensure that an A record with invalid TTL cannot be added to a query\n"
			"\t* Ensure that an A record with NULL data cannot be added to a query\n"
			"\t* Ensure that an A record with invalid length cannot be added to a query\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	memset(&some_query, 0, sizeof(some_query));

	inet_ntop(AF_INET, LOCAL_ADDR, buf, bufsize);

	/* Nominal Record */
	if (ast_dns_resolver_add_record(&some_query, ns_t_a, ns_c_in, 12345, buf, bufsize)) {
		ast_test_status_update(test, "Unable to add nominal record to query\n");
		return AST_TEST_FAIL;
	}

	/* Invalid RR types */
	if (!ast_dns_resolver_add_record(&some_query, -1, ns_c_in, 12345, buf, bufsize)) {
		ast_test_status_update(test, "Successfully added DNS record with negative RR type\n");
		return AST_TEST_FAIL;
	}
	
	if (!ast_dns_resolver_add_record(&some_query, ns_t_max + 1, ns_c_in, 12345, buf, bufsize)) {
		ast_test_status_update(test, "Successfully added DNS record with too large RR type\n");
		return AST_TEST_FAIL;
	}

	/* Invalid RR classes */
	if (!ast_dns_resolver_add_record(&some_query, ns_t_a, -1, 12345, buf, bufsize)) {
		ast_test_status_update(test, "Successfully added DNS record with negative RR class\n");
		return AST_TEST_FAIL;
	}

	if (!ast_dns_resolver_add_record(&some_query, ns_t_a, ns_c_max + 1, 12345, buf, bufsize)) {
		ast_test_status_update(test, "Successfully added DNS record with too large RR class\n");
		return AST_TEST_FAIL;
	}

	/* Invalid TTL */
	if (!ast_dns_resolver_add_record(&some_query, ns_t_a, ns_c_in, -1, buf, bufsize)) {
		ast_test_status_update(test, "Successfully added DNS record with negative TTL\n");
		return AST_TEST_FAIL;
	}

	/* No data */
	if (!ast_dns_resolver_add_record(&some_query, ns_t_a, ns_c_in, 12345, NULL, 0)) {
		ast_test_status_update(test, "Successfully added a DNS record with no data\n");
		return AST_TEST_FAIL;
	}

	/* Lie about the length */
	if (!ast_dns_resolver_add_record(&some_query, ns_t_a, ns_c_in, 12345, buf, 0)) {
		ast_test_status_update(test, "Successfully added a DNS record with length zero\n");
		return AST_TEST_FAIL;
	}
	
	if (!ast_dns_resolver_add_record(&some_query, ns_t_a, ns_c_in, 12345, buf, bufsize * 3)) {
		ast_test_status_update(test, "Successfully added a DNS record with overly-large length\n");
		return AST_TEST_FAIL;
	}

	return AST_TEST_PASS;
}

static int test_results(struct ast_test *test, const struct ast_dns_query *query,
		int expected_nxdomain, int expected_secure, int expected_bogus)
{
	struct ast_dns_result *result;

	result = ast_dns_query_get_result(query);
	if (!result) {
		ast_test_status_update(test, "Unable to retrieve result from query\n");
		return -1;
	}

	if (ast_dns_result_get_nxdomain(result) != expected_nxdomain ||
			ast_dns_result_get_secure(result) != expected_secure ||
			ast_dns_result_get_bogus(result) != expected_bogus) {
		ast_test_status_update(test, "Unexpected values in result from query\n");
		return -1;
	}

	return 0;
}

AST_TEST_DEFINE(resolver_set_result)
{
	struct ast_dns_query some_query;

	switch (cmd) {
	case TEST_INIT:
		info->name = "resolver_set_result";
		info->category = "/main/dns/";
		info->summary = "Test setting and getting results on DNS queries";
		info->description =
			"This test performs the following:\n"
			"\t* Sets a result that is not secure, bogus, or nxdomain\n"
			"\t* Sets a result that is not secure or nxdomain, but is secure\n"
			"\t* Sets a result that is not bogus or nxdomain, but is secure\n"
			"\t* Sets a result that is not secure or bogus, but is nxdomain\n"
			"After each result is set, we ensure that parameters retrieved from\n"
			"the result have the expected values.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	memset(&some_query, 0, sizeof(some_query));

	if (ast_dns_resolver_set_result(&some_query, 0, 0, 0, "asterisk.org")) {
		ast_test_status_update(test, "Unable to add legitimate DNS result to query\n");
		return AST_TEST_FAIL;
	}

	if (test_results(test, &some_query, 0, 0, 0)) {
		return AST_TEST_FAIL;
	}

	if (ast_dns_resolver_set_result(&some_query, 0, 0, 1, "asterisk.org")) {
		ast_test_status_update(test, "Unable to add bogus DNS result to query\n");
		return AST_TEST_FAIL;
	}

	if (test_results(test, &some_query, 0, 0, 1)) {
		return AST_TEST_FAIL;
	}

	if (ast_dns_resolver_set_result(&some_query, 0, 1, 0, "asterisk.org")) {
		ast_test_status_update(test, "Unable to add secure DNS result to query\n");
		return AST_TEST_FAIL;
	}

	if (test_results(test, &some_query, 0, 1, 0)) {
		return AST_TEST_FAIL;
	}

	if (ast_dns_resolver_set_result(&some_query, 1, 0, 0, "asterisk.org")) {
		ast_test_status_update(test, "Unable to add nxdomain DNS result to query\n");
		return AST_TEST_FAIL;
	}

	if (test_results(test, &some_query, 1, 0, 0)) {
		return AST_TEST_FAIL;
	}

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(resolver_set_result_off_nominal)
{
	struct ast_dns_query some_query;

	switch (cmd) {
	case TEST_INIT:
		info->name = "resolver_set_result_off_nominal";
		info->category = "/main/dns/";
		info->summary = "Test setting off-nominal DNS results\n";
		info->description =
			"This test performs the following:\n"
			"\t* Attempt to add a DNS result that is both bogus and secure\n"
			"\t* Attempt to add a DNS result that has no canonical name\n";
	case TEST_EXECUTE:
		break;
	}

	memset(&some_query, 0, sizeof(some_query));

	if (!ast_dns_resolver_set_result(&some_query, 0, 1, 1, "asterisk.org")) {
		ast_test_status_update(test, "Successfully added a result that was both secure and bogus\n");
		return AST_TEST_FAIL;
	}

	if (!ast_dns_resolver_set_result(&some_query, 0, 0, 0, NULL)) {
		ast_test_status_update(test, "Successfully added result with no canonical name\n");
		return AST_TEST_FAIL;
	}

	return AST_TEST_PASS;
}

static int unload_module(void)
{
	AST_TEST_UNREGISTER(resolver_register_unregister);
	AST_TEST_UNREGISTER(resolver_register_off_nominal);
	AST_TEST_UNREGISTER(resolver_unregister_off_nominal);
	AST_TEST_UNREGISTER(resolver_data);
	AST_TEST_UNREGISTER(resolver_add_record);
	AST_TEST_UNREGISTER(resolver_set_result);
	AST_TEST_UNREGISTER(resolver_set_result_off_nominal);

	return 0;
}

static int load_module(void)
{
	AST_TEST_REGISTER(resolver_register_unregister);
	AST_TEST_REGISTER(resolver_register_off_nominal);
	AST_TEST_REGISTER(resolver_unregister_off_nominal);
	AST_TEST_REGISTER(resolver_data);
	AST_TEST_REGISTER(resolver_add_record);
	AST_TEST_REGISTER(resolver_set_result);
	AST_TEST_REGISTER(resolver_set_result_off_nominal);

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "DNS API Tests");
