#include "ggml_v2-opencl.h"

#include <array>
#include <atomic>
#include <sstream>

#define CL_TARGET_OPENCL_VERSION 110
#include <clblast.h>
#include <clblast_c.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ggml_v2.h"

#define CL_DMMV_BLOCK_SIZE 32;

#define MULTILINE_QUOTE(...) #__VA_ARGS__
static std::string program_source = MULTILINE_QUOTE(

typedef char int8_t;
typedef uchar uint8_t;
typedef int int32_t;
typedef uint uint32_t;

struct block_q4_0
{
    float d;
    uint8_t qs[16];
};

struct block_q4_1
{
    float d;
    float m;
    uint8_t qs[16];
};

struct __attribute__ ((packed)) block_q5_0
{
    half d;
    uint32_t qh;
    uint8_t qs[16];
};

struct block_q5_1
{
    half d;
    half m;
    uint32_t qh;
    uint8_t qs[16];
};

struct block_q8_0
{
    float d;
    uint8_t qs[32];
};


__kernel void convert_fp16_to_fp32(__global half* x, __global float* y) {
    const uint i = get_global_id(0);

    y[i] = vload_half(0, &x[i]);
}

void dequantize_q4_0(__global const struct block_q4_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = x[ib].d;

    const uint8_t vui = x[ib].qs[iqs];

    const int8_t vi0 = vui & 0xF;
    const int8_t vi1 = vui >> 4;

    *v0 = (vi0 - 8)*d;
    *v1 = (vi1 - 8)*d;
}
void dequantize_q4_1(__global const struct block_q4_1* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = x[ib].d;
    const float m = x[ib].m;

    const uint8_t vui = x[ib].qs[iqs];

    const int8_t vi0 = vui & 0xF;
    const int8_t vi1 = vui >> 4;

    *v0 = vi0*d + m;
    *v1 = vi1*d + m;
}
void dequantize_q5_0(__global const struct block_q5_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, (__global half*) &x[ib].d);

    uint32_t qh = x[ib].qh;

    const uint8_t xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const uint8_t xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    const int32_t x0 = ((x[ib].qs[iqs] & 0xf) | xh_0) - 16;
    const int32_t x1 = ((x[ib].qs[iqs] >>  4) | xh_1) - 16;

    *v0 = x0*d;
    *v1 = x1*d;
}
void dequantize_q5_1(__global const struct block_q5_1* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, (__global half*) &x[ib].d);
    const float m = vload_half(0, (__global half*) &x[ib].m);

    uint32_t qh = x[ib].qh;

    const uint8_t xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const uint8_t xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    const int32_t x0 = ((x[ib].qs[iqs] & 0xf) | xh_0);
    const int32_t x1 = ((x[ib].qs[iqs] >>  4) | xh_1);

    *v0 = x0*d + m;
    *v1 = x1*d + m;
}
void dequantize_q8_0(__global const struct block_q8_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = x[ib].d;

    const int8_t vi0 = x[ib].qs[iqs + 0];
    const int8_t vi1 = x[ib].qs[iqs + 1];

    *v0 = vi0*d;
    *v1 = vi1*d;
}
static void convert_f16(__global half* x, const int ib, const int iqs, float* v0, float* v1){
    *v0 = vload_half(0, &x[ib + 0]);
    *v1 = vload_half(0, &x[ib + 1]);
}
);

static std::string dequant_template = MULTILINE_QUOTE(
__kernel void KERNEL_NAME(__global X_TYPE* x, __global float* y) {
    const int i = get_group_id(0)*get_local_size(0) + get_local_id(0)*2;

    if (i >= get_global_size(0)) {
        return;
    }

    const uint qk = QUANT_K;
    const uint qr = QUANT_R;

    const int ib = i/qk; // block index
    const int iqs = (i%qk)/qr; // quant index
    const int iybs = i - i%qk; // y block start index
    const int y_offset = qr == 1 ? 1 : qk/2;

    // dequantize
    float v0, v1;
    DEQUANT_FUNC(x, ib, iqs, &v0, &v1);
    y[iybs + iqs + 0] = v0;
    y[iybs + iqs + y_offset] = v1;
}
);

