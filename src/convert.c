#include <math.h>
#include <string.h>
#include "launch.h"

typedef enum { LENGTH, MASS, TEMPERATURE, VOLUME, SPEED, DATA } Dimension;

typedef struct {
    const char *symbol, *aliases;
    Dimension dimension;
    double scale, offset;
} Unit;

/* Values in metres, grams, Celsius, litres, metres per second, and bytes. */
static const Unit units[] = {
    {"mm", "millimeter|millimeters|millimetre|millimetres", LENGTH, 0.001, 0},
    {"cm", "centimeter|centimeters|centimetre|centimetres", LENGTH, 0.01, 0},
    {"m", "meter|meters|metre|metres", LENGTH, 1, 0},
    {"km", "kilometer|kilometers|kilometre|kilometres", LENGTH, 1000, 0},
    {"in", "inch|inches", LENGTH, 0.0254, 0},
    {"ft", "foot|feet", LENGTH, 0.3048, 0},
    {"yd", "yard|yards", LENGTH, 0.9144, 0},
    {"mi", "mile|miles", LENGTH, 1609.344, 0},
    {"nmi", "nautical mile|nautical miles", LENGTH, 1852, 0},
    {"mg", "milligram|milligrams", MASS, 0.001, 0},
    {"g", "gram|grams", MASS, 1, 0},
    {"kg", "kilogram|kilograms", MASS, 1000, 0},
    {"t", "tonne|tonnes|metric ton|metric tons", MASS, 1000000, 0},
    {"oz", "ounce|ounces", MASS, 28.349523125, 0},
    {"lb", "lbs|pound|pounds", MASS, 453.59237, 0},
    {"°C", "c|degc|celsius", TEMPERATURE, 1, 0},
    {"°F", "f|degf|fahrenheit", TEMPERATURE, 5.0 / 9, -32 * (5.0 / 9)},
    {"K", "kelvin|kelvins", TEMPERATURE, 1, -273.15},
    {"mL", "milliliter|milliliters|millilitre|millilitres", VOLUME, 0.001, 0},
    {"L", "liter|liters|litre|litres", VOLUME, 1, 0},
    {"US gal", "gal|gallon|gallons|us gallon|us gallons", VOLUME, 3.785411784, 0},
    {"imp gal", "imperial gallon|imperial gallons|uk gal", VOLUME, 4.54609, 0},
    {"US fl oz", "fl oz|fluid ounce|fluid ounces", VOLUME, 0.0295735295625, 0},
    {"US cup", "cup|cups|us cups", VOLUME, 0.2365882365, 0},
    {"m/s", "", SPEED, 1, 0},
    {"km/h", "kph|kmph", SPEED, 1.0 / 3.6, 0},
    {"mph", "mi/h", SPEED, 0.44704, 0},
    {"kn", "knot|knots|kt", SPEED, 1852.0 / 3600, 0},
    {"B", "byte|bytes", DATA, 1, 0},
    {"kB", "kilobyte|kilobytes", DATA, 1000, 0},
    {"MB", "megabyte|megabytes", DATA, 1e6, 0},
    {"GB", "gigabyte|gigabytes", DATA, 1e9, 0},
    {"TB", "terabyte|terabytes", DATA, 1e12, 0},
    {"KiB", "kibibyte|kibibytes", DATA, 1024, 0},
    {"MiB", "mebibyte|mebibytes", DATA, 1048576, 0},
    {"GiB", "gibibyte|gibibytes", DATA, 1073741824, 0},
    {"TiB", "tebibyte|tebibytes", DATA, 1099511627776.0, 0},
    {"b", "bit|bits", DATA, 0.125, 0},
    {"kb", "kilobit|kilobits", DATA, 125, 0},
    {"Mb", "megabit|megabits", DATA, 125000, 0},
    {"Gb", "gigabit|gigabits", DATA, 125000000, 0},
    {"Tb", "terabit|terabits", DATA, 125000000000.0, 0},
    {"Kib", "kibibit|kibibits", DATA, 128, 0},
    {"Mib", "mebibit|mebibits", DATA, 131072, 0},
    {"Gib", "gibibit|gibibits", DATA, 134217728, 0},
    {"Tib", "tebibit|tebibits", DATA, 137438953472.0, 0},
};

static bool matches(const char *text, size_t length, const char *name, size_t size,
                    bool prefix, bool sensitive)
{
    return (prefix ? length <= size : length == size) &&
           (sensitive ? strncmp(text, name, length) : g_ascii_strncasecmp(text, name, length)) == 0;
}

