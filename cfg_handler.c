/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Config data handler
 *
 * Copyright (C) 2016 Nick Kossifidis <mickflemm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE		/* Needed for strptime() */
#include "scheduler.h"
#include "utils.h"
#include <string.h>		/* For memset() / strncmp() */
#include <time.h>		/* For strptime() and time() */
#include <signal.h>		/* For sig_atomic_t */
#include <stdlib.h>		/* For strtof(), malloc/realloc/free() */
#include <libxml/parser.h>	/* For parser context etc */
#include <libxml/tree.h>	/* For grabbing stuff off the tree */
#include <libxml/valid.h>	/* For validation context etc */
#include <libxml/xmlschemas.h>	/* For schema context etc */

static volatile sig_atomic_t	parser_failed = 0;


/*********\
* HELPERS *
\*********/

static char*
cfg_get_string(xmlDocPtr config, xmlNodePtr element)
{
	char* value = (char*) xmlNodeListGetString(config,
				element->xmlChildrenNode, 1);
	if(value == NULL)
		parser_failed = 1;

	utils_trim_string(value);

	utils_dbg(CFG, "Got string: %s\n", value);
	return value;
}

static float
cfg_get_float(xmlDocPtr config, xmlNodePtr element)
{
	float ret = 0;
	char* end_ptr = NULL;
	char* value = cfg_get_string(config, element);
	if(parser_failed)
		return -1.0;

	ret = strtof(value, &end_ptr);
	if(value == end_ptr) {
		utils_perr(CFG, "Failed to parse float");
		xmlFree(value);
		parser_failed = 1;
		return -1.0;
	}

	xmlFree(value);
	utils_dbg(CFG, "Got float: %g\n", ret);
	return ret;
}

static int
cfg_get_integer(xmlDocPtr config, xmlNodePtr element)
{
	int ret = 0;
	char* value = cfg_get_string(config, element);
	if(parser_failed)
		return -1;

	ret = atoi(value);

	xmlFree(value);
	utils_dbg(CFG, "Got integer: %i\n", ret);
	return ret;
}

static int
cfg_get_boolean(xmlDocPtr config, xmlNodePtr element)
{
	int ret = 0;
	char* value = cfg_get_string(config, element);
	if(parser_failed)
		return -1;

	if (!strncmp(value, "true", 5))
		ret = 1;
	else
		ret = 0;

	xmlFree(value);
	utils_dbg(CFG, "Got boolean: %s\n", ret ? "true" : "false");
	return ret;
}

static char*
cfg_get_str_attr(xmlNodePtr element, const char* attr)
{
	char* value = (char*) xmlGetProp(element, (const xmlChar*) attr);
	if(value == NULL) {
		parser_failed = 1;
		return NULL;
	}
	utils_trim_string(value);
	return value;
}

static void
cfg_get_start_attr(xmlDocPtr config, xmlNodePtr element, struct tm *time)
{
	char* time_string = (char*) xmlGetProp(element, (const xmlChar*) "Start");
	if(time_string == NULL) {
		parser_failed = 1;
		return;
	}
	utils_trim_string(time_string);
	strptime(time_string, "%T", time);
	utils_dbg(CFG, "Got start time: %s\n", time_string);
	xmlFree((xmlChar*) time_string);
}


/****************\
* FADER HANDLING *
\****************/

static void
cfg_free_fader(struct fader *fdr)
{
	free(fdr);
}

