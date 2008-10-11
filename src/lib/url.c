#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "url.h"
#include "lex.h"
#include "error.h"
#include "log.h"
#include "misc.h"

enum url_token {
    URL_INVALID,
    
    URL_BEGIN,

    // kludge to resolve ambiguous URL_SCHEME/URL_USERNAME+URL_PASSWORD/URL_HOSTNAME+URL_SERVICE at the beginning
    URL_BEGIN_ALNUM,
    URL_BEGIN_COLON,

    URL_SCHEME,
    URL_SCHEME_SEP,
    URL_SCHEME_END_COL,
    URL_SCHEME_END_SLASH1,
    URL_SCHEME_END_SLASH2,

    // kludge to resolve ambiguous URL_USERNAME+URL_PASSWORD/URL_HOSTNAME+URL_SERVICE after a scheme 
    URL_USERHOST_ALNUM,
    URL_USERHOST_COLON,
    URL_USERHOST_ALNUM2,
    
    URL_USERNAME,
    URL_PASSWORD_SEP,
    URL_PASSWORD,
    URL_USERNAME_END,

    URL_HOSTNAME,

    URL_SERVICE_SEP,
    URL_SERVICE,

    URL_PATH_START,
    URL_PATH,

    URL_OPT_START,
    URL_OPT_KEY,
    URL_OPT_EQ,
    URL_OPT_VAL,
    URL_OPT_SEP,
    
    URL_MAX,
};

/*
 * Parser state
 */
struct url_state {
    // the URL to parse into
    struct url *url;
    
    // our lookahead-kludge
    const char *alnum, *alnum2;
    
};

static int _url_append_scheme (struct url *url, const char *data, int copy) {
    if (!url->schema) {
        if ((url->schema = malloc(sizeof(struct url_schema) + (1 * sizeof(const char *)))) == NULL)
            ERROR("malloc");

        url->schema->count = 1;

    } else {
        url->schema->count++;
        
        // I'm starting to hate flexible array members...
        if ((url->schema = realloc(url->schema, sizeof(struct url_schema) + url->schema->count * sizeof(const char *))) == NULL)
            ERROR("realloc");
    }
    
    if ((url->schema->list[url->schema->count - 1] = copy ? strdup(data) : data) == NULL)
        ERROR("strdup");

    // k
    return 0;

error:
    return -1;
}

static struct url_opt *_url_get_opt (struct url *url, int new) {
    if (!url->opts) {
        if ((url->opts = malloc(sizeof(struct url_opts) + (1 * sizeof(struct url_opt)))) == NULL)
            ERROR("malloc");

        url->opts->count = 1;

    } else if (new) {
        url->opts->count++;

        if ((url->opts = realloc(url->opts, sizeof(struct url_opts) + url->opts->count * sizeof(struct url_opt))) == NULL)
            ERROR("realloc");
    }
    
    // success
    return &url->opts->list[url->opts->count - 1];

error:
    return NULL;
}

static int _url_append_opt_key (struct url *url, const char *key) {
    struct url_opt *opt;

    if ((opt = _url_get_opt(url, 1)) == NULL)
        goto error;

    if ((opt->key = strdup(key)) == NULL)
        ERROR("strdup");

    opt->value = NULL;

    return 0;

error:
    return -1;
} 

static int _url_append_opt_val (struct url *url, const char *value) {
    struct url_opt *opt;

    if ((opt = _url_get_opt(url, 0)) == NULL)
        goto error;

    if ((opt->value = strdup(value)) == NULL)
        ERROR("strdup");

    return 0;

error:
    return -1;
}

static int url_lex_token (int _this_token, char *token_data, int _next_token, int _prev_token, void *arg);

static struct lex url_lex = {
    .token_fn = url_lex_token,
    .char_fn = NULL,
    .end_fn = NULL,

