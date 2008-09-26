
#include <stdlib.h>

#include "dirbuf.h"
#include "lib/log.h"
#include "lib/math.h"

int dirbuf_init (struct dirbuf *buf, size_t req_size) {
    buf->len = req_size;
    buf->off = 0;
    
    INFO("\tdirbuf.init: req_size=%zu", req_size);

    // allocate the mem
    if ((buf->buf = malloc(buf->len)) == NULL)
        ERROR("malloc");
    
    // ok
    return 0;

error:
    return -1;
}

int dirbuf_add (fuse_req_t req, off_t req_off, struct dirbuf *buf, off_t ent_off, off_t next_off, const char *ent_name, fuse_ino_t ent_ino, mode_t ent_mode) {
    struct stat stbuf;
    size_t ent_size;

    INFO("\tdirbuf.add: req_off=%zu, buf->len=%zu, buf->off=%zu, ent_off=%zu, next_off=%zu, ent_name=`%s`, ent_ino=%lu, ent_mode=%07o",
        req_off, buf->len, buf->off, ent_off, next_off, ent_name, ent_ino, ent_mode);
    
    // skip entries as needed
    if (ent_off < req_off) 
        return 0;

    // set ino
    stbuf.st_ino = ent_ino;
    stbuf.st_mode = ent_mode;
    
    // try and add the dirent, and see if it fits
    if ((ent_size = fuse_add_direntry(req, buf->buf + buf->off, buf->len - buf->off, ent_name, &stbuf, next_off)) > (buf->len - buf->off)) {
        // 'tis full
        return 1;

    } else {
        // it fit
        buf->off += ent_size;
    }

    // success
    return 0;
}

int dirbuf_done (fuse_req_t req, struct dirbuf *buf) {
    int err;
    
    // send the reply, return the error later
    err = fuse_reply_buf(req, buf->buf, buf->off);

    INFO("\tdirbuf.done: size=%zu/%zu, err=%d", buf->off, buf->len, err);

    // free the dirbuf
    free(buf->buf);

    // return the error code
    return err;
}

