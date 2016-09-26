/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2016, Digium, Inc.
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

/*!
 * \file
 * \brief Metrics API
 *
 * \author Moises Silva <msilva@sangoma.com>
 */

#ifndef _AST_METRICS_H_
#define _AST_METRICS_H_

/*! \brief Types of metrics */
enum ast_metric_type {
	AST_METRIC_TYPE_COUNTER,
	AST_METRIC_TYPE_GAUGE,
	AST_METRIC_TYPE_TIMER
};

typedef float (*ast_metric_func_t)(void);

/*! \brief Metric 
 *  \todo Provide macro to initialize this statically */
struct ast_metric {
	enum ast_metric_type type;
	const char *name;
	const char *description;
	float value;
	ast_mutex_t mutex;
	/* optional function to retrieve the metric value if the float value is not used */
	ast_metric_func_t func;
};

#define AST_METRIC(mtype, mname, mdesc) \
static struct ast_metric mname = \
{ \
	.type = mtype, \
	.name = #mname, \
	.description = mdesc, \
	.value = 0.0, \
	.mutex = AST_MUTEX_INIT_VALUE, \
	.func = NULL \
}

#define AST_METRIC_FUNC(mtype, mname, mdesc, mfunc) \
static struct ast_metric mname = \
{ \
	.type = mtype, \
	.name = #mname, \
	.description = mdesc, \
	.value = 0.0, \
	.mutex = AST_MUTEX_INIT_VALUE, \
	.func = mfunc \
}

/*!
 * \brief Initialize metrics support within the core.
 *
 * \retval 0 success
 * \retval -1 failure
 */
int ast_metrics_init(void);

/*!
 * \brief Register a metric
 *
 * \retval -1 failure
 */
int ast_metric_register(struct ast_metric *metric);

/*!
 * \brief Initialize metrics support within the core.
 *
 * \retval -1 failure
 */
int ast_metric_unregister(struct ast_metric *metric);

/*!
 * \brief Increment metric value
 *
 * \retval New metric value
 */
float ast_metric_increment(struct ast_metric *metric);
float ast_metric_increment_by(struct ast_metric *metric, float value);

/*!
 * \brief Decrement metric value
 *
 * \retval New metric value
 */
float ast_metric_decrement(struct ast_metric *metric);
float ast_metric_decrement_by(struct ast_metric *metric, float value);

/*!
 * \brief Set the metric to the given value
 *
 * \retval New metric value
 */
float ast_metric_gauge_set(struct ast_metric *metric, float value);

/*!
 * \brief Retrieve metric value
 *
 * \retval Get metric value
 */
float ast_metric_value(struct ast_metric *metric);

#endif /* _AST_METRICS_H */
