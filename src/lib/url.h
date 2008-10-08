#ifndef LIB_URL_H
#define LIB_URL_H

/*
 * A trivial parser for simple URLs
 *
 * [ <scheme> [ "+" <scheme> [ ... ] ] "://" ] [ <username> [ ":" <password> ] "@" ] [ <hostname> ] [ ":" <service> ] [ "/" <path> ] [ "?" [ <key> [ "=" <value> ] ] [ "&" [ <key> [ "="     <value> ] ] [ ... ] ]
 *
 *  example.com
 *  tcp://example.com:7348/
 *  psql://postgres@localhost/test_db?charset=utf8
 *  
 */

#include <sys/types.h>
#include <stdio.h>

/*
 * The schema
 */
struct url_schema {
    size_t count;
    const char *list[];
};

/*
 * The options at the end
 */
struct url_opts {
    size_t count;
    struct url_opt {
        const char *key;
        const char *value;
    } list[];
};

/*
 * A parsed URL
 */
struct url {
    struct url_schema *schema;
    const char *username;
    const char *password;
    const char *hostname;
    const char *service;
    const char *path;
    struct url_opts *opts;
};

/*
 * Parse the given `text` as an URL, returning the result in `url`. Optional fields that are missing in the text will
 * cause those values to be returned unmodified.
 *
 * Returns zero if the url was valid and was parsed, nonzero if it was invalid.
 */
int url_parse (struct url *url, const char *text);

/*
 * Prints a url in a debug-output format.
 */
void url_dump (const struct url *url, FILE *stream);

#endif /* LIB_URL_H */
