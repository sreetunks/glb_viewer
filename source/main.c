#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

#include <intrin.h>

#define VK_NO_PROTOTYPES
#include "vulkan/vulkan_core.h"
#include "vulkan/vulkan_win32.h"

#include "math.h"
#include "types.h"
#include "utils.h"

#define JSMN_HEADER
#include "jsmn/jsmn.h"

#define assert(x)

typedef struct gltf_allocator {
    void *(*alloc)(u64 size);
    void (*free)(void *addr);
} gltf_allocator;

static HANDLE process_heap;

static void *
gltf_alloc(u64 size) {
    return HeapAlloc(process_heap, HEAP_ZERO_MEMORY, size);
}

static void
gltf_free(void *addr) {
    HeapFree(process_heap, 0, addr);
}

static gltf_allocator g_allocator= {.alloc= gltf_alloc, .free= gltf_free};

typedef struct glb_header {
    u32 magic;
    u32 version;
    u32 length;
} glb_header;

typedef enum glb_chunk_type {
    glb_chunk_json         = 0x4E4F534A,
    glb_chunk_bin          = 0x004E4942,
    glb_chunk_type_max_enum= ~(0u)
} glb_chunk_type;

typedef struct glb_chunk_header {
    u32 length;
    u32 type;
} glb_chunk_header;

typedef enum gltf_accessor_component_type {
    gltf_accessor_component_sbyte   = 5120u,
    gltf_accessor_component_ubyte   = 5121u,
    gltf_accessor_component_sshort  = 5122u,
    gltf_accessor_component_ushort  = 5123u,
    gltf_accessor_component_uint    = 5125u,
    gltf_accessor_component_float   = 5126u,
    gltf_accessor_component_max_enum= ~(0u)
} gltf_accessor_component_type;

typedef enum gltf_accessor_type {
    gltf_accessor_undefined,
    gltf_accessor_scalar,
    gltf_accessor_vec2,
    gltf_accessor_vec3,
    gltf_accessor_vec4,
    gltf_accessor_mat2,
    gltf_accessor_mat3,
    gltf_accessor_mat4,
    gltf_accessor_max_enum= ~(0u)
} gltf_accessor_type;

typedef struct gltf_accessor {
    u32                          buffer_view;
    gltf_accessor_component_type component_type;
    gltf_accessor_type           type;
    u32                          count;
    u64                          byte_offset;
} gltf_accessor;

static jsmntok_t *
gltf_parse_accessor(
    gltf_accessor *out,
    jsmntok_t     *accessor_token,
    const char    *json_data) {
    gltf_accessor accessor= {0};
    assert(accessor_token->type != JSMN_OBJECT);
    jsmntok_t *key_token  = &accessor_token[1];
    jsmntok_t *value_token= &accessor_token[2];
    for(u32 i= 0; i < accessor_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 10, "bufferView")) {
            accessor.buffer_view = convert_string_to_u32(value_str, value_len);
            key_token            = value_token + 1;
            value_token          = key_token + 1;
        } else if(compare_string_utf8(key_str, 10, "byteOffset")) {
            accessor.byte_offset = convert_string_to_u32(value_str, value_len);
            key_token            = value_token + 1;
            value_token          = key_token + 1;
        } else if(compare_string_utf8(key_str, 13, "componentType")) {
            accessor.component_type= (gltf_accessor_component_type)
                convert_string_to_u32(value_str, value_len);
            key_token  = value_token + 1;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 5, "count")) {
            accessor.count = convert_string_to_u32(value_str, value_len);
            key_token      = value_token + 1;
            value_token    = key_token + 1;
        } else if(compare_string_utf8(key_str, 4, "type")) {
            accessor.type= (gltf_accessor_type)0;
            if(compare_string_utf8(value_str, value_len, "SCALAR"))
                accessor.type= gltf_accessor_scalar;
            else if(compare_string_utf8(value_str, value_len, "VEC2"))
                accessor.type= gltf_accessor_vec2;
            else if(compare_string_utf8(value_str, value_len, "VEC3"))
                accessor.type= gltf_accessor_vec3;
            else if(compare_string_utf8(value_str, value_len, "VEC4"))
                accessor.type= gltf_accessor_vec4;
            else if(compare_string_utf8(value_str, value_len, "MAT2"))
                accessor.type= gltf_accessor_mat2;
            else if(compare_string_utf8(value_str, value_len, "MAT3"))
                accessor.type= gltf_accessor_mat3;
            else if(compare_string_utf8(value_str, value_len, "MAT4"))
                accessor.type= gltf_accessor_mat4;
            key_token  = value_token + 1;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 3, "max")) {
            key_token  = value_token + value_token->size + 1;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 3, "min")) {
            key_token  = value_token + value_token->size + 1;
            value_token= key_token + 1;
        }
    }
    if(out) {
        out->buffer_view   = accessor.buffer_view;
        out->byte_offset   = accessor.byte_offset;
        out->component_type= accessor.component_type;
        out->type          = accessor.type;
        out->count         = accessor.count;
    }
    return key_token;
}

typedef struct gltf_buffer {
    u64 byte_length;
} gltf_buffer;

static jsmntok_t *
gltf_parse_buffer(
    gltf_buffer *out,
    jsmntok_t   *buffer_token,
    const char  *json_data) {
    gltf_buffer buffer     = {0};
    jsmntok_t  *key_token  = &buffer_token[1];
    jsmntok_t  *value_token= &buffer_token[2];
    for(u32 i= 0; i < buffer_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 10, "byteLength")) {
            buffer.byte_length= convert_string_to_u32(value_str, value_len);
            key_token         = value_token + 1;
            value_token       = key_token + 1;
        }
    }
    if(out) out->byte_length= buffer.byte_length;
    return key_token;
}

typedef enum gltf_buffer_view_target {
    gltf_buffer_view_target_undefined,
    gltf_buffer_view_target_array_buffer,
    gltf_buffer_view_target_element_array_buffer,
    gltf_buffer_view_target_max_enum= ~(0u)
} gltf_buffer_view_target;

typedef struct gltf_buffer_view {
    u32                     buffer;
    u32                     byte_length;
    u32                     byte_offset;
    u32                     byte_stride;
    gltf_buffer_view_target target;
} gltf_buffer_view;

static jsmntok_t *
gltf_parse_buffer_view(
    gltf_buffer_view *out,
    jsmntok_t        *buffer_view_token,
    const char       *json_data) {
    gltf_buffer_view buffer_view= {0};
    jsmntok_t       *key_token  = &buffer_view_token[1];
    jsmntok_t       *value_token= &buffer_view_token[2];
    for(u32 i= 0; i < buffer_view_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 6, "buffer")) {
            buffer_view.buffer= convert_string_to_u32(value_str, value_len);
            key_token         = value_token + 1;
            value_token       = key_token + 1;
        } else if(compare_string_utf8(key_str, 10, "byteLength")) {
            buffer_view.byte_length=
                convert_string_to_u32(value_str, value_len);
            key_token  = value_token + 1;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 10, "byteOffset")) {
            buffer_view.byte_offset=
                convert_string_to_u32(value_str, value_len);
            key_token  = value_token + 1;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 6, "target")) {
            key_token  = value_token + 1;
            value_token= key_token + 1;
        }
    }
    if(out) {
        out->buffer     = buffer_view.buffer;
        out->byte_length= buffer_view.byte_length;
        out->byte_offset= buffer_view.byte_offset;
    }
    return key_token;
}

static jsmntok_t *
gltf_parse_image(void *out, jsmntok_t *image_token, const char *json_data) {
    return image_token + 5;
}

static jsmntok_t *
gltf_parse_pbr(void *out, jsmntok_t *pbr_token, const char *json_data) {
    jsmntok_t *key_token  = &pbr_token[1];
    jsmntok_t *value_token= &pbr_token[2];
    for(u32 i= 0; i < pbr_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 15, "baseColorFactor")) {
            key_token  = value_token + 5;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 16, "baseColorTexture")) {
            key_token  = value_token + 3;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 14, "metallicFactor")) {
            key_token  = value_token + 1;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 15, "metallicTexture")) {
            key_token  = value_token + 3;
            value_token= key_token + 1;
        }
    }
    return key_token;
}

static jsmntok_t *
gltf_parse_material(
    void       *out,
    jsmntok_t  *material_token,
    const char *json_data) {
    jsmntok_t *key_token  = &material_token[1];
    jsmntok_t *value_token= &material_token[2];
    for(u32 i= 0; i < material_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 14, "emissiveFactor")) {
            key_token  = value_token + 3;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 15, "emissiveTexture")) {
            key_token  = value_token + 3;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 13, "normalTexture")) {
            key_token  = value_token + 3;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 17, "occlusionTexture")) {
            key_token  = value_token + 3;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 4, "name")) {
            key_token  = value_token + 1;
            value_token= key_token + 1;
        } else if(compare_string_utf8(key_str, 20, "pbrMetallicRoughness")) {
            key_token= gltf_parse_pbr(null, value_token, json_data);
        }
    }
    return key_token;
}

typedef struct gltf_mesh_primitive {
    u32 pos_accessor;
    u32 nrm_accessor;
    u32 idx_accessor;
    u32 material;
    u32 mode; // TODO: Enum
} gltf_mesh_primitive;

static jsmntok_t *
gltf_parse_mesh_primitive(
    gltf_mesh_primitive *out,
    jsmntok_t           *prim_token,
    const char          *json_data) {
    gltf_mesh_primitive prim       = {0};
    prim.mode                      = 4; // TODO: Enum
    jsmntok_t *key_token  = &prim_token[1];
    jsmntok_t *value_token= &prim_token[2];
    for(u32 i= 0; i < prim_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 10, "attributes")) {
            jsmntok_t *attributes_token= value_token;
            key_token                  = &attributes_token[1];
            value_token                = &attributes_token[2];
            for(u32 i= 0; i < attributes_token->size; ++i) {
                key_str  = &json_data[key_token->start];
                value_str= &json_data[value_token->start];
                value_len= value_token->end - value_token->start;
                if(compare_string_utf8(key_str, 8, "POSITION")) {
                    prim.pos_accessor=
                        convert_string_to_u32(value_str, value_len);
                } else if(compare_string_utf8(key_str, 6, "NORMAL")) {
                    prim.nrm_accessor=
                        convert_string_to_u32(value_str, value_len);
                }
                key_token  = value_token + 1;
                value_token= key_token + 1;
            }
        } else if(compare_string_utf8(key_str, 7, "indices")) {
            prim.idx_accessor= convert_string_to_u32(value_str, value_len);
            key_token        = value_token + 1;
            value_token      = key_token + 1;
        } else if(compare_string_utf8(key_str, 8, "material")) {
            prim.material= convert_string_to_u32(value_str, value_len);
            key_token    = value_token + 1;
            value_token  = key_token + 1;
        } else if(compare_string_utf8(key_str, 4, "mode")) {
            prim.mode  = convert_string_to_u32(value_str, value_len);
            key_token  = value_token + 1;
            value_token= key_token + 1;
        }
    }
    if(out) {
        out->pos_accessor= prim.pos_accessor;
        out->nrm_accessor= prim.nrm_accessor;
        out->idx_accessor= prim.idx_accessor;
        out->material    = prim.material;
        out->mode        = prim.mode;
    }
    return key_token;
}