    .state_count = URL_MAX,
    .initial_state = URL_BEGIN,
    .state_list = {
        LEX_STATE ( URL_BEGIN ) {
            LEX_ALNUM       (           URL_BEGIN_ALNUM         ),
            LEX_CHAR        (   ':',    URL_SERVICE_SEP         ),
            LEX_CHAR        (   '/',    URL_PATH_START          ),
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_END
        },
        
        // this can be URL_SCHEME, URL_USERNAME or URL_HOSTNAME
        LEX_STATE_END ( URL_BEGIN_ALNUM ) {
            LEX_CHAR        (   '+',    URL_SCHEME_SEP          ),  // it was URL_SCHEME
            LEX_CHAR        (   ':',    URL_BEGIN_COLON         ), 
            LEX_CHAR        (   '@',    URL_USERNAME_END        ),  // it was URL_USERNAME
            LEX_CHAR        (   '/',    URL_PATH_START          ),  // it was URL_HOSTNAME
            LEX_CHAR        (   '?',    URL_OPT_START           ),  // it was URL_HOSTNAME
            LEX_DEFAULT     (           URL_BEGIN_ALNUM         )
        },
        
        // this can be URL_SCHEME_END_COL, URL_USERNAME_END or URL_SERVICE_SEP
        LEX_STATE ( URL_BEGIN_COLON ) {
            LEX_CHAR        (   '/',    URL_SCHEME_END_SLASH1   ),  // it was URL_SCHEME
            LEX_ALNUM       (           URL_USERHOST_ALNUM2     ),
            LEX_END
        },
       

        LEX_STATE ( URL_SCHEME ) { 
            LEX_ALNUM       (           URL_SCHEME              ),
            LEX_CHAR        (   '+',    URL_SCHEME_SEP          ),
            LEX_CHAR        (   ':',    URL_SCHEME_END_COL      ),
            LEX_END
        },

        LEX_STATE ( URL_SCHEME_SEP ) {
            LEX_ALNUM       (           URL_SCHEME              ),
            LEX_END
        },

        LEX_STATE ( URL_SCHEME_END_COL ) {
            LEX_CHAR        (   '/',    URL_SCHEME_END_SLASH1   ),
            LEX_END
        },

        LEX_STATE ( URL_SCHEME_END_SLASH1 ) {
            LEX_CHAR        (   '/',    URL_SCHEME_END_SLASH2   ),
            LEX_END
        },

        LEX_STATE_END ( URL_SCHEME_END_SLASH2 ) {
            LEX_ALNUM       (           URL_USERHOST_ALNUM      ),
            LEX_CHAR        (   ':',    URL_SERVICE_SEP         ),
            LEX_CHAR        (   '/',    URL_PATH_START          ),
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_END
        },
        
        // this can be URL_USERNAME or URL_HOSTNAME
        LEX_STATE_END ( URL_USERHOST_ALNUM ) {
            LEX_CHAR        (   ':',    URL_USERHOST_COLON      ), 
            LEX_CHAR        (   '@',    URL_USERNAME_END        ),  // it was URL_USERNAME
            LEX_CHAR        (   '/',    URL_PATH_START          ),  // it was URL_HOSTNAME
            LEX_CHAR        (   '?',    URL_OPT_START           ),  // it was URL_HOSTNAME
            LEX_DEFAULT     (           URL_USERHOST_ALNUM      ),
        },
        
        // this can be URL_USERNAME_END or URL_SERVICE_SEP
        LEX_STATE ( URL_USERHOST_COLON ) {
            LEX_ALNUM       (           URL_USERHOST_ALNUM2        ),
            LEX_END
        },
        
        // this can be URL_PASSWORD or URL_SERVICE
        LEX_STATE_END ( URL_USERHOST_ALNUM2 ) {
            LEX_CHAR        (   '@',    URL_USERNAME_END        ),  // it was URL_PASSSWORD
            LEX_CHAR        (   '/',    URL_PATH_START          ),  // it was URL_SERVICE
            LEX_CHAR        (   '?',    URL_OPT_START           ),  // it was URL_SERVICE
            LEX_DEFAULT     (           URL_USERHOST_ALNUM2     ),
        },
        
        // dummy states, covered by URL_USERHOST_ALNUM/URL_USERHOST_COLON/URL_USERHOST_ALNUM2
        LEX_STATE ( URL_USERNAME ) {
            LEX_END
        },

        LEX_STATE ( URL_PASSWORD_SEP ) {
            LEX_END
        },

        LEX_STATE ( URL_PASSWORD ) {
            LEX_END
        },


        LEX_STATE_END ( URL_USERNAME_END ) {
            LEX_ALNUM       (           URL_HOSTNAME            ), 
            LEX_CHAR        (   ':',    URL_SERVICE_SEP         ),
            LEX_CHAR        (   '/',    URL_PATH_START          ),
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_END
        },


        LEX_STATE_END ( URL_HOSTNAME ) {
            LEX_ALNUM       (           URL_HOSTNAME            ), 
            LEX_CHAR        (   ':',    URL_SERVICE_SEP         ),
            LEX_CHAR        (   '/',    URL_PATH_START          ),
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_END
        },


        LEX_STATE ( URL_SERVICE_SEP ) {
            LEX_ALNUM       (           URL_SERVICE            ), 
            LEX_CHAR        (   '/',    URL_PATH_START          ),
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_END
        },

        LEX_STATE_END ( URL_SERVICE ) {
            LEX_ALNUM       (           URL_SERVICE            ), 
            LEX_CHAR        (   '/',    URL_PATH_START          ),
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_END
        },


        LEX_STATE_END ( URL_PATH_START ) {
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_DEFAULT     (           URL_PATH                ),
        },

        LEX_STATE_END ( URL_PATH ) {
            LEX_CHAR        (   '?',    URL_OPT_START           ),
            LEX_DEFAULT     (           URL_PATH                ),
        },


        LEX_STATE_END ( URL_OPT_START ) {
            LEX_CHAR        (   '&',    URL_OPT_SEP             ),
            LEX_INVALID     (   '='                             ),
            LEX_DEFAULT     (           URL_OPT_KEY             ),
        },

        LEX_STATE_END ( URL_OPT_KEY ) {
            LEX_CHAR        (   '&',    URL_OPT_SEP             ),
            LEX_CHAR        (   '=',    URL_OPT_EQ              ),
            LEX_DEFAULT     (           URL_OPT_KEY             ),
        },

        LEX_STATE_END ( URL_OPT_EQ ) {
            LEX_CHAR        (   '&',    URL_OPT_SEP             ),
            LEX_INVALID     (   '='                             ),
            LEX_DEFAULT     (           URL_OPT_VAL             ),
        },

        LEX_STATE_END ( URL_OPT_VAL ) {
            LEX_CHAR        (   '&',    URL_OPT_SEP             ),
            LEX_INVALID     (   '='                             ),
            LEX_DEFAULT     (           URL_OPT_VAL             ),
        },

        LEX_STATE_END ( URL_OPT_SEP ) {
            LEX_CHAR        (   '&',    URL_OPT_SEP             ),
            LEX_INVALID     (   '='                             ),
            LEX_DEFAULT     (           URL_OPT_KEY             ),
        },
        
        LEX_STATE ( URL_ERROR ) {
            LEX_END
        },
    }
};

