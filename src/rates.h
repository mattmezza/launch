#ifndef RATES_H
#define RATES_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Rates Rates;

int rates_currency(const char *);
const char *rates_code(int);
double rates_value(const Rates *, int);
const char *rates_date(const Rates *);
Rates *rates_open(void);
void rates_close(Rates *);
int rates_fd(const Rates *);
bool rates_receive(Rates *);
bool rates_refresh(void);

#endif
