/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2009, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthm@freeswitch.org>
 * Ken Rice <krice at suspicious dot org
 * Mathieu Rene <mathieu.rene@gmail.com>
 * Bret McDanel <trixter AT 0xdecafbad.com>
 * Rupa Schomaker <rupa@rupa.com>
 *
 * mod_limit.c -- Resource Limit Module
 *
 */

#include <switch.h>

#define LIMIT_EVENT_USAGE "limit::usage"
#define LIMIT_IGNORE_TRANSFER_VARIABLE "limit_ignore_transfer"

SWITCH_MODULE_LOAD_FUNCTION(mod_limit_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_limit_shutdown);
SWITCH_MODULE_DEFINITION(mod_limit, mod_limit_load, mod_limit_shutdown, NULL);

static struct {
	switch_memory_pool_t *pool;
	char hostname[256];
	char *dbname;
	char *odbc_dsn;
	switch_mutex_t *mutex;
	switch_mutex_t *limit_hash_mutex;
	switch_hash_t *limit_hash;	
	switch_mutex_t *db_hash_mutex;
	switch_hash_t *db_hash;	
	switch_odbc_handle_t *master_odbc;
} globals;

typedef struct  {
	uint32_t total_usage;
	uint32_t rate_usage;
	time_t last_check;
} limit_hash_item_t;

typedef struct {
	switch_hash_t *hash;
} limit_hash_private_t;

static char limit_sql[] =
	"CREATE TABLE limit_data (\n"
	"   hostname   VARCHAR(255),\n"
	"   realm      VARCHAR(255),\n"
	"   id         VARCHAR(255),\n"
	"   uuid       VARCHAR(255)\n"
	");\n";

static char db_sql[] =
	"CREATE TABLE db_data (\n"
	"   hostname   VARCHAR(255),\n"
	"   realm      VARCHAR(255),\n"
	"   data_key   VARCHAR(255),\n"
	"   data       VARCHAR(255)\n"
	");\n";

static char group_sql[] =
	"CREATE TABLE group_data (\n"
	"   hostname   VARCHAR(255),\n"
	"   groupname  VARCHAR(255),\n"
	"   url        VARCHAR(255)\n"
	");\n";

static switch_status_t limit_execute_sql(char *sql, switch_mutex_t *mutex)
{
	switch_core_db_t *db;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (mutex) {
		switch_mutex_lock(mutex);
	}

	if (switch_odbc_available() && globals.odbc_dsn) {
		switch_odbc_statement_handle_t stmt;
		if (switch_odbc_handle_exec(globals.master_odbc, sql, &stmt) != SWITCH_ODBC_SUCCESS) {
			char *err_str;
			err_str = switch_odbc_handle_get_error(globals.master_odbc, stmt);
			if (!zstr(err_str)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ERR: [%s]\n[%s]\n", sql, switch_str_nil(err_str));
			}
			switch_safe_free(err_str);
			status = SWITCH_STATUS_FALSE;
		}
		switch_odbc_statement_handle_free(&stmt);
	} else {
		if (!(db = switch_core_db_open_file(globals.dbname))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening DB %s\n", globals.dbname);
			status = SWITCH_STATUS_FALSE;
			goto end;
		}
		status = switch_core_db_persistant_execute(db, sql, 1);
		switch_core_db_close(db);
	}

  end:
	if (mutex) {
		switch_mutex_unlock(mutex);
	}

	return status;
}

static switch_bool_t limit_execute_sql_callback(switch_mutex_t *mutex, char *sql, switch_core_db_callback_func_t callback, void *pdata)
{
	switch_bool_t ret = SWITCH_FALSE;
	switch_core_db_t *db;
	char *errmsg = NULL;

	if (mutex) {
		switch_mutex_lock(mutex);
	}

	if (switch_odbc_available() && globals.odbc_dsn) {
		switch_odbc_handle_callback_exec(globals.master_odbc, sql, callback, pdata, NULL);
	} else {
		if (!(db = switch_core_db_open_file(globals.dbname))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening DB %s\n", globals.dbname);
			goto end;
		}

		switch_core_db_exec(db, sql, callback, pdata, &errmsg);

		if (errmsg) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SQL ERR: [%s] %s\n", sql, errmsg);
			free(errmsg);
		}

		if (db) {
			switch_core_db_close(db);
		}
	}

  end:
	if (mutex) {
		switch_mutex_unlock(mutex);
	}

	return ret;
}

static switch_xml_config_string_options_t limit_config_dsn = { NULL, 0, "[^:]+:[^:]+:.+" };

static switch_xml_config_item_t config_settings[] = {
	SWITCH_CONFIG_ITEM("odbc-dsn", SWITCH_CONFIG_STRING, 0, &globals.odbc_dsn, NULL, &limit_config_dsn,  
		"dsn:username:password", "If set, the ODBC DSN used by the limit and db applications"),
	SWITCH_CONFIG_ITEM_END()
};

