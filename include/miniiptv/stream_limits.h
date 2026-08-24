#ifndef MINIIPTV_STREAM_LIMITS_H
#define MINIIPTV_STREAM_LIMITS_H

/* Shared ordinary-RAM safety boundaries. Keeping these in one header prevents
 * the live producer, atomic staging, and shadow recommendations from drifting
 * to incompatible assumptions. */
#define MINIIPTV_STREAM_RING_CAPACITY_BYTES (6u * 1024u * 1024u)
#define MINIIPTV_STREAM_ATOMIC_SEGMENT_LIMIT_BYTES (4u * 1024u * 1024u)

#endif
