#ifndef LB_HEALTH_H
#define LB_HEALTH_H

#include "pool.h"

/*
 * Active health checking. The server runs a sweep periodically (driven by the
 * event loop's wait timeout): each backend gets a short TCP connect probe, and
 * the result is fed to pool_mark_health(), which applies rise/fall thresholds
 * and flips a backend in or out of rotation.
 */

/* Probe every backend once and update health state. Logs transitions. */
void health_sweep(pool_t *pool);

#endif /* LB_HEALTH_H */