static switch_status_t do_config()
{
	switch_core_db_t *db;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	char *odbc_user = NULL;
	char *odbc_pass = NULL;
	char *sql = NULL;
	
	limit_config_dsn.pool = globals.pool;

	if (switch_xml_config_parse_module_settings("limit.conf", SWITCH_FALSE, config_settings) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_TERM;
	}
	
	if (switch_odbc_available() && globals.odbc_dsn) {
		if ((odbc_user = strchr(globals.odbc_dsn, ':'))) {
			*odbc_user++ = '\0';
			if ((odbc_pass = strchr(odbc_user, ':'))) {
				*odbc_pass++ = '\0';
			}
		}
	} else if (globals.odbc_dsn) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ODBC IS NOT AVAILABLE!\n");
	}
	

	if (zstr(globals.odbc_dsn) || zstr(odbc_user) || zstr(odbc_pass)) {
		globals.dbname = "call_limit";
	}

	if (switch_odbc_available() && globals.odbc_dsn) {
		int x;
		char *indexes[] = {
			"create index ld_hostname on limit_data (hostname)",
			"create index ld_uuid on limit_data (uuid)",
			"create index ld_realm on limit_data (realm)",
			"create index ld_id on limit_data (id)",
			"create index dd_realm on db_data (realm)",
			"create index dd_data_key on db_data (data_key)",
			"create index gd_groupname on group_data (groupname)",
			"create index gd_url on group_data (url)",
			NULL
		};
		


		if (!(globals.master_odbc = switch_odbc_handle_new(globals.odbc_dsn, odbc_user, odbc_pass))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Cannot Open ODBC Database!\n");
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		if (switch_odbc_handle_connect(globals.master_odbc) != SWITCH_ODBC_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Cannot Open ODBC Database!\n");
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Connected ODBC DSN: %s\n", globals.odbc_dsn);
		if (switch_odbc_handle_exec(globals.master_odbc, "select count(*) from limit_data", NULL) != SWITCH_STATUS_SUCCESS) {
			if (switch_odbc_handle_exec(globals.master_odbc, limit_sql, NULL) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Cannot Create SQL Database!\n");
			}
		}
		if (switch_odbc_handle_exec(globals.master_odbc, "select count(*) from db_data", NULL) != SWITCH_STATUS_SUCCESS) {
			if (switch_odbc_handle_exec(globals.master_odbc, db_sql, NULL) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Cannot Create SQL Database!\n");
			}
		}
		if (switch_odbc_handle_exec(globals.master_odbc, "select count(*) from group_data", NULL) != SWITCH_STATUS_SUCCESS) {
			if (switch_odbc_handle_exec(globals.master_odbc, group_sql, NULL) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Cannot Create SQL Database!\n");
			}
		}

		for (x = 0; indexes[x]; x++) {
			switch_odbc_handle_exec(globals.master_odbc, indexes[x], NULL);
		}
	} else {
		if ((db = switch_core_db_open_file(globals.dbname))) {
			switch_core_db_test_reactive(db, "select * from limit_data", NULL, limit_sql);
			switch_core_db_test_reactive(db, "select * from db_data", NULL, db_sql);
			switch_core_db_test_reactive(db, "select * from group_data", NULL, group_sql);
			
			switch_core_db_exec(db, "create index if not exists ld_hostname on limit_data (hostname)", NULL, NULL, NULL);
			switch_core_db_exec(db, "create index if not exists ld_uuid on limit_data (uuid)", NULL, NULL, NULL);
			switch_core_db_exec(db, "create index if not exists ld_realm on limit_data (realm)", NULL, NULL, NULL);
			switch_core_db_exec(db, "create index if not exists ld_id on limit_data (id)", NULL, NULL, NULL);

			switch_core_db_exec(db, "create index if not exists dd_realm on db_data (realm)", NULL, NULL, NULL);
			switch_core_db_exec(db, "create index if not exists dd_data_key on db_data (data_key)", NULL, NULL, NULL);

			switch_core_db_exec(db, "create index if not exists gd_groupname on group_data (groupname)", NULL, NULL, NULL);
			switch_core_db_exec(db, "create index if not exists gd_url on group_data (url)", NULL, NULL, NULL);

		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Cannot Open SQL Database!\n");
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		switch_core_db_close(db);
	}

  done:

	sql = switch_mprintf("delete from limit_data where hostname='%q';", globals.hostname);
	limit_execute_sql(sql, globals.mutex);
	switch_safe_free(sql);

	return status;
}

static void limit_fire_event(const char *realm, const char *key, uint32_t usage, uint32_t rate, uint32_t max, uint32_t ratemax)
{
	switch_event_t *event;
	
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, LIMIT_EVENT_USAGE) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "realm", realm);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "key", key);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "usage", "%d", usage);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "rate", "%d", rate);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "max", "%d", max);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "ratemax", "%d", ratemax);
		switch_event_fire(&event);
	}
}

static switch_status_t db_state_handler(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_channel_state_t state = switch_channel_get_state(channel);
	char *sql = NULL;
	const char *vval = switch_channel_get_variable(channel, LIMIT_IGNORE_TRANSFER_VARIABLE);

	if (state >= CS_HANGUP || (state == CS_ROUTING && !switch_true(vval))) {
		sql = switch_mprintf("delete from limit_data where uuid='%q';",
							 switch_core_session_get_uuid(session));
		limit_execute_sql(sql, globals.mutex);
		switch_safe_free(sql);
		switch_core_event_hook_remove_state_change(session, db_state_handler);
		/* Remove limit_realm variable so we register another hook if limit is called again */
		switch_channel_set_variable(channel, "limit_realm", NULL);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t hash_state_handler(switch_core_session_t *session) 
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_channel_state_t state = switch_channel_get_state(channel);
	limit_hash_private_t *pvt = switch_channel_get_private(channel, "limit_hash");
	const char *vval = switch_channel_get_variable(channel, LIMIT_IGNORE_TRANSFER_VARIABLE);
	
	/* The call is either hung up, or is going back into the dialplan, decrement appropriate couters */
	if (state >= CS_HANGUP || (state == CS_ROUTING && !switch_true(vval))) {	
		switch_hash_index_t *hi;
		switch_mutex_lock(globals.limit_hash_mutex);

		/* Loop through the channel's hashtable which contains mapping to all the limit_hash_item_t referenced by that channel */
		while((hi = switch_hash_first(NULL, pvt->hash)))
		{
			void *val = NULL;
			const void *key;
			switch_ssize_t keylen;
			limit_hash_item_t *item = NULL;
			
			switch_hash_this(hi, &key, &keylen, &val);
			
			item = (limit_hash_item_t*)val;
			item->total_usage--;	
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d\n", (const char*)key, item->total_usage);
			
			if (item->total_usage == 0)  {
				/* Noone is using this item anymore */
				switch_core_hash_delete(globals.limit_hash, (const char*)key);
				free(item);
			}
			
			switch_core_hash_delete(pvt->hash, (const char*)key);
		}
		
		/* Remove handler */
		switch_core_event_hook_remove_state_change(session, hash_state_handler);
		
		switch_mutex_unlock(globals.limit_hash_mutex);
	}
	
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t memcache_state_handler(switch_core_session_t *session) 
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_channel_state_t state = switch_channel_get_state(channel);
	int argc = 0;
	char *argv[32] = { 0 };
	char *pvt = switch_channel_get_private(channel, "limit_keys");
	char *mydata = NULL;
	int i = 0;

	char *hashkey = NULL;
	uint32_t total_usage = 0;
	char *cmd = NULL;
	switch_stream_handle_t stream = { 0 };
	const char *vval = switch_channel_get_variable(channel, LIMIT_IGNORE_TRANSFER_VARIABLE);
	
	SWITCH_STANDARD_STREAM(stream);
	
	switch_assert(pvt);
	
	/* The call is either hung up, or is going back into the dialplan, decrement appropriate couters */
	if (state >= CS_HANGUP || (state == CS_ROUTING && !switch_true(vval))) {	

		/* Loop through the channel's keys which contains mapping to all the limits referenced by that channel */
		mydata = switch_core_session_strdup(session, pvt);
		argc = switch_separate_string(mydata, ',', argv, (sizeof(argv) / sizeof(argv[0])));
		for (i = 0; i < argc; i++) {
			hashkey = argv[i];
			cmd = switch_core_session_sprintf(session, "decrement limit:%s_total 1", hashkey);
			switch_api_execute("memcache", cmd, session, &stream);
			if (strncmp("-ERR", stream.data, 4)) {
				total_usage = atoi(stream.data);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d\n", (const char*)hashkey, total_usage);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error from mod_memcache %s.", (const char *) stream.data);
				goto end;
			}
			
			stream.end = stream.data;
		}
			
		/* Remove handler */
		switch_core_event_hook_remove_state_change(session, memcache_state_handler);
		switch_channel_set_private(channel, "limit_keys", NULL);
	}
	
end:
	switch_safe_free(stream.data);
	return SWITCH_STATUS_SUCCESS;
}

struct callback {
	char *buf;
	size_t len;
	int matches;
};

typedef struct callback callback_t;