static std::string dequant_mul_mat_vec_template = MULTILINE_QUOTE(
__kernel void KERNEL_NAME(__global X_TYPE* x, __local float* tmp, __global float* y, __global float* dst, const int ncols) {
    const int block_size = get_local_size(0);
    const int row = get_global_id(0) / block_size;
    const int tid = get_local_id(0);

    const uint qk = QUANT_K;
    const uint qr = QUANT_R;

    const int y_offset = qr == 1 ? 1 : qk/2;

    tmp[tid] = 0;

    for (int i = 0; i < ncols/block_size; i += 2) {
        const int col = i*block_size + 2*tid;
        const int ib = (row*ncols + col)/qk; // block index
        const int iqs = (col%qk)/qr; // quant index
        const int iybs = col - col%qk; // y block start index

        // dequantize
        float v0, v1;
        DEQUANT_FUNC(x, ib, iqs, &v0, &v1);

        // matrix multiplication
        tmp[tid] += v0 * y[iybs + iqs + 0];
        tmp[tid] += v1 * y[iybs + iqs + y_offset];
    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=block_size/2; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}
);

static std::array<std::string, 5> dequant_str_keys = {
    "KERNEL_NAME", "X_TYPE", "QUANT_K", "QUANT_R", "DEQUANT_FUNC"
};

static std::array<std::string, 30> dequant_str_values = {
    "dequantize_row_q4_0", "struct block_q4_0", "32", "2", "dequantize_q4_0",
    "dequantize_row_q4_1", "struct block_q4_1", "32", "2", "dequantize_q4_1",
    "dequantize_row_q5_0", "struct block_q5_0", "32", "2", "dequantize_q5_0",
    "dequantize_row_q5_1", "struct block_q5_1", "32", "2", "dequantize_q5_1",
    "dequantize_row_q8_0", "struct block_q8_0", "32", "1", "dequantize_q8_0",
    "convert_row_f16", "half", "1", "1", "convert_f16"
};

static std::array<std::string, 30> dequant_mul_mat_vec_str_values = {
    "dequantize_mul_mat_vec_q4_0", "struct block_q4_0", "32", "2", "dequantize_q4_0",
    "dequantize_mul_mat_vec_q4_1", "struct block_q4_1", "32", "2", "dequantize_q4_1",
    "dequantize_mul_mat_vec_q5_0", "struct block_q5_0", "32", "2", "dequantize_q5_0",
    "dequantize_mul_mat_vec_q5_1", "struct block_q5_1", "32", "2", "dequantize_q5_1",
    "dequantize_mul_mat_vec_q8_0", "struct block_q8_0", "32", "1", "dequantize_q8_0",
    "convert_mul_mat_vec_f16", "half", "1", "1", "convert_f16"
};

static std::string& sreplace2(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
         s.replace(pos, from.length(), to);
         pos += to.length();
    }
    return s;
}

static std::string generate_kernels() {
    std::stringstream src;
    src << program_source << '\n';
    for (size_t i = 0; i < dequant_str_values.size(); i += dequant_str_keys.size()) {
        std::string dequant_kernel = dequant_template;
        std::string dmmv_kernel = dequant_mul_mat_vec_template;
        for (size_t j = 0; j < dequant_str_keys.size(); j++) {
            sreplace2(dequant_kernel, dequant_str_keys[j], dequant_str_values[i + j]);
            sreplace2(dmmv_kernel, dequant_str_keys[j], dequant_mul_mat_vec_str_values[i + j]);
        }
        src << dequant_kernel << '\n';
        src << dmmv_kernel << '\n';
    }
    return src.str();
}

#define CL_CHECK(err, name)                                                                     \
    do {                                                                                        \
        cl_int err_ = (err);                                                                    \
        if (err_ != CL_SUCCESS) {                                                               \
            fprintf(stderr, "OpenCL %s error %d at %s:%d\n", name, err_, __FILE__, __LINE__);   \
            fprintf(stderr, "You may be out of VRAM. Please check if you have enough.\n");      \
            exit(1);                                                                            \
        }                                                                                       \
    } while (0)

static cl_platform_id platform;
static cl_device_id device;
static cl_context context;
static cl_command_queue queue;
static cl_program program;
static cl_mem cl_buffer_a, cl_buffer_qb, cl_buffer_b, cl_buffer_c;
static size_t cl_size_a = 0, cl_size_qb = 0, cl_size_b = 0, cl_size_c = 0;
static cl_kernel convert_row_f16_cl;
static cl_kernel dequantize_row_q4_0_cl, dequantize_row_q4_1_cl, dequantize_row_q5_0_cl, dequantize_row_q5_1_cl, dequantize_row_q8_0_cl;
static cl_kernel dequantize_mul_mat_vec_q4_0_cl, dequantize_mul_mat_vec_q4_1_cl, dequantize_mul_mat_vec_q5_0_cl, dequantize_mul_mat_vec_q5_1_cl, dequantize_mul_mat_vec_q8_0_cl, convert_mul_mat_vec_f16_cl;
static bool fp16_support = false;

static cl_program build_program_from_source(cl_context ctx, cl_device_id dev, const char* program_buffer) {
    cl_program p;
    char *program_log;
    size_t program_size, log_size;
    int err;

    program_size = strlen(program_buffer);

    p = clCreateProgramWithSource(ctx, 1, (const char**)&program_buffer, &program_size, &err);
    if(err < 0) {
        fprintf(stderr, "OpenCL error creating program");
        exit(1);
    }

    err = clBuildProgram(p, 0, NULL, NULL, NULL, NULL);
    if(err < 0) {

        clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        program_log = (char*) malloc(log_size + 1);
        program_log[log_size] = '\0';
        clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, log_size + 1, program_log, NULL);
        printf("%s\n", program_log);
        free(program_log);
        exit(1);
    }

    return p;
}

