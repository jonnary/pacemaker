/* 
 * Copyright (C) 2009 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/cluster.h>

#include <crm/stonith-ng.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>
#include <internal.h>

extern xmlNode *stonith_create_op(
    int call_id, const char *token, const char *op, xmlNode *data, int call_options);

enum op_state 
{
    st_query,
    st_exec,
    st_done,
    st_failed,
};

GHashTable *remote_op_list = NULL;

typedef struct st_query_result_s
{
	char *host;
	int devices;

} st_query_result_t;

typedef struct remote_fencing_op_s
{
	char *id;
	char *target;
	char *action;
	guint replies;

	guint op_timer;	
	guint query_timer;
	guint base_timeout;

	char *delegate;
	time_t completed;	
	long long call_options;

	enum op_state state;
	char *originator;
	GListPtr query_results;
	xmlNode *request;
	
} remote_fencing_op_t;

static void free_remote_query(gpointer data)
{
    st_query_result_t *query = data;

    crm_free(query->host);
    crm_free(query);
}

static void free_remote_op(gpointer data)
{
    remote_fencing_op_t *op = data;

    crm_free(op->id);
    crm_free(op->action);
    crm_free(op->target);
    crm_free(op->originator);

    if(op->query_timer) {
	g_source_remove(op->query_timer);
    }
    if(op->op_timer) {
	g_source_remove(op->op_timer);
    }
    if(op->query_results) {
	slist_destroy(st_query_result_t, result, op->query_results,
		      free_remote_query(result);
	    );
    }
    if(op->request) {
	free_xml(op->request);
	op->request = NULL;
    }
    crm_free(op);
}

static void remote_op_reply_and_notify(remote_fencing_op_t *op, xmlNode *data, int rc) 
{
    xmlNode *reply = NULL;
    xmlNode *local_data = NULL;

    op->completed = time(NULL);
    
    if(data == NULL) {
	data = create_xml_node(NULL, "remote-op");
	local_data = data;

    } else {
	op->delegate = crm_element_value_copy(data, F_ORIG);
    }
    
    crm_xml_add_int(data, "state", op->state);
    crm_xml_add(data, F_STONITH_TARGET,    op->target);
    crm_xml_add(data, F_STONITH_OPERATION, op->action); 
   
    reply = stonith_construct_reply(op->request, NULL, data, rc);
    crm_xml_add(reply, F_STONITH_DELEGATE,  op->delegate);

    crm_info("Notifing clients of %s (%s of %s by %s): %d",
	     op->id, op->action, op->target, op->delegate, op->state);

    do_stonith_notify(0, STONITH_OP_FENCE, rc, reply, NULL);
    do_local_reply(reply, op->originator, op->call_options & st_opt_sync_call, FALSE);

    free_xml(local_data);
    free_xml(reply);

    /* Free non-essential parts of the record
     * Keep the record around so we can query the history
     */
    if(op->query_results) {
	slist_destroy(st_query_result_t, result, op->query_results,
		      free_remote_query(result);
	    );
	op->query_results = NULL;
    }

    if(op->request) {
	free_xml(op->request);
	op->request = NULL;
    }
}


static gboolean remote_op_timeout(gpointer userdata)
{
    remote_fencing_op_t *op = userdata;
    crm_err("Action %s (%s) for %s timed out", op->action, op->id, op->target);
    op->query_timer = 0;

    remote_op_reply_and_notify(op, NULL, st_err_timeout);
    
    op->state = st_failed;

    return FALSE;
}

static gboolean remote_op_query_timeout(gpointer data)
{
    remote_fencing_op_t *op = data;
    crm_err("Query %s for %s timed out", op->id, op->target);
    op->query_timer = 0;
    if(op->op_timer) {
	g_source_remove(op->op_timer);
	op->op_timer = 0;
    }
    remote_op_timeout(op);
    return FALSE;
}

void *create_remote_stonith_op(const char *client, xmlNode *request)
{
    cl_uuid_t new_uuid;
    char uuid_str[UU_UNPARSE_SIZEOF];

    remote_fencing_op_t *op = NULL;
    xmlNode *dev = get_xpath_object("//@"F_STONITH_TARGET, request, LOG_ERR);

    crm_malloc0(op, sizeof(remote_fencing_op_t));
    crm_element_value_int(dev, "timeout", (int*)&(op->base_timeout));    

    cl_uuid_generate(&new_uuid);
    cl_uuid_unparse(&new_uuid, uuid_str);
    
    op->id = crm_strdup(uuid_str);
    g_hash_table_replace(remote_op_list, op->id, op);

    op->state = st_query;
    op->action = crm_element_value_copy(dev, F_STONITH_ACTION);
    
    op->originator = crm_strdup(client);
    op->target = crm_element_value_copy(dev, F_STONITH_TARGET);
    op->request = copy_xml(request); /* TODO: Figure out how to avoid this */
    crm_element_value_int(request, F_STONITH_CALLOPTS, (int*)&(op->call_options));

    return op;
}