static int sql2str_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	callback_t *cbt = (callback_t *) pArg;

	switch_copy_string(cbt->buf, argv[0], cbt->len);
	cbt->matches++;
	return 0;
}

static int group_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	callback_t *cbt = (callback_t *) pArg;
	switch_snprintf(cbt->buf + strlen(cbt->buf), cbt->len - strlen(cbt->buf), "%s%c", argv[0], *argv[1]);
	cbt->matches++;
	return 0;
}

SWITCH_STANDARD_API(db_api_function)
{
	int argc = 0;
	char *argv[4] = { 0 };
	char *mydata = NULL;
	char *sql;

	switch_mutex_lock(globals.mutex);

	if (!zstr(cmd)) {
		mydata = strdup(cmd);
		switch_assert(mydata);
		argc = switch_separate_string(mydata, '/', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 1 || !argv[0]) {
		goto error;
	}

	if (!strcasecmp(argv[0], "insert")) {
		if (argc < 4) {
			goto error;
		}
		sql = switch_mprintf("delete from db_data where realm='%q' and data_key='%q'", argv[1], argv[2]);
		switch_assert(sql);
		limit_execute_sql(sql, NULL);
		switch_safe_free(sql);
		sql =
			switch_mprintf("insert into db_data (hostname, realm, data_key, data) values('%q','%q','%q','%q');", globals.hostname, argv[1], argv[2],
						   argv[3]);
		switch_assert(sql);
		limit_execute_sql(sql, NULL);
		switch_safe_free(sql);
		stream->write_function(stream, "+OK");
		goto done;
	} else if (!strcasecmp(argv[0], "delete")) {
		if (argc < 2) {
			goto error;
		}
		sql = switch_mprintf("delete from db_data where realm='%q' and data_key='%q'", argv[1], argv[2]);
		switch_assert(sql);
		limit_execute_sql(sql, NULL);
		switch_safe_free(sql);
		stream->write_function(stream, "+OK");
		goto done;
	} else if (!strcasecmp(argv[0], "select")) {
		char buf[256] = "";
		callback_t cbt = { 0 };
		if (argc < 3) {
			goto error;
		}
		cbt.buf = buf;
		cbt.len = sizeof(buf);
		sql = switch_mprintf("select data from db_data where realm='%q' and data_key='%q'", argv[1], argv[2]);
		limit_execute_sql_callback(NULL, sql, sql2str_callback, &cbt);
		switch_safe_free(sql);
		stream->write_function(stream, "%s", buf);
		goto done;
	}

  error:
	stream->write_function(stream, "!err!");

  done:

	switch_mutex_unlock(globals.mutex);
	switch_safe_free(mydata);
	return SWITCH_STATUS_SUCCESS;
}

#define DB_USAGE "[insert|delete]/<realm>/<key>/<val>"
#define DB_DESC "save data"

SWITCH_STANDARD_APP(db_function)
{
	int argc = 0;
	char *argv[4] = { 0 };
	char *mydata = NULL;
	char *sql = NULL;

	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, '/', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 3 || !argv[0]) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: db %s\n", DB_USAGE);
		return;
	}

	if (!strcasecmp(argv[0], "insert")) {
		if (argc < 4) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: db %s\n", DB_USAGE);
			return;
		}
		sql = switch_mprintf("delete from db_data where realm='%q' and data_key='%q'", argv[1], argv[2]);
		switch_assert(sql);
		limit_execute_sql(sql, globals.mutex);
		switch_safe_free(sql);

		sql =
			switch_mprintf("insert into db_data (hostname, realm, data_key, data) values('%q','%q','%q','%q');", globals.hostname, argv[1], argv[2],
						   argv[3]);
	} else if (!strcasecmp(argv[0], "delete")) {
		sql = switch_mprintf("delete from db_data where realm='%q' and data_key='%q'", argv[1], argv[2]);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: db %s\n", DB_USAGE);
		return;
	}

	if (sql) {
		limit_execute_sql(sql, globals.mutex);
		switch_safe_free(sql);
	}
}

#define HASH_USAGE "[insert|delete]/<realm>/<key>/<val>"
#define HASH_DESC "save data"

