#ifndef DSCO_WEATHER_BATCH_H
#define DSCO_WEATHER_BATCH_H

#include <stdbool.h>
#include <stddef.h>

/* Native bounded-concurrency OpenWeatherMap batch retrieval. This is an I/O
 * primitive: it does not create agent/model workers or invoke the swarm API. */
bool tool_weather_batch(const char *input, char *result, size_t rlen);

#endif /* DSCO_WEATHER_BATCH_H */
