#ifndef LAUNCH_CALENDAR_H
#define LAUNCH_CALENDAR_H

#include <glib.h>
#include <stdbool.h>

typedef enum { CAL_YEAR, CAL_MONTH, CAL_WEEK, CAL_THREE_DAYS, CAL_DAY, CALENDAR_TABS } CalendarTab;

typedef struct {
    CalendarTab tab;
    GDate focus, today;
    int preferred_day;
} Calendar;

void calendar_open(Calendar *);
void calendar_go_today(Calendar *);
bool calendar_update_today(Calendar *);
void calendar_shift(Calendar *, int direction);

#endif
