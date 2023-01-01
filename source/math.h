#include "types.h"

#define M_DEG_2_RAD 0.01745329251994329576F

#define M_TO_RAD(x) x *M_DEG_2_RAD

//clang-format off
/* Sin-11th degree and Cos 10th degree approximations
 * Credit to L.Spiro(@TheRealLSpiro)
 * Detailed in gamedev.net post below:
 * https://www.gamedev.net/forums/topic/681723-faster-sin-and-cos */

#define t_1     (f32)(9.99999701976776123047e-01)
#define t_1_neg (f32)(-9.99999701976776123047e-01)
#define t_2     (f32)(-1.66665777564048767090e-01)
#define t_3     (f32)(8.33255797624588012695e-03)
#define t_4     (f32)(-1.98125766473822295666e-04)
#define t_5     (f32)(2.70405212177138309926e-06)
#define t_6     (f32)(-2.05329886426852681325e-08)
static inline f32
sin(f32 fX) {
    s32 i32I= (s32)(fX * 0.31830988618379067153776752674503f); // 1 / PI.
    fX      = (fX - (f32)(i32I)*3.1415926535897932384626433832795f);
    f32 X2  = fX * fX;
    return (i32I & 1) ?
               -fX * (t_1 +
                      X2 * (t_2 +
                            X2 * (t_3 + X2 * (t_4 + X2 * (t_5 + X2 * t_6))))) :
               fX * (t_1 +
                     X2 * (t_2 +
                           X2 * (t_3 + X2 * (t_4 + X2 * (t_5 + X2 * t_6)))));
}
#undef t_2
#undef t_3
#undef t_4
#undef t_5
#undef t_6
#define t_2 (f32)(-4.99995589256286621094e-01)
#define t_3 (f32)(4.16610352694988250732e-02)
#define t_4 (f32)(-1.38627504929900169373e-03)
#define t_5 (f32)(2.42532332777045667171e-05)
#define t_6 (f32)(-2.21941789391166821588e-07)
static inline f32
cos(f32 fX) {
    s32 i32I= (s32)(fX * 0.31830988618379067153776752674503f); // 1 / PI.
    fX      = (fX - (f32)(i32I)*3.1415926535897932384626433832795f);
    f32 X2  = fX * fX;
    return (i32I & 1) ?
               t_1_neg -
                   X2 *
                       (t_2 + X2 * (t_3 + X2 * (t_4 + X2 * (t_5 + X2 * t_6)))) :
               t_1 +
                   X2 * (t_2 + X2 * (t_3 + X2 * (t_4 + X2 * (t_5 + X2 * t_6))));
}
#undef t_1
#undef t_1_neg
#undef t_2
#undef t_3
#undef t_4
#undef t_5
#undef t_6
//clang-format on

static inline void
vec4_mul(const vec4 *_1, const vec4 *_2, vec4 *out) {
    vec4 temp= {
        _1->x * _2->x,
        _1->y * _2->y,
        _1->z * _2->z,
        _1->w * _2->w,
    };
    out->x= temp.x;
    out->y= temp.y;
    out->z= temp.z;
    out->w= temp.w;
}

static inline void
mat4x4_transpose(const mat4x4 *_mat, mat4x4 *out) {
    vec4 col1= {_mat->data[0], _mat->data[4], _mat->data[8], _mat->data[12]};
    vec4 col2= {_mat->data[1], _mat->data[5], _mat->data[9], _mat->data[13]};
    vec4 col3= {_mat->data[2], _mat->data[6], _mat->data[10], _mat->data[14]};
    vec4 col4= {_mat->data[3], _mat->data[7], _mat->data[11], _mat->data[15]};
    vec4_set(out->columns[0], col1.x, col1.y, col1.z, col1.w);
    vec4_set(out->columns[1], col2.x, col2.y, col2.z, col2.w);
    vec4_set(out->columns[2], col3.x, col3.y, col3.z, col3.w);
    vec4_set(out->columns[3], col4.x, col4.y, col4.z, col4.w);
};

static inline void
mat4x4_mul(const mat4x4 *_1, const mat4x4 *_2, mat4x4 *out) {
    mat4x4 _1_transpose, temp;
    mat4x4_transpose(_1, &_1_transpose);
    for(u32 i= 0; i < 4; ++i) {
        for(u32 j= 0; j < 4; ++j) {
            vec4 sum_vec;
            vec4_mul(&_1_transpose.columns[i], &_2->columns[j], &sum_vec);
            temp.data[(i * 4) + j]=
                sum_vec.x + sum_vec.y + sum_vec.z + sum_vec.w;
        }
    }
    for(u32 i= 0; i < 16; ++i) out->data[i]= temp.data[i];
}

static inline void
mat4x4_make_rot_matrix(f32 x, f32 y, f32 z, mat4x4 *out) {
    // clang-format off
    mat4x4 rot_x= (mat4x4){0};
    vec4_set(rot_x.columns[0], 1, 0, 0, 0);
    vec4_set(rot_x.columns[1], 0, cos(x), -sin(x), 0);
    vec4_set(rot_x.columns[2], 0, sin(x),  cos(x), 0);
    vec4_set(rot_x.columns[3], 0, 0, 0, 1);
    mat4x4 rot_y= (mat4x4){0};
    vec4_set(rot_y.columns[0], cos(y), 0, -sin(y), 0);
    vec4_set(rot_y.columns[1], 0, 1, 0, 0);
    vec4_set(rot_y.columns[2], sin(y), 0,  cos(y), 0);
    vec4_set(rot_y.columns[3], 0, 0, 0, 1);
    mat4x4 rot_z= (mat4x4){0};
    vec4_set(rot_z.columns[0], cos(z), sin(z), 0, 0);
    vec4_set(rot_z.columns[1], sin(z), cos(z), 0, 0);
    vec4_set(rot_z.columns[2], 0, 0, 1, 0);
    vec4_set(rot_z.columns[3], 0, 0, 0, 1);
    // clang-format on
    mat4x4_mul(&rot_x, &rot_y, out);
    mat4x4_mul(out, &rot_z, out);
}

static inline void
mat4x4_make_view_matrix(const vec3 *pos, const vec3 *eul, mat4x4 *out) {
    *out= (mat4x4){0};
    mat4x4_make_rot_matrix(-eul->x, -eul->y, -eul->z, out);
    vec4_set(out->columns[3], -pos->x, -pos->y, -pos->z, 1);
}

static inline void
mat4x4_make_persp_proj_matrix(f32 fov, f32 aspect, f32 n, f32 f, mat4x4 *out) {
    *out             = (mat4x4){0};
    const f32 z_range= f - n;
    const f32 a      = cos(fov) / sin(fov);
    const f32 b      = a / aspect;
    out->data[0]     = b;
    out->data[5]     = a;
    out->data[10]    = n / z_range;
    out->data[11]    = out->data[10] * f;
    out->data[14]    = -1.F;
}