static int url_lex_token (int _this_token, char *token_data, int _next_token, int _prev_token, void *arg) {
    enum url_token this_token = _this_token, next_token = _next_token, prev_token = _prev_token;
    struct url_state *state = arg;
    const char **copy_to = NULL;

    (void) prev_token;
    
    switch (this_token) {
        case URL_BEGIN:
            // irrelevant
            break;

        case URL_BEGIN_ALNUM:
            switch (next_token) {
                case URL_SCHEME_SEP:
                    // store the scheme
                    if (_url_append_scheme(state->url, token_data, 1))
                        goto error;
                    
                    break;
                
                case URL_USERNAME_END:
                    // store the username
                    copy_to = &state->url->username; break;
                
                case URL_PATH_START:
                case URL_OPT_START:
                case LEX_EOF:
                    // store the hostname
                    copy_to = &state->url->hostname; break;

                case URL_BEGIN_COLON:
                    // gah...
                    copy_to = &state->alnum; break;
                

                default:
                    FATAL("weird next token");
            }
            
            break;

        case URL_BEGIN_COLON:
            switch (next_token) {
                case URL_SCHEME_END_SLASH1:
                    // store the schema
                    if (_url_append_scheme(state->url, state->alnum, 0))
                        goto error;
                    
                    state->alnum = NULL;

                    break;
                
                case URL_USERHOST_ALNUM2:
                    // gah..
                    break;

                default:
                    FATAL("weird next token");
            }

            break;

        case URL_SCHEME:
            // store the scheme
            if (_url_append_scheme(state->url, token_data, 1))
                goto error;

            break;
    
        case URL_SCHEME_SEP:
            // ignore
            break;

        case URL_SCHEME_END_COL:
        case URL_SCHEME_END_SLASH1:
        case URL_SCHEME_END_SLASH2:
            // ignore
            break;
        
        case URL_USERHOST_ALNUM:
            switch (next_token) {
                case URL_USERNAME_END:
                    // store the username
                    copy_to = &state->url->username; break;
                
                case URL_PATH_START:
                case URL_OPT_START:
                case LEX_EOF:
                    // store the hostname
                    copy_to = &state->url->hostname; break;

                case URL_USERHOST_COLON:
                    // gah...
                    copy_to = &state->alnum; break;

                default:
                    FATAL("weird next token");
            }
            
            break;

        case URL_USERHOST_COLON:
            // ignore
            break;

        case URL_USERHOST_ALNUM2:
            switch (next_token) {
                case URL_USERNAME_END:
                    // store the username and password
                    state->url->username = state->alnum; state->alnum = NULL;
                    copy_to = &state->url->password;

                    break;

                case URL_PATH_START:
                case URL_OPT_START:
                case LEX_EOF:
                    // store the hostname and service
                    state->url->hostname = state->alnum; state->alnum = NULL;
                    copy_to = &state->url->service; break;

                default:
                    FATAL("weird next token");
            }

            break;

        case URL_USERNAME:
        case URL_PASSWORD_SEP:
        case URL_PASSWORD:
            FATAL("these should be overshadowed");
        
        case URL_USERNAME_END:
            // ignore
            break;

        case URL_HOSTNAME:
            // store
            copy_to = &state->url->hostname; break;

        case URL_SERVICE_SEP:
            // ignore
            break;

        case URL_SERVICE:
            // store
            copy_to = &state->url->service; break;
        
        case URL_PATH_START:
            // ignore
            break;

        case URL_PATH:
            // store
            copy_to = &state->url->path; break;

        case URL_OPT_START:
            // ignore
            break;

        case URL_OPT_KEY:
            // store
            if (_url_append_opt_key(state->url, token_data))
                goto error;

            break;

        case URL_OPT_EQ:
            // ignore
            break;

        case URL_OPT_VAL:
            // store
            if (_url_append_opt_val(state->url, token_data))
                goto error;

            break;
        
        case URL_OPT_SEP:
            // ignore
            break;
        
        default:
            ERROR("invalid token");
    }
    
    if (copy_to) {
        // copy the token data
        if ((*copy_to = strdup(token_data)) == NULL)
            ERROR("strdup");
    }

    // good
    return 0;

error:
    DEBUG("token: %s -> %s -> %s: %s", 
        LEX_STATE_NAME(&url_lex, prev_token), LEX_STATE_NAME(&url_lex, this_token), LEX_STATE_NAME(&url_lex, next_token),
        token_data
    );
    return -1;
}


