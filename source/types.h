#pragma once
typedef unsigned __int8  u8;
typedef __int8           s8;
typedef unsigned __int16 u16;
typedef __int16          s16;
typedef unsigned __int32 u32;
typedef __int32          s32;
typedef unsigned __int64 u64;
typedef __int64          s64;
typedef float            f32;
typedef double           f64;
typedef _Bool bool;

#define true  1
#define false 0

#define null (void *)0
typedef union vec3 {
    struct {
        f32 x, y, z;
    };
    f32 data[3];
} vec3;

#define vec3_make(x, y, z)                                                     \
    (vec3) { x, y, z }
#define vec3_set(vec, _x, _y, _z)                                              \
    {                                                                          \
        vec.x= _x;                                                             \
        vec.y= _y;                                                             \
        vec.z= _z;                                                             \
    }

typedef union vec4 {
    struct {
        f32 x, y, z, w;
    };
    f32 data[4];
} vec4;

#define vec4_make(x, y, z)                                                     \
    (vec4) { x, y, z, w }
#define vec4_set(vec, _x, _y, _z, _w)                                          \
    {                                                                          \
        vec.x= _x;                                                             \
        vec.y= _y;                                                             \
        vec.z= _z;                                                             \
        vec.w= _w;                                                             \
    }

typedef union mat4x4 {
    vec4 columns[4];
    f32  data[16];
} mat4x4;

typedef struct vertex {
    vec4 pos;
    vec4 nrm;
} vertex;
