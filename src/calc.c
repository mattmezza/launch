#include <math.h>
#include "launch.h"

typedef struct {
    const char *next;
    unsigned depth;
    bool valid;
} Calc;

static bool eat(Calc *c, char ch)
{
    while (g_ascii_isspace(*c->next))
        c->next++;
    if (*c->next != ch)
        return false;
    c->next++;
    return true;
}

static double sum(Calc *c);
static double unary(Calc *c);

static double number(Calc *c)
{
    if (eat(c, '(')) {
        double value = sum(c);
        if (!eat(c, ')'))
            c->valid = false;
        return value;
    }
    if (g_str_has_prefix(c->next, "pi")) {
        c->next += 2;
        return G_PI;
    }
    if (*c->next == 'e') {
        c->next++;
        return G_E;
    }

    char *end;
    if (!g_ascii_isdigit(*c->next) && *c->next != '.') {
        c->valid = false;
        return NAN;
    }
    double value = g_ascii_strtod(c->next, &end);
    if (end == c->next)
        c->valid = false;
    c->next = end;
    return value;
}

static double unary(Calc *c)
{
    if (++c->depth > 128) {
        c->valid = false;
        return NAN;
    }
    double value;
    if (eat(c, '-'))
        value = -unary(c);
    else if (eat(c, '+'))
        value = unary(c);
    else {
        value = number(c);
        if (c->valid && eat(c, '^'))
            value = pow(value, unary(c));
    }
    c->depth--;
    return value;
}

static double product(Calc *c)
{
    double value = unary(c);
    while (c->valid) {
        if (eat(c, '*'))
            value *= unary(c);
        else if (eat(c, '/'))
            value /= unary(c);
        else if (eat(c, '%')) {
            double divisor = unary(c);
            value = fmod(value, divisor);
        } else
            break;
    }
    return value;
}

static double sum(Calc *c)
{
    double value = product(c);
    while (c->valid) {
        if (eat(c, '+'))
            value += product(c);
        else if (eat(c, '-'))
            value -= product(c);
        else
            break;
    }
    return value;
}

bool calculate(const char *query, char *output, size_t size)
{
    Calc calc = {.next = query, .valid = true};
    double value = sum(&calc);
    if (!calc.valid || *calc.next || !isfinite(value))
        return false;
    g_ascii_formatd(output, (int)size, "%.15g", value == 0 ? 0 : value);
    return true;
}