static struct fader*
cfg_get_fader(xmlDocPtr config, xmlNodePtr fdr_node)
{
	struct fader *fdr = NULL;
	xmlNodePtr element = NULL;
	int failed = 0;

	if(parser_failed)
		return NULL;

	/* Allocate a new fader structure and
	 * zero it out */
	fdr = (struct fader*) malloc(sizeof(struct fader));
	if (!fdr) {
		utils_err(CFG, "Unable to allocate fader structure !\n");
		parser_failed = 1;
		return NULL;
	}
	memset(fdr, 0, sizeof(struct fader));

	/* Fill it up */
	element = fdr_node->xmlChildrenNode;
	while (element != NULL) {
		if(!strncmp((const char*) element->name, "FadeInDurationSecs", 19))
			fdr->fadein_duration_secs = cfg_get_integer(config, element);
		else if(!strncmp((const char*) element->name, "FadeOutDurationSecs", 20))
			fdr->fadeout_duration_secs = cfg_get_integer(config, element);
		else if(!strncmp((const char*) element->name, "MinLevel", 9))
			fdr->min_lvl = cfg_get_float(config, element);
		else if(!strncmp((const char*) element->name, "MaxLevel", 9))
			fdr->max_lvl = cfg_get_float(config, element);
		if(parser_failed) {
			utils_err(CFG, "Parsing of fader element failed\n");
			parser_failed = 1;
			goto cleanup;
		}
		element = element->next;
	}

	/* Sanity check, at least one duration field needs to be set,
	 * note that fader is an optional element so failure here
	 * should not be fatal */
	if(!fdr->fadein_duration_secs || !fdr->fadeout_duration_secs) {
		utils_wrn(CFG, "Got empty fader element\n");
		failed = 1;
		goto cleanup;
	}

	utils_dbg(CFG, "Got fader\n\tFade in duration (secs): %i\n\t"
			"Fade out duration (secs): %i\n\t"
			"Minimum level: %g\n\t"
			"Maximum level: %g\n",
			fdr->fadein_duration_secs, fdr->fadeout_duration_secs,
			fdr->min_lvl, fdr->max_lvl);

cleanup:
	if(parser_failed || failed) {
		cfg_free_fader(fdr);
		fdr = NULL;
	}
	return fdr;
}

/*******************\
* PLAYLIST HANDLING *
\*******************/

static void
cfg_free_pls(struct playlist *pls)
{
	if(!pls)
		return;

	if(pls->filepath) {
		xmlFree((xmlChar*) pls->filepath);
		pls->filepath = NULL;
	}

	if(pls->fader) {
		cfg_free_fader(pls->fader);
		pls->fader = NULL;
	}

	pls_files_cleanup(pls);

	free(pls);
}