SWITCH_STANDARD_APP(hash_function)
{
	int argc = 0;
	char *argv[4] = { 0 };
	char *mydata = NULL;
	char *hash_key = NULL;
	char *value = NULL;
	
	switch_mutex_lock(globals.db_hash_mutex);
	
	if (!zstr(data)) {
		mydata = strdup(data);
		switch_assert(mydata);
		argc = switch_separate_string(mydata, '/', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 3 || !argv[0]) {
		goto usage;
	}
	
	hash_key = switch_mprintf("%s_%s", argv[1], argv[2]);
	
	if (!strcasecmp(argv[0], "insert")) {
		if (argc < 4) {
			goto usage;
		}
		if ((value = switch_core_hash_find(globals.db_hash, hash_key))) {
			free(value);
			switch_core_hash_delete(globals.db_hash, hash_key);
		}
		value = strdup(argv[3]);
		switch_assert(value);
		switch_core_hash_insert(globals.db_hash, hash_key, value);
	} else if (!strcasecmp(argv[0], "delete")) {
		if ((value = switch_core_hash_find(globals.db_hash, hash_key))) {
			switch_safe_free(value);
			switch_core_hash_delete(globals.db_hash, hash_key);
		}
	} else {
		goto usage;
	}
	
	goto done;

usage:
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: hash %s\n", HASH_USAGE);

done:
	switch_mutex_unlock(globals.db_hash_mutex);
	switch_safe_free(mydata);
	switch_safe_free(hash_key);
}

#define HASH_API_USAGE "insert|select|delete/realm/key[/value]"
SWITCH_STANDARD_API(hash_api_function)
{
	int argc = 0;
	char *argv[4] = { 0 };
	char *mydata = NULL;
	char *value = NULL;
	char *hash_key = NULL;

	switch_mutex_lock(globals.db_hash_mutex);

	if (!zstr(cmd)) {
		mydata = strdup(cmd);
		switch_assert(mydata);
		argc = switch_separate_string(mydata, '/', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 3 || !argv[0]) {
		goto usage;
	}
	
	hash_key = switch_mprintf("%s_%s", argv[1], argv[2]);
	
	if (!strcasecmp(argv[0], "insert")) {
		if (argc < 4) {
			goto usage;
		}
		if ((value = switch_core_hash_find(globals.db_hash, hash_key))) {
			switch_safe_free(value);
			switch_core_hash_delete(globals.db_hash, hash_key);
		}
		value = strdup(argv[3]);
		switch_assert(value);
		switch_core_hash_insert(globals.db_hash, hash_key, value);
		stream->write_function(stream, "+OK\n");
	} else if (!strcasecmp(argv[0], "delete")) {
		if ((value = switch_core_hash_find(globals.db_hash, hash_key))) {
			switch_safe_free(value);
			switch_core_hash_delete(globals.db_hash, hash_key);
			stream->write_function(stream, "+OK\n");
		} else {
			stream->write_function(stream, "-ERR Not found\n");
		}
	} else if (!strcasecmp(argv[0], "select")) {
		if ((value = switch_core_hash_find(globals.db_hash, hash_key))) {
			stream->write_function(stream, "%s", value);
		}
	} else {
		goto usage;
	}
	
	goto done;
	
usage:
	stream->write_function(stream, "-ERR Usage: hash %s\n", HASH_API_USAGE);
	
done:
	switch_mutex_unlock(globals.db_hash_mutex);
	switch_safe_free(mydata);
	switch_safe_free(hash_key);
	
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(group_api_function)
{
	int argc = 0;
	char *argv[4] = { 0 };
	char *mydata = NULL;
	char *sql;

	switch_mutex_lock(globals.mutex);

	if (!zstr(cmd)) {
		mydata = strdup(cmd);
		argc = switch_separate_string(mydata, ':', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 2 || !argv[0]) {
		goto error;
	}

	if (!strcasecmp(argv[0], "insert")) {
		if (argc < 3) {
			goto error;
		}
		sql = switch_mprintf("delete from group_data where groupname='%q' and url='%q';", argv[1], argv[2]);
		switch_assert(sql);

		limit_execute_sql(sql, NULL);
		switch_safe_free(sql);
		sql = switch_mprintf("insert into group_data (hostname, groupname, url) values('%q','%q','%q');", globals.hostname, argv[1], argv[2]);
		switch_assert(sql);
		limit_execute_sql(sql, NULL);
		switch_safe_free(sql);
		stream->write_function(stream, "+OK");
		goto done;
	} else if (!strcasecmp(argv[0], "delete")) {
		if (argc < 3) {
			goto error;
		}
		if (!strcmp(argv[2], "*")) {
			sql = switch_mprintf("delete from group_data where groupname='%q';", argv[1]);
		} else {
			sql = switch_mprintf("delete from group_data where groupname='%q' and url='%q';", argv[1], argv[2]);
		}
		switch_assert(sql);
		limit_execute_sql(sql, NULL);
		switch_safe_free(sql);
		stream->write_function(stream, "+OK");
		goto done;
	} else if (!strcasecmp(argv[0], "call")) {
		char buf[4096] = "";
		char *how = ",";
		callback_t cbt = { 0 };
		cbt.buf = buf;
		cbt.len = sizeof(buf);

		if (argc > 2) {
			if (!strcasecmp(argv[2], "order")) {
				how = "|";
			}
		}

		sql = switch_mprintf("select url,'%q' from group_data where groupname='%q'", how, argv[1]);
		switch_assert(sql);

		limit_execute_sql_callback(NULL, sql, group_callback, &cbt);
		switch_safe_free(sql);
		
		*(buf + (strlen(buf) - 1)) = '\0';
		stream->write_function(stream, "%s", buf);

		goto done;
	}

  error:
	stream->write_function(stream, "!err!");

  done:

	switch_mutex_unlock(globals.mutex);
	switch_safe_free(mydata);
	return SWITCH_STATUS_SUCCESS;
}

#define GROUP_USAGE "[insert|delete]:<group name>:<val>"
#define GROUP_DESC "save data"

SWITCH_STANDARD_APP(group_function)
{
	int argc = 0;
	char *argv[3] = { 0 };
	char *mydata = NULL;
	char *sql;

	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ':', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 3 || !argv[0]) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: group %s\n", DB_USAGE);
		return;
	}

	if (!strcasecmp(argv[0], "insert")) {
		sql = switch_mprintf("insert into group_data (hostname, groupname, url) values('%q','%q','%q');", globals.hostname, argv[1], argv[2]);
		switch_assert(sql);
		limit_execute_sql(sql, globals.mutex);
		switch_safe_free(sql);
	} else if (!strcasecmp(argv[0], "delete")) {
		sql = switch_mprintf("delete from group_data where groupname='%q' and url='%q';", argv[1], argv[2]);
		switch_assert(sql);
		limit_execute_sql(sql, globals.mutex);
		switch_safe_free(sql);
	}
}

/* \brief Enforces limit restrictions
 * \param session current session
 * \param realm limit realm
 * \param id limit id
 * \param max maximum count
 * \return SWITCH_TRUE if the access is allowed, SWITCH_FALSE if it isnt
 */
static switch_bool_t do_limit(switch_core_session_t *session, const char *realm, const char *id, int max)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int got = 0;
	char *sql = NULL;
	char buf[80] = "";
	callback_t cbt = { 0 };
	switch_status_t status = SWITCH_TRUE;

	switch_mutex_lock(globals.mutex);

	switch_channel_set_variable(channel, "limit_realm", realm);
	switch_channel_set_variable(channel, "limit_id", id);
	switch_channel_set_variable(channel, "limit_max", switch_core_session_sprintf(session, "%d", max));

	cbt.buf = buf;
	cbt.len = sizeof(buf);
	sql = switch_mprintf("select count(hostname) from limit_data where realm='%q' and id like '%q'", realm, id);
	limit_execute_sql_callback(NULL, sql, sql2str_callback, &cbt);
	switch_safe_free(sql);
	got = atoi(buf);

	if (max < 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s_%s is now %d\n", realm, id, got+1);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s_%s is now %d/%d\n", realm, id, got+1, max);
	}

	if (max >= 0 && got + 1 > max) {
		status = SWITCH_FALSE;
		goto done;
	}

	switch_core_event_hook_add_state_change(session, db_state_handler);

	sql =
		switch_mprintf("insert into limit_data (hostname, realm, id, uuid) values('%q','%q','%q','%q');", globals.hostname, realm, id,
					   switch_core_session_get_uuid(session));
	limit_execute_sql(sql, NULL);
	switch_safe_free(sql);
	
	{
		const char *susage = switch_core_session_sprintf(session, "%d", ++got);

		switch_channel_set_variable(channel, "limit_usage", susage);
		switch_channel_set_variable(channel, switch_core_session_sprintf(session, "limit_usage_%s_%s", realm, id), susage);
	}
	limit_fire_event(realm, id, got, 0, max, 0);

  done:
	switch_mutex_unlock(globals.mutex);
	return status;
}

#define LIMIT_USAGE "<realm> <id> [<max> [number  [dialplan [context]]]]"
#define LIMIT_DESC "limit access to a resource and transfer to an extension if the limit is exceeded"
static char *limit_def_xfer_exten = "limit_exceeded";

SWITCH_STANDARD_APP(limit_function)
{
	int argc = 0;
	char *argv[6] = { 0 };
	char *mydata = NULL;
	char *realm = NULL;
	char *id = NULL;
	char *xfer_exten = NULL;
	int max = 0;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 3) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: limit %s\n", LIMIT_USAGE);
		return;
	}

	realm = argv[0];
	id = argv[1];
	
	/* Accept '-' as unlimited (act as counter)*/
	if (argv[2][0] == '-') {
		max = -1;
	} else {
		max = atoi(argv[2]);

		if (max < 0) {
			max = 0;
		}	
	}

	if (argc >= 4) {
		xfer_exten = argv[3];
	} else {
		xfer_exten = limit_def_xfer_exten;
	}

	if (!do_limit(session, realm, id, max)) {
		/* Limit exceeded */
		if (*xfer_exten == '!') {
			switch_channel_hangup(channel, switch_channel_str2cause(xfer_exten+1));
		} else {
			switch_ivr_session_transfer(session, xfer_exten, argv[4], argv[5]);
		}
	}
	
}