static bool unit_matches(const Unit *unit, const char *text, bool prefix)
{
    size_t length = strlen(text);
    bool sensitive = unit->dimension == DATA;
    if (matches(text, length, unit->symbol, strlen(unit->symbol), prefix, sensitive))
        return true;
    if (strcmp(unit->symbol, "kB") == 0 && matches(text, length, "KB", 2, prefix, true))
        return true;
    for (const char *name = unit->aliases; *name;) {
        const char *end = strchr(name, '|');
        if (matches(text, length, name, end ? (size_t)(end - name) : strlen(name), prefix, false))
            return true;
        if (!end)
            break;
        name = end + 1;
    }
    return false;
}

static const Unit *find_unit(const char *text)
{
    if (*text)
        for (size_t i = 0; i < G_N_ELEMENTS(units); i++)
            if (unit_matches(&units[i], text, false))
                return &units[i];
    return NULL;
}

static void add_result(Model *m, double value, const char *symbol, const char *format,
                       bool trim, const char *date)
{
    if (!isfinite(value))
        return;
    char number[64];
    g_ascii_formatd(number, sizeof number, fabs(value) < 1e12 ? format : "%.10g", value);
    if (trim && fabs(value) < 1e12) {
        size_t length = strlen(number);
        while (length && number[length - 1] == '0')
            number[--length] = '\0';
        if (length && number[length - 1] == '.')
            number[length - 1] = '\0';
    }
    const char *digits = number;
    if (*digits == '-' && g_ascii_strtod(digits, NULL) == 0)
        digits++;
    g_autofree char *label = g_strconcat(digits, " ", symbol, NULL);
    Item *item = item_add(m->conversion_items, label, label, NULL, NULL, ITEM_CONVERSION);
    item->key = g_strdup(symbol);
    if (date)
        item->detail = g_strdup_printf("ECB rate · %s", date);
}

static bool convert_units(Model *m, double amount, const Unit *from, const char *target)
{
    const Unit *exact = find_unit(target);
    if (exact && exact->dimension != from->dimension) {
        m->error = g_strdup("These units measure different quantities");
        return true;
    }
    bool matched = false;
    for (size_t i = 0; i < G_N_ELEMENTS(units); i++) {
        const Unit *to = &units[i];
        if (exact ? to != exact : to == from || to->dimension != from->dimension ||
                                 !unit_matches(to, target, true))
            continue;
        double value = fma(amount, from->scale / to->scale, (from->offset - to->offset) / to->scale);
        bool temperature = to->dimension == TEMPERATURE;
        add_result(m, value, to->symbol, temperature ? "%.2f" : "%.10g", temperature, NULL);
        matched = true;
    }
    if (matched && !m->conversion_items->len)
        m->error = g_strdup("The converted amount is out of range");
    return matched;
}

static bool convert_currency(Model *m, double amount, int from, const char *target)
{
    int exact = rates_currency(target);
    size_t length = strlen(target);
    bool matched = false;
    const char *code;
    for (int to = 0; (code = rates_code(to)); to++) {
        if (exact >= 0 ? to != exact : to == from || g_ascii_strncasecmp(target, code, length) != 0)
            continue;
        matched = true;
        double value = amount;
        const char *date = NULL;
        if (to != from) {
            if (!m->rates)
                m->rates = rates_open();
            double source = rates_value(m->rates, from), destination = rates_value(m->rates, to);
            if (!source || !destination)
                continue;
            value *= destination / source;
            date = rates_date(m->rates);
        }
        bool whole = strcmp(code, "JPY") == 0 || strcmp(code, "KRW") == 0 || strcmp(code, "ISK") == 0;
        add_result(m, value, code, whole ? "%.0f" : "%.2f", false, date);
    }
    if (matched && !m->conversion_items->len) {
        bool pair_has_rate = exact >= 0 && rates_value(m->rates, from) && rates_value(m->rates, exact);
        m->error = g_strdup(rates_fd(m->rates) >= 0 ? "Fetching ECB exchange rates…" :
                            pair_has_rate ? "The converted amount is out of range" :
                            "ECB exchange rate unavailable");
    }
    return matched;
}

bool convert(Model *m)
{
    const char *input = m->query;
    while (g_ascii_isspace(*input))
        input++;
    if (!g_ascii_isdigit(*input) && *input != '-' && *input != '+' && *input != '.')
        return false;
    char *end;
    double amount = g_ascii_strtod(input, &end);
    if (end == input || !isfinite(amount))
        return false;

    const char *split = end;
    while (*split && !(split > end && g_ascii_isspace(split[-1]) &&
                       g_ascii_strncasecmp(split, "to", 2) == 0 &&
                       (!split[2] || g_ascii_isspace(split[2]))))
        split++;
    if (!*split)
        return false;
    g_autofree char *source = g_strndup(end, split - end);
    g_autofree char *target = g_strdup(split + 2);
    g_strstrip(source);
    g_strstrip(target);
    const Unit *from = find_unit(source);
    if (from)
        return convert_units(m, amount, from, target);
    int currency = rates_currency(source);
    return currency >= 0 && convert_currency(m, amount, currency, target);
}
