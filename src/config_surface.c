#include "config_surface.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} json_builder_t;

static void set_error(char *error, size_t capacity, const char *format, ...) {
    if (!error || capacity == 0)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, capacity, format, args);
    va_end(args);
}

static bool ascii_equal(const char *left, const char *right) {
    if (!left || !right)
        return false;
    while (*left && *right) {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++))
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool parse_bool(const char *text, bool *out) {
    static const char *const truthy[] = {"1", "true", "yes", "on", "y", "t", NULL};
    static const char *const falsy[] = {"0", "false", "no", "off", "n", "f", NULL};
    for (size_t i = 0; truthy[i]; i++)
        if (ascii_equal(text, truthy[i])) {
            *out = true;
            return true;
        }
    for (size_t i = 0; falsy[i]; i++)
        if (ascii_equal(text, falsy[i])) {
            *out = false;
            return true;
        }
    return false;
}

static bool trailing_space_only(const char *cursor) {
    while (cursor && *cursor && isspace((unsigned char)*cursor))
        cursor++;
    return cursor && *cursor == '\0';
}

static bool spec_valid(const dsco_config_spec_t *spec) {
    if (!spec || !spec->name || !spec->name[0] || spec->kind < DSCO_CONFIG_BOOL ||
        spec->kind > DSCO_CONFIG_STRING)
        return false;
    if ((spec->has_minimum || spec->has_maximum) &&
        (spec->kind == DSCO_CONFIG_BOOL || spec->kind == DSCO_CONFIG_STRING))
        return false;
    if (spec->has_minimum && spec->has_maximum) {
        switch (spec->kind) {
            case DSCO_CONFIG_SIZE:
                return spec->minimum.size <= spec->maximum.size;
            case DSCO_CONFIG_U64:
                return spec->minimum.u64 <= spec->maximum.u64;
            case DSCO_CONFIG_DOUBLE:
                return isfinite(spec->minimum.number) && isfinite(spec->maximum.number) &&
                       spec->minimum.number <= spec->maximum.number;
            default:
                return false;
        }
    }
    if (spec->kind == DSCO_CONFIG_DOUBLE) {
        if ((spec->has_minimum && !isfinite(spec->minimum.number)) ||
            (spec->has_maximum && !isfinite(spec->maximum.number)) ||
            (spec->has_default && !isfinite(spec->default_value.number)))
            return false;
    }
    return true;
}

static bool value_in_bounds(const dsco_config_spec_t *spec, const dsco_config_value_t *value) {
    switch (spec->kind) {
        case DSCO_CONFIG_SIZE:
            return (!spec->has_minimum || value->size >= spec->minimum.size) &&
                   (!spec->has_maximum || value->size <= spec->maximum.size);
        case DSCO_CONFIG_U64:
            return (!spec->has_minimum || value->u64 >= spec->minimum.u64) &&
                   (!spec->has_maximum || value->u64 <= spec->maximum.u64);
        case DSCO_CONFIG_DOUBLE:
            return isfinite(value->number) &&
                   (!spec->has_minimum || value->number >= spec->minimum.number) &&
                   (!spec->has_maximum || value->number <= spec->maximum.number);
        default:
            return true;
    }
}

static bool parse_value(const dsco_config_spec_t *spec, const char *text,
                        dsco_config_value_t *value) {
    char *end = NULL;
    errno = 0;
    switch (spec->kind) {
        case DSCO_CONFIG_BOOL:
            return parse_bool(text, &value->boolean);
        case DSCO_CONFIG_SIZE: {
            if (*text == '-')
                return false;
            unsigned long long parsed = strtoull(text, &end, 10);
            if (errno || end == text || !trailing_space_only(end) ||
                parsed > (unsigned long long)SIZE_MAX)
                return false;
            value->size = (size_t)parsed;
            return true;
        }
        case DSCO_CONFIG_U64: {
            if (*text == '-')
                return false;
            unsigned long long parsed = strtoull(text, &end, 10);
            if (errno || end == text || !trailing_space_only(end))
                return false;
            value->u64 = (uint64_t)parsed;
            return true;
        }
        case DSCO_CONFIG_DOUBLE:
            value->number = strtod(text, &end);
            return !errno && end != text && trailing_space_only(end) && isfinite(value->number);
        case DSCO_CONFIG_STRING:
            value->string = text;
            return true;
    }
    return false;
}