/* !\brief Releases usage of a limit_-controlled ressource  */
static void limit_release(switch_core_session_t *session, const char *realm, const char *id)
{
	char *sql = NULL;

	sql = switch_mprintf("delete from limit_data where uuid='%q' and realm='%q' and id like '%q'", 
						switch_core_session_get_uuid(session), realm, id);
	limit_execute_sql(sql, globals.mutex);
	switch_safe_free(sql);
}

#define LIMITEXECUTE_USAGE "<realm> <id> [<max>] [application] [application arguments]"
#define LIMITEXECUTE_DESC "limit access to a resource. the specified application will only be executed if the resource is available"
SWITCH_STANDARD_APP(limit_execute_function)
{
	int argc = 0;
	char *argv[5] = { 0 };
	char *mydata = NULL;
	char *realm = NULL;
	char *id = NULL;
	char *app = NULL;
	char *app_arg = NULL;
	int max = -1;

	/* Parse application data  */
	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 2) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: limit_execute %s\n", LIMITEXECUTE_USAGE);
		return;
	}
	
	realm = argv[0];
	id = argv[1];
	
	/* Accept '-' as unlimited (act as counter)*/
	if (argv[2][0] == '-') {
		max = -1;
	} else {
		max = atoi(argv[2]);

		if (max < 0) {
			max = 0;
		}	
	}

	app = argv[3];
	app_arg = argv[4];

	if (zstr(app)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing application\n");
		return;
	}

	if (do_limit(session, realm, id, max)) {
		switch_core_session_execute_application(session, app, app_arg);
		limit_release(session, realm, id);
	}
}

#define LIMIT_USAGE_USAGE "<realm> <id>"
SWITCH_STANDARD_API(limit_usage_function)
{
	int argc = 0;
	char *argv[6] = { 0 };
	char *mydata = NULL;
	char *sql = NULL;
	char *realm = NULL;
	char *id = NULL;
	char buf[80] = "";
	callback_t cbt = { 0 };


	if (!zstr(cmd)) {
		mydata = strdup(cmd);
		switch_assert(mydata);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 2) {
		stream->write_function(stream, "USAGE: limit_usage %s\n", LIMIT_USAGE_USAGE);
		goto end;
	}


	realm = argv[0];
	id = argv[1];

	cbt.buf = buf;
	cbt.len = sizeof(buf);
	sql = switch_mprintf("select count(hostname) from limit_data where realm='%q' and id like '%q'", realm, id);
	limit_execute_sql_callback(NULL, sql, sql2str_callback, &cbt);
	switch_safe_free(sql);

	stream->write_function(stream, "%s", buf);

end:
	switch_safe_free(mydata);
	return SWITCH_STATUS_SUCCESS;
}

/* \brief Enforces limit_hash restrictions
 * \param session current session
 * \param realm limit realm
 * \param id limit id
 * \param max maximum count
 * \param interval interval for rate limiting
 * \return SWITCH_TRUE if the access is allowed, SWITCH_FALSE if it isnt
 */
static switch_bool_t do_limit_hash(switch_core_session_t *session, const char *realm, const char *id, int max, int interval)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char *hashkey = NULL;
	switch_bool_t status = SWITCH_TRUE;
	limit_hash_item_t *item = NULL;
	time_t now = switch_epoch_time_now(NULL);
	limit_hash_private_t *pvt = NULL;
	uint8_t increment = 1;
	
	hashkey = switch_core_session_sprintf(session, "%s_%s", realm, id);

	switch_mutex_lock(globals.limit_hash_mutex);
	/* Check if that realm+id has ever been checked */
	if (!(item = (limit_hash_item_t*)switch_core_hash_find(globals.limit_hash, hashkey))) {
		/* No, create an empty structure and add it, then continue like as if it existed */
		item = (limit_hash_item_t*)malloc(sizeof(limit_hash_item_t));
		switch_assert(item);
		memset(item, 0, sizeof(limit_hash_item_t));
		switch_core_hash_insert(globals.limit_hash, hashkey, item);
	}

	/* Did we already run on this channel before? */
	if ((pvt = switch_channel_get_private(channel, "limit_hash")))
	{
		/* Yes, but check if we did that realm+id
		   If we didnt, allow incrementing the counter.
		   If we did, dont touch it but do the validation anyways
		 */
		increment = !switch_core_hash_find(pvt->hash, hashkey);
	} else {
		/* This is the first limit check on this channel, create a hashtable, set our prviate data and add a state handler */
		pvt = (limit_hash_private_t*)switch_core_session_alloc(session, sizeof(limit_hash_private_t));
		memset(pvt, 0, sizeof(limit_hash_private_t));
		switch_core_hash_init(&pvt->hash, switch_core_session_get_pool(session));
		switch_channel_set_private(channel, "limit_hash", pvt);
	}

	if (interval > 0) {
		if (item->last_check <= (now - interval)) {
			item->rate_usage = 1;
			item->last_check = now;
		} else {
			/* Always increment rate when its checked as it doesnt depend on the channel */
			item->rate_usage++;

			if ((max >= 0) && (item->rate_usage > (uint32_t)max)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s exceeds maximum rate of %d/%ds, now at %d\n", hashkey, max, interval, item->rate_usage);
				status = SWITCH_FALSE;
				goto end;
			}
		}
	} else if ((max >= 0) && (item->total_usage + increment > (uint32_t)max)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is already at max value (%d)\n", hashkey, item->total_usage);
		status = SWITCH_FALSE;
		goto end;
	}

	if (increment) {
		item->total_usage++;

		switch_core_hash_insert(pvt->hash, hashkey, item);

		if (max == -1) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d\n", hashkey, item->total_usage);
		} else if (interval == 0) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d/%d\n", hashkey, item->total_usage, max);	
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d/%d for the last %d seconds\n", hashkey, item->rate_usage, max, interval);
		}

		limit_fire_event(realm, id, item->total_usage, item->rate_usage, max, max >=0 ? (uint32_t)max : 0);
	}

	/* Save current usage & rate into channel variables so it can be used later in the dialplan, or added to CDR records */
	{
		const char *susage = switch_core_session_sprintf(session, "%d", item->total_usage);
		const char *srate = switch_core_session_sprintf(session, "%d", item->rate_usage);

		switch_channel_set_variable(channel, "limit_usage", susage);
		switch_channel_set_variable(channel, switch_core_session_sprintf(session, "limit_usage_%s", hashkey), susage);

		switch_channel_set_variable(channel, "limit_rate", srate);
		switch_channel_set_variable(channel, switch_core_session_sprintf(session, "limit_rate_%s", hashkey), srate);
	}

	switch_core_event_hook_add_state_change(session, hash_state_handler);