typedef struct gltf_mesh {
    u64                  primitive_count;
    gltf_mesh_primitive *primitive_list;
} gltf_mesh;

static jsmntok_t *
gltf_parse_mesh(
    gltf_mesh            *out,
    jsmntok_t            *mesh_token,
    const char           *json_data,
    const gltf_allocator *allocator) {
    gltf_mesh  mesh       = {0};
    jsmntok_t *key_token  = &mesh_token[1];
    jsmntok_t *value_token= &mesh_token[2];
    for(u32 i= 0; i < mesh_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 4, "name")) {
            key_token  = value_token + 1;
        } else if(compare_string_utf8(key_str, 10, "primitives")) {
            jsmntok_t *out_token= &key_token[2];
            mesh.primitive_count= value_token->size;
            if(out) {
                mesh.primitive_list= allocator->alloc(
                    sizeof(gltf_mesh_primitive) * mesh.primitive_count);
            }
            for(u32 i= 0; i < value_token->size; ++i) {
                gltf_mesh_primitive *primitive= null;
                if(out) primitive= &mesh.primitive_list[i];
                out_token=
                    gltf_parse_mesh_primitive(primitive, out_token, json_data);
            }
            key_token= out_token;
        }
        value_token= key_token + 1;
    }
    if(out) {
        out->primitive_count= mesh.primitive_count;
        out->primitive_list = mesh.primitive_list;
    }
    return key_token;
}

static jsmntok_t *
gltf_parse_scene(void *out, jsmntok_t *scene_token, const char *json_data) {
    jsmntok_t *key_token  = &scene_token[1];
    jsmntok_t *value_token= &scene_token[2];
    for(u32 i= 0; i < scene_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 4, "name")) {
            key_token= value_token + 1;
        } else if(compare_string_utf8(key_str, 5, "nodes")) {
            jsmntok_t *out_token= &key_token[2];
            for(u32 i= 0; i < value_token->size; ++i) { ++out_token; }
            key_token= out_token;
        }
        value_token= key_token + 1;
    }
    return key_token;
}

static jsmntok_t *
gltf_parse_node(void *out, jsmntok_t *node_token, const char *json_data) {
    jsmntok_t *key_token  = &node_token[1];
    jsmntok_t *value_token= &node_token[2];
    for(u32 i= 0; i < node_token->size; ++i) {
        const char *key_str  = &json_data[key_token->start];
        const char *value_str= &json_data[value_token->start];
        u64         value_len= value_token->end - value_token->start;
        if(compare_string_utf8(key_str, 4, "name")) {
            key_token= value_token + 1;
        } else if(compare_string_utf8(key_str, 4, "mesh")) {
            key_token= value_token + 1;
        } else if(compare_string_utf8(key_str, 8, "children")) {
            jsmntok_t *out_token= &key_token[2];
            for(u32 i= 0; i < value_token->size; ++i) { ++out_token; }
            key_token= out_token;
        } else if(compare_string_utf8(key_str, 6, "matrix")) {
            jsmntok_t *out_token= &key_token[2];
            for(u32 i= 0; i < value_token->size; ++i) { ++out_token; }
            key_token= out_token;
        } else if(compare_string_utf8(key_str, 8, "rotation")) {
            jsmntok_t *out_token= &key_token[2];
            for(u32 i= 0; i < value_token->size; ++i) { ++out_token; }
            key_token= out_token;
        }
        value_token= key_token + 1;
    }
    return key_token;
}

typedef struct gltf_json_data {
    u32               accessor_count;
    u32               buffer_view_count;
    gltf_buffer_view *buffer_view_list;
    gltf_accessor    *accessor_list;
    gltf_buffer       buffer;
    gltf_mesh         mesh;
} gltf_json_data;

static void
gltf_parse_json(
    gltf_json_data       *gltf_json,
    const char           *json_data,
    u64                   json_length,
    const gltf_allocator *allocator) {
    jsmn_parser parser;
    jsmn_init(&parser);

    int        count= jsmn_parse(&parser, json_data, json_length, null, 0);
    jsmntok_t *tokens=
        HeapAlloc(process_heap, HEAP_ZERO_MEMORY, sizeof(jsmntok_t) * count);
    jsmn_init(&parser);
    jsmn_parse(&parser, json_data, json_length, tokens, count);
    assert(tokens[0].type == JSMN_OBJECT);
    jsmntok_t *token= &tokens[1];

    while(token != &tokens[count + 1]) {
        assert(token->type == JSMN_STRING);
        const char *key_str= &json_data[token->start];
        if(compare_string_utf8(key_str, 9, "accessors")) {
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            gltf_json->accessor_count= value_token->size;
            gltf_json->accessor_list = allocator->alloc(
                sizeof(gltf_accessor) * gltf_json->accessor_count);
            for(int i= 0; i < value_token->size; ++i) {
                gltf_accessor *accessor= &gltf_json->accessor_list[i];
                out_token= gltf_parse_accessor(accessor, out_token, json_data);
            }
            token= out_token;
        } else if(compare_string_utf8(key_str, 5, "asset")) {
            token= token + 6;
        } else if(compare_string_utf8(key_str, 11, "bufferViews")) {
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            gltf_json->buffer_view_count= value_token->size;
            gltf_json->buffer_view_list = allocator->alloc(
                sizeof(gltf_buffer_view) * gltf_json->buffer_view_count);
            for(int i= 0; i < value_token->size; ++i) {
                gltf_buffer_view *buffer_view= &gltf_json->buffer_view_list[i];
                out_token=
                    gltf_parse_buffer_view(buffer_view, out_token, json_data);
            }
            token= out_token;
        } else if(compare_string_utf8(key_str, 7, "buffers")) {
            assert(value_token->size == 1);
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            for(u32 i= 0; i < value_token->size; ++i) {
                out_token=
                    gltf_parse_buffer(&gltf_json->buffer, out_token, json_data);
            }
            token= out_token;
        } else if(compare_string_utf8(key_str, 6, "images")) {
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            for(u32 i= 0; i < value_token->size; ++i) {
                out_token= gltf_parse_image(null, out_token, json_data);
            }
            token= out_token;
        } else if(compare_string_utf8(key_str, 9, "materials")) {
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            for(u32 i= 0; i < value_token->size; ++i) {
                out_token= gltf_parse_material(null, out_token, json_data);
            }
            token= out_token;
        } else if(compare_string_utf8(key_str, 6, "meshes")) {
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            for(u32 i= 0; i < value_token->size; ++i) {
                out_token= gltf_parse_mesh(
                    &gltf_json->mesh,
                    out_token,
                    json_data,
                    allocator);
            }
            token= out_token;
        } else if(compare_string_utf8(key_str, 6, "scenes")) {
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            for(u32 i= 0; i < value_token->size; ++i) {
                out_token= gltf_parse_scene(null, out_token, json_data);
            }
            token= out_token;
        } else if(compare_string_utf8(key_str, 5, "scene")) {
            token+= 2;
        } else if(compare_string_utf8(key_str, 5, "nodes")) {
            jsmntok_t *value_token= &token[1];
            jsmntok_t *out_token  = &token[2];
            for(u32 i= 0; i < value_token->size; ++i) {
                out_token= gltf_parse_node(null, out_token, json_data);
            }
            token= out_token;
        } else {
            ++token;
        }
    }
    HeapFree(process_heap, 0, tokens);
}

static BOOL running= FALSE;

static LRESULT
Wndproc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    switch(umsg) {
    case WM_CLOSE:
        running= FALSE;
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProc(hwnd, umsg, wparam, lparam);
    }
}

static HINSTANCE win32_instance;
static HANDLE    win32_window;

