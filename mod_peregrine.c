#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_log.h"
#include "ap_config.h"
#include "ap_regex.h"

#include <jansson.h>
#include <mysql.h>
#include <hiredis/hiredis.h>

typedef struct {
	const char *validation_regex;
	const char *param_name;
	const char *cache_socket;
	const char *database_username;
	const char *database_password;
	const char *database_name;
	const char *database_socket;
} config;

static config peregrine_config;

int log_error(ap_regex_t *preg, redisContext **redis_context,
		MYSQL **mysql_connection, int statusCode, request_rec *r, char *format,
		const char *arg) {

	ap_log_error(APLOG_MARK, APLOG_ERR, statusCode, r->server, format, arg);

	if (preg != NULL) {
		ap_regfree(preg);
	}

	if (redis_context != NULL) {
		redisFree(*redis_context);
	}

	if (mysql_connection != NULL) {
		mysql_close(*mysql_connection);
	}

	return statusCode;
}

static int peregrine_handler(request_rec *r) {
	if (strcmp(r->handler, "peregrine-handler") != 0) {
		return DECLINED;
	}
	r->content_type = "application/json";

	if (strcmp(r->method, "GET") != 0) {
		return log_error(NULL, NULL, NULL, HTTP_BAD_REQUEST, r, "%s",
				"peregrine only GET requests are supported");
	}

	apr_table_t *GET;
	ap_args_to_table(r, &GET);

	char *action = apr_table_get(GET, peregrine_config.param_name);

	if (!action) {
		return log_error(NULL, NULL, NULL, HTTP_BAD_REQUEST, r,
				"peregrine parameter: %s invalid/not found",
				peregrine_config.param_name);
	}

	if (strlen(action) > 255) {
		return log_error(NULL, NULL, NULL, HTTP_BAD_REQUEST, r,
				"peregrine parameter: %s too long", peregrine_config.param_name);
	}

	ap_regex_t preg;

	if (ap_regcomp(&preg, peregrine_config.validation_regex, AP_REG_DEFAULT)
			!= 0) {
		return log_error(NULL, NULL, NULL, HTTP_INTERNAL_SERVER_ERROR, r, "%s",
				"peregrine regex compilation error");
	} else {
		if (ap_regexec(&preg, action, 0, NULL, 0) != 0) {
			return log_error(&preg, NULL, NULL, HTTP_BAD_REQUEST, r, "%s",
					"peregrine regex validation error");
		}
	}

	ap_regfree(&preg);

	redisContext *redis_context = redisConnectUnix(
			peregrine_config.cache_socket);
	if (redis_context == NULL || redis_context->err) {
		if (redis_context) {
			return log_error(NULL, &redis_context, NULL,
					HTTP_INTERNAL_SERVER_ERROR, r,
					"peregrine: redis connect error %s", redis_context->errstr);
		} else {
			return log_error(NULL, &redis_context, NULL,
					HTTP_INTERNAL_SERVER_ERROR, r, "%s",
					"peregrine: redis connect can't allocate redis_context");
		}
	}

	redisReply *redis_reply = redisCommand(redis_context, "GET %s", action);

	if (redis_reply != NULL) {
		if (redis_reply->type == REDIS_REPLY_STRING) {
			ap_rprintf(r, "%s", redis_reply->str);
			freeReplyObject(redis_reply);
		} else {
			char *jsonResponse = NULL;

			MYSQL *mysql_connection = mysql_init(NULL);

			if (mysql_connection == NULL) {
				return log_error(NULL, &redis_context, &mysql_connection,
						HTTP_INTERNAL_SERVER_ERROR, r,
						"peregrine: mysql connect error %s",
						mysql_error(mysql_connection));
			}

			if (mysql_real_connect(mysql_connection, NULL,
					peregrine_config.database_username,
					peregrine_config.database_password,
					peregrine_config.database_name, 0,
					peregrine_config.database_socket, 0) == NULL) {
				return log_error(NULL, &redis_context, &mysql_connection,
						HTTP_INTERNAL_SERVER_ERROR, r,
						"peregrine: mysql connect error %s",
						mysql_error(mysql_connection));
			}

			char storedProcedureName[261];

			sprintf(storedProcedureName, "CALL %s", action);

			if (mysql_query(mysql_connection, storedProcedureName)) {
				return log_error(NULL, &redis_context, &mysql_connection,
						HTTP_INTERNAL_SERVER_ERROR, r,
						"peregrine: mysql query error %s",
						mysql_error(mysql_connection));
			}

			MYSQL_RES *mysql_result = mysql_store_result(mysql_connection);

			if (mysql_result == NULL) {
				return log_error(NULL, &redis_context, &mysql_connection,
						HTTP_INTERNAL_SERVER_ERROR, r,
						"peregrine: mysql store result error %s",
						mysql_error(mysql_connection));
			}

			int num_fields = mysql_num_fields(mysql_result);

			MYSQL_ROW row;
			MYSQL_FIELD *field;

			json_t *root = json_object();
			json_t *json_arr = json_array();
			json_object_set_new(root, "data", json_arr);

			while ((row = mysql_fetch_row(mysql_result))) {

				json_t *current_row = json_object();

				for (int i = 0; i < num_fields; i++) {
					field = mysql_fetch_field_direct(mysql_result, i);

					json_object_set_new(current_row, field->name,
							json_string(row[i] ? row[i] : "NULL"));
				}

				mysql_field_seek(mysql_result, 0);

				json_array_append(json_arr, current_row);
			}

			mysql_free_result(mysql_result);

			jsonResponse = json_dumps(root, JSON_COMPACT);

			ap_rprintf(r, "%s", jsonResponse);

			json_decref(root);

			mysql_close(mysql_connection);

			redisReply *redis_reply = redisCommand(redis_context, "SET %s %s",
					action, jsonResponse);
			freeReplyObject(redis_reply);
		}
	}

	redisFree(redis_context);

	return OK;
}