void ggml_v2_cl_init(void) {
    cl_int err = 0;
    char * GGML_V2_CLBLAST_PLATFORM = getenv("GGML_OPENCL_PLATFORM");
    char * GGML_V2_CLBLAST_DEVICE = getenv("GGML_OPENCL_DEVICE");
    int plat_num = (GGML_V2_CLBLAST_PLATFORM == NULL ? 0 : atoi(GGML_V2_CLBLAST_PLATFORM));
    int dev_num = (GGML_V2_CLBLAST_DEVICE == NULL ? 0 : atoi(GGML_V2_CLBLAST_DEVICE));
    printf("\nInitializing LEGACY v2 CLBlast (First Run)...");
    printf("\nAttempting to use: Platform=%d, Device=%d (If invalid, program will crash)\n",plat_num,dev_num);
    cl_uint num_platforms;
    clGetPlatformIDs(0, NULL, &num_platforms);
    cl_platform_id* platforms = (cl_platform_id*)malloc(num_platforms*sizeof(cl_platform_id));
    clGetPlatformIDs(num_platforms, platforms, NULL);
    platform = platforms[plat_num];
    char platform_buffer[1024];
    clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(platform_buffer), &platform_buffer, NULL);
    cl_uint num_devices;
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, NULL, &num_devices);
    cl_device_id* devices = (cl_device_id*)malloc(num_devices*sizeof(cl_device_id));
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, num_devices, devices, NULL);
    device = devices[dev_num];
    char device_buffer[1024];
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(device_buffer), &device_buffer, NULL);
    size_t ext_str_size;
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, 0, NULL, &ext_str_size);
    char* ext_buffer = (char*) malloc(sizeof(char) * ext_str_size);
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, ext_str_size, ext_buffer, NULL);
    // Check if ext_buffer contains cl_khr_fp16
    for (size_t i = 0; i < ext_str_size - 12; i++) {
        if (memcmp(ext_buffer + i, "cl_khr_fp16", 11) == 0) {
            fp16_support = true;
            break;
        }
    }
    free(ext_buffer);
    printf("Using Platform: %s Device: %s FP16: %d\n", platform_buffer, device_buffer, fp16_support);
    fp16_support = false;
    printf("CL FP16 temporarily disabled pending further optimization.\n");
    context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    CL_CHECK(err, "clCreateContext");
    queue = clCreateCommandQueue(context, device, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &err);
    CL_CHECK(err, "clCreateCommandQueue");

    free(platforms);
    free(devices);

    std::string kernel_src = generate_kernels();

    program = build_program_from_source(context, device, kernel_src.c_str());

    // FP16 to FP32 kernel
    convert_row_f16_cl = clCreateKernel(program, "convert_row_f16", &err);
    CL_CHECK(err, "clCreateKernel");

    // Dequantize kernels
    dequantize_row_q4_0_cl = clCreateKernel(program, "dequantize_row_q4_0", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_row_q4_1_cl = clCreateKernel(program, "dequantize_row_q4_1", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_row_q5_0_cl = clCreateKernel(program, "dequantize_row_q5_0", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_row_q5_1_cl = clCreateKernel(program, "dequantize_row_q5_1", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_row_q8_0_cl = clCreateKernel(program, "dequantize_row_q8_0", &err);
    CL_CHECK(err, "clCreateKernel");

    // dequant mul mat kernel
    dequantize_mul_mat_vec_q4_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_0", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_mul_mat_vec_q4_1_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_1", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_mul_mat_vec_q5_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_0", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_mul_mat_vec_q5_1_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_1", &err);
    CL_CHECK(err, "clCreateKernel");
    dequantize_mul_mat_vec_q8_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q8_0", &err);
    CL_CHECK(err, "clCreateKernel");
    convert_mul_mat_vec_f16_cl = clCreateKernel(program, "convert_mul_mat_vec_f16", &err);
    CL_CHECK(err, "clCreateKernel");
}

static void ggml_v2_cl_malloc(size_t req_size, size_t* cur_size, cl_mem_flags flags, cl_mem* buf) {
    if (req_size <= *cur_size) {
        return;
    }

    // Reallocate buffer with enough space
    if (*cur_size > 0) {
        clReleaseMemObject(*buf);
    }
    cl_int err;
    *buf = clCreateBuffer(context, flags, req_size, NULL, &err);
    *cur_size = req_size;
    CL_CHECK(err, "clCreateBuffer");
}

static cl_kernel* ggml_v2_get_to_fp32_cl(ggml_v2_type type) {
    switch (type) {
        case GGML_V2_TYPE_Q4_0:
            return &dequantize_row_q4_0_cl;
        case GGML_V2_TYPE_Q4_1:
            return &dequantize_row_q4_1_cl;
        case GGML_V2_TYPE_Q5_0:
            return &dequantize_row_q5_0_cl;
        case GGML_V2_TYPE_Q5_1:
            return &dequantize_row_q5_1_cl;
        case GGML_V2_TYPE_Q8_0:
            return &dequantize_row_q8_0_cl;
        case GGML_V2_TYPE_F16:
            return &convert_row_f16_cl;
        default:
            return nullptr;
    }
}

static cl_kernel* ggml_v2_get_dequantize_mul_mat_vec_cl(ggml_v2_type type) {
    switch (type) {
        case GGML_V2_TYPE_Q4_0:
            return &dequantize_mul_mat_vec_q4_0_cl;
        case GGML_V2_TYPE_Q4_1:
            return &dequantize_mul_mat_vec_q4_1_cl;
        case GGML_V2_TYPE_Q5_0:
            return &dequantize_mul_mat_vec_q5_0_cl;
        case GGML_V2_TYPE_Q5_1:
            return &dequantize_mul_mat_vec_q5_1_cl;
        case GGML_V2_TYPE_Q8_0:
            return &dequantize_mul_mat_vec_q8_0_cl;
        case GGML_V2_TYPE_F16:
            return &convert_mul_mat_vec_f16_cl;
        default:
            return nullptr;
    }
}

// buffer pool for cl
#define MAX_CL_BUFFERS 256

struct scoped_spin_lock {
    std::atomic_flag& lock;
    scoped_spin_lock(std::atomic_flag& lock) : lock(lock) {
        while (lock.test_and_set(std::memory_order_acquire)) {
            ; // spin
        }
    }
    ~scoped_spin_lock() {
        lock.clear(std::memory_order_release);
    }
    scoped_spin_lock(const scoped_spin_lock&) = delete;
    scoped_spin_lock& operator=(const scoped_spin_lock&) = delete;
};

struct cl_buffer {
    cl_mem mem;
    size_t size = 0;
};

static cl_buffer g_cl_buffer_pool[MAX_CL_BUFFERS];
static std::atomic_flag g_cl_pool_lock = ATOMIC_FLAG_INIT;

static cl_mem ggml_v2_cl_pool_malloc(size_t size, size_t * actual_size, cl_mem_flags flags) {
    scoped_spin_lock lock(g_cl_pool_lock);
    cl_int err;

    for (int i = 0; i < MAX_CL_BUFFERS; ++i) {
        cl_buffer& b = g_cl_buffer_pool[i];
        if (b.size > 0 && b.size >= size) {
            cl_mem mem = b.mem;
            *actual_size = b.size;
            b.size = 0;
            return mem;
        }
    }
    cl_mem mem = clCreateBuffer(context, flags, size, NULL, &err);
    CL_CHECK(err, "clCreateBuffer");
    *actual_size = size;
    return mem;
}

static void ggml_v2_cl_pool_free(cl_mem mem, size_t size) {
    scoped_spin_lock lock(g_cl_pool_lock);

    for (int i = 0; i < MAX_CL_BUFFERS; ++i) {
        cl_buffer& b = g_cl_buffer_pool[i];
        if (b.size == 0) {
            b.mem = mem;
            b.size = size;
            return;
        }
    }
    fprintf(stderr, "WARNING: cl buffer pool full, increase MAX_CL_BUFFERS\n");
    clReleaseMemObject(mem);
}

static cl_int ggml_v2_cl_h2d_tensor_2d(cl_command_queue queue, cl_mem dst, size_t offset, const struct ggml_v2_tensor * src, uint64_t i3, uint64_t i2, cl_event* ev) {
    cl_int err;
    const uint64_t ne0 = src->ne[0];
    const uint64_t ne1 = src->ne[1];
    const uint64_t nb0 = src->nb[0];
    const uint64_t nb1 = src->nb[1];
    const uint64_t nb2 = src->nb[2];
    const uint64_t nb3 = src->nb[3];
    const enum ggml_v2_type type = src->type;
    const size_t ts = ggml_v2_type_size(type);
    const size_t bs = ggml_v2_blck_size(type);

    const void * x = (const void *) ((const char *) src->data + i2*nb2 + i3*nb3);
    if (nb0 == ts && nb1 == ts*ne0/bs) {
        err = clEnqueueWriteBuffer(queue, dst, CL_FALSE, offset, ne1*nb1, x, 0, NULL, ev);
        return err;
    }
    if (nb0 == ts) {
        const size_t buffer_origin[3] = { offset, 0, 0 };
        const size_t host_origin[3] = { 0, 0, 0 };
        const size_t region[3] = { ts*ne0/bs, ne1, 1 };
        err = clEnqueueWriteBufferRect(queue, dst, CL_FALSE, buffer_origin, host_origin, region, ts*ne0/bs, 0, nb1, 0, x, 0, NULL, ev);
        return err;
    }
    for (uint64_t i1 = 0; i1 < ne1; i1++) {
        // pretend the row is a matrix with cols=1
        const size_t buffer_origin[3] = { offset, i1, 0 };
        const size_t host_origin[3] = { 0, 0, 0 };
        const size_t region[3] = { ts/bs, ne0, 1 };
        err = clEnqueueWriteBufferRect(queue, dst, CL_FALSE, buffer_origin, host_origin, region, 0, 0, nb0, 0, ((const char *)x) + i1*nb0, 0, NULL, ev);
        if (err != CL_SUCCESS) {
            break;
        }
    }
    return err;
}

static void ggml_v2_cl_mul_mat_f32(const ggml_v2_tensor * src0, const ggml_v2_tensor * src1, ggml_v2_tensor * dst) {
    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];
    const int64_t ne03 = src0->ne[3];

    const int64_t ne10 = src1->ne[0];
    const int64_t ne11 = src1->ne[1];

    const int nb2  = dst->nb[2];
    const int nb3  = dst->nb[3];

    const float alpha = 1.0f;
    const float beta = 0.0f;
    const int x_ne = ne01 * ne00;
    const int y_ne = ne11 * ne10;
    const int d_ne = ne11 * ne01;

    size_t x_size, y_size, d_size;
    cl_mem d_X = ggml_v2_cl_pool_malloc(sizeof(float) * x_ne, &x_size, CL_MEM_READ_ONLY);
    cl_mem d_Y = ggml_v2_cl_pool_malloc(sizeof(float) * y_ne, &y_size, CL_MEM_READ_ONLY);
    cl_mem d_D = ggml_v2_cl_pool_malloc(sizeof(float) * d_ne, &d_size, CL_MEM_WRITE_ONLY);

    cl_int err;

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            // copy data to device
            err = ggml_v2_cl_h2d_tensor_2d(queue, d_X, 0, src0, i03, i02, NULL);
            err |= ggml_v2_cl_h2d_tensor_2d(queue, d_Y, 0, src1, i03, i02, NULL);
            CL_CHECK(err, "ggml_v2_cl_h2d_tensor_2d");

            CL_CHECK(clFinish(queue), "clFinish");

            // compute
            cl_event ev_sgemm;

            clblast::StatusCode status = (clblast::StatusCode)CLBlastSgemm((CLBlastLayout)clblast::Layout::kColMajor,
                                            (CLBlastTranspose)clblast::Transpose::kYes, (CLBlastTranspose)clblast::Transpose::kNo,
                                            ne01, ne11, ne10,
                                            alpha,
                                            d_X, 0, ne00,
                                            d_Y, 0, ne10,
                                            beta,
                                            d_D, 0, ne01,
                                            &queue, &ev_sgemm);

            if (status != clblast::StatusCode::kSuccess) {
                printf("\nF32 Matmul Failed (%d): [dims: %ld,%ld,%ld,%ld] You may be out of VRAM. Please check if you have enough.\n",static_cast<int>(status),ne00,ne01,ne10,ne11);
                GGML_V2_ASSERT(false);
            }

            // copy dst to host
            float * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);
            err = clEnqueueReadBuffer(queue, d_D, true, 0, sizeof(float) * d_ne, d, 1, &ev_sgemm, NULL);
            CL_CHECK(err, "clEnqueueReadBuffer");
        }
    }

    ggml_v2_cl_pool_free(d_X, x_size);
    ggml_v2_cl_pool_free(d_Y, y_size);
    ggml_v2_cl_pool_free(d_D, d_size);
}

