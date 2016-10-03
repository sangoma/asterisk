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
 * \brief Metrics API
 *
 * \author Moises Silva <msilva@sangoma.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_REGISTER_FILE()

#include "asterisk/logger.h"
#include "asterisk/strings.h"
#include "asterisk/module.h"
#include "asterisk/cli.h"
#include "asterisk/metrics.h"
#include "asterisk/linkedlists.h"

/* FIXME: Is a linked list the right data structure to use for the metric registry? note
 *        collectd uses an avl tree and given that we'll basically
 *        provide a tree of metrics, perhaps it'd be fitting to use that too? */
/*! \brief List of metrics */
struct ast_metrics ast_metrics = AST_RWLIST_HEAD_INIT_VALUE;
static uint32_t metric_count = 0;

static const char *ast_metric_type2str(enum ast_metric_type type)
{
	switch (type) {
	case AST_METRIC_TYPE_COUNTER:
		return "counter";
	case AST_METRIC_TYPE_GAUGE:
		return "gauge";
	case AST_METRIC_TYPE_TIMER:
		return "timer";
	default:
		return "<unknown>";
	}
}

static char *show_metrics(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_metric_node *mn;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show metrics";
		e->usage =
			"Usage: core show metrics\n"
			"       Displays metrics\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}

	ast_cli(a->fd, "%-40s %-10s %-80s %-10s\n","NAME","TYPE","DESCRIPTION","VALUE");
	ast_cli(a->fd, "---------------------------------------------------------------------------------------------------------------------------------------------\n");

	AST_RWLIST_RDLOCK(&ast_metrics);
	AST_RWLIST_TRAVERSE(&ast_metrics, mn, list) {
		ast_cli(a->fd, "%-40s %-10s %-80s %f\n",
			mn->metric->name,
			ast_metric_type2str(mn->metric->type),
			mn->metric->description,
			ast_metric_value(mn->metric));
	}
	AST_RWLIST_UNLOCK(&ast_metrics);
	ast_cli(a->fd, "---------------------------------------------------------------------------------------------------------------------------------------------\n");
	ast_cli(a->fd, "%d metrics registered.\n", metric_count);

	return CLI_SUCCESS;
}

/* Builtin Asterisk CLI-commands for debugging */
static struct ast_cli_entry metrics_cli[] = {
	AST_CLI_DEFINE(show_metrics, "Displays a list of registered metrics"),
};

/*! \brief Function called when the process is shutting down */
static void metrics_shutdown(void)
{
	ast_cli_unregister_multiple(metrics_cli, ARRAY_LEN(metrics_cli));
}

int ast_metrics_init(void)
{
	ast_cli_register_multiple(metrics_cli, ARRAY_LEN(metrics_cli));
	ast_register_cleanup(metrics_shutdown);
	return 0;
}

int ast_metric_register(struct ast_metric *metric)
{
	struct ast_metric_node *mn;
	AST_RWLIST_WRLOCK(&ast_metrics);

	AST_RWLIST_TRAVERSE(&ast_metrics, mn, list) {
		if (!strcasecmp(metric->name, mn->metric->name)) {
			AST_RWLIST_UNLOCK(&ast_metrics);
			ast_log(LOG_ERROR, "A metric with name %s has already been registered\n", metric->name);
			return -1;
		}
	}

	if (!(mn = ast_calloc(1, sizeof(*mn)))) {
		AST_RWLIST_UNLOCK(&ast_metrics);
		return -1;
	}
	mn->metric = metric;
	AST_RWLIST_INSERT_HEAD(&ast_metrics, mn, list);
	metric_count++;
	AST_RWLIST_UNLOCK(&ast_metrics);

	ast_debug(1, "Registered metric '%s' ('%s') with type '%s'\n",
			     metric->name, metric->description, ast_metric_type2str(metric->type));
	return 0;
}

int ast_metric_unregister(struct ast_metric *metric)
{
	struct ast_metric_node *mn;
	AST_RWLIST_WRLOCK(&ast_metrics);

	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&ast_metrics, mn, list) {
		if (!strcasecmp(metric->name, mn->metric->name)) {
			AST_LIST_REMOVE_CURRENT(list);
			ast_free(mn);
			metric_count--;
			ast_debug(1, "Unregistered metric '%s'\n", metric->name);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END

	AST_RWLIST_UNLOCK(&ast_metrics);

	ast_debug(1, "Registered metric '%s' ('%s') with type '%s'\n",
			      metric->name, metric->description, ast_metric_type2str(metric->type));
	return 0;
}

float ast_metric_increment(struct ast_metric *metric)
{
	float ret;
	ast_mutex_lock(&metric->mutex);
	metric->value++;
	ret = metric->value;
	ast_mutex_unlock(&metric->mutex);
	return ret;
}

float ast_metric_increment_by(struct ast_metric *metric, float value)
{
	float ret;
	ast_mutex_lock(&metric->mutex);
	metric->value += value;
	ret = metric->value;
	ast_mutex_unlock(&metric->mutex);
	return ret;
}

float ast_metric_decrement(struct ast_metric *metric)
{
	float ret;
	ast_mutex_lock(&metric->mutex);
	metric->value--;
	ret = metric->value;
	ast_mutex_unlock(&metric->mutex);
	return ret;
}

float ast_metric_decrement_by(struct ast_metric *metric, float value)
{
	float ret;
	ast_mutex_lock(&metric->mutex);
	metric->value -= value;
	ret = metric->value;
	ast_mutex_unlock(&metric->mutex);
	return ret;
}

float ast_metric_gauge_set(struct ast_metric *metric, float value)
{
	float ret;
	ast_mutex_lock(&metric->mutex);
	metric->value = value;
	ret = metric->value;
	ast_mutex_unlock(&metric->mutex);
	return ret;
}

float ast_metric_value(struct ast_metric *metric)
{
	float ret;
	ast_mutex_lock(&metric->mutex);
	if (!metric->func) {
		ret = metric->value;
	} else {
		ret = metric->func();
	}
	ast_mutex_unlock(&metric->mutex);
	return ret;
}