static void peregrine_register_hooks(apr_pool_t *p) {

	peregrine_config.validation_regex = "^[a-zA-Z0-9-_]+$";
	peregrine_config.param_name = "sp";
	peregrine_config.cache_socket = "/var/run/redis/redis-server.sock";
	peregrine_config.database_username = "peregrineuser";
	peregrine_config.database_password = "peregrinepass";
	peregrine_config.database_name = "peregrine";
	peregrine_config.database_socket = "/var/run/mysqld/mysqld.sock";

	ap_hook_handler(peregrine_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

const char* set_validation_regex(cmd_parms *cmd, void *cfg, const char *arg) {
	peregrine_config.validation_regex = arg;
	return NULL;
}

const char* set_param_name(cmd_parms *cmd, void *cfg, const char *arg) {
	peregrine_config.param_name = arg;
	return NULL;
}

const char* set_cache_socket(cmd_parms *cmd, void *cfg, const char *arg) {
	peregrine_config.cache_socket = arg;
	return NULL;
}

const char* set_database_username(cmd_parms *cmd, void *cfg, const char *arg) {
	peregrine_config.database_username = arg;
	return NULL;
}

const char* set_database_password(cmd_parms *cmd, void *cfg, const char *arg) {
	peregrine_config.database_password = arg;
	return NULL;
}

const char* set_database_name(cmd_parms *cmd, void *cfg, const char *arg) {
	peregrine_config.database_name = arg;
	return NULL;
}

const char* set_database_socket(cmd_parms *cmd, void *cfg, const char *arg) {
	peregrine_config.database_socket = arg;
	return NULL;
}

static const command_rec peregrine_directives[] = {
		AP_INIT_TAKE1("peregrineValidationRegex", set_validation_regex, NULL, RSRC_CONF,"Validation Regex"),
		AP_INIT_TAKE1("peregrineParamName", set_param_name,NULL, RSRC_CONF, "Param Name"),
		AP_INIT_TAKE1("peregrineCacheSocket",set_cache_socket, NULL, RSRC_CONF, "Cache Hostname"),
		AP_INIT_TAKE1("peregrineDatabaseUsername", set_database_username, NULL, RSRC_CONF,"Database Username"),
		AP_INIT_TAKE1("peregrineDatabasePassword",set_database_password, NULL, RSRC_CONF, "Database Password"),
		AP_INIT_TAKE1("peregrineDatabaseName", set_database_name, NULL,RSRC_CONF, "Database Name"),
		AP_INIT_TAKE1("peregrineDatabaseSocket", set_database_socket, NULL, RSRC_CONF,"Database Socket"),
		{ NULL }
};

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA peregrine_module = { STANDARD20_MODULE_STUFF,
		NULL, /* create per-dir    config structures */
		NULL, /* merge  per-dir    config structures */
		NULL, /* create per-server config structures */
		NULL, /* merge  per-server config structures */
		peregrine_directives, /* table of config file commands       */
		peregrine_register_hooks /* register hooks                      */
};

