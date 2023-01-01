#pragma once

#include "types.h"

bool
compare_string_utf8(const char *val, u64 length, const char *cmp);
u32
convert_string_to_u32(const char *str, u64 length);