static void
win32_create_window() {
    win32_instance  = (HINSTANCE)GetModuleHandle(NULL);
    WNDCLASS wc     = {0};
    wc.lpfnWndProc  = Wndproc;
    wc.hInstance    = win32_instance;
    wc.lpszClassName= TEXT("GLBVWndClass");
    RegisterClass(&wc);
    RECT rect= {0, 0, 1280, 720};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    win32_window= CreateWindow(
        TEXT("GLBVWndClass"),
        TEXT("GLB Viewer"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL,
        NULL,
        win32_instance,
        NULL);
    UpdateWindow(win32_window);
    ShowWindow(win32_window, SW_SHOWDEFAULT);
}

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
PFN_vkCreateInstance      vkCreateInstance;

HANDLE vulkan_library;

static void
vulkan_load_library() {
    vulkan_library       = LoadLibrary(L"vulkan-1.dll");
    vkGetInstanceProcAddr= (PFN_vkGetInstanceProcAddr)GetProcAddress(
        vulkan_library,
        "vkGetInstanceProcAddr");
    vkCreateInstance= (PFN_vkCreateInstance)vkGetInstanceProcAddr(
        VK_NULL_HANDLE,
        "vkCreateInstance");
}

PFN_vkDestroyInstance             vkDestroyInstance;
PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT;
PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
PFN_vkEnumeratePhysicalDevices    vkEnumeratePhysicalDevices;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
PFN_vkGetPhysicalDeviceQueueFamilyProperties
                        vkGetPhysicalDeviceQueueFamilyProperties;
PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR
                            vkGetPhysicalDeviceWin32PresentationSupportKHR;
PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR
                        vkGetPhysicalDeviceSurfacePresentModesKHR;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
PFN_vkCreateDevice      vkCreateDevice;

VkInstance vk_instance;

static void
vulkan_create_instance() {
    VkApplicationInfo app_info        = {0};
    app_info.sType                    = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.apiVersion               = VK_MAKE_API_VERSION(0, 1, 0, 0);
    const char *instance_layers[1]    = {"VK_LAYER_KHRONOS_validation"};
    const char *instance_extensions[4]= {
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_surface",
        "VK_KHR_win32_surface",
        "VK_EXT_debug_utils"};
    VkInstanceCreateInfo create_info   = {0};
    create_info.sType                  = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo       = &app_info;
    create_info.enabledExtensionCount  = 4;
    create_info.ppEnabledExtensionNames= instance_extensions;
    create_info.enabledLayerCount      = 1;
    create_info.ppEnabledLayerNames    = instance_layers;
    VkResult res     = vkCreateInstance(&create_info, NULL, &vk_instance);
    vkDestroyInstance= (PFN_vkDestroyInstance)vkGetInstanceProcAddr(
        vk_instance,
        "vkDestroyInstance");
    vkCreateDebugUtilsMessengerEXT= (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(vk_instance, "vkCreateDebugUtilsMessengerEXT");
    vkDestroyDebugUtilsMessengerEXT= (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(vk_instance, "vkDestroyDebugUtilsMessengerEXT");
    vkEnumeratePhysicalDevices= (PFN_vkEnumeratePhysicalDevices)
        vkGetInstanceProcAddr(vk_instance, "vkEnumeratePhysicalDevices");
    vkGetPhysicalDeviceProperties= (PFN_vkGetPhysicalDeviceProperties)
        vkGetInstanceProcAddr(vk_instance, "vkGetPhysicalDeviceProperties");
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR=
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkGetInstanceProcAddr(
            vk_instance,
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    vkGetPhysicalDeviceSurfaceFormatsKHR=
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkGetInstanceProcAddr(
            vk_instance,
            "vkGetPhysicalDeviceSurfaceFormatsKHR");
    vkGetPhysicalDeviceSurfacePresentModesKHR=
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(
            vk_instance,
            "vkGetPhysicalDeviceSurfacePresentModesKHR");
    vkDestroySurfaceKHR= (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(
        vk_instance,
        "vkDestroySurfaceKHR");
    vkGetPhysicalDeviceMemoryProperties=
        (PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(
            vk_instance,
            "vkGetPhysicalDeviceMemoryProperties");
    vkGetPhysicalDeviceQueueFamilyProperties=
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(
            vk_instance,
            "vkGetPhysicalDeviceQueueFamilyProperties");
    vkGetPhysicalDeviceWin32PresentationSupportKHR=
        (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)
            vkGetInstanceProcAddr(
                vk_instance,
                "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    vkCreateWin32SurfaceKHR= (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(
        vk_instance,
        "vkCreateWin32SurfaceKHR");
    vkGetDeviceProcAddr= (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(
        vk_instance,
        "vkGetDeviceProcAddr");
    vkCreateDevice= (PFN_vkCreateDevice)vkGetInstanceProcAddr(
        vk_instance,
        "vkCreateDevice");
}

VkDebugUtilsMessengerEXT vk_dbg_messenger;

static VkBool32
vulkan_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT             messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void                                       *pUserData) {
    //__debugbreak();
    return VK_FALSE;
}

static void
vulkan_create_debug_messenger() {
    VkDebugUtilsMessengerCreateInfoEXT create_info= {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        NULL,
        0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        vulkan_debug_callback,
        NULL};
    vkCreateDebugUtilsMessengerEXT(
        vk_instance,
        &create_info,
        NULL,
        &vk_dbg_messenger);
}

VkPhysicalDevice vk_physical_device;

static void
vulkan_select_physical_device() {
    DWORD phys_dev_count= 0;
    vkEnumeratePhysicalDevices(vk_instance, &phys_dev_count, NULL);
    if(!phys_dev_count) return;
    VkPhysicalDevice *phy_dev_list= (VkPhysicalDevice *)HeapAlloc(
        process_heap,
        HEAP_ZERO_MEMORY,
        sizeof(VkPhysicalDevice) * phys_dev_count);
    vkEnumeratePhysicalDevices(vk_instance, &phys_dev_count, phy_dev_list);
    VkPhysicalDeviceProperties phys_dev_props= {0};
    for(DWORD i= 0; i < phys_dev_count; ++i) {
        vkGetPhysicalDeviceProperties(phy_dev_list[i], &phys_dev_props);
        if(phys_dev_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            vk_physical_device= phy_dev_list[i];
            HeapFree(process_heap, 0, phy_dev_list);
            return;
        }
    }
    vk_physical_device= phy_dev_list[0];
    HeapFree(process_heap, 0, phy_dev_list);
}

VkSurfaceKHR vk_surface;

static void
vulkan_create_surface() {
    VkWin32SurfaceCreateInfoKHR create_info= {0};
    create_info.sType    = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create_info.hinstance= win32_instance;
    create_info.hwnd     = win32_window;
    VkResult res=
        vkCreateWin32SurfaceKHR(vk_instance, &create_info, NULL, &vk_surface);
}

PFN_vkDestroyDevice          vkDestroyDevice;
PFN_vkAllocateMemory              vkAllocateMemory;
PFN_vkMapMemory                   vkMapMemory;
PFN_vkUnmapMemory                 vkUnmapMemory;
PFN_vkFreeMemory                  vkFreeMemory;
PFN_vkGetDeviceQueue         vkGetDeviceQueue;
PFN_vkQueueSubmit            vkQueueSubmit;
PFN_vkQueuePresentKHR        vkQueuePresentKHR;
PFN_vkQueueWaitIdle          vkQueueWaitIdle;
PFN_vkCreateSemaphore        vkCreateSemaphore;
PFN_vkDestroySemaphore       vkDestroySemaphore;
PFN_vkCreateImageView        vkCreateImageView;
PFN_vkDestroyImageView       vkDestroyImageView;
PFN_vkCreateSwapchainKHR     vkCreateSwapchainKHR;
PFN_vkDestroySwapchainKHR    vkDestroySwapchainKHR;
PFN_vkCreateShaderModule     vkCreateShaderModule;
PFN_vkDestroyShaderModule    vkDestroyShaderModule;
PFN_vkCreateBuffer            vkCreateBuffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
PFN_vkBindBufferMemory            vkBindBufferMemory;
PFN_vkDestroyBuffer               vkDestroyBuffer;
PFN_vkCreateImage                 vkCreateImage;
PFN_vkGetImageMemoryRequirements  vkGetImageMemoryRequirements;
PFN_vkBindImageMemory             vkBindImageMemory;
PFN_vkDestroyImage                vkDestroyImage;
PFN_vkCreatePipelineLayout   vkCreatePipelineLayout;
PFN_vkDestroyPipelineLayout  vkDestroyPipelineLayout;
PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
PFN_vkDestroyPipeline         vkDestroyPipeline;
PFN_vkGetSwapchainImagesKHR  vkGetSwapchainImagesKHR;
PFN_vkAcquireNextImageKHR    vkAcquireNextImageKHR;
PFN_vkCreateCommandPool      vkCreateCommandPool;
PFN_vkDestroyCommandPool     vkDestroyCommandPool;
PFN_vkResetCommandPool       vkResetCommandPool;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
PFN_vkFreeCommandBuffers     vkFreeCommandBuffers;
PFN_vkBeginCommandBuffer     vkBeginCommandBuffer;
PFN_vkEndCommandBuffer       vkEndCommandBuffer;
PFN_vkCmdPipelineBarrier          vkCmdPipelineBarrier;
PFN_vkCmdCopyBuffer               vkCmdCopyBuffer;
PFN_vkCmdBindPipeline             vkCmdBindPipeline;
PFN_vkCmdPushConstants            vkCmdPushConstants;
PFN_vkCmdBindVertexBuffers        vkCmdBindVertexBuffers;
PFN_vkCmdBindIndexBuffer          vkCmdBindIndexBuffer;
PFN_vkCmdBeginRenderingKHR   vkCmdBeginRenderingKHR;
PFN_vkCmdEndRenderingKHR     vkCmdEndRenderingKHR;
PFN_vkCmdSetViewport              vkCmdSetViewport;
PFN_vkCmdSetScissor               vkCmdSetScissor;
PFN_vkCmdDrawIndexed              vkCmdDrawIndexed;

VkDevice vk_device;
VkQueue  vk_gfx_queue, vk_cpy_queue, vk_wsi_queue;
DWORD    transfer_queue_family_index= ~(0u);
DWORD    graphics_queue_family_index= ~(0u);
DWORD    present_queue_family_index = ~(0u);

static void
vulkan_create_device() {
    DWORD queue_family_count= 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        vk_physical_device,
        &queue_family_count,
        NULL);
    VkQueueFamilyProperties *queue_family_props=
        (VkQueueFamilyProperties *)HeapAlloc(
            process_heap,
            HEAP_ZERO_MEMORY,
            sizeof(VkQueueFamilyProperties) * queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        vk_physical_device,
        &queue_family_count,
        queue_family_props);
    VkDeviceQueueCreateInfo queue_create_infos[3]= {0};
    for(u32 i= 0; i < 3; ++i)
        queue_create_infos[i].sType= VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    const char *device_extensions[5]= {
        "VK_KHR_swapchain",
        "VK_KHR_maintenance1",
        "VK_KHR_create_renderpass2",
        "VK_KHR_depth_stencil_resolve",
        "VK_KHR_dynamic_rendering"};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeature= {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
        NULL,
        VK_TRUE};
    VkPhysicalDeviceFeatures2KHR features= {0};
    features.sType= VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR;
    features.pNext= &dynamicRenderingFeature;
    VkDeviceCreateInfo create_info     = {0};
    create_info.sType                  = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext                  = &features;
    create_info.enabledExtensionCount  = 5;
    create_info.ppEnabledExtensionNames= device_extensions;
    create_info.pQueueCreateInfos      = queue_create_infos;
    VkDeviceQueueCreateInfo *queue_create_info_it= queue_create_infos;
    for(DWORD i= 0; i < queue_family_count; ++i) {
        if((queue_family_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) ==
               VK_QUEUE_TRANSFER_BIT &&
           !(queue_family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
           !(queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            transfer_queue_family_index           = i;
            queue_create_info_it->queueFamilyIndex= i;
            ++queue_create_info_it->queueCount;
            if(queue_family_props[i].queueCount >
                   queue_create_info_it->queueCount &&
               vkGetPhysicalDeviceWin32PresentationSupportKHR(
                   vk_physical_device,
                   i) == VK_TRUE) {
                present_queue_family_index= i;
                ++queue_create_info_it->queueCount;
            }
            ++queue_create_info_it;
            ++create_info.queueCreateInfoCount;
            break;
        }
    }
    for(DWORD i= 0; i < queue_family_count; ++i) {
        DWORD queue_count= 0;
        if((queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ==
           VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue_family_index           = i;
            queue_create_info_it->queueFamilyIndex= i;
            ++queue_create_info_it->queueCount;
            if(transfer_queue_family_index == ~(0u) &&
               queue_family_props[i].queueCount >
                   queue_create_info_it->queueCount) {
                transfer_queue_family_index= i;
                ++queue_create_info_it->queueCount;
            }
            if(present_queue_family_index == ~(0u) &&
               vkGetPhysicalDeviceWin32PresentationSupportKHR &&
               queue_family_props[i].queueCount >
                   queue_create_info_it->queueCount) {
                present_queue_family_index= i;
                ++queue_create_info_it->queueCount;
            }
            ++queue_create_info_it;
            ++create_info.queueCreateInfoCount;
            break;
        }
    }
    for(DWORD i= 0;
        transfer_queue_family_index == ~(0u) && i < queue_family_count;
        ++i) {
        if((queue_family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ==
               VK_QUEUE_COMPUTE_BIT &&
           graphics_queue_family_index != i) {
            transfer_queue_family_index           = i;
            queue_create_info_it->queueFamilyIndex= i;
            ++queue_create_info_it->queueCount;
            if(queue_family_props[i].queueCount >
                   queue_create_info_it->queueCount &&
               vkGetPhysicalDeviceWin32PresentationSupportKHR(
                   vk_physical_device,
                   i) == VK_TRUE) {
                present_queue_family_index= i;
                ++queue_create_info_it->queueCount;
            }
            ++queue_create_info_it;
            ++create_info.queueCreateInfoCount;
            break;
        }
    }
    for(DWORD i= 0;
        present_queue_family_index == ~(0u) && i < queue_family_count;
        ++i) {
        if(present_queue_family_index == ~(0u) &&
           graphics_queue_family_index != i &&
           transfer_queue_family_index != i &&
           vkGetPhysicalDeviceWin32PresentationSupportKHR(
               vk_physical_device,
               i) == VK_TRUE) {
            present_queue_family_index            = i;
            queue_create_info_it->queueFamilyIndex= i;
            ++queue_create_info_it->queueCount;
            ++create_info.queueCreateInfoCount;
            break;
        }
    }
    const float  queue_priorities_1[1]  = {1.f};
    const float  queue_priorities_2[2]  = {0.5f, 0.5f};
    const float  queue_priorities_3[3]  = {0.34f, 0.33f, 0.33f};
    const float *queue_priorities[4]    = {
        null,
        queue_priorities_1,
        queue_priorities_2,
        queue_priorities_3,
    };
    for(u32 i= 0; i < 3; ++i)
        queue_create_infos[i].pQueuePriorities=
            queue_priorities[queue_create_infos[i].queueCount];
    VkResult res=
        vkCreateDevice(vk_physical_device, &create_info, NULL, &vk_device);
    vkDestroyDevice=
        (PFN_vkDestroyDevice)vkGetDeviceProcAddr(vk_device, "vkDestroyDevice");
    vkAllocateMemory= (PFN_vkAllocateMemory)vkGetDeviceProcAddr(
        vk_device,
        "vkAllocateMemory");
    vkMapMemory= (PFN_vkMapMemory)vkGetDeviceProcAddr(vk_device, "vkMapMemory");
    vkUnmapMemory=
        (PFN_vkUnmapMemory)vkGetDeviceProcAddr(vk_device, "vkUnmapMemory");
    vkFreeMemory=
        (PFN_vkFreeMemory)vkGetDeviceProcAddr(vk_device, "vkFreeMemory");
    vkGetDeviceQueue= (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(
        vk_device,
        "vkGetDeviceQueue");
    vkQueueSubmit=
        (PFN_vkQueueSubmit)vkGetDeviceProcAddr(vk_device, "vkQueueSubmit");
    vkQueuePresentKHR= (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(
        vk_device,
        "vkQueuePresentKHR");
    vkQueueWaitIdle=
        (PFN_vkQueueWaitIdle)vkGetDeviceProcAddr(vk_device, "vkQueueWaitIdle");
    vkCreateSemaphore= (PFN_vkCreateSemaphore)vkGetDeviceProcAddr(
        vk_device,
        "vkCreateSemaphore");
    vkDestroySemaphore= (PFN_vkDestroySemaphore)vkGetDeviceProcAddr(
        vk_device,
        "vkDestroySemaphore");
    vkCreateImageView= (PFN_vkCreateImageView)vkGetDeviceProcAddr(
        vk_device,
        "vkCreateImageView");
    vkDestroyImageView= (PFN_vkDestroyImageView)vkGetDeviceProcAddr(
        vk_device,
        "vkDestroyImageView");
    vkCreateSwapchainKHR= (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(
        vk_device,
        "vkCreateSwapchainKHR");
    vkDestroySwapchainKHR= (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(
        vk_device,
        "vkDestroySwapchainKHR");
    vkCreateBuffer=
        (PFN_vkCreateBuffer)vkGetDeviceProcAddr(vk_device, "vkCreateBuffer");
    vkGetBufferMemoryRequirements= (PFN_vkGetBufferMemoryRequirements)
        vkGetDeviceProcAddr(vk_device, "vkGetBufferMemoryRequirements");
    vkBindBufferMemory= (PFN_vkBindBufferMemory)vkGetDeviceProcAddr(
        vk_device,
        "vkBindBufferMemory");
    vkDestroyBuffer=
        (PFN_vkDestroyBuffer)vkGetDeviceProcAddr(vk_device, "vkDestroyBuffer");
    vkCreateImage=
        (PFN_vkCreateImage)vkGetDeviceProcAddr(vk_device, "vkCreateImage");
    vkGetImageMemoryRequirements= (PFN_vkGetImageMemoryRequirements)
        vkGetDeviceProcAddr(vk_device, "vkGetImageMemoryRequirements");
    vkBindImageMemory= (PFN_vkBindImageMemory)vkGetDeviceProcAddr(
        vk_device,
        "vkBindImageMemory");
    vkDestroyImage=
        (PFN_vkDestroyImage)vkGetDeviceProcAddr(vk_device, "vkDestroyImage");
    vkCreateShaderModule= (PFN_vkCreateShaderModule)vkGetDeviceProcAddr(
        vk_device,
        "vkCreateShaderModule");
    vkDestroyShaderModule= (PFN_vkDestroyShaderModule)vkGetDeviceProcAddr(
        vk_device,
        "vkDestroyShaderModule");
    vkCreatePipelineLayout= (PFN_vkCreatePipelineLayout)vkGetDeviceProcAddr(
        vk_device,
        "vkCreatePipelineLayout");
    vkDestroyPipelineLayout= (PFN_vkDestroyPipelineLayout)vkGetDeviceProcAddr(
        vk_device,
        "vkDestroyPipelineLayout");
    vkCreateGraphicsPipelines= (PFN_vkCreateGraphicsPipelines)
        vkGetDeviceProcAddr(vk_device, "vkCreateGraphicsPipelines");
    vkDestroyPipeline= (PFN_vkDestroyPipeline)vkGetDeviceProcAddr(
        vk_device,
        "vkDestroyPipeline");
    vkGetSwapchainImagesKHR= (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(
        vk_device,
        "vkGetSwapchainImagesKHR");
    vkAcquireNextImageKHR= (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(
        vk_device,
        "vkAcquireNextImageKHR");
    vkCreateCommandPool= (PFN_vkCreateCommandPool)vkGetDeviceProcAddr(
        vk_device,
        "vkCreateCommandPool");
    vkDestroyCommandPool= (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(
        vk_device,
        "vkDestroyCommandPool");
    vkResetCommandPool= (PFN_vkResetCommandPool)vkGetDeviceProcAddr(
        vk_device,
        "vkResetCommandPool");
    vkAllocateCommandBuffers= (PFN_vkAllocateCommandBuffers)vkGetDeviceProcAddr(
        vk_device,
        "vkAllocateCommandBuffers");
    vkFreeCommandBuffers= (PFN_vkFreeCommandBuffers)vkGetDeviceProcAddr(
        vk_device,
        "vkFreeCommandBuffers");
    vkBeginCommandBuffer= (PFN_vkBeginCommandBuffer)vkGetDeviceProcAddr(
        vk_device,
        "vkBeginCommandBuffer");
    vkEndCommandBuffer= (PFN_vkEndCommandBuffer)vkGetDeviceProcAddr(
        vk_device,
        "vkEndCommandBuffer");
    vkCmdPipelineBarrier= (PFN_vkCmdPipelineBarrier)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdPipelineBarrier");
    vkCmdCopyBuffer=
        (PFN_vkCmdCopyBuffer)vkGetDeviceProcAddr(vk_device, "vkCmdCopyBuffer");
    vkCmdBindPipeline= (PFN_vkCmdBindPipeline)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdBindPipeline");
    vkCmdPushConstants= (PFN_vkCmdPushConstants)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdPushConstants");
    vkCmdBindVertexBuffers= (PFN_vkCmdBindVertexBuffers)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdBindVertexBuffers");
    vkCmdBindIndexBuffer= (PFN_vkCmdBindIndexBuffer)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdBindIndexBuffer");
    vkCmdBeginRenderingKHR= (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdBeginRenderingKHR");
    vkCmdEndRenderingKHR= (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdEndRenderingKHR");
    vkCmdSetViewport= (PFN_vkCmdSetViewport)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdSetViewport");
    vkCmdSetScissor=
        (PFN_vkCmdSetScissor)vkGetDeviceProcAddr(vk_device, "vkCmdSetScissor");
    vkCmdDrawIndexed= (PFN_vkCmdDrawIndexed)vkGetDeviceProcAddr(
        vk_device,
        "vkCmdDrawIndexed");
    DWORD queue_index= 0;
    vkGetDeviceQueue(
        vk_device,
        graphics_queue_family_index,
        queue_index,
        &vk_gfx_queue);
    if(transfer_queue_family_index == graphics_queue_family_index)
        ++queue_index;
    vkGetDeviceQueue(
        vk_device,
        transfer_queue_family_index,
        queue_index,
        &vk_cpy_queue);
    queue_index= 0;
    if(present_queue_family_index == graphics_queue_family_index) ++queue_index;
    if(present_queue_family_index == transfer_queue_family_index) ++queue_index;
    vkGetDeviceQueue(
        vk_device,
        present_queue_family_index,
        queue_index,
        &vk_wsi_queue);
}

VkSurfaceCapabilitiesKHR surface_capabilities= {0};
VkSurfaceFormatKHR       surface_format      = {0};
VkSwapchainKHR           vk_swapchain;

static void
vulkan_create_swapchain() {
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk_physical_device,
        vk_surface,
        &surface_capabilities);
    DWORD surface_format_count= 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        vk_physical_device,
        vk_surface,
        &surface_format_count,
        NULL);
    VkSurfaceFormatKHR *surface_formats= HeapAlloc(
        process_heap,
        HEAP_ZERO_MEMORY,
        sizeof(VkSurfaceFormatKHR) * surface_format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        vk_physical_device,
        vk_surface,
        &surface_format_count,
        surface_formats);
    for(DWORD i= 0; i < surface_format_count; ++i) {
        if(surface_formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            continue;
        if(surface_formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
           surface_formats[i].format == VK_FORMAT_R8G8B8A8_SRGB)
            surface_format= surface_formats[i];
    }
    HeapFree(process_heap, 0, surface_formats);
    DWORD surface_present_mode_count= 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        vk_physical_device,
        vk_surface,
        &surface_present_mode_count,
        NULL);
    VkPresentModeKHR *surface_present_modes= HeapAlloc(
        process_heap,
        HEAP_ZERO_MEMORY,
        sizeof(VkPresentModeKHR) * surface_present_mode_count);
    VkPresentModeKHR surface_present_mode= VK_PRESENT_MODE_MAX_ENUM_KHR;
    for(DWORD i= 0; i < surface_present_mode_count; ++i) {
        if(surface_present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            surface_present_mode= VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        }
    }
    if(surface_present_mode == VK_PRESENT_MODE_MAX_ENUM_KHR)
        surface_present_mode= surface_present_modes[0];
    HeapFree(process_heap, 0, surface_present_modes);
    VkSwapchainCreateInfoKHR create_info= {0};
    create_info.sType           = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface         = vk_surface;
    create_info.minImageCount   = surface_capabilities.minImageCount;
    create_info.imageUsage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.imageExtent     = surface_capabilities.currentExtent;
    create_info.imageArrayLayers= 1;
    create_info.imageFormat     = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.preTransform    = surface_capabilities.currentTransform;
    if((surface_capabilities.supportedCompositeAlpha &
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ==
       VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        create_info.compositeAlpha= VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if(
        (surface_capabilities.supportedCompositeAlpha &
         VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ==
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        create_info.compositeAlpha= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }
    create_info.presentMode= surface_present_mode;
    VkResult res=
        vkCreateSwapchainKHR(vk_device, &create_info, NULL, &vk_swapchain);
}

struct {
    VkShaderModule   vertex_shader;
    VkShaderModule   fragment_shader;
    VkPipelineLayout pipeline_layout;
    VkPipeline       pipeline;
} vk_pipeline;

static void
vulkan_create_pipeline() {
    {
        HANDLE file_handle= CreateFile(
            L"shaders/vert.spv",
            FILE_READ_ATTRIBUTES | FILE_READ_DATA,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            NULL);
        LARGE_INTEGER file_size= {0};
        GetFileSizeEx(file_handle, &file_size);
        void *shader_bytecode=
            HeapAlloc(process_heap, HEAP_ZERO_MEMORY, file_size.QuadPart);
        DWORD      bytes_read= 0u;
        OVERLAPPED overlapped= {0};
        overlapped.OffsetHigh= 0u;
        overlapped.Offset    = 0u;
        ReadFile(
            file_handle,
            shader_bytecode,
            file_size.QuadPart,
            &bytes_read,
            &overlapped);
        VkShaderModuleCreateInfo create_info= {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            NULL,
            0,
            file_size.QuadPart,
            (u32 *)shader_bytecode};
        vkCreateShaderModule(
            vk_device,
            &create_info,
            NULL,
            &vk_pipeline.vertex_shader);
        HeapFree(process_heap, 0, shader_bytecode);
    }
    {
        HANDLE file_handle= CreateFile(
            L"shaders/frag.spv",
            FILE_READ_ATTRIBUTES | FILE_READ_DATA,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            NULL);
        LARGE_INTEGER file_size= {0};
        GetFileSizeEx(file_handle, &file_size);
        void *shader_bytecode=
            HeapAlloc(process_heap, HEAP_ZERO_MEMORY, file_size.QuadPart);
        DWORD      bytes_read= 0u;
        OVERLAPPED overlapped= {0};
        overlapped.OffsetHigh= 0u;
        overlapped.Offset    = 0u;
        ReadFile(
            file_handle,
            shader_bytecode,
            file_size.QuadPart,
            &bytes_read,
            &overlapped);
        VkShaderModuleCreateInfo create_info= {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            NULL,
            0,
            file_size.QuadPart,
            (u32 *)shader_bytecode};
        vkCreateShaderModule(
            vk_device,
            &create_info,
            NULL,
            &vk_pipeline.fragment_shader);
        HeapFree(process_heap, 0, shader_bytecode);
    }
    {
        VkPushConstantRange push_constant_ranges[1]= {
            {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4x4) * 2}};
        VkPipelineLayoutCreateInfo create_info= {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            NULL,
            0,
            0,
            NULL,
            1,
            push_constant_ranges};
        vkCreatePipelineLayout(
            vk_device,
            &create_info,
            NULL,
            &vk_pipeline.pipeline_layout);
    }
    {
        VkPipelineShaderStageCreateInfo shader_stage_infos[2]= {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             NULL,
             0,
             VK_SHADER_STAGE_VERTEX_BIT,
             vk_pipeline.vertex_shader,
             "vert_main",
             NULL},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             NULL,
             0,
             VK_SHADER_STAGE_FRAGMENT_BIT,
             vk_pipeline.fragment_shader,
             "frag_main",
             NULL},
        };
        VkVertexInputBindingDescription binding_descs[1]= {
            {0, sizeof(float) * 8, VK_VERTEX_INPUT_RATE_VERTEX}};
        VkVertexInputAttributeDescription attribute_descs[2]= {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 4},
        };
        VkPipelineVertexInputStateCreateInfo vertex_input_state= {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            NULL,
            0,
            1,
            binding_descs,
            2,
            attribute_descs};
        VkPipelineInputAssemblyStateCreateInfo input_assembly_state= {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            NULL,
            0,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_FALSE};
        VkPipelineViewportStateCreateInfo viewport_state= {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            NULL,
            0,
            1,
            NULL,
            1,
            NULL};
        VkPipelineRasterizationStateCreateInfo rasterization_state= {
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            NULL,
            0,
            VK_FALSE,
            VK_FALSE,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_BACK_BIT,
            VK_FRONT_FACE_COUNTER_CLOCKWISE,
            VK_FALSE,
            0.0F,
            0.0F,
            0.0F,
            1.0F};
        VkPipelineMultisampleStateCreateInfo multisample_state= {
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            NULL,
            0,
            VK_SAMPLE_COUNT_1_BIT,
            VK_FALSE,
            0.0F,
            NULL,
            VK_FALSE,
            VK_FALSE};
        VkPipelineDepthStencilStateCreateInfo depth_stencil_state= {
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            NULL,
            0,
            VK_TRUE,
            VK_TRUE,
            VK_COMPARE_OP_GREATER_OR_EQUAL,
            VK_FALSE,
            VK_FALSE,
            (VkStencilOpState){0},
            (VkStencilOpState){0},
            0.0F,
            1.0F};
        VkPipelineColorBlendAttachmentState color_blend_attachments[1]= {
            {VK_FALSE,
             VK_BLEND_FACTOR_ONE,
             VK_BLEND_FACTOR_ZERO,
             VK_BLEND_OP_ADD,
             VK_BLEND_FACTOR_ONE,
             VK_BLEND_FACTOR_ZERO,
             VK_BLEND_OP_ADD,
             0x0F}};
        VkPipelineColorBlendStateCreateInfo color_blend_state= {
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            NULL,
            0,
            VK_FALSE,
            VK_LOGIC_OP_CLEAR,
            1,
            color_blend_attachments,
            {0.0F}};
        VkDynamicState dynamic_states[2]= {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic_state= {
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            NULL,
            0,
            2,
            dynamic_states};
        VkPipelineRenderingCreateInfo rendering_info= {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            NULL,
            0,
            1,
            &surface_format.format,
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_UNDEFINED};
        VkGraphicsPipelineCreateInfo create_info= {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            &rendering_info,
            0,
            2,
            shader_stage_infos,
            &vertex_input_state,
            &input_assembly_state,
            NULL,
            &viewport_state,
            &rasterization_state,
            &multisample_state,
            &depth_stencil_state,
            &color_blend_state,
            &dynamic_state,
            vk_pipeline.pipeline_layout,
            VK_NULL_HANDLE,
            0,
            VK_NULL_HANDLE,
            -1};
        vkCreateGraphicsPipelines(
            vk_device,
            VK_NULL_HANDLE,
            1,
            &create_info,
            NULL,
            &vk_pipeline.pipeline);
    }
}

DWORD        vk_swapchain_image_count;
VkImage     *vk_swapchain_images;
VkImageView *vk_swapchain_image_views;

static void
vulkan_create_swapchain_attachments() {
    vkGetSwapchainImagesKHR(
        vk_device,
        vk_swapchain,
        &vk_swapchain_image_count,
        NULL);
    vk_swapchain_images= HeapAlloc(
        process_heap,
        HEAP_ZERO_MEMORY,
        sizeof(VkImage) * vk_swapchain_image_count);
    vkGetSwapchainImagesKHR(
        vk_device,
        vk_swapchain,
        &vk_swapchain_image_count,
        vk_swapchain_images);
    vk_swapchain_image_views= HeapAlloc(
        process_heap,
        HEAP_ZERO_MEMORY,
        sizeof(VkImageView) * vk_swapchain_image_count);
    VkImageViewCreateInfo create_info= {0};
    create_info.sType                = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    create_info.viewType             = VK_IMAGE_VIEW_TYPE_2D;
    if(surface_format.format == VK_FORMAT_R8G8B8A8_SRGB)
        create_info.format= VK_FORMAT_R8G8B8A8_SRGB;
    else if(surface_format.format == VK_FORMAT_B8G8R8A8_SRGB)
        create_info.format= VK_FORMAT_B8G8R8A8_SRGB;
    create_info.subresourceRange.aspectMask    = VK_IMAGE_ASPECT_COLOR_BIT;
    create_info.subresourceRange.baseArrayLayer= 0;
    create_info.subresourceRange.layerCount    = 1;
    create_info.subresourceRange.baseMipLevel  = 0;
    create_info.subresourceRange.levelCount    = 1;
    create_info.components.r                   = VK_COMPONENT_SWIZZLE_IDENTITY;
    create_info.components.g                   = VK_COMPONENT_SWIZZLE_IDENTITY;
    create_info.components.b                   = VK_COMPONENT_SWIZZLE_IDENTITY;
    create_info.components.a                   = VK_COMPONENT_SWIZZLE_IDENTITY;
    VkResult res;
    for(DWORD i= 0; i < vk_swapchain_image_count; ++i) {
        create_info.image= vk_swapchain_images[i];
        res              = vkCreateImageView(
            vk_device,
            &create_info,
            NULL,
            &vk_swapchain_image_views[i]);
    }
}

static void
vulkan_destroy_swapchain_attachments() {
    for(DWORD i= 0; i < vk_swapchain_image_count; ++i) {
        vkDestroyImageView(vk_device, vk_swapchain_image_views[i], NULL);
    }
    HeapFree(process_heap, 0, vk_swapchain_image_views);
    HeapFree(process_heap, 0, vk_swapchain_images);
}

VkCommandPool   vk_gfx_cmd_pool;
VkCommandBuffer vk_gfx_cmd_buffer;

static void
vulkan_create_command_context() {
    VkCommandPoolCreateInfo create_info= {0};
    create_info.sType           = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    create_info.queueFamilyIndex= graphics_queue_family_index;
    VkResult res=
        vkCreateCommandPool(vk_device, &create_info, NULL, &vk_gfx_cmd_pool);
    VkCommandBufferAllocateInfo alloc_info= {0};
    alloc_info.sType      = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool= vk_gfx_cmd_pool;
    alloc_info.level      = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount= 1;
    res= vkAllocateCommandBuffers(vk_device, &alloc_info, &vk_gfx_cmd_buffer);
}

VkImage        vk_depth_image;
VkImageView    vk_depth_image_view;
VkDeviceMemory vk_depth_memory;

static void
vulkan_create_depth_attachment() {
    {
        VkImageCreateInfo create_info= {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            NULL,
            0,
            VK_IMAGE_TYPE_2D,
            VK_FORMAT_D32_SFLOAT,
            {1280, 720, 1},
            1,
            1,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_SHARING_MODE_EXCLUSIVE,
            0,
            NULL,
            VK_IMAGE_LAYOUT_UNDEFINED};
        vkCreateImage(vk_device, &create_info, 0, &vk_depth_image);
    }
    {
        VkPhysicalDeviceMemoryProperties mem_props= {0};
        vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mem_props);

        VkMemoryRequirements mem_reqs= {0};
        vkGetImageMemoryRequirements(vk_device, vk_depth_image, &mem_reqs);

        u32 mem_type_mask= mem_reqs.memoryTypeBits;
        u32 mem_type_idx= ~(0u);
        for(u32 i= 0; i < 32; ++i) {
            if(mem_type_mask & 1 << i == 0) continue;
            VkMemoryType *type= &mem_props.memoryTypes[i];
            VkMemoryHeap *heap= &mem_props.memoryHeaps[type->heapIndex];
            if((type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
               // On certain gpus, a smaller heap is present to allow
               // fast CPU to GPU updates that we don't want to use for
               // Vertex/Index Buffer memory
               heap->size > 256 * 1024 * 1024) {
                mem_type_idx= i;
                break;
            }
        }
        assert(mem_type_idx != ~(0u));

        VkMemoryAllocateInfo alloc_info= {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            NULL,
            mem_reqs.size,
            mem_type_idx};

        vkAllocateMemory(vk_device, &alloc_info, NULL, &vk_depth_memory);
        vkBindImageMemory(vk_device, vk_depth_image, vk_depth_memory, 0);
    }
    {
        VkImageViewCreateInfo create_info= {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            NULL,
            0,
            vk_depth_image,
            VK_IMAGE_VIEW_TYPE_2D,
            VK_FORMAT_D32_SFLOAT,
            {VK_COMPONENT_SWIZZLE_IDENTITY},
            {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        vkCreateImageView(vk_device, &create_info, NULL, &vk_depth_image_view);
    }
    {
        VkCommandBufferBeginInfo begin_info= {0};
        begin_info.sType= VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags= VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkResult res    = vkBeginCommandBuffer(vk_gfx_cmd_buffer, &begin_info);
        VkImageMemoryBarrier image_barrier= {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            NULL,
            0,
            0,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            vk_depth_image,
            {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(
            vk_gfx_cmd_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            0,
            0,
            NULL,
            0,
            NULL,
            1,
            &image_barrier);
        /*------------------------------------------------------------------------*/
        res= vkEndCommandBuffer(vk_gfx_cmd_buffer);
        /*========================================================================*/
        /* Submit Command Buffer For Execution */
        /*========================================================================*/
        VkSubmitInfo submit_info      = {0};
        submit_info.sType             = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount= 1;
        submit_info.pCommandBuffers   = &vk_gfx_cmd_buffer;
        submit_info.waitSemaphoreCount= 0;
        submit_info.pWaitSemaphores   = NULL;
        submit_info.pWaitDstStageMask = NULL;
        vkQueueSubmit(vk_gfx_queue, 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk_gfx_queue);
    }
}

VkSemaphore vk_acquire_semaphore;

static void
vulkan_create_semaphores() {
    VkSemaphoreCreateInfo create_info= {0};
    create_info.sType                = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkResult res=
        vkCreateSemaphore(vk_device, &create_info, NULL, &vk_acquire_semaphore);
}

static VkBuffer vk_vertex_buffer;

static void
vulkan_create_vertex_buffer(u64 buffer_size) {
    VkBufferCreateInfo create_info= {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        NULL,
        0,
        buffer_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        NULL};
    vkCreateBuffer(vk_device, &create_info, NULL, &vk_vertex_buffer);
}

static VkBuffer vk_index_buffer;

static void
vulkan_create_index_buffer(u64 buffer_size) {
    VkBufferCreateInfo create_info= {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        NULL,
        0,
        buffer_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        NULL};
    vkCreateBuffer(vk_device, &create_info, NULL, &vk_index_buffer);
}

static VkDeviceMemory vk_buffer_memory;

static vulkan_allocate_buffer_memory() {
    VkPhysicalDeviceMemoryProperties mem_props= {0};
    vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mem_props);

    VkMemoryRequirements vertex_buffer_memreqs= {0};
    vkGetBufferMemoryRequirements(
        vk_device,
        vk_vertex_buffer,
        &vertex_buffer_memreqs);
    VkMemoryRequirements index_buffer_memreqs= {0};
    vkGetBufferMemoryRequirements(
        vk_device,
        vk_index_buffer,
        &index_buffer_memreqs);

    u32 mem_type_mask= vertex_buffer_memreqs.memoryTypeBits &
                       index_buffer_memreqs.memoryTypeBits;
    u32 mem_type_idx= ~(0u);
    for(u32 i= 0; i < 32; ++i) {
        if(mem_type_mask & 1 << i == 0) continue;
        VkMemoryType *type= &mem_props.memoryTypes[i];
        VkMemoryHeap *heap= &mem_props.memoryHeaps[type->heapIndex];
        if((type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
           // On certain gpus, a smaller heap is present to allow
           // fast CPU to GPU updates that we don't want to use for
           // Vertex/Index Buffer memory
           heap->size > 256 * 1024 * 1024) {
            mem_type_idx= i;
            break;
        }
    }
    assert(mem_type_idx != ~(0u));

    VkMemoryAllocateInfo alloc_info= {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        NULL,
        vertex_buffer_memreqs.size + index_buffer_memreqs.size,
        mem_type_idx};

    vkAllocateMemory(vk_device, &alloc_info, NULL, &vk_buffer_memory);
    vkBindBufferMemory(vk_device, vk_vertex_buffer, vk_buffer_memory, 0);
    vkBindBufferMemory(
        vk_device,
        vk_index_buffer,
        vk_buffer_memory,
        vertex_buffer_memreqs.size);
}

static void
vulkan_create_staging_buffer(
    u64             vertex_buffer_size,
    u64             index_buffer_size,
    VkBuffer       *staging_buffer,
    VkDeviceMemory *staging_memory) {
    VkBufferCreateInfo create_info= {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        NULL,
        0,
        vertex_buffer_size + index_buffer_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        NULL};
    vkCreateBuffer(vk_device, &create_info, NULL, staging_buffer);

    VkPhysicalDeviceMemoryProperties mem_props= {0};
    vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mem_props);

    VkMemoryRequirements mem_reqs= {0};
    vkGetBufferMemoryRequirements(vk_device, *staging_buffer, &mem_reqs);

    u32 mem_type_mask= mem_reqs.memoryTypeBits;
    u32 mem_type_idx = ~(0u);
    for(u32 i= 0; i < 32; ++i) {
        if(mem_type_mask & 1 << i == 0) continue;
        VkMemoryType *type= &mem_props.memoryTypes[i];
        VkMemoryHeap *heap= &mem_props.memoryHeaps[type->heapIndex];
        if((type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0 &&
           (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
           // On certain gpus, a smaller heap is present to allow
           // fast CPU to GPU updates that we don't want to use for
           // Vertex/Index Buffer memory
           heap->size > 256 * 1024 * 1024) {
            mem_type_idx= i;
            break;
        }
    }
    assert(mem_type_idx != ~(0u));

    VkMemoryAllocateInfo alloc_info= {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        NULL,
        mem_reqs.size,
        mem_type_idx};

    vkAllocateMemory(vk_device, &alloc_info, NULL, staging_memory);
    vkBindBufferMemory(vk_device, *staging_buffer, *staging_memory, 0);
}

static void
vulkan_copy_mesh_data_to_gpu(
    VkBuffer staging_buffer,
    u64      vertex_buffer_size,
    u64      index_buffer_size) {
    /*========================================================================*/
    /* Record Command Buffer With Upload Commands                             */
    /*========================================================================*/
    VkCommandBufferBeginInfo begin_info= {0};
    begin_info.sType= VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags= VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkResult res    = vkBeginCommandBuffer(vk_gfx_cmd_buffer, &begin_info);
    /*------------------------------------------------------------------------*/
    VkBufferCopy buffer_copy= {0, 0, vertex_buffer_size};
    vkCmdCopyBuffer(
        vk_gfx_cmd_buffer,
        staging_buffer,
        vk_vertex_buffer,
        1,
        &buffer_copy);
    buffer_copy.srcOffset= vertex_buffer_size;
    buffer_copy.size     = index_buffer_size;
    vkCmdCopyBuffer(
        vk_gfx_cmd_buffer,
        staging_buffer,
        vk_index_buffer,
        1,
        &buffer_copy);
    /*------------------------------------------------------------------------*/
    res= vkEndCommandBuffer(vk_gfx_cmd_buffer);
    /*========================================================================*/
    /* Submit Command Buffer For Execution                                    */
    /*========================================================================*/
    VkSubmitInfo submit_info      = {0};
    submit_info.sType             = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount= 1;
    submit_info.pCommandBuffers   = &vk_gfx_cmd_buffer;
    submit_info.waitSemaphoreCount= 0;
    submit_info.pWaitSemaphores   = NULL;
    submit_info.pWaitDstStageMask = NULL;
    vkQueueSubmit(vk_gfx_queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_gfx_queue);
}

typedef struct mesh_primitive_t {
    u32 vertex_count;
    u32 vertex_offset;
    u32 index_count;
    u32 index_offset;
} mesh_primitive_t;

static void
vulkan_render_frame(u32 primitive_count, mesh_primitive_t *primitive_list) {
    vkResetCommandPool(vk_device, vk_gfx_cmd_pool, 0);
    DWORD index= 0;
    vkAcquireNextImageKHR(
        vk_device,
        vk_swapchain,
        UINT64_MAX,
        vk_acquire_semaphore,
        VK_NULL_HANDLE,
        &index);
    /*========================================================================*/
    /* Record Command Buffer With Rendering Commands                          */
    /*========================================================================*/
    VkCommandBufferBeginInfo begin_info= {0};
    begin_info.sType= VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags= VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkResult res    = vkBeginCommandBuffer(vk_gfx_cmd_buffer, &begin_info);
    /*------------------------------------------------------------------------*/
    /* Color Attachment                                                       */
    /*------------------------------------------------------------------------*/
    VkRenderingAttachmentInfoKHR color_attachment_info= {0};
    color_attachment_info.sType=
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    color_attachment_info.imageView  = vk_swapchain_image_views[index];
    color_attachment_info.imageLayout= VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment_info.loadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment_info.storeOp    = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment_info.clearValue.color.float32[0]= 0.39F;
    color_attachment_info.clearValue.color.float32[1]= 0.58F;
    color_attachment_info.clearValue.color.float32[2]= 0.93F;
    color_attachment_info.clearValue.color.float32[3]= 1.F;
    /*------------------------------------------------------------------------*/
    /* Depth Attachment                                                       */
    /*------------------------------------------------------------------------*/
    VkRenderingAttachmentInfoKHR depth_attachment_info= {0};
    depth_attachment_info.sType=
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    depth_attachment_info.imageView= vk_depth_image_view;
    depth_attachment_info.imageLayout=
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment_info.loadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment_info.storeOp    = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment_info.clearValue.depthStencil.depth  = 0.F;
    depth_attachment_info.clearValue.depthStencil.stencil= 0;
    VkRenderingInfoKHR render_info                   = {0};
    render_info.sType               = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    render_info.renderArea.extent   = surface_capabilities.currentExtent;
    render_info.layerCount          = 1;
    render_info.colorAttachmentCount= 1;
    render_info.pColorAttachments   = &color_attachment_info;
    render_info.pDepthAttachment    = &depth_attachment_info;
    vkCmdBeginRenderingKHR(vk_gfx_cmd_buffer, &render_info);
    /*------------------------------------------------------------------------*/
    vkCmdBindPipeline(
        vk_gfx_cmd_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        vk_pipeline.pipeline);
    VkDeviceSize offset= 0;
    vkCmdBindVertexBuffers(vk_gfx_cmd_buffer, 0, 1, &vk_vertex_buffer, &offset);
    vkCmdBindIndexBuffer(
        vk_gfx_cmd_buffer,
        vk_index_buffer,
        offset,
        VK_INDEX_TYPE_UINT16);
    VkViewport viewport= {0, 720, 1280, -720, 0, 1};
    vkCmdSetViewport(vk_gfx_cmd_buffer, 0, 1, &viewport);
    VkRect2D scissor= {0, 0, 1280, 720};
    vkCmdSetScissor(vk_gfx_cmd_buffer, 0, 1, &scissor);
    mat4x4 view= {0};
    vec3   pos = vec3_make(0, 0.F, 0.F);
    vec3   eul = vec3_make(M_TO_RAD(0), 0, 0);
    mat4x4_make_view_matrix(&pos, &eul, &view);
    mat4x4 proj= {0};
    mat4x4_make_persp_proj_matrix(M_TO_RAD(45), 16.F / 9, 0.1F, 100.0F, &proj);
    mat4x4 view_proj= {0};
    mat4x4_mul(&view, &proj, &view_proj);
    mat4x4 world= {0};
    mat4x4_make_rot_matrix(M_TO_RAD(-90), M_TO_RAD(0), M_TO_RAD(0), &world);
    vec4_set(world.columns[3], 0.F, 0.F, -3.F, 1.F);
    vkCmdPushConstants(
        vk_gfx_cmd_buffer,
        vk_pipeline.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(mat4x4),
        &world);
    vkCmdPushConstants(
        vk_gfx_cmd_buffer,
        vk_pipeline.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        sizeof(mat4x4),
        sizeof(mat4x4),
        &view_proj);
    for(u32 i= 0; i < primitive_count; ++i) {
        mesh_primitive_t *primitive= &primitive_list[i];
        vkCmdDrawIndexed(
            vk_gfx_cmd_buffer,
            primitive->index_count,
            1,
            primitive->index_offset,
            primitive->vertex_offset,
            0);
    }
    /*------------------------------------------------------------------------*/
    vkCmdEndRenderingKHR(vk_gfx_cmd_buffer);
    VkImageMemoryBarrier image_barrier= {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        NULL,
        0,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        vk_swapchain_images[index],
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(
        vk_gfx_cmd_buffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &image_barrier);
    /*------------------------------------------------------------------------*/
    res                                = vkEndCommandBuffer(vk_gfx_cmd_buffer);
    /*========================================================================*/
    /* Submit Command Buffer For Execution                                    */
    /*========================================================================*/
    VkPipelineStageFlags wait_stages[1]= {
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    VkSubmitInfo submit_info      = {0};
    submit_info.sType             = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount= 1;
    submit_info.pCommandBuffers   = &vk_gfx_cmd_buffer;
    submit_info.waitSemaphoreCount= 1;
    submit_info.pWaitSemaphores   = &vk_acquire_semaphore;
    submit_info.pWaitDstStageMask = wait_stages;
    vkQueueSubmit(vk_gfx_queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_gfx_queue);
    VkPresentInfoKHR present_info= {0};
    present_info.sType           = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.swapchainCount  = 1;
    present_info.pSwapchains     = &vk_swapchain;
    present_info.pImageIndices   = &index;
    vkQueuePresentKHR(vk_wsi_queue, &present_info);
}

int _fltused= 0;

void
main() {
    int     argc;
    LPWSTR *argv;
    argv= CommandLineToArgvW(GetCommandLineW(), &argc);
    if(argc < 2) ExitProcess(-1);
    process_heap= GetProcessHeap();
    /*========================================================================*/
    /* Open GLB File                  */
    /*========================================================================*/
    HANDLE file_handle= CreateFile(
        argv[1],
        FILE_READ_ATTRIBUTES | FILE_READ_DATA,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);
    if(file_handle == INVALID_HANDLE_VALUE) {
        DWORD err= GetLastError();
        ExitProcess(-1);
    }
    DWORD      bytes_read= 0u;
    glb_header header    = {0};
    OVERLAPPED overlapped= {0};
    overlapped.OffsetHigh= 0u;
    overlapped.Offset    = 0u;
    ReadFile(
        file_handle,
        &header,
        sizeof(glb_header),
        &bytes_read,
        &overlapped);
    if(header.magic != 0x46546C67u) {
        CloseHandle(file_handle);
        ExitProcess(-1);
    }
    DWORD json_chunk_offset    = sizeof(glb_header);
    overlapped.Offset          = json_chunk_offset;
    glb_chunk_header json_chunk= {0};
    ReadFile(
        file_handle,
        &json_chunk,
        sizeof(glb_chunk_header),
        &bytes_read,
        &overlapped);
    if(json_chunk.type != glb_chunk_json) {
        CloseHandle(file_handle);
        ExitProcess(-1);
    }
    DWORD bin_chunk_offset=
        json_chunk_offset + sizeof(glb_chunk_header) + json_chunk.length;
    overlapped.Offset         = bin_chunk_offset;
    glb_chunk_header bin_chunk= {0};
    ReadFile(
        file_handle,
        &bin_chunk,
        sizeof(glb_chunk_header),
        &bytes_read,
        &overlapped);
    if(bin_chunk.type != glb_chunk_bin) {
        CloseHandle(file_handle);
        ExitProcess(-1);
    }
    overlapped.Offset= json_chunk_offset + sizeof(glb_chunk_header);
    void *json_chunk_data=
        HeapAlloc(process_heap, HEAP_ZERO_MEMORY, json_chunk.length);
    ReadFile(
        file_handle,
        json_chunk_data,
        json_chunk.length,
        &bytes_read,
        &overlapped);
    gltf_json_data gltf_json= {0};
    gltf_parse_json(
        &gltf_json,
        json_chunk_data,
        json_chunk.length,
        &g_allocator);
    HeapFree(process_heap, 0, json_chunk_data);
    /*========================================================================*/
    /* Vulkan Initialization                                                  */
    /*========================================================================*/
    vulkan_load_library();
    vulkan_create_instance();
    vulkan_create_debug_messenger();
    vulkan_select_physical_device();
    vulkan_create_device();
    vulkan_create_command_context();
    win32_create_window();
    vulkan_create_surface();
    vulkan_create_swapchain();
    vulkan_create_swapchain_attachments();
    vulkan_create_depth_attachment();
    vulkan_create_semaphores();
    vulkan_create_pipeline();
    /*------------------------------------------------------------------------*/
    /* Buffer Creation                                                        */
    /*------------------------------------------------------------------------*/
    u32               mesh_prim_count= gltf_json.mesh.primitive_count;
    mesh_primitive_t *mesh_prim_list = HeapAlloc(
        process_heap,
        HEAP_ZERO_MEMORY,
        sizeof(mesh_primitive_t) * mesh_prim_count);
    u64 vertex_buffer_size= 0, index_buffer_size= 0;
    u32 vertex_count= 0, vertex_offset= 0, index_count= 0, index_offset= 0;
    for(u32 i= 0; i < gltf_json.mesh.primitive_count; ++i) {
        gltf_mesh_primitive *gltf_primitive= &gltf_json.mesh.primitive_list[i];
        /*--------------------------------------------------------------------*/
        /* POSITION Attribute                                                 */
        /*--------------------------------------------------------------------*/
        gltf_accessor *pos_accessor=
            &gltf_json.accessor_list[gltf_primitive->pos_accessor];
        /*--------------------------------------------------------------------*/
        /* NORMAL Attribute                                                   */
        /*--------------------------------------------------------------------*/
        gltf_accessor *nrm_accessor=
            &gltf_json.accessor_list[gltf_primitive->nrm_accessor];
        vertex_buffer_size+= sizeof(vertex) * pos_accessor->count;
        /*--------------------------------------------------------------------*/
        /* INDEX                                                              */
        /*--------------------------------------------------------------------*/
        gltf_accessor *idx_accessor=
            &gltf_json.accessor_list[gltf_primitive->idx_accessor];
        index_buffer_size+= sizeof(u16) * idx_accessor->count;
        /*--------------------------------------------------------------------*/
        /* Fill the Mesh Primitive Struct                                     */
        /*--------------------------------------------------------------------*/
        mesh_primitive_t *primitive= &mesh_prim_list[i];
        primitive->vertex_count    = pos_accessor->count;
        primitive->index_count     = idx_accessor->count;
        primitive->vertex_offset   = vertex_offset;
        primitive->index_offset    = index_offset;
        /*--------------------------------------------------------------------*/
        vertex_offset= pos_accessor->count;
        index_offset = idx_accessor->count;
    }
    vulkan_create_vertex_buffer(vertex_buffer_size);
    vulkan_create_index_buffer(index_buffer_size);
    vulkan_allocate_buffer_memory();
    VkBuffer       staging_buffer;
    VkDeviceMemory staging_memory;
    vulkan_create_staging_buffer(
        vertex_buffer_size,
        index_buffer_size,
        &staging_buffer,
        &staging_memory);
    /*========================================================================*/
    /* GLTF Binary Data                                                       */
    /*========================================================================*/
    overlapped.Offset= bin_chunk_offset + sizeof(glb_chunk_header);
    void *bin_chunk_data=
        HeapAlloc(process_heap, HEAP_ZERO_MEMORY, bin_chunk.length);
    ReadFile(
        file_handle,
        bin_chunk_data,
        bin_chunk.length,
        &bytes_read,
        &overlapped);
    /*========================================================================*/
    /* Copy Data to GPU                                                       */
    /*========================================================================*/
    /* Copy Data from Binary Chunk to Staging Buffer                          */
    /*------------------------------------------------------------------------*/
    vertex *vertices;
    vkMapMemory(
        vk_device,
        staging_memory,
        0,
        vertex_buffer_size,
        0,
        (void **)&vertices);
    for(u32 i= 0; i < gltf_json.mesh.primitive_count; ++i) {
        gltf_mesh_primitive *gltf_primitive= &gltf_json.mesh.primitive_list[i];
        /*--------------------------------------------------------------------*/
        /* POSITION Attribute                                                 */
        /*--------------------------------------------------------------------*/
        gltf_accessor *pos_accessor=
            &gltf_json.accessor_list[gltf_primitive->pos_accessor];
        gltf_buffer_view *pos_buffer_view=
            &gltf_json.buffer_view_list[pos_accessor->buffer_view];
#define at_offset(addr, offset) ((void *)((u8 *)addr + offset))
        vec3 *pos_data=
            (vec3 *)at_offset(bin_chunk_data, pos_buffer_view->byte_offset);
        pos_data= (vec3 *)at_offset(pos_data, pos_accessor->byte_offset);
        /*--------------------------------------------------------------------*/
        /* NORMAL Attribute                                                   */
        /*--------------------------------------------------------------------*/
        gltf_accessor *nrm_accessor=
            &gltf_json.accessor_list[gltf_primitive->nrm_accessor];
        gltf_buffer_view *nrm_buffer_view=
            &gltf_json.buffer_view_list[nrm_accessor->buffer_view];
        vec3 *nrm_data=
            (vec3 *)at_offset(bin_chunk_data, nrm_buffer_view->byte_offset);
        nrm_data= (vec3 *)at_offset(nrm_data, nrm_accessor->byte_offset);
        for(u32 i= 0; i < pos_accessor->count; ++i) {
            vertices[i].pos.x= pos_data[i].x;
            vertices[i].pos.y= pos_data[i].y;
            vertices[i].pos.z= pos_data[i].z;
            vertices[i].pos.w= 1.0F;
            vertices[i].nrm.x= nrm_data[i].x;
            vertices[i].nrm.y= nrm_data[i].y;
            vertices[i].nrm.z= nrm_data[i].z;
            vertices[i].nrm.w= 0.F;
        }
    }
    vkUnmapMemory(vk_device, staging_memory);
    u16 *indices;
    vkMapMemory(
        vk_device,
        staging_memory,
        vertex_buffer_size,
        index_buffer_size,
        0,
        (void **)&indices);
    for(u32 i= 0; i < gltf_json.mesh.primitive_count; ++i) {
        gltf_mesh_primitive *gltf_primitive= &gltf_json.mesh.primitive_list[i];
        /*--------------------------------------------------------------------*/
        /* INDEX                                                              */
        /*--------------------------------------------------------------------*/
        gltf_accessor *idx_accessor=
            &gltf_json.accessor_list[gltf_primitive->idx_accessor];
        gltf_buffer_view *idx_buffer_view=
            &gltf_json.buffer_view_list[idx_accessor->buffer_view];
        u16 *idx_data=
            (u16 *)at_offset(bin_chunk_data, idx_buffer_view->byte_offset);
        for(u32 i= 0; i < idx_accessor->count; ++i) { *indices++= idx_data[i]; }
#undef at_offset
    }
    vkUnmapMemory(vk_device, staging_memory);
    /*------------------------------------------------------------------------*/
    /* Copy Data from Staging Buffer to Vertex Buffer and Index Buffer        */
    /*------------------------------------------------------------------------*/
    vulkan_copy_mesh_data_to_gpu(
        staging_buffer,
        vertex_buffer_size,
        index_buffer_size);
    /*------------------------------------------------------------------------*/
    vkDestroyBuffer(vk_device, staging_buffer, NULL);
    vkFreeMemory(vk_device, staging_memory, NULL);
    HeapFree(process_heap, 0, bin_chunk_data);
    CloseHandle(file_handle);
    /*========================================================================*/
    /* Main Loop                                                              */
    /*========================================================================*/
    running= TRUE;
    while(running) {
        vulkan_render_frame(mesh_prim_count, mesh_prim_list);
        MSG msg= {0};
        while(PeekMessage(&msg, NULL, 0, 00, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    /*========================================================================*/
    vkDestroyImageView(vk_device, vk_depth_image_view, NULL);
    vkDestroyImage(vk_device, vk_depth_image, NULL);
    vkFreeMemory(vk_device, vk_depth_memory, NULL);
    vkDestroyBuffer(vk_device, vk_vertex_buffer, NULL);
    vkDestroyBuffer(vk_device, vk_index_buffer, NULL);
    HeapFree(process_heap, 0, mesh_prim_list);
    vkFreeMemory(vk_device, vk_buffer_memory, NULL);
    vkDestroySemaphore(vk_device, vk_acquire_semaphore, NULL);
    vulkan_destroy_swapchain_attachments();
    vkDestroyPipeline(vk_device, vk_pipeline.pipeline, NULL);
    vkDestroyPipelineLayout(vk_device, vk_pipeline.pipeline_layout, NULL);
    vkDestroyShaderModule(vk_device, vk_pipeline.vertex_shader, NULL);
    vkDestroyShaderModule(vk_device, vk_pipeline.fragment_shader, NULL);
    vkDestroySwapchainKHR(vk_device, vk_swapchain, NULL);
    vkDestroySurfaceKHR(vk_instance, vk_surface, NULL);
    DestroyWindow(win32_window);
    vkFreeCommandBuffers(vk_device, vk_gfx_cmd_pool, 1, &vk_gfx_cmd_buffer);
    vkDestroyCommandPool(vk_device, vk_gfx_cmd_pool, NULL);
    vkDestroyDevice(vk_device, NULL);
    vkDestroyDebugUtilsMessengerEXT(vk_instance, vk_dbg_messenger, NULL);
    vkDestroyInstance(vk_instance, NULL);
    FreeLibrary(vulkan_library);
    g_allocator.free(gltf_json.accessor_list);
    g_allocator.free(gltf_json.buffer_view_list);
    g_allocator.free(gltf_json.mesh.primitive_list);
    ExitProcess(0);
}