void initiate_remote_stonith_op(stonith_client_t *client, xmlNode *request) 
{
    xmlNode *query = NULL;
    remote_fencing_op_t *op = NULL;

    if(remote_op_list == NULL) {
	remote_op_list = g_hash_table_new_full(
	    g_str_hash, g_str_equal, NULL, free_remote_op);
    }

    op = create_remote_stonith_op(client->id, request);
    op->op_timer = g_timeout_add(1000*op->base_timeout, remote_op_timeout, op);
    op->query_timer = g_timeout_add(100*op->base_timeout, remote_op_query_timeout, op);

    query = stonith_create_op(0, op->id, STONITH_OP_QUERY, NULL, 0);
    crm_xml_add(query, F_STONITH_REMOTE, op->id);
    crm_xml_add(query, F_STONITH_TARGET, op->target);
    crm_xml_add(query, F_STONITH_ACTION, op->action);    
    
    crm_info("Initiating remote operation %s for %s: %s", op->action, op->target, op->id);
    CRM_CHECK(op->action, return);
    
    send_cluster_message(NULL, crm_msg_stonith_ng, query, FALSE);

    free_xml(query);
}

static void call_remote_stonith(remote_fencing_op_t *op, st_query_result_t *result) 
{
    xmlNode *query = stonith_create_op(0, op->id, STONITH_OP_FENCE, NULL, 0);;
    crm_xml_add(query, F_STONITH_REMOTE, op->id);
    crm_xml_add(query, F_STONITH_TARGET, op->target);    
    crm_xml_add(query, F_STONITH_ACTION, op->action);    
    
    op->state = st_exec;
    crm_info("Requesting that %s perform op %s %s", result->host, op->action, op->target);
    
    send_cluster_message(result->host, crm_msg_stonith_ng, query, FALSE);
    free_xml(query);
}


int process_remote_stonith_query(xmlNode *msg) 
{
    int devices = 0;
    const char *id = NULL;
    remote_fencing_op_t *op = NULL;
    st_query_result_t *result = NULL;
    xmlNode *dev = get_xpath_object("//@"F_STONITH_REMOTE, msg, LOG_ERR);

    crm_log_xml_info(msg, "QueryResult");

    CRM_CHECK(dev != NULL, return st_err_internal);

    id = crm_element_value(dev, F_STONITH_REMOTE);
    CRM_CHECK(id != NULL, return st_err_internal);
    
    dev = get_xpath_object("//@st-available-devices", msg, LOG_ERR);
    CRM_CHECK(dev != NULL, return st_err_internal);
    crm_element_value_int(dev, "st-available-devices", &devices);

    op = g_hash_table_lookup(remote_op_list, id);
    if(op == NULL) {
	crm_debug("Unknown or expired remote op: %s", id);
	return st_err_unknown_operation;
    }

    op->replies++;
    crm_malloc0(result, sizeof(st_query_result_t));
    result->host = crm_element_value_copy(msg, F_ORIG);
    result->devices = devices;

    /* TODO: Implement options
     * A) If we have anyone that can do the job
     * B) If we have someone that can do the job and some percent of the known peers
     * C) If all known peers have responded
     *
     * Implement A first
     */

    /* Track A */

    if(result->devices > 0) {
	if(op->call_options & st_opt_allow_suicide) {
	    crm_info("Allowing %s to potentialy fence itself", op->target);

	} else if(safe_str_eq(result->host, op->target)) {
	    crm_info("Ignoring reply from %s, hosts are not permitted to commit suicide", op->target);
	    free_remote_query(result);
	    return 0;
	}
	
	if(op->query_timer) {
	    g_source_remove(op->query_timer);
	    op->query_timer = 0;
	}
	
	if(op->state == st_query) {
	    call_remote_stonith(op, result);	    
	    free_remote_query(result);
	    
	} else if(op->state == st_exec) {
	    /* TODO: insert in sorted order (key = num devices) */
	    crm_info("Queuing query result from %s while operation is pending", result->host);
	    op->query_results = g_list_append(op->query_results, result);

	} else {
	    crm_info("Discarding query result from %s.  Operation is in state %d",
		     result->host, op->state);
	    free_remote_query(result);
	}
	

    } else {
	crm_info("Discarding query result from %s.  No valid devices", result->host);
	free_remote_query(result);
    }
    
    return 0;
}

int process_remote_stonith_exec(xmlNode *msg) 
{
    int rc = 0;
    const char *id = NULL;
    remote_fencing_op_t *op = NULL;
    xmlNode *dev = get_xpath_object("//@"F_STONITH_REMOTE, msg, LOG_ERR);

    crm_log_xml_info(msg, "ExecResult");

    CRM_CHECK(dev != NULL, return st_err_internal);

    id = crm_element_value(dev, F_STONITH_REMOTE);
    CRM_CHECK(id != NULL, return st_err_internal);
    
    dev = get_xpath_object("//@"F_STONITH_RC, msg, LOG_ERR);
    CRM_CHECK(dev != NULL, return st_err_internal);

    op = g_hash_table_lookup(remote_op_list, id);
    if(op == NULL) {
	crm_err("Unknown or expired remote op: %s", id);
	return st_err_unknown_operation;
    }
    
    crm_element_value_int(dev, F_STONITH_RC, &rc);
    if(rc == stonith_ok || op->state != st_exec) {
	if(op->op_timer) {
	    g_source_remove(op->op_timer);
	    op->op_timer = 0;
	}
	remote_op_reply_and_notify(op, msg, rc);
	
    } else if(rc < stonith_ok) {
	if(op->state == st_exec) {
	    st_query_result_t *result = g_list_nth_data(op->query_results, 0);
	    op->query_results = g_list_remove(op->query_results, result);
	    
	    if(result && result->devices > 0) {
		call_remote_stonith(op, result);

	    } else {
		remote_op_timeout(op);
	    }
	    
	    if(result) {
		free_remote_query(result);
	    }
	}
    }
    return rc;
}