static void ggml_v2_cl_mul_mat_f16(const ggml_v2_tensor * src0, const ggml_v2_tensor * src1, ggml_v2_tensor * dst, void * wdata, size_t /* wsize */) {
    GGML_V2_ASSERT(fp16_support);

    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];
    const int64_t ne03 = src0->ne[3];

    const int64_t ne10 = src1->ne[0];
    const int64_t ne11 = src1->ne[1];

    const int nb10 = src1->nb[0];
    const int nb11 = src1->nb[1];
    const int nb12 = src1->nb[2];
    const int nb13 = src1->nb[3];

    const int nb2  = dst->nb[2];
    const int nb3  = dst->nb[3];

    const ggml_v2_fp16_t alpha = ggml_v2_fp32_to_fp16(1.0f);
    const ggml_v2_fp16_t beta = ggml_v2_fp32_to_fp16(0.0f);
    const int x_ne = ne01 * ne00;
    const int y_ne = ne11 * ne10;
    const int d_ne = ne11 * ne01;

    size_t x_size, y_size, d_size;
    cl_mem d_X = ggml_v2_cl_pool_malloc(sizeof(ggml_v2_fp16_t) * x_ne, &x_size, CL_MEM_READ_ONLY);
    cl_mem d_Y = ggml_v2_cl_pool_malloc(sizeof(ggml_v2_fp16_t) * y_ne, &y_size, CL_MEM_READ_ONLY);
    cl_mem d_D = ggml_v2_cl_pool_malloc(sizeof(ggml_v2_fp16_t) * d_ne, &d_size, CL_MEM_WRITE_ONLY);

    cl_int err;

    bool src1_cont_rows = nb10 == sizeof(float);
    bool src1_cont_cols = (size_t)nb11 == ne11*sizeof(float);

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            // copy src0 to device
            err = ggml_v2_cl_h2d_tensor_2d(queue, d_X, 0, src0, i03, i02, NULL);
            CL_CHECK(err, "ggml_v2_cl_h2d_tensor_2d");

            // convert src1 to fp16
            // TODO: use multiple threads
            ggml_v2_fp16_t * const tmp = (ggml_v2_fp16_t *) wdata + (ne11 * ne10) * (i03 * ne02 + i02);
            char * src1i = (char *) src1->data + i03*nb13 + i02*nb12;
            if (src1_cont_rows) {
                if (src1_cont_cols) {
                    ggml_v2_fp32_to_fp16_row((float *) src1i, tmp, ne10*ne11);
                }
                else {
                    for (int64_t i01 = 0; i01 < ne11; i01++) {
                        ggml_v2_fp32_to_fp16_row((float *) (src1i + i01*nb11), tmp + i01*ne10, ne10);
                    }
                }
            }
            else {
                for (int64_t i01 = 0; i01 < ne11; i01++) {
                    for (int64_t i00 = 0; i00 < ne10; i00++) {
                        // very slow due to no inlining
                        tmp[i01*ne10 + i00] = ggml_v2_fp32_to_fp16(*(float *) (src1i + i01*nb11 + i00*nb10));
                    }
                }
            }

            // copy src1 to device
            err |= clEnqueueWriteBuffer(queue, d_Y, false, 0, sizeof(ggml_v2_fp16_t) * y_ne, tmp, 0, NULL, NULL);
            CL_CHECK(err, "ggml_v2_cl_h2d_tensor_2d");

            CL_CHECK(clFinish(queue), "clFinish");

            // compute
            cl_event ev_sgemm;
            clblast::StatusCode status = (clblast::StatusCode)CLBlastHgemm((CLBlastLayout)clblast::Layout::kColMajor,
                                            (CLBlastTranspose)clblast::Transpose::kYes, (CLBlastTranspose)clblast::Transpose::kNo,
                                            ne01, ne11, ne10,
                                            alpha,
                                            d_X, 0, ne00,
                                            d_Y, 0, ne10,
                                            beta,
                                            d_D, 0, ne01,
                                            &queue, &ev_sgemm);

            if (status != clblast::StatusCode::kSuccess) {
                printf("\nF16 Matmul Failed (%d): [dims: %ld,%ld,%ld,%ld] You may be out of VRAM. Please check if you have enough.\n",static_cast<int>(status),ne00,ne01,ne10,ne11);
                GGML_V2_ASSERT(false);
            }

            // copy dst to host, then convert to float
            err = clEnqueueReadBuffer(queue, d_D, true, 0, sizeof(ggml_v2_fp16_t) * d_ne, tmp, 1, &ev_sgemm, NULL);

            float * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);

            ggml_v2_fp16_to_fp32_row(tmp, d, d_ne);
        }
    }

    ggml_v2_cl_pool_free(d_X, x_size);
    ggml_v2_cl_pool_free(d_Y, y_size);
    ggml_v2_cl_pool_free(d_D, d_size);
}

