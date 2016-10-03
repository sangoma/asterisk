/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2016, Sangoma Technologies, Corp.
 *
 * Moises Silva <msilva@sangoma.com>
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
 * \brief Prometheus metrics client
 *
 * \author Moises Silva <msilva@sangoma.com>
 *
 * https://prometheus.io/
 * https://prometheus.io/docs/instrumenting/exposition_formats/
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

ASTERISK_REGISTER_FILE()

#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/metrics.h"
#include "asterisk/http.h"

static int prometheus_http_callback(struct ast_tcptls_session_instance *ser, const struct ast_http_uri *urih, const char *uri, enum ast_http_method method, struct ast_variable *get_params,  struct ast_variable *headers);

static struct ast_http_uri prometheus_metrics_uri = {
	.description = "Prometheus Metrics Endpoint",
	.uri = "prometheus_metrics",
	.callback = prometheus_http_callback,
};

static int prometheus_http_callback(struct ast_tcptls_session_instance *ser, const struct ast_http_uri *urih, const char *uri, enum ast_http_method method, struct ast_variable *get_params,  struct ast_variable *headers)
{
	struct ast_str *http_header, *metrics;
	struct ast_metric_node *mn;
	struct timeval now;
	uint64_t currtime;

	ast_http_request_close_on_completion(ser);

	http_header = ast_str_create(128);
	metrics = ast_str_create(4096);

	if (!http_header || !metrics) {
		ast_http_error(ser, 500, "Server Error", "Internal Server Error (ast_str_create() out of memory)");
		goto http_done;
	}

	ast_str_append(&http_header, 0, "Content-Type: text/plain; version=0.0.4\r\n");
	/* TODO: Iterate over metrics and return them */
	now = ast_tvnow();
	currtime = (now.tv_sec * 1000) + (now.tv_usec / 1000);
	AST_RWLIST_RDLOCK(&ast_metrics);
	AST_RWLIST_TRAVERSE(&ast_metrics, mn, list) {
		ast_str_append(&metrics, 0, "%s %f %lu\n", mn->metric->name, ast_metric_value(mn->metric), currtime);
		// FIXME: Check for overflow? or check if ast_str will grow the buffer?
	}
	AST_RWLIST_UNLOCK(&ast_metrics);

	ast_http_send(ser, method, 200, NULL, http_header, metrics, 0, 0);
	http_header = NULL;
	metrics = NULL;

http_done:
	ast_free(http_header);
	ast_free(metrics);
	return 0;
}

static int load_module(void)
{
	ast_verb(1, "Loading Prometheus Module\n");
	ast_http_uri_link(&prometheus_metrics_uri);
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_verb(1, "Unloading Prometheus Module\n");
	ast_http_uri_unlink(&prometheus_metrics_uri);
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Expose Asterisk Metrics for Prometheus",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
);