bool dsco_config_resolve(const dsco_config_spec_t *spec, const char *explicit_text,
                         dsco_config_resolved_t *out, char *error, size_t error_capacity) {
    if (error && error_capacity)
        error[0] = '\0';
    if (!out || !spec_valid(spec)) {
        set_error(error, error_capacity, "invalid configuration specification");
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *text = explicit_text;
    dsco_config_source_t source = DSCO_CONFIG_FROM_EXPLICIT;
    if (!text) {
        text = spec->environment && spec->environment[0] ? getenv(spec->environment) : NULL;
        if (text && text[0])
            source = DSCO_CONFIG_FROM_ENVIRONMENT;
        else
            text = NULL;
    }
    if (!text) {
        if (!spec->has_default) {
            if (spec->required) {
                set_error(error, error_capacity, "%s is required", spec->name);
                return false;
            }
            out->source = DSCO_CONFIG_UNSET;
            return true;
        }
        out->is_set = true;
        out->source = DSCO_CONFIG_FROM_DEFAULT;
        out->value = spec->default_value;
        if (!value_in_bounds(spec, &out->value)) {
            set_error(error, error_capacity, "%s has an invalid declared default", spec->name);
            return false;
        }
        return true;
    }

    if (!parse_value(spec, text, &out->value)) {
        set_error(error, error_capacity, "%s must be %s", spec->name,
                  dsco_config_kind_name(spec->kind));
        return false;
    }
    if (!value_in_bounds(spec, &out->value)) {
        set_error(error, error_capacity, "%s is outside its declared range", spec->name);
        return false;
    }
    out->is_set = true;
    out->source = source;
    return true;
}

const char *dsco_config_kind_name(dsco_config_kind_t kind) {
    switch (kind) {
        case DSCO_CONFIG_BOOL:
            return "boolean";
        case DSCO_CONFIG_SIZE:
            return "size";
        case DSCO_CONFIG_U64:
            return "uint64";
        case DSCO_CONFIG_DOUBLE:
            return "number";
        case DSCO_CONFIG_STRING:
            return "string";
    }
    return "unknown";
}

const char *dsco_config_source_name(dsco_config_source_t source) {
    switch (source) {
        case DSCO_CONFIG_UNSET:
            return "unset";
        case DSCO_CONFIG_FROM_DEFAULT:
            return "default";
        case DSCO_CONFIG_FROM_ENVIRONMENT:
            return "environment";
        case DSCO_CONFIG_FROM_EXPLICIT:
            return "explicit";
    }
    return "unknown";
}

static void json_append(json_builder_t *builder, const char *text, size_t length) {
    if (builder->failed)
        return;
    if (length > SIZE_MAX - builder->length - 1) {
        builder->failed = true;
        return;
    }
    size_t required = builder->length + length + 1;
    if (required > builder->capacity) {
        size_t next = builder->capacity ? builder->capacity : required;
        while (next < required) {
            if (next > SIZE_MAX / 2) {
                next = required;
                break;
            }
            next *= 2;
        }
        char *grown = realloc(builder->data, next);
        if (!grown) {
            builder->failed = true;
            return;
        }
        builder->data = grown;
        builder->capacity = next;
    }
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
}

static void json_text(json_builder_t *builder, const char *text) {
    json_append(builder, text, strlen(text));
}

static void json_format(json_builder_t *builder, const char *format, ...) {
    if (builder->failed)
        return;
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (needed < 0 || (size_t)needed == SIZE_MAX) {
        va_end(args);
        builder->failed = true;
        return;
    }
    size_t capacity = (size_t)needed + 1;
    char *formatted = malloc(capacity);
    if (!formatted) {
        va_end(args);
        builder->failed = true;
        return;
    }
    vsnprintf(formatted, capacity, format, args);
    va_end(args);
    json_append(builder, formatted, (size_t)needed);
    free(formatted);
}

static void json_string(json_builder_t *builder, const char *value) {
    static const char hex[] = "0123456789abcdef";
    json_text(builder, "\"");
    for (const unsigned char *cursor = (const unsigned char *)(value ? value : ""); *cursor;
         cursor++) {
        switch (*cursor) {
            case '\"':
                json_text(builder, "\\\"");
                break;
            case '\\':
                json_text(builder, "\\\\");
                break;
            case '\b':
                json_text(builder, "\\b");
                break;
            case '\f':
                json_text(builder, "\\f");
                break;
            case '\n':
                json_text(builder, "\\n");
                break;
            case '\r':
                json_text(builder, "\\r");
                break;
            case '\t':
                json_text(builder, "\\t");
                break;
            default:
                if (*cursor < ' ') {
                    char escaped[] = {'\\', 'u', '0', '0', hex[*cursor >> 4], hex[*cursor & 15],
                                      '\0'};
                    json_text(builder, escaped);
                } else {
                    json_append(builder, (const char *)cursor, 1);
                }
        }
    }
    json_text(builder, "\"");
}

static void json_value(json_builder_t *builder, dsco_config_kind_t kind,
                       dsco_config_value_t value) {
    switch (kind) {
        case DSCO_CONFIG_BOOL:
            json_text(builder, value.boolean ? "true" : "false");
            break;
        case DSCO_CONFIG_SIZE:
            json_format(builder, "%zu", value.size);
            break;
        case DSCO_CONFIG_U64:
            json_format(builder, "%llu", (unsigned long long)value.u64);
            break;
        case DSCO_CONFIG_DOUBLE:
            json_format(builder, "%.17g", value.number);
            break;
        case DSCO_CONFIG_STRING:
            json_string(builder, value.string);
            break;
    }
}

size_t dsco_config_format_schema(const dsco_config_spec_t *specs, size_t count, char *buffer,
                                 size_t capacity) {
    if (count && !specs)
        return SIZE_MAX;
    for (size_t i = 0; i < count; i++)
        if (!spec_valid(&specs[i]))
            return SIZE_MAX;

    json_builder_t builder = {0};
    json_text(&builder, "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
                        "\"type\":\"object\",\"properties\":{");
    for (size_t i = 0; i < count; i++) {
        const dsco_config_spec_t *spec = &specs[i];
        if (i)
            json_text(&builder, ",");
        json_string(&builder, spec->name);
        json_text(&builder, ":{\"type\":");
        json_string(&builder, spec->kind == DSCO_CONFIG_SIZE || spec->kind == DSCO_CONFIG_U64
                                  ? "integer"
                                  : dsco_config_kind_name(spec->kind));
        if ((spec->kind == DSCO_CONFIG_SIZE || spec->kind == DSCO_CONFIG_U64) && !spec->has_minimum)
            json_text(&builder, ",\"minimum\":0");
        if (spec->description && spec->description[0]) {
            json_text(&builder, ",\"description\":");
            json_string(&builder, spec->description);
        }
        if (spec->environment && spec->environment[0]) {
            json_text(&builder, ",\"x-environment\":");
            json_string(&builder, spec->environment);
        }
        if (spec->has_default) {
            json_text(&builder, ",\"default\":");
            json_value(&builder, spec->kind, spec->default_value);
        }
        if (spec->has_minimum) {
            json_text(&builder, ",\"minimum\":");
            json_value(&builder, spec->kind, spec->minimum);
        }
        if (spec->has_maximum) {
            json_text(&builder, ",\"maximum\":");
            json_value(&builder, spec->kind, spec->maximum);
        }
        json_text(&builder, "}");
    }
    json_text(&builder, "},\"additionalProperties\":false");

    bool first_required = true;
    for (size_t i = 0; i < count; i++) {
        if (!specs[i].required)
            continue;
        if (first_required)
            json_text(&builder, ",\"required\":[");
        else
            json_text(&builder, ",");
        json_string(&builder, specs[i].name);
        first_required = false;
    }
    if (!first_required)
        json_text(&builder, "]");
    json_text(&builder, "}");

    if (builder.failed) {
        free(builder.data);
        return SIZE_MAX;
    }
    if (buffer && capacity) {
        size_t copied = builder.length < capacity - 1 ? builder.length : capacity - 1;
        memcpy(buffer, builder.data, copied);
        buffer[copied] = '\0';
    }
    size_t length = builder.length;
    free(builder.data);
    return length;
}