static void ggml_v2_cl_mul_mat_q_f32(const ggml_v2_tensor * src0, const ggml_v2_tensor * src1, ggml_v2_tensor * dst) {
    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];
    const int64_t ne03 = src0->ne[3];

    const int64_t ne10 = src1->ne[0];
    const int64_t ne11 = src1->ne[1];

    const int nb2  = dst->nb[2];
    const int nb3  = dst->nb[3];
    const ggml_v2_type type = src0->type;
    const bool mul_mat_vec = ne11 == 1;

    const float alpha = 1.0f;
    const float beta = 0.0f;
    const int x_ne = ne01 * ne00;
    const int y_ne = ne11 * ne10;
    const int d_ne = ne11 * ne01;
    const size_t q_sz = ggml_v2_type_size(type) * x_ne / ggml_v2_blck_size(type);

    size_t x_size, y_size, d_size, q_size;
    cl_mem d_X;
    if (!mul_mat_vec) {
        d_X = ggml_v2_cl_pool_malloc(sizeof(float) * x_ne, &x_size, CL_MEM_READ_WRITE);
    }
    cl_mem d_Y = ggml_v2_cl_pool_malloc(sizeof(float) * y_ne, &y_size, CL_MEM_READ_ONLY);
    cl_mem d_D = ggml_v2_cl_pool_malloc(sizeof(float) * d_ne, &d_size, CL_MEM_WRITE_ONLY);
    cl_mem d_Q;
    if (src0->backend == GGML_V2_BACKEND_CPU) {
        d_Q = ggml_v2_cl_pool_malloc(q_sz, &q_size, CL_MEM_READ_ONLY);
    }

    cl_kernel* to_fp32_cl = ggml_v2_get_to_fp32_cl(type);
    cl_kernel* dmmv = ggml_v2_get_dequantize_mul_mat_vec_cl(type);
    GGML_V2_ASSERT(to_fp32_cl != nullptr);

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            cl_event ev_sgemm;

            // copy src0 to device if necessary
            if (src0->backend == GGML_V2_BACKEND_CPU) {
                CL_CHECK(ggml_v2_cl_h2d_tensor_2d(queue, d_Q, 0, src0, i03, i02, NULL), "ggml_v2_cl_h2d_tensor_2d");
            } else if (src0->backend == GGML_V2_BACKEND_CL) {
                d_Q = *(cl_mem*) src0->data;
            } else {
                GGML_V2_ASSERT(false);
            }
            if (mul_mat_vec) { // specialized dequantize_mul_mat_vec kernel
                // copy src1 to device
                CL_CHECK(ggml_v2_cl_h2d_tensor_2d(queue, d_Y, 0, src1, i03, i02, NULL), "ggml_v2_cl_h2d_tensor_2d");

                // compute
                const size_t global = ne01 * CL_DMMV_BLOCK_SIZE;
                const size_t local = CL_DMMV_BLOCK_SIZE;
                const cl_int ncols = ne00;
                CL_CHECK(clSetKernelArg(*dmmv, 0, sizeof(cl_mem), &d_Q), "clSetKernelArg");
                CL_CHECK(clSetKernelArg(*dmmv, 1, sizeof(float) * local, NULL), "clSetKernelArg");
                CL_CHECK(clSetKernelArg(*dmmv, 2, sizeof(cl_mem), &d_Y), "clSetKernelArg");
                CL_CHECK(clSetKernelArg(*dmmv, 3, sizeof(cl_mem), &d_D), "clSetKernelArg");
                CL_CHECK(clSetKernelArg(*dmmv, 4, sizeof(cl_int), &ncols), "clSetKernelArg");
                CL_CHECK(clFinish(queue), "clFinish");
                CL_CHECK(clEnqueueNDRangeKernel(queue, *dmmv, 1, NULL, &global, &local, 0, NULL, &ev_sgemm), "clEnqueueNDRangeKernel");
            } else { // general dequantization kernel + CLBlast matrix matrix multiplication
                // convert src0 to fp32 on device
                const size_t global = x_ne;
                CL_CHECK(clSetKernelArg(*to_fp32_cl, 0, sizeof(cl_mem), &d_Q), "clSetKernelArg");
                CL_CHECK(clSetKernelArg(*to_fp32_cl, 1, sizeof(cl_mem), &d_X), "clSetKernelArg");
                CL_CHECK(clFinish(queue), "clFinish");
                CL_CHECK(clEnqueueNDRangeKernel(queue, *to_fp32_cl, 1, NULL, &global, NULL, 0, NULL, NULL), "clEnqueueNDRangeKernel");

                // copy src1 to device
                CL_CHECK(ggml_v2_cl_h2d_tensor_2d(queue, d_Y, 0, src1, i03, i02, NULL), "ggml_v2_cl_h2d_tensor_2d");

                // wait for conversion
                CL_CHECK(clFinish(queue), "clFinish");

                // compute
                clblast::StatusCode status = (clblast::StatusCode)CLBlastSgemm((CLBlastLayout)clblast::Layout::kColMajor,
                                            (CLBlastTranspose)clblast::Transpose::kYes, (CLBlastTranspose)clblast::Transpose::kNo,
                                            ne01, ne11, ne10,
                                            alpha,
                                            d_X, 0, ne00,
                                            d_Y, 0, ne10,
                                            beta,
                                            d_D, 0, ne01,
                                            &queue, &ev_sgemm);

                if (status != clblast::StatusCode::kSuccess) {
                    printf("\nQF32 Matmul Failed (%d): [dims: %ld,%ld,%ld,%ld] You may be out of VRAM. Please check if you have enough.\n",static_cast<int>(status),ne00,ne01,ne10,ne11);
                    GGML_V2_ASSERT(false);
                }
            }

            // copy dst to host
            float * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);
            CL_CHECK(clEnqueueReadBuffer(queue, d_D, true, 0, sizeof(float) * d_ne, d, 1, &ev_sgemm, NULL), "clEnqueueReadBuffer");
            clReleaseEvent(ev_sgemm);
        }
    }

    if (!mul_mat_vec) {
        ggml_v2_cl_pool_free(d_X, x_size);
    }
    ggml_v2_cl_pool_free(d_Y, y_size);
    ggml_v2_cl_pool_free(d_D, d_size);
    if (src0->backend == GGML_V2_BACKEND_CPU) {
        ggml_v2_cl_pool_free(d_Q, q_size);
    }
}


