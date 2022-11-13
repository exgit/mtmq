#ifndef MTMQ_H_INCLUDED
#define MTMQ_H_INCLUDED


// Opaque type for queue.
typedef struct mtmq mtmq_t;

// Result codes.
enum {
    MTMQ_RC_OK, // operation performed successfully
    MTMQ_RC_FINALIZED, // operation was not performed because queue is finalized
    MTMQ_RC_TIMEDOUT, // operation not performed in given amount of time
    MTMQ_RC_INTERRUPTED, // interrupted by signal (should never happen on posix-compliant system)
    MTMQ_RC_ERROR // some error happened (requires investigation and debugging)
};


mtmq_t *mtmq_create(int size);
int mtmq_destroy(mtmq_t *q);
int mtmq_push(mtmq_t *q, int code, void *data, int timeout);
int mtmq_pop(mtmq_t *q, int *code, void **data, int timeout);
void mtmq_finalize(mtmq_t *q);
int mtmq_is_finalized(mtmq_t *q);


#endif
