#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/queue.h>
#include <assert.h>
#include <string.h>

#include "evsql.h"
#include "evpq.h"
#include "lib/log.h"
#include "lib/error.h"
#include "lib/misc.h"

enum evsql_type {
    EVSQL_EVPQ,
};

struct evsql {
    // callbacks
    evsql_error_cb error_fn;
    void *cb_arg;

    // backend engine
    enum evsql_type type;

    union {
        struct evpq_conn *evpq;
    } engine;
    
    // list of queries running or waiting to run
    TAILQ_HEAD(evsql_queue, evsql_query) queue;
};

struct evsql_query {
    // the evsql we are querying
    struct evsql *evsql;

    // the actual SQL query, this may or may not be ours, see _evsql_query_exec
    char *command;

    // our callback
    evsql_query_cb cb_fn;
    void *cb_arg;

    // our position in the query list
    TAILQ_ENTRY(evsql_query) entry;

    // the result
    union {
        PGresult *evpq;
    } result;
};

/*
 * Actually execute the given query.
 *
 * The backend should be able to accept the query at this time.
 *
 * query->command must be valid during the execution of this function, but once it returns, the command is not needed
 * anymore, and should be set to NULL.
 */
static int _evsql_query_exec (struct evsql *evsql, struct evsql_query *query, const char *command) {
    switch (evsql->type) {
        case EVSQL_EVPQ:
            // just pass it through
            return evpq_query(evsql->engine.evpq, command);
        
        default:
            FATAL("evsql->type");
    }
}

/*
 * Dequeue the query, execute the callback, and free it.
 */
static void _evsql_query_done (struct evsql_query *query, const struct evsql_result_info *result_info) {
    assert(query->command == NULL);

    // dequeue
    TAILQ_REMOVE(&query->evsql->queue, query, entry);
    
    if (result_info) 
        // call the callback
        query->cb_fn(*result_info, query->cb_arg);
    
    // free
    free(query);
}

/*
 * A query has failed, notify the user and remove it.
 */
static void _evsql_query_failure (struct evsql *evsql, struct evsql_query *query) {
    struct evsql_result_info result; ZINIT(result);

    // set up the result_info
    result.evsql = evsql;
    result.error = 1;

    // finish it off
    _evsql_query_done(query, &result);
}

/*
 * Clear every enqueued query and then free the evsql.
 *
 * If result_info is given, each query will also recieve it via their callback, and the error_fn will be called.
 */
static void _evsql_destroy (struct evsql *evsql, const struct evsql_result_info *result_info) {
    struct evsql_query *query;
    
    // clear the queue
    while ((query = TAILQ_FIRST(&evsql->queue)) != NULL) {
        _evsql_query_done(query, result_info);
        
        TAILQ_REMOVE(&evsql->queue, query, entry);
    }
    
    // do the error callback if required
    if (result_info)
        evsql->error_fn(evsql, evsql->cb_arg);
    
    // free
    free(evsql);
}


/*
 * Sends the next query if there are more enqueued
 */
static void _evsql_pump (struct evsql *evsql) {
    struct evsql_query *query;
    
    // look for the next query
    if ((query = TAILQ_FIRST(&evsql->queue)) != NULL) {
        // try and execute it
        if (_evsql_query_exec(evsql, query, query->command)) {
            // the query failed
            _evsql_query_failure(evsql, query);
        }

        // free the command
        free(query->command); query->command = NULL;

        // ok, then we just wait
    }
}


static void _evsql_evpq_connected (struct evpq_conn *conn, void *arg) {
    struct evsql *evsql = arg;

    // no state to update, just pump any waiting queries
    _evsql_pump(evsql);
}

static void _evsql_evpq_result (struct evpq_conn *conn, PGresult *result, void *arg) {
    struct evsql *evsql = arg;
    struct evsql_query *query;

    assert((query = TAILQ_FIRST(&evsql->queue)) != NULL);

    // if we get multiple results, only return the first one
    if (query->result.evpq) {
        WARNING("[evsql] evpq query returned multiple results, discarding previous one");
        
        PQclear(query->result.evpq); query->result.evpq = NULL;
    }
    
    // remember the result
    query->result.evpq = result;
}