bool ggml_v2_cl_can_mul_mat(const struct ggml_v2_tensor * src0, const struct ggml_v2_tensor * src1, struct ggml_v2_tensor * dst) {
    const int64_t ne10 = src1->ne[0];

    const int64_t ne0 = dst->ne[0];
    const int64_t ne1 = dst->ne[1];

    // TODO: find the optimal values for these
    if ((src0->type == GGML_V2_TYPE_F32 || src0->type == GGML_V2_TYPE_F16 || ggml_v2_is_quantized(src0->type)) &&
        src1->type == GGML_V2_TYPE_F32 &&
        dst->type == GGML_V2_TYPE_F32 &&
        ((GetQuantsUnshuffled() && ne0 >= 32 && ne1 >= 32 && ne10 >= 32) || src0->backend == GGML_V2_BACKEND_CL)) {
        return true;
    }

    return false;
}

bool ggml_v2_cl_mul_mat_use_f16(const struct ggml_v2_tensor * src0, const struct ggml_v2_tensor * src1, struct ggml_v2_tensor * /* dst */) {
    // If device doesn't support FP16
    if (!fp16_support) {
        return false;
    }

    size_t src0_sz = ggml_v2_nbytes(src0);
    size_t src1_sz = ggml_v2_nbytes(src1);

    // mul_mat_q: src0 is converted to fp32 on device
    size_t mul_mat_q_transfer = src0_sz + src1_sz;

    // mul_mat_f16: src1 is converted to fp16 on cpu
    size_t mul_mat_f16_transfer = src0_sz + sizeof(ggml_v2_fp16_t) * ggml_v2_nelements(src1);

    // choose the smaller one to transfer to the device
    // TODO: this is not always the best choice due to the overhead of converting to fp16
    return mul_mat_f16_transfer < mul_mat_q_transfer;
}