end:	
	switch_mutex_unlock(globals.limit_hash_mutex);
	return status;
}

/* !\brief Releases usage of a limit_hash-controlled ressource  */
static void limit_hash_release(switch_core_session_t *session, const char *realm, const char *id)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	limit_hash_private_t *pvt = switch_channel_get_private(channel, "limit_hash");
	limit_hash_item_t *item = NULL;
	char *hashkey = NULL;
	
	if (!pvt || !pvt->hash) {
		return;
	}
	
	hashkey = switch_core_session_sprintf(session, "%s_%s", realm, id);
	
	switch_mutex_lock(globals.limit_hash_mutex);
	
	if ((item = (limit_hash_item_t*)switch_core_hash_find(pvt->hash, hashkey))) {
		item->total_usage--;	
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d\n", (const char*)hashkey, item->total_usage);
	
		switch_core_hash_delete(pvt->hash, hashkey);
	
		if (item->total_usage == 0)  {
			/* Noone is using this item anymore */
			switch_core_hash_delete(globals.limit_hash, (const char*)hashkey);
			free(item);
		}
	}
	
	switch_mutex_unlock(globals.limit_hash_mutex);
}

#define LIMITHASH_USAGE "<realm> <id> [<max>[/interval]] [number [dialplan [context]]]"
#define LIMITHASH_DESC "limit access to a resource and transfer to an extension if the limit is exceeded"
SWITCH_STANDARD_APP(limit_hash_function)
{
	int argc = 0;
	char *argv[6] = { 0 };
	char *mydata = NULL;
	char *realm = NULL;
	char *id = NULL;
	char *xfer_exten = NULL;
	int max = -1;
	int interval = 0;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	/* Parse application data  */
	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 2) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: limit_hash %s\n", LIMITHASH_USAGE);
		return;
	}
	
	realm = argv[0];
	id = argv[1];
	
	/* If max is omitted or negative, only act as a counter and skip maximum checks */
	if (argc > 2) {
		if (argv[2][0] == '-') {
			max = -1;
		} else {
			char *szinterval = NULL;
			if ((szinterval = strchr(argv[2], '/')))
			{
				*szinterval++ = '\0';
				interval = atoi(szinterval);
			}
			
			max = atoi(argv[2]);
			
			if (max < 0) {
				max = 0;
			}
		}
	}

	if (argc > 3) {
		xfer_exten = argv[3];
	} else {
		xfer_exten = limit_def_xfer_exten;
	}
	
	if (!do_limit_hash(session, realm, id, max, interval)) {
		/* Limit exceeded */
		if (*xfer_exten == '!') {
			switch_channel_hangup(channel, switch_channel_str2cause(xfer_exten+1));
		} else {
			switch_ivr_session_transfer(session, xfer_exten, argv[4], argv[5]);
		}
	}
}


#define LIMITHASHEXECUTE_USAGE "<realm> <id> [<max>[/interval]] [application] [application arguments]"
#define LIMITHASHEXECUTE_DESC "limit access to a resource. the specified application will only be executed if the resource is available"
SWITCH_STANDARD_APP(limit_hash_execute_function)
{
	int argc = 0;
	char *argv[5] = { 0 };
	char *mydata = NULL;
	char *realm = NULL;
	char *id = NULL;
	char *app = NULL;
	char *app_arg = NULL;
	int max = -1;
	int interval = 0;

	/* Parse application data  */
	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 5) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: limit_hash_execute %s\n", LIMITHASHEXECUTE_USAGE);
		return;
	}
	
	realm = argv[0];
	id = argv[1];
	
	/* Accept '-' as unlimited (act as counter)*/
	if (argv[2][0] == '-') {
		max = -1;
	} else {
		char *szinterval = NULL;

		if ((szinterval = strchr(argv[2], '/')))
		{
			*szinterval++ = '\0';
			interval = atoi(szinterval);
		}

		max = atoi(argv[2]);

		if (max < 0) {
			max = 0;
		}	
	}
	
	app = argv[3];
	app_arg = argv[4];

	if (zstr(app)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing application\n");
		return;
	}

	if (do_limit_hash(session, realm, id, max, interval)) {
		switch_core_session_execute_application(session, app, app_arg);
		limit_hash_release(session, realm, id);
	}
}


#define LIMIT_HASH_USAGE_USAGE "<realm> <id>"
SWITCH_STANDARD_API(limit_hash_usage_function)
{
	int argc = 0;
	char *argv[3] = { 0 };
	char *mydata = NULL;
	char *hash_key = NULL;
	limit_hash_item_t *item = NULL;
	uint32_t count = 0;

	switch_mutex_lock(globals.limit_hash_mutex);
	
	if (!zstr(cmd)) {
		mydata = strdup(cmd);
		switch_assert(mydata);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 2) {
		stream->write_function(stream, "USAGE: limit_hash_usage %s\n", LIMIT_HASH_USAGE_USAGE);
		goto end;
	}
	
	hash_key = switch_mprintf("%s_%s", argv[0], argv[1]);
	
	if ((item = switch_core_hash_find(globals.limit_hash, hash_key))) {
		count = item->total_usage;
	} 
	
	stream->write_function(stream, "%d", count);
	
end:
	switch_safe_free(mydata);
	switch_safe_free(hash_key);
	switch_mutex_unlock(globals.limit_hash_mutex);
	
	return SWITCH_STATUS_SUCCESS;
}

/* \brief Enforces limit_memcache restrictions
 * \param session current session
 * \param realm limit realm
 * \param id limit id
 * \param max maximum count
 * \param interval interval for rate limiting
 * \return SWITCH_TRUE if the access is allowed, SWITCH_FALSE if it isnt
 */
