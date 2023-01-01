#include "utils.h"

bool
compare_string_utf8(const char *val, u64 length, const char *cmp) {
    while(length--) {
        if(*val++ != *cmp++) return false;
    }
    return true;
}

u32
convert_string_to_u32(const char *str, u64 length) {
    u32 out= 0;
    for(u32 i= 0; i < length; ++i) {
        out*= 10;
        out+= str[i] - 0x30;
    }
    return out;
}