void ggml_v2_cl_mul_mat(const struct ggml_v2_tensor * src0, const struct ggml_v2_tensor * src1, struct ggml_v2_tensor * dst, void * wdata, size_t wsize) {
    GGML_V2_ASSERT(ggml_v2_cl_can_mul_mat(src0, src1, dst));

    if (src0->type == GGML_V2_TYPE_F32) {
        ggml_v2_cl_mul_mat_f32(src0, src1, dst);
    }
    else if (src0->type == GGML_V2_TYPE_F16) {
        if (ggml_v2_cl_mul_mat_use_f16(src0, src1, dst)) {
            ggml_v2_cl_mul_mat_f16(src0, src1, dst, wdata, wsize);
        }
        else {
            ggml_v2_cl_mul_mat_q_f32(src0, src1, dst);
        }
    }
    else if (ggml_v2_is_quantized(src0->type)) {
        ggml_v2_cl_mul_mat_q_f32(src0, src1, dst);
    }
    else {
        GGML_V2_ASSERT(false);
    }
}

size_t ggml_v2_cl_mul_mat_get_wsize(const struct ggml_v2_tensor * src0, const struct ggml_v2_tensor * src1, struct ggml_v2_tensor * dst) {
    if (ggml_v2_cl_mul_mat_use_f16(src0, src1, dst)) {
        return ggml_v2_nelements(src1) * sizeof(ggml_v2_fp16_t);
    }
    return 0;
}

void ggml_v2_cl_transform_tensor(ggml_v2_tensor * tensor) {
    const int64_t ne0 = tensor->ne[0];
    const int64_t ne1 = tensor->ne[1];
    const int64_t ne2 = tensor->ne[2];
    const int64_t ne3 = tensor->ne[3];

    const ggml_v2_type type = tensor->type;
    const size_t q_sz = ggml_v2_type_size(type) * ne0 * ne1 * ne2 * ne3 / ggml_v2_blck_size(type);

    size_t q_size;
    cl_mem* dst = (cl_mem*) malloc(sizeof(cl_mem));
    *dst = ggml_v2_cl_pool_malloc(q_sz, &q_size, CL_MEM_READ_ONLY);

    // copy tensor to device
    for (int64_t i3 = 0; i3 < ne3; i3++) {
        for (int64_t i2 = 0; i2 < ne2; i2++) {
            int i = i3*ne2 + i2;
            CL_CHECK(ggml_v2_cl_h2d_tensor_2d(queue, *dst, i*ne0*ne1, tensor, i3, i2, NULL), "ggml_v2_cl_h2d_tensor_2d");
        }
    }

    CL_CHECK(clFinish(queue), "clFinish");

    tensor->data = dst;
    tensor->backend = GGML_V2_BACKEND_CL;
}