int url_parse (struct url *url, const char *text) {
    struct url_state state; ZINIT(state);
    int ret;

    // set up state
    state.url = url;
    
    // parse it
    if ((ret = lexer(&url_lex, text, &state)))
        ERROR("invalid URL");

    // success
    return 0;

error:
    return -1;
}

static void _url_dump_part (const char *field, const char *val, FILE *stream) {
    if (val) {
        fprintf(stream, "%s=%s ", field, val);
    }
}

void url_dump (const struct url *url, FILE *stream) {
    int i;

    if (url->schema) {
        fprintf(stream, "schema=(");

        for (i = 0; i < url->schema->count; i++) {
            if (i > 0)
                fprintf(stream, ",");

            fprintf(stream, "%s", url->schema->list[i]);
        }

        fprintf(stream, ") ");
    }

    _url_dump_part("username", url->username, stream);
    _url_dump_part("password", url->password, stream);
    _url_dump_part("hostname", url->hostname, stream);
    _url_dump_part("service", url->service, stream);
    _url_dump_part("path", url->path, stream);

    if (url->opts) {
        fprintf(stream, "opts: ");

        for (i = 0; i < url->opts->count; i++) {
            fprintf(stream, "%s=%s ", url->opts->list[i].key, url->opts->list[i].value);
        }
    }

    fprintf(stream, "\n");
}