static switch_bool_t do_limit_memcache(switch_core_session_t *session, const char *realm, const char *id, int max, int interval)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char *hashkey = NULL;
	switch_bool_t status = SWITCH_TRUE;
	uint8_t increment = 1;
	uint32_t total_usage = 0;
	uint32_t rate_usage = 0;
	char *cmd = NULL;
	char *pvt = NULL;
	int argc = 0;
	int i = 0;
	char *argv[32] = { 0 };
	char *mydata = NULL;
	
	switch_stream_handle_t stream = { 0 };
	
	if (switch_loadable_module_exists("mod_memcache") != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_TRUE;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_memcache is not loaded.");
		goto end;
	}
	
	SWITCH_STANDARD_STREAM(stream);
	hashkey = switch_core_session_sprintf(session, "%s_%s", realm, id);
	
	/* Did we already run on this channel before? */
	if ((pvt = switch_channel_get_private(channel, "limit_keys")))
	{
		/* Yes, but check if we did that realm+id
		   If we didnt, allow incrementing the counter.
		   If we did, dont touch it but do the validation anyways
		 */
		mydata = switch_core_session_strdup(session, pvt);
		argc = switch_separate_string(mydata, ',', argv, (sizeof(argv) / sizeof(argv[0])));
		for (i = 0; i < argc; i++) {
			if (!strcmp(hashkey, argv[i])) {
				increment = 0;
				break;
			}
		}
	}
	
	cmd = switch_core_session_sprintf(session, "get limit:%s_total", hashkey);
	switch_api_execute("memcache", cmd, session, &stream);
	if (strncmp("-ERR", stream.data, 4)) {
		total_usage = atoi(stream.data);
	} else if (!strncmp("-ERR NOT FOUND", stream.data, 14)) {
		total_usage = 0;
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error from mod_memcache %s.", (const char *) stream.data);
		status = SWITCH_TRUE;
		goto end;
	}
	
	stream.end = stream.data;

	if (interval > 0) {
		cmd = switch_core_session_sprintf(session, "increment limit:%s_rate 1 %d", hashkey, interval);
		switch_api_execute("memcache", cmd, session, &stream);
		if (strncmp("-ERR", stream.data, 4)) {
			rate_usage = atoi(stream.data);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error from mod_memcache %s.", (const char *) stream.data);
			status = SWITCH_TRUE;
			goto end;
		}
		if ((max >= 0) && (rate_usage > (uint32_t)max)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s exceeds maximum rate of %d/%ds, now at %d\n", hashkey, max, interval, rate_usage);
			status = SWITCH_FALSE;
			goto end;
		}
	} else if ((max >= 0) && (total_usage + increment > (uint32_t)max)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is already at max value (%d)\n", hashkey, total_usage);
		status = SWITCH_FALSE;
		goto end;
	}
	
	stream.end = stream.data;

	if (increment) {
		cmd = switch_core_session_sprintf(session, "increment limit:%s_total 1", hashkey);
		switch_api_execute("memcache", cmd, session, &stream);
		if (strncmp("-ERR", stream.data, 4)) {
			total_usage = atoi(stream.data);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error from mod_memcache %s.", (const char *) stream.data);
			status = SWITCH_TRUE;
			goto end;
		}
		if (max == -1) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d\n", hashkey, total_usage);
		} else if (interval == 0) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d/%d\n", hashkey, total_usage, max);	
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d/%d for the last %d seconds\n", hashkey, rate_usage, max, interval);
		}

		limit_fire_event(realm, id, total_usage, rate_usage, max, max >=0 ? (uint32_t)max : 0);
	}

	/* Save current usage & rate into channel variables so it can be used later in the dialplan, or added to CDR records */
	{
		const char *susage = switch_core_session_sprintf(session, "%d", total_usage);
		const char *srate = switch_core_session_sprintf(session, "%d", rate_usage);

		switch_channel_set_variable(channel, "limit_usage", susage);
		switch_channel_set_variable(channel, switch_core_session_sprintf(session, "limit_usage_%s", hashkey), susage);

		switch_channel_set_variable(channel, "limit_rate", srate);
		switch_channel_set_variable(channel, switch_core_session_sprintf(session, "limit_rate_%s", hashkey), srate);
	}
	
	if (pvt && increment) {
		pvt = switch_core_session_sprintf(session, "%s,%s", pvt, hashkey);
	} else {
		/* This is the first limit check on this channel, create a hashtable, set our prviate data and add a state handler */
		pvt = (char *) hashkey;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "hash_keys: %s\n", pvt);
	switch_channel_set_private(channel, "limit_keys", pvt);

	switch_core_event_hook_add_state_change(session, memcache_state_handler);

end:	
	switch_safe_free(stream.data);
	return status;
}

/* !\brief Releases usage of a limit_hash-controlled ressource  */
static void limit_memcache_release(switch_core_session_t *session, const char *realm, const char *id)
{
	char *hashkey = NULL;
	uint32_t total_usage = 0;
	char *cmd = NULL;
	switch_stream_handle_t stream = { 0 };
	SWITCH_STANDARD_STREAM(stream);
	
	if (switch_loadable_module_exists("mod_memcache") != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_memcache is not loaded.");
		goto end;
	}

	hashkey = switch_core_session_sprintf(session, "%s_%s", realm, id);
	
	cmd = switch_core_session_sprintf(session, "decrement limit:%s_total 1", hashkey);
	switch_api_execute("memcache", cmd, session, &stream);
	if (strncmp("-ERR", stream.data, 4)) {
		total_usage = atoi(stream.data);
	} else if (!strncmp("-ERR NOT FOUND", stream.data, 14)) {
		total_usage = 0;
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error from mod_memcache %s.", (const char *) stream.data);
		goto end;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Usage for %s is now %d\n", (const char*)hashkey, total_usage);

end:
	switch_safe_free(stream.data);
}

#define LIMITMEMCACHE_USAGE "<realm> <id> [<max>[/interval]] [number [dialplan [context]]]"
#define LIMITMEMCACHE_DESC "limit access to a resource and transfer to an extension if the limit is exceeded"
SWITCH_STANDARD_APP(limit_memcache_function)
{
	int argc = 0;
	char *argv[6] = { 0 };
	char *mydata = NULL;
	char *realm = NULL;
	char *id = NULL;
	char *xfer_exten = NULL;
	int max = -1;
	int interval = 0;
	char *szinterval = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	/* Parse application data  */
	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 2) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: limit_memcache %s\n", LIMITMEMCACHE_USAGE);
		return;
	}
	
	realm = argv[0];
	id = argv[1];
	
	/* If max is omitted, only act as a counter and skip maximum checks */
	if (argc > 2) {
		if ((szinterval = strchr(argv[2], '/')))
		{
			*szinterval++ = '\0';
			interval = atoi(szinterval);
		}
		
		max = atoi(argv[2]);
		
		if (max < 0) {
			max = 0;
		}
	}

	if (argc > 3) {
		xfer_exten = argv[3];
	} else {
		xfer_exten = limit_def_xfer_exten;
	}
	
	if (!do_limit_memcache(session, realm, id, max, interval)) {
		/* Limit exceeded */
		if (*xfer_exten == '!') {
			switch_channel_hangup(channel, switch_channel_str2cause(xfer_exten+1));
		} else {
			switch_ivr_session_transfer(session, xfer_exten, argv[4], argv[5]);
		}
	}
}


#define LIMITMEMCACHEEXECUTE_USAGE "<realm> <id> [<max>[/interval]] [application] [application arguments]"
#define LIMITMEMCACHEEXECUTE_DESC "limit access to a resource. the specified application will only be executed if the resource is available"
SWITCH_STANDARD_APP(limit_memcache_execute_function)
{
	int argc = 0;
	char *argv[5] = { 0 };
	char *mydata = NULL;
	char *realm = NULL;
	char *id = NULL;
	char *app = NULL;
	char *app_arg = NULL;
	int max = -1;
	int interval = 0;
	char *szinterval = NULL;

	/* Parse application data  */
	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 2) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "USAGE: limit_memcache_execute %s\n", LIMITMEMCACHEEXECUTE_USAGE);
		return;
	}
	
	realm = argv[0];
	id = argv[1];
	
	/* If max is omitted, only act as a counter and skip maximum checks */
	if (argc > 2) {
		if ((szinterval = strchr(argv[2], '/')))
		{
			*szinterval++ = '\0';
			interval = atoi(szinterval);
		}
		
		max = atoi(argv[2]);
		
		if (max < 0) {
			max = 0;
		}
	}


	app = argv[3];
	app_arg = argv[4];

	if (zstr(app)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing application\n");
		return;
	}

	if (do_limit_memcache(session, realm, id, max, interval)) {
		switch_core_session_execute_application(session, app, app_arg);
		limit_memcache_release(session, realm, id);
	}
}