void ggml_v2_cl_sgemm_wrapper(
        const enum ggml_v2_blas_order order, const enum ggml_v2_blas_op trans_a, const enum ggml_v2_blas_op trans_b,
        const int m, const int n, const int k,
        const float alpha, const void *host_a, const int lda,
        const float *host_b, const int ldb, const float beta,
        float *host_c, const int ldc, const int btype) {
    cl_int err = 0;

    cl_kernel * kernel = ggml_v2_get_to_fp32_cl((ggml_v2_type)btype);
    size_t global = n * k, local, size_qb;
    bool dequant;

    switch (btype) {
    case GGML_V2_TYPE_F32:
        dequant = false;
        break;
    case GGML_V2_TYPE_Q4_0:
        dequant = true;
        local = 16;
        size_qb = global * (sizeof(float) + local) / 32;
        break;
    case GGML_V2_TYPE_Q4_1:
        dequant = true;
        local = 16;
        size_qb = global * (sizeof(float) * 2 + local) / 32;
        break;
    case GGML_V2_TYPE_Q5_0:
        dequant = true;
        local = 16;
        size_qb = global * (sizeof(ggml_v2_fp16_t) + sizeof(uint32_t) + local) / 32;
        break;
    case GGML_V2_TYPE_Q5_1:
        dequant = true;
        local = 16;
        size_qb = global * (sizeof(ggml_v2_fp16_t) * 2 + sizeof(uint32_t) + local) / 32;
        break;
    case GGML_V2_TYPE_Q8_0:
        dequant = true;
        local = 32;
        size_qb = global * (sizeof(float) + local) / 32;
        break;
    default:
        fprintf(stderr, "Error: Unsupported OpenCL btype %d\n", btype);
        abort();
    }

    const size_t size_a =  m * k * sizeof(float);
    const size_t size_b =  n * k * sizeof(float);
    const size_t size_c =  m * n * sizeof(float);

    // Prepare buffers
    ggml_v2_cl_malloc(size_a, &cl_size_a, CL_MEM_READ_ONLY, &cl_buffer_a);
    if (dequant) {
        ggml_v2_cl_malloc(size_qb, &cl_size_qb, CL_MEM_READ_ONLY, &cl_buffer_qb);
    }
    ggml_v2_cl_malloc(size_b, &cl_size_b, CL_MEM_READ_WRITE, &cl_buffer_b);
    ggml_v2_cl_malloc(size_c, &cl_size_c, CL_MEM_WRITE_ONLY, &cl_buffer_c);

    cl_event ev_a, ev_qb, ev_b;

    if (dequant) {
        err = clSetKernelArg(*kernel, 0, sizeof(cl_mem), &cl_buffer_qb);
        err |= clSetKernelArg(*kernel, 1, sizeof(cl_mem), &cl_buffer_b);
        CL_CHECK(err, "clSetKernelArg");
        err = clEnqueueWriteBuffer(queue, cl_buffer_qb, CL_FALSE, 0, size_qb, host_b, 0, NULL, &ev_qb);
        CL_CHECK(err, "clEnqueueWriteBuffer qb");
    } else {
        err = clEnqueueWriteBuffer(queue, cl_buffer_b, CL_FALSE, 0, size_b, host_b, 0, NULL, &ev_b);
        CL_CHECK(err, "clEnqueueWriteBuffer b");
    }

    err = clEnqueueWriteBuffer(queue, cl_buffer_a, CL_FALSE, 0, size_a, host_a, 0, NULL, &ev_a);
    CL_CHECK(err, "clEnqueueWriteBuffer a");
    if (dequant) {
        err = clEnqueueNDRangeKernel(queue, *kernel, 1, NULL, &global, &local, 1, &ev_qb, &ev_b);
        CL_CHECK(err, "clEnqueueNDRangeKernel");
        clReleaseEvent(ev_qb);
    }
    clWaitForEvents(1, &ev_a);
    clWaitForEvents(1, &ev_b);
    clReleaseEvent(ev_a);
    clReleaseEvent(ev_b);

    cl_event ev_sgemm;
    CLBlastStatusCode status = CLBlastSgemm((CLBlastLayout)order,
                                            (CLBlastTranspose)trans_a, (CLBlastTranspose)trans_b,
                                            m, n, k,
                                            alpha,
                                            cl_buffer_a, 0, lda,
                                            cl_buffer_b, 0, ldb,
                                            beta,
                                            cl_buffer_c, 0, ldc,
                                            &queue, &ev_sgemm);

    if (status != CLBlastSuccess) {
        fprintf(stderr, "Error: CLBlast SGEMM %d\n", status);
        abort();
    }

    cl_event ev_c;
    clEnqueueReadBuffer(queue, cl_buffer_c, CL_TRUE, 0, size_c, host_c, 1, &ev_sgemm, &ev_c);

    // Wait for completion
    clWaitForEvents(1, &ev_c);
    clReleaseEvent(ev_sgemm);
    clReleaseEvent(ev_c);
}
