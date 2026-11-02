/* Layered wind field. Deterministic and smooth in altitude so the wrist
 * instrument can show the winds you are about to climb or descend into. */
#ifndef BALLOON_WIND_H
#define BALLOON_WIND_H

typedef struct WindSample {
    float x;       /* m/s, east  (+X) */
    float z;       /* m/s, north (+Z) */
    float speed;   /* m/s */
    float bearing; /* radians, 0 = north, positive toward east */
} WindSample;

void Wind_AtAltitude(float altitude, WindSample* out);

/* Eight-point compass label for a bearing in radians. Never NULL. */
const char* Wind_Compass(float bearingRadians);

#endif