static struct playlist*
cfg_get_pls(xmlDocPtr config, xmlNodePtr pls_node)
{
	struct playlist *pls = NULL;
	xmlNodePtr element = NULL;
	int ret = 0;

	if(parser_failed)
		return NULL;

	/* Allocate a new playlist structure and
	 * zero it out */
	pls = (struct playlist*) malloc(sizeof(struct playlist));
	if (!pls) {
		utils_err(CFG, "Unable to allocate playlist structure !\n");
		parser_failed = 1;
		return NULL;
	}
	memset(pls, 0, sizeof(struct playlist));

	/* Fill it up */
	element = pls_node->xmlChildrenNode;
	while (element != NULL) {
		if(!strncmp((const char*) element->name, "Path", 5))
			pls->filepath = cfg_get_string(config, element);
		else if(!strncmp((const char*) element->name, "Shuffle", 8))
			pls->shuffle = cfg_get_boolean(config, element);
		else if(!strncmp((const char*) element->name, "Fader", 5))
			pls->fader = cfg_get_fader(config, element);
		if(parser_failed) {
			utils_err(CFG, "Parsing of playlist element failed\n");
			goto cleanup;
		}
		element = element->next;
	}

	/* Sanity check, note that fade duration is optional */
	if(!pls->filepath) {
		utils_err(CFG, "Filepath missing from playlist element\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Fill up the items array */
	ret = pls_process(pls);
	if(ret < 0) {
		utils_err(CFG, "Got empty/malformed playlist: %s\n", pls->filepath);
		parser_failed = 1;
		goto cleanup;	
	}

	utils_dbg(CFG, "Got playlist: %s\n\tShuffle: %s\n\tFader: %s\n",
		   pls->filepath, pls->shuffle ? "true" : "false",
		   pls->fader ? "true" : "false");

cleanup:
	if(parser_failed) {
		cfg_free_pls(pls);
		pls = NULL;
	}
	return pls;
}


/********************************\
* INTERMEDIATE PLAYLIST HANDLING *
\********************************/

static void
cfg_free_ipls(struct intermediate_playlist *ipls)
{
	if(!ipls)
		return;

	if(ipls->name) {
		xmlFree((xmlChar*) ipls->name);
		ipls->name = NULL;
	}
	if(ipls->filepath) {
		xmlFree((xmlChar*) ipls->filepath);
		ipls->filepath = NULL;
	}
	if(ipls->fader) {
		cfg_free_fader(ipls->fader);
		ipls->fader = NULL;
	}

	pls_files_cleanup((struct playlist*) ipls);

	free(ipls);
}

static struct intermediate_playlist*
cfg_get_ipls(xmlDocPtr config, xmlNodePtr ipls_node)
{
	struct intermediate_playlist *ipls = NULL;
	time_t curr_time = time(NULL);
	xmlNodePtr element = NULL;
	int ret = 0;

	if(parser_failed)
		return NULL;

	/* Allocate a new intermediate playlist structure
	 * and zero it out */
	ipls = (struct intermediate_playlist*)
		malloc(sizeof(struct intermediate_playlist));
	if (!ipls) {
		utils_err(CFG, "Unable to allocate intermediate playlist !\n");
		parser_failed = 1;
		return NULL;
	}
	memset(ipls, 0, sizeof(struct intermediate_playlist));

	/* Name attribute is mandatory */
	ipls->name = cfg_get_str_attr(ipls_node, "Name");
	if(parser_failed) {
		utils_err(CFG,  "Could not get name atrribute"
				" for an intermediate playlist\n");
		goto cleanup;
	}

	/* Fill it up */
	element = ipls_node->xmlChildrenNode;
	while (element != NULL) {
		if(!strncmp((const char*) element->name, "Path", 5))
			ipls->filepath = cfg_get_string(config, element);
		else if(!strncmp((const char*) element->name, "Shuffle", 8))
			ipls->shuffle = cfg_get_boolean(config, element);
		else if(!strncmp((const char*) element->name, "Fader", 5))
			ipls->fader = cfg_get_fader(config, element);
		else if(!strncmp((const char*) element->name, "SchedIntervalMins", 18))
			ipls->sched_interval_mins = cfg_get_integer(config, element);
		else if(!strncmp((const char*) element->name, "NumSchedItems", 14))
			ipls->num_sched_items = cfg_get_integer(config, element);
		if(parser_failed) {
			utils_err(CFG, "Parsing of intermediate playlist %s failed\n",
				  ipls->name);
			goto cleanup;
		}
		element = element->next;
	}

	/* Sanity check, note that fade duration is optional */
	if(!ipls->filepath) {
		utils_err(CFG, "Filepath missing from %s\n", ipls->name);
		parser_failed = 1;
		goto cleanup;
	}

	if(!ipls->sched_interval_mins) {
		utils_err(CFG, "No scheduling interval set for %s\n", ipls->name);
		parser_failed = 1;
		goto cleanup;
	}

	if(!ipls->num_sched_items) {
		utils_err(CFG, "Number of items to be scheduled set to 0 for %s\n",
			  ipls->name);
		parser_failed = 1;
		goto cleanup;
	}

	/* Fill up the items array */
	ret = pls_process((struct playlist*) ipls);
	if(ret < 0) {
		utils_err(CFG, "Got empty/malformed playlist: %s\n", ipls->filepath);
		parser_failed = 1;
		goto cleanup;	
	}

	/* Initialize ipls by setting sched_items_pending and last_scheduled */
	ipls->sched_items_pending = ipls->num_sched_items;
	ipls->last_scheduled = curr_time;

	utils_dbg(CFG, "Got intermediate playlist: %s\n\tFile:%s\n\tShuffle: %s\n\t",
		  ipls->name, ipls->filepath, ipls->shuffle ? "true" : "false");

	utils_dbg(CFG|SKIP, "Fader: %s\n\tScheduling interval: %i\n\t"
		   "Items to schedule: %i\n", ipls->fader ? "true" : "false",
		   ipls->sched_interval_mins, ipls->num_sched_items);

cleanup:
	if(parser_failed) {
		cfg_free_ipls(ipls);
		ipls = NULL;
	}
	return ipls;
}


/***************\
* ZONE HANDLING *
\***************/

static void
cfg_free_zone(struct zone *zone)
{
	int i = 0;

	if(!zone)
		return;

	if(zone->name) {
		xmlFree((xmlChar*) zone->name);
		zone->name = NULL;
	}
	if(zone->maintainer) {
		xmlFree((xmlChar*) zone->maintainer);
		zone->maintainer = NULL;
	}
	if(zone->description) {
		xmlFree((xmlChar*) zone->description);
		zone->description = NULL;
	}
	if(zone->comment) {
		xmlFree((xmlChar*) zone->comment);
		zone->comment = NULL;
	}
	if(zone->main_pls) {
		cfg_free_pls(zone->main_pls);
		zone->main_pls = NULL;
	}
	if(zone->fallback_pls) {
		cfg_free_pls(zone->fallback_pls);
		zone->fallback_pls = NULL;
	}
	if (zone->others) {
		for(i = 0; i < zone->num_others; i++)
			if(zone->others[i]) {
				cfg_free_ipls(zone->others[i]);
				zone->others[i] = NULL;
			}
		free(zone->others);
		zone->others = NULL;
	}
	free(zone);
}

static struct zone*
cfg_get_zone(xmlDocPtr config, xmlNodePtr zone_node)
{
	struct zone *zn = NULL;
	xmlNodePtr element = NULL;

	if(parser_failed)
		return NULL;

	/* Allocate a new zone structure and
	 * zero it out */
	zn = (struct zone*) malloc(sizeof(struct zone));
	if (!zn) {
		utils_err(CFG, "Unable to allocate zone !\n");
		parser_failed = 1;
		return zn;
	}
	memset(zn, 0, sizeof(struct zone));

	/* Name and start time attributes are both
	 * mandatory */
	zn->name = cfg_get_str_attr(zone_node, "Name");
	if(parser_failed) {
		utils_err(CFG, "Could not get name atrribute for a zone\n");
		goto cleanup;
	}

	cfg_get_start_attr(config, zone_node, &zn->start_time);
	if(parser_failed) {
		utils_err(CFG, "Could not get start time attribute for zone %s\n",
			  zn->name);
		goto cleanup;
	}

	/* Fill it up */
	element = zone_node->xmlChildrenNode;
	while (element != NULL) {
		if(!strncmp((const char*) element->name, "Maintainer", 11))
			zn->maintainer = cfg_get_string(config, element);
		else if(!strncmp((const char*) element->name, "Description", 12))
			zn->description = cfg_get_string(config, element);
		else if(!strncmp((const char*) element->name, "Comment", 8))
			zn->comment = cfg_get_string(config, element);
		else if(!strncmp((const char*) element->name, "Main", 5))
			zn->main_pls = cfg_get_pls(config,element);
		else if(!strncmp((const char*) element->name, "Fallback", 9))
			zn->fallback_pls = cfg_get_pls(config,element);
		else if(!strncmp((const char*) element->name, "Intermediate", 13)) {
			/* Expand the others array */
			struct intermediate_playlist **new_ptr = realloc(zn->others, ((zn->num_others + 1) *
									 (sizeof(struct intermediate_playlist*))));
			if(!new_ptr) {
				utils_err(CFG, "Could not re-alloc zone!\n");
				parser_failed = 1;
				goto cleanup;
			}
			zn->num_others++;
			zn->others = new_ptr;

			/* Grab and store the ipls */
			zn->others[zn->num_others - 1] = cfg_get_ipls(config,element);
		}
		if(parser_failed) {
			utils_err(CFG, "Parsing of zone %s failed\n", zn->name);
			goto cleanup;
		}
		element = element->next;
	}

	/* Note: only Main playlist is mandatory */
	if(!zn->main_pls) {
		utils_err(CFG, "Got zone with no main playlist: %s\n", zn->name);
		parser_failed = 1;
		goto cleanup;
	}

	utils_dbg(CFG, "Got zone: %s\n\tMaintainer: %s\n\tDescription: %s\n\t",
		  zn->name, zn->maintainer, zn->description, zn->comment);
	utils_dbg(CFG|SKIP, "Comment: %s\n\tnum_others: %i\n", zn->comment,
		   zn->num_others);

cleanup:
	if(parser_failed) {
		cfg_free_zone(zn);
		zn = NULL;
	}
	return zn;
}


/***********************\
* DAY SCHEDULE HANDLING *
\***********************/

static void
cfg_free_day_schedule(struct day_schedule* ds)
{
	int i = 0;

	if(!ds)
		return;

	if(ds->zones) {
		for(i = 0; i < ds->num_zones; i++)
			if(ds->zones[i] != NULL) {
				cfg_free_zone(ds->zones[i]);
				ds->zones[i] = NULL;
			}
		free(ds->zones);
	}

	free(ds);
}

static struct day_schedule*
cfg_get_day_schedule(xmlDocPtr config, xmlNodePtr ds_node)
{
	struct day_schedule *ds = NULL;
	struct zone *tmp_zn0 = NULL;
	struct zone *tmp_zn1 = NULL;
	struct tm *tmp_tm = NULL;
	xmlNodePtr element = NULL;
	int got_start_of_day = 0;
	int ret = 0;

	if(parser_failed)
		return NULL;

	/* Allocate a day schedule structure and
	 * zero it out */
	ds = (struct day_schedule*) malloc(sizeof(struct day_schedule));
	if (!ds) {
		utils_err(CFG, "Unable to allocate day schedule !\n");
		parser_failed = 1;
		return ds;
	}
	memset(ds, 0, sizeof(struct day_schedule));

	/* Fill it up */
	element = ds_node->xmlChildrenNode;
	while (element != NULL) {
		/* Only zones are expected */
		if(strncmp((const char*) element->name, "Zone",5) != 0) {
			element = element->next;
			continue;
		}

		/* Expand the zones array */
		struct zone **new_ptr = realloc(ds->zones, ((ds->num_zones + 1) * sizeof(struct zone*)));
		if(!new_ptr) {
			utils_err(CFG, "Could not re-alloc day schedule!\n");
			parser_failed = 1;
			goto cleanup;
		}
		ds->num_zones++;
		ds->zones = new_ptr;

		ds->zones[ds->num_zones - 1] = cfg_get_zone(config,element);
		if((!ds->zones[ds->num_zones - 1]) || parser_failed){
			utils_err(CFG, "Parsing of a day schedule failed\n");
			goto cleanup;
		}

		/* Check if we got a zone with a start time of  00:00:00 */
		tmp_tm = &ds->zones[ds->num_zones - 1]->start_time;
		if(tmp_tm->tm_hour == 0 && tmp_tm->tm_min == 0 &&
		   tmp_tm->tm_sec == 0)
			got_start_of_day = 1;

		/* Demand that zones are stored in ascending order
		 * based on their start time. We do this to keep
		 * the lookup code simple and efficient. */
		if(ds->num_zones > 1) {
			tmp_zn0 = ds->zones[ds->num_zones - 2];
			tmp_zn1 = ds->zones[ds->num_zones - 1];
			ret = utils_compare_time(&tmp_zn1->start_time,
						 &tmp_zn0->start_time, 1);
			if(ret < 0) {
				utils_err(CFG, "Zones stored in wrong order for %s\n",
					  ds_node->name);
				parser_failed = 1;
				goto cleanup;
			} else if (!ret) {
				utils_err(CFG, "Overlapping zones on %s\n",
					  ds_node->name);
				parser_failed = 1;
				goto cleanup;
			}
		}

		element = element->next;
	}

	/* At least a zone is needed */
	if(!ds->num_zones) {
		utils_err(CFG, "Got empty day schedule element (%s)\n",
			  ds_node->name);
		parser_failed = 1;
		goto cleanup;
	}

	if(!got_start_of_day)
		utils_wrn(CFG, "Nothing scheduled on 00:00:00 for %s\n",
			  ds_node->name);

	utils_info(CFG, "Got day schedule for %s, num_zones: %i\n",
		   ds_node->name, ds->num_zones);

cleanup:
	if(parser_failed) {
		cfg_free_day_schedule(ds);
		ds = NULL;
	}
	return ds;
}


/************************\
* WEEK SCHEDULE HANDLING *
\************************/

static void
cfg_free_week_schedule(struct week_schedule *ws)
{
	int i = 0;
	if(!ws)
		return;

	for(i = 0; i < 7; i++)
		if(ws->days[i] != NULL) {
			cfg_free_day_schedule(ws->days[i]);
			ws->days[i] = NULL;
		}

	free(ws);
}

static struct week_schedule*
cfg_get_week_schedule(xmlDocPtr config, xmlNodePtr ws_node)
{
	struct week_schedule *ws = NULL;
	xmlNodePtr element = NULL;
	int i = 0;

	/* Allocate a week schedule structure
	 * and zero it out */
	ws = (struct week_schedule*) malloc(sizeof(struct week_schedule));
	if (!ws) {
		utils_err(CFG, "Unable to allocate week schedule !\n");
		parser_failed = 1;
		return NULL;
	}
	memset(ws, 0, sizeof(struct week_schedule));

	/* Fill it up */
	element = ws_node->xmlChildrenNode;
	while (element != NULL) {
		/* Note: Match these ids with the mapping on struct tm
		 * which means that Sunday = 0, Monday = 1 etc */
		if(!strncmp((const char*) element->name, "Sun",4))
			ws->days[0] = cfg_get_day_schedule(config,element);
		else if(!strncmp((const char*) element->name, "Mon",4))
			ws->days[1] = cfg_get_day_schedule(config,element);
		else if(!strncmp((const char*) element->name, "Tue",4))
			ws->days[2] = cfg_get_day_schedule(config,element);
		else if(!strncmp((const char*) element->name, "Wed",4))
			ws->days[3] = cfg_get_day_schedule(config,element);
		else if(!strncmp((const char*) element->name, "Thu",4))
			ws->days[4] = cfg_get_day_schedule(config,element);
		else if(!strncmp((const char*) element->name, "Fri",4))
			ws->days[5] = cfg_get_day_schedule(config,element);
		else if(!strncmp((const char*) element->name, "Sat",4))
			ws->days[6] = cfg_get_day_schedule(config,element);
		if(parser_failed) {
			utils_err(CFG, "Parsing of week schedule failed\n");
			goto cleanup;
		}
		element = element->next;
	}

	/* All days of the week should be filled */
	for(i = 0; i < 7; i++ ) {
		if(ws->days[i])
			continue;
		utils_err(CFG, "Got empty/incomplete week schedule\n");
		parser_failed = 1;
		goto cleanup;
	}

	utils_info(CFG, "Got week schedule\n");

cleanup:
	if(parser_failed) {
		cfg_free_week_schedule(ws);
		ws = NULL;
	}
	return ws;
}


/***********************\
* XML SCHEMA VALIDATION *
\***********************/

static void
cfg_print_validation_error_msg(void *ctx, const char *fmt, ...)
{
	if (!fmt)
		return;
	va_list args;
	va_start(args, fmt);
	utils_err(CFG, "Config validation failed: ");
	utils_verr(NONE, fmt, args);
	va_end(args);
}

/* Linked-in config XSD schema file (config_schema.xsd)*/
extern const char _binary_config_schema_xsd_start;
extern const char _binary_config_schema_xsd_end;

static int
cfg_validate_against_schema(xmlDocPtr config)
{
	xmlSchemaParserCtxtPtr ctx = NULL;
	xmlSchemaPtr schema = NULL;
	xmlSchemaValidCtxtPtr validation_ctx = NULL;
	unsigned int len = 0;
	int ret = 0;

	/* Load XSD shema file from memory and create a parser context */
	len = (unsigned int) (&_binary_config_schema_xsd_end -
			      &_binary_config_schema_xsd_start);
	ctx = xmlSchemaNewMemParserCtxt(&_binary_config_schema_xsd_start, len);
	if (!ctx) {
		utils_err(CFG, "Could not create XSD schema parsing context.\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Run the schema parser and put the result in memory */
	schema = xmlSchemaParse(ctx);
	if (!schema) {
		utils_err(CFG, "Could not parse XSD schema.\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Create a validation context */
	validation_ctx = xmlSchemaNewValidCtxt(schema);
	if (!validation_ctx) {
		utils_err(CFG, "Could not create XSD schema validation context.\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Register error printing callbacks */
	xmlSetGenericErrorFunc(NULL, cfg_print_validation_error_msg);
	xmlThrDefSetGenericErrorFunc(NULL, cfg_print_validation_error_msg);

	/* Run validation */
	ret = xmlSchemaValidateDoc(validation_ctx, config);
	if (ret != 0)
		parser_failed = 1;

cleanup:
	if (ctx)
		xmlSchemaFreeParserCtxt(ctx);
	if (schema)
		xmlSchemaFree(schema);
	if (validation_ctx)
		xmlSchemaFreeValidCtxt(validation_ctx);

	return (parser_failed ? -1 : 0);
}


/**************\
* ENTRY POINTS *
\**************/

void
cfg_cleanup(struct config *cfg)
{
	if(!cfg)
		return;
	if(cfg->ws != NULL)
		cfg_free_week_schedule(cfg->ws);
	cfg->ws = NULL;
}

int
cfg_process(struct config *cfg)
{
	xmlParserCtxtPtr ctx = NULL;
	xmlDocPtr  config = NULL;
	xmlNodePtr root_node = NULL;
	int ret = 0;
	parser_failed = 0;

	/* Sanity checks */
	if(cfg->filepath == NULL) {
		utils_err(CFG, "Called with null argument\n");
		parser_failed = 1;
		goto cleanup;
	}

	if(!utils_is_readable_file(cfg->filepath)) {
		parser_failed = 1;
		goto cleanup;
	}

	/* Store mtime for later checks */
	cfg->last_mtime = utils_get_mtime(cfg->filepath);
	if(!cfg->last_mtime) {
		parser_failed = 1;
		goto cleanup;
	}

	/* Initialize libxml2 and do version checks for ABI compatibility */
	LIBXML_TEST_VERSION

	/* Create a parser context */
	ctx = xmlNewParserCtxt();
	if (!ctx) {
		utils_err(CFG, "Failed to allocate parser context\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Parse config file and put result to memory */
	config = xmlParseFile(cfg->filepath);
	if (!config) {
		utils_err(CFG, "Document not parsed successfully.\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Grab the root node, should be a WeekSchedule element */
	root_node = xmlDocGetRootElement(config);
	if (!root_node) {
		utils_err(CFG, "Empty config\n");
		parser_failed = 1;
		goto cleanup;
	}
	if (strncmp((const char*) root_node->name, "WeekSchedule", 13)) {
		utils_err(CFG, "Root element is not a WeekSchedule\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Validate configuration against the configuration schema */
	ret = cfg_validate_against_schema(config);
	if (ret < 0) {
		utils_err(CFG, "Configuration did not pass shema validation\n");
		parser_failed = 1;
		goto cleanup;
	}

	/* Fill the data to the config struct */
	cfg->ws = cfg_get_week_schedule(config, root_node);

cleanup:
	/* Cleanup the config and any leftovers from the parser */
	if(config)
		xmlFreeDoc(config);
	if(ctx)
		xmlFreeParserCtxt(ctx);
	xmlCleanupParser();
	if(parser_failed) {
		ret = -1;
		cfg_cleanup(cfg);
	}
	return ret;
}

int
cfg_reload_if_needed(struct config *cfg)
{
	time_t mtime = utils_get_mtime(cfg->filepath);
	if(!mtime) {
		utils_err(CFG, "Unable to check mtime for %s\n", cfg->filepath);
		return -1;
	}

	/* mtime didn't change, no need to reload */
	if(mtime == cfg->last_mtime)
		return 0;

	utils_info(CFG, "Got different mtime, reloading %s\n", cfg->filepath);

	/* Re-load playlist */
	cfg_cleanup(cfg);
	return cfg_process(cfg);
}