static void _evsql_evpq_done (struct evpq_conn *conn, void *arg) {
    struct evsql *evsql = arg;
    struct evsql_query *query;
    struct evsql_result_info result; ZINIT(result);

    assert((query = TAILQ_FIRST(&evsql->queue)) != NULL);
    
    // set up the result_info
    result.evsql = evsql;
    
    if (query->result.evpq == NULL) {
        // if a query didn't return any results (bug?), warn and fail the query
        WARNING("[evsql] evpq query didn't return any results");

        result.error = 1;

    } else {
        result.error = 0;
        result.result.pq = query->result.evpq;

    }

    // finish it off
    _evsql_query_done(query, &result);

    // pump the next one
    _evsql_pump(evsql);
}

static void _evsql_evpq_failure (struct evpq_conn *conn, void *arg) {
    struct evsql *evsql = arg;
    struct evsql_result_info result; ZINIT(result);
    
    // OH SHI...
    
    // set up the result_info
    result.evsql = evsql;
    result.error = 1;

    // finish off the whole connection
    _evsql_destroy(evsql, &result);
}

static struct evpq_callback_info _evsql_evpq_cb_info = {
    .fn_connected       = _evsql_evpq_connected,
    .fn_result          = _evsql_evpq_result,
    .fn_done            = _evsql_evpq_done,
    .fn_failure         = _evsql_evpq_failure,
};

static struct evsql *_evsql_new_base (evsql_error_cb error_fn, void *cb_arg) {
    struct evsql *evsql = NULL;
    
    // allocate it
    if ((evsql = calloc(1, sizeof(*evsql))) == NULL)
        ERROR("calloc");

    // store
    evsql->error_fn = error_fn;
    evsql->cb_arg = cb_arg;

    // init
    TAILQ_INIT(&evsql->queue);

    // done
    return evsql;

error:
    return NULL;
}

struct evsql *evsql_new_pq (struct event_base *ev_base, const char *pq_conninfo, evsql_error_cb error_fn, void *cb_arg) {
    struct evsql *evsql = NULL;
    
    // base init
    if ((evsql = _evsql_new_base (error_fn, cb_arg)) == NULL)
        goto error;

    // connect the engine
    if ((evsql->engine.evpq = evpq_connect(ev_base, pq_conninfo, _evsql_evpq_cb_info, evsql)) == NULL)
        goto error;

    // done
    return evsql;

error:
    // XXX: more complicated than this?
    free(evsql); 

    return NULL;
}

/*
 * Checks what the state of the connection is in regards to executing a query.
 *
 * Returns:
 *      <0      connection failure, query not possible
 *      0       connection idle, can query immediately
 *      1       connection busy, must queue query
 */
static int _evsql_query_idle (struct evsql *evsql) {
    switch (evsql->type) {
        case EVSQL_EVPQ: {
            enum evpq_state state = evpq_state(evsql->engine.evpq);
            
            switch (state) {
                case EVPQ_CONNECT:
                case EVPQ_QUERY:
                    return 1;
                
                case EVPQ_CONNECTED:
                    return 0;

                case EVPQ_INIT:
                case EVPQ_FAILURE:
                    return -1;
                
                default:
                    FATAL("evpq_state");
            }

        }
        
        default:
            FATAL("evsql->type");
    }
}


struct evsql_query *evsql_query (struct evsql *evsql, const char *command, evsql_query_cb query_fn, void *cb_arg) {
    struct evsql_query *query;
    int idle;

    // allocate it
    if ((query = calloc(1, sizeof(*query))) == NULL)
        ERROR("calloc");

    // store
    query->evsql = evsql;
    query->cb_fn = query_fn;
    query->cb_arg = cb_arg;
    
    // check state
    if ((idle = _evsql_query_idle(evsql)) < 0)
        ERROR("connection is not valid");
    
    if (idle) {
        assert(TAILQ_EMPTY(&evsql->queue));

        // execute directly
        if (_evsql_query_exec(evsql, query, command))
            goto error;

    } else {
        // copy the command for later execution
        if ((query->command = strdup(command)) == NULL)
            ERROR("strdup");
    }
    
    // store it on the list
    TAILQ_INSERT_TAIL(&evsql->queue, query, entry);
    
    // success
    return query;

error:
    // do *NOT* free query->command, ever
    free(query);

    return NULL;
}