#define LIMIT_MEMCACHE_USAGE_USAGE "<realm> <id>"
SWITCH_STANDARD_API(limit_memcache_usage_function)
{
	int argc = 0;
	char *argv[3] = { 0 };
	char *mydata = NULL;
	uint32_t count = 0;
	char *apicmd = NULL;
	switch_bool_t status = SWITCH_TRUE;
	switch_stream_handle_t apistream = { 0 };

	SWITCH_STANDARD_STREAM(apistream);
	if (switch_loadable_module_exists("mod_memcache") != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_TRUE;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_memcache is not loaded.");
		stream->write_function(stream, "-ERR mod_memcache not loaded");
		goto end;
	}

	if (!zstr(cmd)) {
		mydata = strdup(cmd);
		switch_assert(mydata);
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	
	if (argc < 2) {
		stream->write_function(stream, "USAGE: limit_memcache_usage %s\n", LIMIT_MEMCACHE_USAGE_USAGE);
		goto end;
	}
	
	apicmd = switch_mprintf("get limit:%s_%s_total", argv[0], argv[1]);
	switch_api_execute("memcache", apicmd, session, &apistream);
	if (strncmp("-ERR", apistream.data, 4)) {
		count = atoi(apistream.data);
	} else if (!strncmp("-ERR NOT FOUND", apistream.data, 14)) {
		count = 0;
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error from mod_memcache %s.", (const char *) apistream.data);
		status = SWITCH_TRUE;
		goto end;
	}
	
	stream->write_function(stream, "%d", count);
	
end:
	switch_safe_free(mydata);
	switch_safe_free(apistream.data);
	switch_safe_free(apicmd);
	
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_LOAD_FUNCTION(mod_limit_load)
{
	switch_status_t status;
	switch_application_interface_t *app_interface;
	switch_api_interface_t *commands_api_interface;

	memset(&globals, 0, sizeof(&globals));
	gethostname(globals.hostname, sizeof(globals.hostname));
	globals.pool = pool;


	if (switch_event_reserve_subclass(LIMIT_EVENT_USAGE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldnt register event subclass \"%s\"", 
			LIMIT_EVENT_USAGE);
		return SWITCH_STATUS_FALSE;
	}
	
	if ((status = do_config() != SWITCH_STATUS_SUCCESS)) {
		return status;
	}

	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);
	switch_mutex_init(&globals.limit_hash_mutex, SWITCH_MUTEX_NESTED, globals.pool);
	switch_mutex_init(&globals.db_hash_mutex, SWITCH_MUTEX_NESTED, globals.pool);
	switch_core_hash_init(&globals.limit_hash, pool);
	switch_core_hash_init(&globals.db_hash, pool);

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
		


	SWITCH_ADD_APP(app_interface, "limit", "Limit", LIMIT_DESC, limit_function, LIMIT_USAGE, SAF_SUPPORT_NOMEDIA);
	SWITCH_ADD_APP(app_interface, "limit_execute", "Limit", LIMITEXECUTE_USAGE, limit_execute_function, LIMITEXECUTE_USAGE, SAF_SUPPORT_NOMEDIA);
	SWITCH_ADD_APP(app_interface, "limit_hash", "Limit (hash)", LIMITHASH_DESC, limit_hash_function, LIMITHASH_USAGE, SAF_SUPPORT_NOMEDIA);
	SWITCH_ADD_APP(app_interface, "limit_hash_execute", "Limit (hash)", LIMITHASHEXECUTE_USAGE, limit_hash_execute_function, LIMITHASHEXECUTE_USAGE, SAF_SUPPORT_NOMEDIA);
	SWITCH_ADD_APP(app_interface, "limit_memcache", "Limit (memcache)", LIMITMEMCACHE_DESC, limit_memcache_function, LIMITMEMCACHE_USAGE, SAF_SUPPORT_NOMEDIA);
	SWITCH_ADD_APP(app_interface, "limit_memcache_execute", "Limit (memcache)", LIMITMEMCACHEEXECUTE_USAGE, limit_memcache_execute_function, LIMITMEMCACHEEXECUTE_USAGE, SAF_SUPPORT_NOMEDIA);
	SWITCH_ADD_APP(app_interface, "db", "Insert to the db", DB_DESC, db_function, DB_USAGE, SAF_SUPPORT_NOMEDIA);
	SWITCH_ADD_APP(app_interface, "hash", "Insert into the hashtable", HASH_DESC, hash_function, HASH_USAGE, SAF_SUPPORT_NOMEDIA)
	SWITCH_ADD_APP(app_interface, "group", "Manage a group", GROUP_DESC, group_function, GROUP_USAGE, SAF_SUPPORT_NOMEDIA);

	SWITCH_ADD_API(commands_api_interface, "limit_hash_usage", "Gets the usage count of a limited resource", limit_hash_usage_function,  LIMIT_HASH_USAGE_USAGE);
	SWITCH_ADD_API(commands_api_interface, "limit_memcache_usage", "Gets the usage count of a limited resource", limit_memcache_usage_function,  LIMIT_MEMCACHE_USAGE_USAGE);
	SWITCH_ADD_API(commands_api_interface, "limit_usage", "Gets the usage count of a limited resource", limit_usage_function, "<realm> <id>");
	SWITCH_ADD_API(commands_api_interface, "db", "db get/set", db_api_function, "[insert|delete|select]/<realm>/<key>/<value>");
	switch_console_set_complete("add db insert");
	switch_console_set_complete("add db delete");
	switch_console_set_complete("add db select");
	SWITCH_ADD_API(commands_api_interface, "hash", "hash get/set", hash_api_function, "[insert|delete|select]/<realm>/<key>/<value>");
	switch_console_set_complete("add hash insert");
	switch_console_set_complete("add hash delete");
	switch_console_set_complete("add hash select");
	SWITCH_ADD_API(commands_api_interface, "group", "group [insert|delete|call]", group_api_function, "[insert|delete|call]:<group name>:<url>");
	switch_console_set_complete("add group insert");
	switch_console_set_complete("add group delete");
	switch_console_set_complete("add group call");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_limit_shutdown) 
{
	
	if (globals.master_odbc) {
		switch_odbc_handle_destroy(&globals.master_odbc);
	}
	
	switch_event_free_subclass(LIMIT_EVENT_USAGE);
	
	switch_xml_config_cleanup(config_settings);
	
	switch_mutex_destroy(globals.mutex);
	switch_mutex_destroy(globals.limit_hash_mutex);
	switch_mutex_destroy(globals.db_hash_mutex);

	switch_core_hash_destroy(&globals.limit_hash);
	switch_core_hash_destroy(&globals.db_hash);

	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
