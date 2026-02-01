/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_TIME_SWITCH

#include "../SDL_time_c.h"
#include <errno.h>
#include <langinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void SDL_GetSystemTimeLocalePreferences(SDL_DateFormat *df, SDL_TimeFormat *tf)
{
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
    CHECK_PARAM(!ticks) {
        return SDL_InvalidParamError("ticks");
    }

    struct timespec tp;

    if (clock_gettime(CLOCK_REALTIME, &tp) == 0) {
        //tp.tv_sec = SDL_min(tp.tv_sec, SDL_NS_TO_SECONDS(SDL_MAX_TIME) - 1);
        *ticks = SDL_SECONDS_TO_NS(tp.tv_sec) + tp.tv_nsec;
        return true;
    }

    SDL_SetError("Failed to retrieve system time (%i)", errno);

    return false;
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *dt, bool localTime)
{
    struct tm *tm = NULL;

    CHECK_PARAM(!dt) {
        return SDL_InvalidParamError("dt");
    }

    const time_t tval = (time_t)SDL_NS_TO_SECONDS(ticks);

    tm = localtime(&tval);

    if (tm) {
        dt->year = tm->tm_year + 1900;
        dt->month = tm->tm_mon + 1;
        dt->day = tm->tm_mday;
        dt->hour = tm->tm_hour;
        dt->minute = tm->tm_min;
        dt->second = tm->tm_sec;
        dt->nanosecond = ticks % SDL_NS_PER_SECOND;
        dt->day_of_week = tm->tm_wday;

        dt->utc_offset = 0;

        return true;
    }

    return SDL_SetError("SDL_DateTime conversion failed (%i)", errno);
}

#endif // SDL_TIME_SWITCH
