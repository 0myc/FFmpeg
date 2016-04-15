/*
 * H.264 hardware encoding using nvidia nvenc
 * Copyright (c) 2014 Timo Rothenpieler <timo@rothenpieler.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* External headers */
#include <nvEncodeAPI.h>
#include <cudautils.h>

/* FFmpeg headers */
#include "libavutil/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "internal.h"
#include "thread.h"


#if defined(_WIN32)
#define LOAD_FUNC(l, s) GetProcAddress(l, s)
#define DL_CLOSE_FUNC(l) FreeLibrary(l)
#else
#define LOAD_FUNC(l, s) dlsym(l, s)
#define DL_CLOSE_FUNC(l) dlclose(l)
#endif

#define BLOCKSX  128
#define THREADSX 128

typedef NVENCSTATUS (NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST *functionList);

typedef struct NvencInputSurface
{
    NV_ENC_INPUT_PTR input_surface;
    CUdeviceptr      dptr;
    void*            hRes;
    int width;
    int height;
    size_t pitch;

    int lockCount;

    NV_ENC_BUFFER_FORMAT format;
} NvencInputSurface;

typedef struct NvencOutputSurface
{
    NV_ENC_OUTPUT_PTR output_surface;
    int size;

    NvencInputSurface* input_surface;

    int busy;
} NvencOutputSurface;

typedef struct NvencData
{
    union {
        int64_t timestamp;
        NvencOutputSurface *surface;
    } u;
} NvencData;

typedef struct NvencDataList
{
    NvencData* data;

    uint32_t pos;
    uint32_t count;
    uint32_t size;
} NvencDataList;

typedef struct NvencDynLoadFunctions
{
    NV_ENCODE_API_FUNCTION_LIST nvenc_funcs;

#if defined(_WIN32)
    HMODULE nvenc_lib;
#else
    void* nvenc_lib;
#endif
} NvencDynLoadFunctions;

typedef struct NvencValuePair
{
    const char *str;
    uint32_t num;
} NvencValuePair;

typedef struct NvencContext
{
    AVClass *avclass;

    NvencDynLoadFunctions nvenc_dload_funcs;
    CudaDynLoadFunctions* cuda_dload_funcs;

    NV_ENC_INITIALIZE_PARAMS init_encode_params;
    NV_ENC_CONFIG encode_config;
    CUcontext cu_context;
    CUmodule  cu_module;
    CUfunction cu_func_interleaveChroma;

    int max_surface_count;
    NvencInputSurface *input_surfaces;
    NvencOutputSurface *output_surfaces;
    NvencInputSurface transferSurf;

    NvencDataList output_surface_queue;
    NvencDataList output_surface_ready_queue;
    NvencDataList timestamp_list;
    int64_t last_dts;

    void *nvencoder;

    char *preset;
    char *profile;
    char *level;
    char *tier;
    int cbr;
    int twopass;
    int gpu;
    int buffer_delay;
    int aq;
} NvencContext;


static const NvencValuePair nvenc_h264_level_pairs[] = {
    { "auto", NV_ENC_LEVEL_AUTOSELECT },
    { "1"   , NV_ENC_LEVEL_H264_1     },
    { "1.0" , NV_ENC_LEVEL_H264_1     },
    { "1b"  , NV_ENC_LEVEL_H264_1b    },
    { "1.0b", NV_ENC_LEVEL_H264_1b    },
    { "1.1" , NV_ENC_LEVEL_H264_11    },
    { "1.2" , NV_ENC_LEVEL_H264_12    },
    { "1.3" , NV_ENC_LEVEL_H264_13    },
    { "2"   , NV_ENC_LEVEL_H264_2     },
    { "2.0" , NV_ENC_LEVEL_H264_2     },
    { "2.1" , NV_ENC_LEVEL_H264_21    },
    { "2.2" , NV_ENC_LEVEL_H264_22    },
    { "3"   , NV_ENC_LEVEL_H264_3     },
    { "3.0" , NV_ENC_LEVEL_H264_3     },
    { "3.1" , NV_ENC_LEVEL_H264_31    },
    { "3.2" , NV_ENC_LEVEL_H264_32    },
    { "4"   , NV_ENC_LEVEL_H264_4     },
    { "4.0" , NV_ENC_LEVEL_H264_4     },
    { "4.1" , NV_ENC_LEVEL_H264_41    },
    { "4.2" , NV_ENC_LEVEL_H264_42    },
    { "5"   , NV_ENC_LEVEL_H264_5     },
    { "5.0" , NV_ENC_LEVEL_H264_5     },
    { "5.1" , NV_ENC_LEVEL_H264_51    },
    { NULL }
};

static const NvencValuePair nvenc_hevc_level_pairs[] = {
    { "auto", NV_ENC_LEVEL_AUTOSELECT },
    { "1"   , NV_ENC_LEVEL_HEVC_1     },
    { "1.0" , NV_ENC_LEVEL_HEVC_1     },
    { "2"   , NV_ENC_LEVEL_HEVC_2     },
    { "2.0" , NV_ENC_LEVEL_HEVC_2     },
    { "2.1" , NV_ENC_LEVEL_HEVC_21    },
    { "3"   , NV_ENC_LEVEL_HEVC_3     },
    { "3.0" , NV_ENC_LEVEL_HEVC_3     },
    { "3.1" , NV_ENC_LEVEL_HEVC_31    },
    { "4"   , NV_ENC_LEVEL_HEVC_4     },
    { "4.0" , NV_ENC_LEVEL_HEVC_4     },
    { "4.1" , NV_ENC_LEVEL_HEVC_41    },
    { "5"   , NV_ENC_LEVEL_HEVC_5     },
    { "5.0" , NV_ENC_LEVEL_HEVC_5     },
    { "5.1" , NV_ENC_LEVEL_HEVC_51    },
    { "5.2" , NV_ENC_LEVEL_HEVC_52    },
    { "6"   , NV_ENC_LEVEL_HEVC_6     },
    { "6.0" , NV_ENC_LEVEL_HEVC_6     },
    { "6.1" , NV_ENC_LEVEL_HEVC_61    },
    { "6.2" , NV_ENC_LEVEL_HEVC_62    },
    { NULL }
};

static int input_string_to_uint32(AVCodecContext *avctx, const NvencValuePair *pair, const char *input, uint32_t *output)
{
    for (; pair->str; ++pair) {
        if (!strcmp(input, pair->str)) {
            *output = pair->num;
            return 0;
        }
    }

    return AVERROR(EINVAL);
}

static NvencData* data_queue_dequeue(NvencDataList* queue)
{
    uint32_t mask;
    uint32_t read_pos;

    av_assert0(queue);
    av_assert0(queue->size);
    av_assert0(queue->data);

    if (!queue->count)
        return NULL;

    /* Size always is a multiple of two */
    mask = queue->size - 1;
    read_pos = (queue->pos - queue->count) & mask;
    queue->count--;

    return &queue->data[read_pos];
}

static int data_queue_enqueue(NvencDataList* queue, NvencData *data)
{
    NvencDataList new_queue;
    NvencData* tmp_data;
    uint32_t mask;

    if (!queue->size) {
        /* size always has to be a multiple of two */
        queue->size = 4;
        queue->pos = 0;
        queue->count = 0;

        queue->data = av_malloc(queue->size * sizeof(*(queue->data)));

        if (!queue->data) {
            queue->size = 0;
            return AVERROR(ENOMEM);
        }
    }

    if (queue->count == queue->size) {
        new_queue.size = queue->size << 1;
        new_queue.pos = 0;
        new_queue.count = 0;
        new_queue.data = av_malloc(new_queue.size * sizeof(*(queue->data)));

        if (!new_queue.data)
            return AVERROR(ENOMEM);

        while (tmp_data = data_queue_dequeue(queue))
            data_queue_enqueue(&new_queue, tmp_data);

        av_free(queue->data);
        *queue = new_queue;
    }

    mask = queue->size - 1;

    queue->data[queue->pos] = *data;
    queue->pos = (queue->pos + 1) & mask;
    queue->count++;

    return 0;
}

static int out_surf_queue_enqueue(NvencDataList* queue, NvencOutputSurface* surface)
{
    NvencData data;
    data.u.surface = surface;

    return data_queue_enqueue(queue, &data);
}

static NvencOutputSurface* out_surf_queue_dequeue(NvencDataList* queue)
{
    NvencData* res = data_queue_dequeue(queue);

    if (!res)
        return NULL;

    return res->u.surface;
}

static int timestamp_queue_enqueue(NvencDataList* queue, int64_t timestamp)
{
    NvencData data;
    data.u.timestamp = timestamp;

    return data_queue_enqueue(queue, &data);
}

static int64_t timestamp_queue_dequeue(NvencDataList* queue)
{
    NvencData* res = data_queue_dequeue(queue);

    if (!res)
        return AV_NOPTS_VALUE;

    return res->u.timestamp;
}

static av_cold int nvenc_check_cuda(AVCodecContext *avctx)
{

    int  target_smver;
    NvencContext *ctx = avctx->priv_data;

    if (!init_cuda())
        return 0;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        target_smver = 0x30;
        break;
    case AV_CODEC_ID_H265:
        target_smver = 0x52;
        break;
    default:
        av_log(avctx, AV_LOG_FATAL, "Unknown codec name\n");
        goto error;
    }

    if (!is_gpu_feature_available(ctx->gpu, target_smver))
    {
        av_log(avctx, AV_LOG_FATAL, "NVENC with Codec %s Not Available at requested GPU %d \n", (avctx->codec->id == AV_CODEC_ID_H264)? "H264" : "H265", ctx->gpu);
        goto error;
    }
    else
    {
        av_log(avctx, AV_LOG_VERBOSE, "NVENC with Codec %s Available at requested GPU %d \n", (avctx->codec->id == AV_CODEC_ID_H264) ? "H264" : "H265", ctx->gpu);
    }

    return 1;

error:
    return 0;
}

static av_cold int nvenc_dyload_nvenc(AVCodecContext *avctx)
{
    PNVENCODEAPICREATEINSTANCE nvEncodeAPICreateInstance = 0;
    NVENCSTATUS nvstatus;

    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;

    if (!nvenc_check_cuda(avctx))
        return 0;

    if (dl_fn->nvenc_lib)
        return 1;

#if defined(_WIN32)
    if (sizeof(void*) == 8) {
        dl_fn->nvenc_lib = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
    } else {
        dl_fn->nvenc_lib = LoadLibrary(TEXT("nvEncodeAPI.dll"));
    }
#else
    dl_fn->nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
#endif

    if (!dl_fn->nvenc_lib) {
        av_log(avctx, AV_LOG_FATAL, "Failed loading the nvenc library\n");
        goto error;
    }

    nvEncodeAPICreateInstance = (PNVENCODEAPICREATEINSTANCE)LOAD_FUNC(dl_fn->nvenc_lib, "NvEncodeAPICreateInstance");
    if (!nvEncodeAPICreateInstance) {
        av_log(avctx, AV_LOG_FATAL, "Failed to load nvenc entrypoint\n");
        goto error;
    }

    dl_fn->nvenc_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    nvstatus = nvEncodeAPICreateInstance(&dl_fn->nvenc_funcs);
    if (nvstatus != NV_ENC_SUCCESS) {
        av_log(avctx, AV_LOG_FATAL, "Failed to create nvenc instance\n");
        goto error;
    }

    return 1;

error:
    if (dl_fn->nvenc_lib)
        DL_CLOSE_FUNC(dl_fn->nvenc_lib);

    dl_fn->nvenc_lib = NULL;
    return 0;
}

static av_cold void nvenc_unload_nvenc(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;

    DL_CLOSE_FUNC(dl_fn->nvenc_lib);
    dl_fn->nvenc_lib = NULL;
    deinit_cuda();
    av_log(avctx, AV_LOG_VERBOSE, "Nvenc unloaded\n");
}

static av_cold int nvenc_encode_init(AVCodecContext *avctx)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS encode_session_params = { 0 };
    NV_ENC_REGISTER_RESOURCE registerParams = { 0 };
    NV_ENC_PRESET_CONFIG preset_config = { 0 };
    CudaDynLoadFunctions *p_cuda;
    CUresult cu_res;
    GUID encoder_preset = NV_ENC_PRESET_HQ_GUID;
    GUID codec;
    NVENCSTATUS nv_status = NV_ENC_SUCCESS;
    AVCPBProperties *cpb_props;
    int surfaceCount = 0;
    int i, num_mbs;
    int isLL = 0;
    int lossless = 0;
    int res = 0;
    int dw, dh;
    int qp_inter_p;
    extern char color_ptx[];

    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    if (!nvenc_dyload_nvenc(avctx))
        return AVERROR_EXTERNAL;

    ctx->last_dts = AV_NOPTS_VALUE;

    ctx->encode_config.version = NV_ENC_CONFIG_VER;
    ctx->init_encode_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
    encode_session_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    encode_session_params.apiVersion = NVENCAPI_VERSION;

    cu_res = get_cuda_context(&ctx->cu_context, ctx->gpu);
    p_cuda = get_cuda_dl_func();

    if (cu_res != CUDA_SUCCESS) {
        av_log(avctx, AV_LOG_FATAL, "Failed creating CUDA context for NVENC: 0x%x\n", (int)cu_res);
        res = AVERROR_EXTERNAL;
        goto error;
    }
	av_log(avctx, AV_LOG_VERBOSE, "NVENC : Cuda Context created 0x%x\n", (int)ctx->cu_context);

    __cu(p_cuda->cu_module_load_data(&ctx->cu_module, color_ptx));
    __cu(p_cuda->cu_module_get_function(&ctx->cu_func_interleaveChroma, ctx->cu_module, "interleaveChroma"));

    encode_session_params.device = ctx->cu_context;
    encode_session_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;

    nv_status = p_nvenc->nvEncOpenEncodeSessionEx(&encode_session_params, &ctx->nvencoder);
    if (nv_status != NV_ENC_SUCCESS) {
        ctx->nvencoder = NULL;
        av_log(avctx, AV_LOG_FATAL, "OpenEncodeSessionEx failed: 0x%x\n", (int)nv_status);
        res = AVERROR_EXTERNAL;
        goto error;
    }

    if (ctx->preset) {
        if (!strcmp(ctx->preset, "slow")) {
            encoder_preset = NV_ENC_PRESET_HQ_GUID;
            ctx->twopass = 1;
        } else if (!strcmp(ctx->preset, "medium")) {
            encoder_preset = NV_ENC_PRESET_HQ_GUID;
            ctx->twopass = 0;
        } else if (!strcmp(ctx->preset, "fast")) {
            encoder_preset = NV_ENC_PRESET_HP_GUID;
            ctx->twopass = 0;
        } else if (!strcmp(ctx->preset, "hq")) {
            encoder_preset = NV_ENC_PRESET_HQ_GUID;
        } else if (!strcmp(ctx->preset, "hp")) {
            encoder_preset = NV_ENC_PRESET_HP_GUID;
        } else if (!strcmp(ctx->preset, "bd")) {
            encoder_preset = NV_ENC_PRESET_BD_GUID;
        } else if (!strcmp(ctx->preset, "ll")) {
            encoder_preset = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;
            isLL = 1;
        } else if (!strcmp(ctx->preset, "llhp")) {
            encoder_preset = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
            isLL = 1;
        } else if (!strcmp(ctx->preset, "llhq")) {
            encoder_preset = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
            isLL = 1;
        } else if (!strcmp(ctx->preset, "lossless")) {
            encoder_preset = NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;
            lossless = 1;
        } else if (!strcmp(ctx->preset, "losslesshp")) {
            encoder_preset = NV_ENC_PRESET_LOSSLESS_HP_GUID;
            lossless = 1;
        } else if (!strcmp(ctx->preset, "default")) {
            encoder_preset = NV_ENC_PRESET_DEFAULT_GUID;
        } else {
            av_log(avctx, AV_LOG_FATAL, "Preset \"%s\" is unknown! Supported presets: slow, medium, high, hp, hq, bd, ll, llhp, llhq, lossless, losslesshp, default\n", ctx->preset);
            res = AVERROR(EINVAL);
            goto error;
        }
    }

    if (ctx->twopass < 0) {
        ctx->twopass = isLL;
    }

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        codec = NV_ENC_CODEC_H264_GUID;
        break;
    case AV_CODEC_ID_H265:
        codec = NV_ENC_CODEC_HEVC_GUID;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unknown codec name\n");
        res = AVERROR(EINVAL);
        goto error;
    }

    nv_status = p_nvenc->nvEncGetEncodePresetConfig(ctx->nvencoder, codec, encoder_preset, &preset_config);
    if (nv_status != NV_ENC_SUCCESS) {
        av_log(avctx, AV_LOG_FATAL, "GetEncodePresetConfig failed: 0x%x\n", (int)nv_status);
        res = AVERROR_EXTERNAL;
        goto error;
    }

    ctx->init_encode_params.encodeGUID = codec;
    ctx->init_encode_params.encodeHeight = avctx->height;
    ctx->init_encode_params.encodeWidth = avctx->width;

    if (avctx->sample_aspect_ratio.num && avctx->sample_aspect_ratio.den &&
        (avctx->sample_aspect_ratio.num != 1 || avctx->sample_aspect_ratio.num != 1)) {
        av_reduce(&dw, &dh,
                  avctx->width * avctx->sample_aspect_ratio.num,
                  avctx->height * avctx->sample_aspect_ratio.den,
                  1024 * 1024);
        ctx->init_encode_params.darHeight = dh;
        ctx->init_encode_params.darWidth = dw;
    } else {
        ctx->init_encode_params.darHeight = avctx->height;
        ctx->init_encode_params.darWidth = avctx->width;
    }

    // De-compensate for hardware, dubiously, trying to compensate for
    // playback at 704 pixel width.
    if (avctx->width == 720 &&
        (avctx->height == 480 || avctx->height == 576)) {
        av_reduce(&dw, &dh,
                  ctx->init_encode_params.darWidth * 44,
                  ctx->init_encode_params.darHeight * 45,
                  1024 * 1024);
        ctx->init_encode_params.darHeight = dh;
        ctx->init_encode_params.darWidth = dw;
    }

    ctx->init_encode_params.frameRateNum = avctx->time_base.den;
    ctx->init_encode_params.frameRateDen = avctx->time_base.num * avctx->ticks_per_frame;

    num_mbs = ((avctx->width + 15) >> 4) * ((avctx->height + 15) >> 4);
    ctx->max_surface_count = (num_mbs >= 8160) ? 32 : 48;

    if (ctx->buffer_delay >= ctx->max_surface_count)
        ctx->buffer_delay = ctx->max_surface_count - 1;

    ctx->init_encode_params.enableEncodeAsync = 0;
    ctx->init_encode_params.enablePTD = 1;

    ctx->init_encode_params.presetGUID = encoder_preset;

    ctx->init_encode_params.encodeConfig = &ctx->encode_config;
    memcpy(&ctx->encode_config, &preset_config.presetCfg, sizeof(ctx->encode_config));
    ctx->encode_config.version = NV_ENC_CONFIG_VER;

    if (avctx->refs >= 0) {
        /* 0 means "let the hardware decide" */
        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            ctx->encode_config.encodeCodecConfig.h264Config.maxNumRefFrames = avctx->refs;
            break;
        case AV_CODEC_ID_H265:
            ctx->encode_config.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = avctx->refs;
            break;
        /* Earlier switch/case will return if unknown codec is passed. */
        }
    }

    if (avctx->gop_size > 0) {
        if (avctx->max_b_frames >= 0) {
            /* 0 is intra-only, 1 is I/P only, 2 is one B Frame, 3 two B frames, and so on. */
            ctx->encode_config.frameIntervalP = avctx->max_b_frames + 1;
        }

        ctx->encode_config.gopLength = avctx->gop_size;
        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            ctx->encode_config.encodeCodecConfig.h264Config.idrPeriod = avctx->gop_size;
            break;
        case AV_CODEC_ID_H265:
            ctx->encode_config.encodeCodecConfig.hevcConfig.idrPeriod = avctx->gop_size;
            break;
        /* Earlier switch/case will return if unknown codec is passed. */
        }
    } else if (avctx->gop_size == 0) {
        ctx->encode_config.frameIntervalP = 0;
        ctx->encode_config.gopLength = 1;
        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            ctx->encode_config.encodeCodecConfig.h264Config.idrPeriod = 1;
            break;
        case AV_CODEC_ID_H265:
            ctx->encode_config.encodeCodecConfig.hevcConfig.idrPeriod = 1;
            break;
        /* Earlier switch/case will return if unknown codec is passed. */
        }
    }

    /* when there're b frames, set dts offset */
    if (ctx->encode_config.frameIntervalP >= 2)
        ctx->last_dts = -2;

    if (avctx->bit_rate > 0)
        ctx->encode_config.rcParams.averageBitRate = avctx->bit_rate;

    if (avctx->rc_max_rate > 0)
        ctx->encode_config.rcParams.maxBitRate = avctx->rc_max_rate;

    if (lossless) {
        if (avctx->codec->id == AV_CODEC_ID_H264)
            ctx->encode_config.encodeCodecConfig.h264Config.qpPrimeYZeroTransformBypassFlag = 1;

        ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        ctx->encode_config.rcParams.constQP.qpInterB = 0;
        ctx->encode_config.rcParams.constQP.qpInterP = 0;
        ctx->encode_config.rcParams.constQP.qpIntra = 0;

        avctx->qmin = -1;
        avctx->qmax = -1;
    } else if (ctx->cbr) {
        if (!ctx->twopass) {
            ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        } else {
            ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_2_PASS_QUALITY;

            if (avctx->codec->id == AV_CODEC_ID_H264) {
                ctx->encode_config.encodeCodecConfig.h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE;
                ctx->encode_config.encodeCodecConfig.h264Config.fmoMode = NV_ENC_H264_FMO_DISABLE;
            }
        }
    } else if (avctx->global_quality > 0) {
        ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        ctx->encode_config.rcParams.constQP.qpInterB = avctx->global_quality;
        ctx->encode_config.rcParams.constQP.qpInterP = avctx->global_quality;
        ctx->encode_config.rcParams.constQP.qpIntra = avctx->global_quality;

        avctx->qmin = -1;
        avctx->qmax = -1;
    } else {
        if (avctx->qmin >= 0 && avctx->qmax >= 0) {
            ctx->encode_config.rcParams.enableMinQP = 1;
            ctx->encode_config.rcParams.enableMaxQP = 1;

            ctx->encode_config.rcParams.minQP.qpInterB = avctx->qmin;
            ctx->encode_config.rcParams.minQP.qpInterP = avctx->qmin;
            ctx->encode_config.rcParams.minQP.qpIntra = avctx->qmin;

            ctx->encode_config.rcParams.maxQP.qpInterB = avctx->qmax;
            ctx->encode_config.rcParams.maxQP.qpInterP = avctx->qmax;
            ctx->encode_config.rcParams.maxQP.qpIntra = avctx->qmax;

            qp_inter_p = (avctx->qmax + 3 * avctx->qmin) / 4; // biased towards Qmin

            if (ctx->twopass) {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_2_PASS_VBR;
                if (avctx->codec->id == AV_CODEC_ID_H264) {
                    ctx->encode_config.encodeCodecConfig.h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE;
                    ctx->encode_config.encodeCodecConfig.h264Config.fmoMode = NV_ENC_H264_FMO_DISABLE;
                }
            } else {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR_MINQP;
            }
        } else {
            qp_inter_p = 26; // default to 26

            if (ctx->twopass) {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_2_PASS_VBR;
            } else {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            }
        }

        ctx->encode_config.rcParams.enableInitialRCQP = 1;
        ctx->encode_config.rcParams.initialRCQP.qpInterP  = qp_inter_p;

        if(avctx->i_quant_factor != 0.0 && avctx->b_quant_factor != 0.0) {
            ctx->encode_config.rcParams.initialRCQP.qpIntra = av_clip(
                qp_inter_p * fabs(avctx->i_quant_factor) + avctx->i_quant_offset, 0, 51);
            ctx->encode_config.rcParams.initialRCQP.qpInterB = av_clip(
                qp_inter_p * fabs(avctx->b_quant_factor) + avctx->b_quant_offset, 0, 51);
        } else {
            ctx->encode_config.rcParams.initialRCQP.qpIntra = qp_inter_p;
            ctx->encode_config.rcParams.initialRCQP.qpInterB = qp_inter_p;
        }
    }

    if (avctx->rc_buffer_size > 0)
        ctx->encode_config.rcParams.vbvBufferSize = avctx->rc_buffer_size;

    if (avctx->flags & AV_CODEC_FLAG_INTERLACED_DCT) {
        ctx->encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD;
    } else {
        ctx->encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    }

    if (ctx->aq)
    {
        ctx->encode_config.rcParams.enableAQ = 1;
    }
    else
    {
        ctx->encode_config.rcParams.enableAQ = 0;
    }

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag = 1;
        ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.videoSignalTypePresentFlag = 1;

        ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix = avctx->colorspace;
        ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries = avctx->color_primaries;
        ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics = avctx->color_trc;

        ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag = avctx->color_range == AVCOL_RANGE_JPEG;

        ctx->encode_config.encodeCodecConfig.h264Config.sliceMode = 3;
        ctx->encode_config.encodeCodecConfig.h264Config.sliceModeData = 1;

        ctx->encode_config.encodeCodecConfig.h264Config.disableSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 1 : 0;
        ctx->encode_config.encodeCodecConfig.h264Config.repeatSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 0 : 1;

        if (!ctx->profile) {
            switch (avctx->profile) {
            case FF_PROFILE_H264_BASELINE:
                ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
                break;
            case FF_PROFILE_H264_MAIN:
                ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
                break;
            case FF_PROFILE_H264_HIGH:
            case FF_PROFILE_UNKNOWN:
                ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
                break;
            default:
                av_log(avctx, AV_LOG_WARNING, "Unsupported profile requested, falling back to high\n");
                ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
                break;
            }
        } else {
            if (!strcmp(ctx->profile, "high")) {
                ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
                avctx->profile = FF_PROFILE_H264_HIGH;
            } else if (!strcmp(ctx->profile, "main")) {
                ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
                avctx->profile = FF_PROFILE_H264_MAIN;
            } else if (!strcmp(ctx->profile, "baseline")) {
                ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
                avctx->profile = FF_PROFILE_H264_BASELINE;
            } else {
                av_log(avctx, AV_LOG_FATAL, "Profile \"%s\" is unknown! Supported profiles: high, main, baseline\n", ctx->profile);
                res = AVERROR(EINVAL);
                goto error;
            }
        }

        ctx->encode_config.encodeCodecConfig.h264Config.chromaFormatIDC = 1;

        if (ctx->level) {
            res = input_string_to_uint32(avctx, nvenc_h264_level_pairs, ctx->level, &ctx->encode_config.encodeCodecConfig.h264Config.level);

            if (res) {
                av_log(avctx, AV_LOG_FATAL, "Level \"%s\" is unknown! Supported levels: auto, 1, 1b, 1.1, 1.2, 1.3, 2, 2.1, 2.2, 3, 3.1, 3.2, 4, 4.1, 4.2, 5, 5.1\n", ctx->level);
                goto error;
            }
        } else {
            ctx->encode_config.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
        }

        break;
    case AV_CODEC_ID_H265:
        ctx->encode_config.encodeCodecConfig.hevcConfig.sliceMode = 3;
        ctx->encode_config.encodeCodecConfig.hevcConfig.sliceModeData = 1;

        ctx->encode_config.encodeCodecConfig.hevcConfig.disableSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 1 : 0;
        ctx->encode_config.encodeCodecConfig.hevcConfig.repeatSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 0 : 1;

        /* No other profile is supported in the current SDK version 5 */
        ctx->encode_config.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
        avctx->profile = FF_PROFILE_HEVC_MAIN;

        if (ctx->level) {
            res = input_string_to_uint32(avctx, nvenc_hevc_level_pairs, ctx->level, &ctx->encode_config.encodeCodecConfig.hevcConfig.level);

            if (res) {
                av_log(avctx, AV_LOG_FATAL, "Level \"%s\" is unknown! Supported levels: auto, 1, 2, 2.1, 3, 3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, 6.2\n", ctx->level);
                goto error;
            }
        } else {
            ctx->encode_config.encodeCodecConfig.hevcConfig.level = NV_ENC_LEVEL_AUTOSELECT;
        }

        if (ctx->tier) {
            if (!strcmp(ctx->tier, "main")) {
                ctx->encode_config.encodeCodecConfig.hevcConfig.tier = NV_ENC_TIER_HEVC_MAIN;
            } else if (!strcmp(ctx->tier, "high")) {
                ctx->encode_config.encodeCodecConfig.hevcConfig.tier = NV_ENC_TIER_HEVC_HIGH;
            } else {
                av_log(avctx, AV_LOG_FATAL, "Tier \"%s\" is unknown! Supported tiers: main, high\n", ctx->tier);
                res = AVERROR(EINVAL);
                goto error;
            }
        }

        break;
    /* Earlier switch/case will return if unknown codec is passed. */
    }

    nv_status = p_nvenc->nvEncInitializeEncoder(ctx->nvencoder, &ctx->init_encode_params);
    if (nv_status != NV_ENC_SUCCESS) {
        av_log(avctx, AV_LOG_FATAL, "InitializeEncoder failed: 0x%x\n", (int)nv_status);
        res = AVERROR_EXTERNAL;
        goto error;
    }

    av_log(avctx, AV_LOG_VERBOSE, "Nvenc initialized successfully\n");

    ctx->input_surfaces = av_malloc(ctx->max_surface_count * sizeof(*ctx->input_surfaces));

    if (!ctx->input_surfaces) {
        res = AVERROR(ENOMEM);
        goto error;
    }

    ctx->output_surfaces = av_malloc(ctx->max_surface_count * sizeof(*ctx->output_surfaces));

    if (!ctx->output_surfaces) {
        res = AVERROR(ENOMEM);
        goto error;
    }

    // Allocation for temp surface used for sys mem -> device mem transfer
    if (avctx->pix_fmt == AV_PIX_FMT_YUV420P)
    {
		ctx->transferSurf.width = (avctx->width + 31) & ~31;
        ctx->transferSurf.height = (avctx->height + 31) & ~31;
        p_cuda->cu_mem_alloc_pitch(&ctx->transferSurf.dptr,
                &ctx->transferSurf.pitch,
                ctx->transferSurf.width,
                ctx->transferSurf.height/ 2, 16);
    }

    for (surfaceCount = 0; surfaceCount < ctx->max_surface_count; ++surfaceCount) {
        NV_ENC_CREATE_BITSTREAM_BUFFER allocOut = { 0 };
        allocOut.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

        ctx->input_surfaces[surfaceCount].width = (avctx->width + 31) & ~31;
        ctx->input_surfaces[surfaceCount].height = (avctx->height + 31) & ~31;

        switch (avctx->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_NV12:
            ctx->input_surfaces[surfaceCount].format = NV_ENC_BUFFER_FORMAT_NV12_PL;
            p_cuda->cu_mem_alloc_pitch(&ctx->input_surfaces[surfaceCount].dptr,
                    &ctx->input_surfaces[surfaceCount].pitch,
                    ctx->input_surfaces[surfaceCount].width,
                    ctx->input_surfaces[surfaceCount].height * 3 / 2, 16);
            break;

        default:
            av_log(avctx, AV_LOG_FATAL, "Invalid input pixel format\n");
            res = AVERROR(EINVAL);
            goto error;
        }

        registerParams.version = NV_ENC_REGISTER_RESOURCE_VER,
        registerParams.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,
        registerParams.width = ctx->input_surfaces[surfaceCount].width,
        registerParams.height = ctx->input_surfaces[surfaceCount].height,
        registerParams.pitch = ctx->input_surfaces[surfaceCount].pitch,
        registerParams.bufferFormat = ctx->input_surfaces[surfaceCount].format;
        registerParams.resourceToRegister = (void*)ctx->input_surfaces[surfaceCount].dptr,
        nv_status = p_nvenc->nvEncRegisterResource(ctx->nvencoder, &registerParams);
        if (nv_status != NV_ENC_SUCCESS) {
            av_log(avctx, AV_LOG_FATAL, "RegisterResource failed\n");
            res = AVERROR_EXTERNAL;
            goto error;
        }
        ctx->input_surfaces[surfaceCount].hRes = registerParams.registeredResource;
        ctx->input_surfaces[surfaceCount].lockCount = 0;

        /* 1MB is large enough to hold most output frames. NVENC increases this automaticaly if it's not enough. */
        allocOut.size = 1024 * 1024;

        allocOut.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;

        nv_status = p_nvenc->nvEncCreateBitstreamBuffer(ctx->nvencoder, &allocOut);
        if (nv_status != NV_ENC_SUCCESS) {
            av_log(avctx, AV_LOG_FATAL, "CreateBitstreamBuffer failed\n");
            ctx->output_surfaces[surfaceCount++].output_surface = NULL;
            res = AVERROR_EXTERNAL;
            goto error;
        }

        ctx->output_surfaces[surfaceCount].output_surface = allocOut.bitstreamBuffer;
        ctx->output_surfaces[surfaceCount].size = allocOut.size;
        ctx->output_surfaces[surfaceCount].busy = 0;
    }

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        uint32_t outSize = 0;
        char tmpHeader[256];
        NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = { 0 };
        payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;

        payload.spsppsBuffer = tmpHeader;
        payload.inBufferSize = sizeof(tmpHeader);
        payload.outSPSPPSPayloadSize = &outSize;

        nv_status = p_nvenc->nvEncGetSequenceParams(ctx->nvencoder, &payload);
        if (nv_status != NV_ENC_SUCCESS) {
            av_log(avctx, AV_LOG_FATAL, "GetSequenceParams failed\n");
            goto error;
        }

        avctx->extradata_size = outSize;
        avctx->extradata = av_mallocz(outSize + AV_INPUT_BUFFER_PADDING_SIZE);

        if (!avctx->extradata) {
            res = AVERROR(ENOMEM);
            goto error;
        }

        memcpy(avctx->extradata, tmpHeader, outSize);
    }

    if (ctx->encode_config.frameIntervalP > 1)
        avctx->has_b_frames = 2;

    if (ctx->encode_config.rcParams.averageBitRate > 0)
        avctx->bit_rate = ctx->encode_config.rcParams.averageBitRate;

    cpb_props = ff_add_cpb_side_data(avctx);
    if (!cpb_props)
        return AVERROR(ENOMEM);
    cpb_props->max_bitrate = ctx->encode_config.rcParams.maxBitRate;
    cpb_props->avg_bitrate = avctx->bit_rate;
    cpb_props->buffer_size = ctx->encode_config.rcParams.vbvBufferSize;

    return 0;

error:
    for (i = 0; i < surfaceCount; ++i) {
        p_nvenc->nvEncUnregisterResource(ctx->nvencoder, ctx->input_surfaces[i].hRes);
        p_cuda->cu_mem_free(ctx->input_surfaces[i].dptr);

        if (ctx->output_surfaces[i].output_surface)
            p_nvenc->nvEncDestroyBitstreamBuffer(ctx->nvencoder, ctx->output_surfaces[i].output_surface);
    }

	p_cuda->cu_mem_free(ctx->transferSurf.dptr);
    if (ctx->nvencoder)
        p_nvenc->nvEncDestroyEncoder(ctx->nvencoder);

    if (ctx->cu_context)
        release_cuda_context(&ctx->cu_context, ctx->gpu);

    nvenc_unload_nvenc(avctx);
    ctx->nvencoder = NULL;
    ctx->cu_context = NULL;

    return res;
}

static av_cold int nvenc_encode_close(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;
    CudaDynLoadFunctions *p_cuda = get_cuda_dl_func();
    int i;

    av_freep(&ctx->timestamp_list.data);
    av_freep(&ctx->output_surface_ready_queue.data);
    av_freep(&ctx->output_surface_queue.data);

    for (i = 0; i < ctx->max_surface_count; ++i) {
        p_nvenc->nvEncUnregisterResource(ctx->nvencoder, ctx->input_surfaces[i].hRes);
        p_cuda->cu_mem_free(ctx->input_surfaces[i].dptr);
        p_nvenc->nvEncDestroyBitstreamBuffer(ctx->nvencoder, ctx->output_surfaces[i].output_surface);
    }
    ctx->max_surface_count = 0;

    if (ctx->nvencoder)
        p_nvenc->nvEncDestroyEncoder(ctx->nvencoder);

    ctx->nvencoder = NULL;

    if (ctx->cu_context)
        release_cuda_context(&ctx->cu_context, ctx->gpu);

    nvenc_unload_nvenc(avctx);

    return 0;
}

static int process_output_surface(AVCodecContext *avctx, AVPacket *pkt, NvencOutputSurface *tmpoutsurf)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    uint32_t slice_mode_data;
    uint32_t *slice_offsets;
    char picType = 'X';
    NV_ENC_LOCK_BITSTREAM lock_params = { 0 };
    NVENCSTATUS nv_status;
    int res = 0;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
      slice_mode_data = ctx->encode_config.encodeCodecConfig.h264Config.sliceModeData;
      break;
    case AV_CODEC_ID_H265:
      slice_mode_data = ctx->encode_config.encodeCodecConfig.hevcConfig.sliceModeData;
      break;
    default:
      av_log(avctx, AV_LOG_ERROR, "Unknown codec name\n");
      res = AVERROR(EINVAL);
      goto error;
    }
    slice_offsets = av_mallocz(slice_mode_data * sizeof(*slice_offsets));

    if (!slice_offsets)
        return AVERROR(ENOMEM);

    lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;

    lock_params.doNotWait = 0;
    lock_params.outputBitstream = tmpoutsurf->output_surface;
    lock_params.sliceOffsets = slice_offsets;

    nv_status = p_nvenc->nvEncLockBitstream(ctx->nvencoder, &lock_params);
    if (nv_status != NV_ENC_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed locking bitstream buffer\n");
        res = AVERROR_EXTERNAL;
        goto error;
    }

    if (res = ff_alloc_packet2(avctx, pkt, lock_params.bitstreamSizeInBytes,0)) {
        p_nvenc->nvEncUnlockBitstream(ctx->nvencoder, tmpoutsurf->output_surface);
        goto error;
    }

    memcpy(pkt->data, lock_params.bitstreamBufferPtr, lock_params.bitstreamSizeInBytes);

    nv_status = p_nvenc->nvEncUnlockBitstream(ctx->nvencoder, tmpoutsurf->output_surface);
    if (nv_status != NV_ENC_SUCCESS)
        av_log(avctx, AV_LOG_ERROR, "Failed unlocking bitstream buffer, expect the gates of mordor to open\n");

    switch (lock_params.pictureType) {
    case NV_ENC_PIC_TYPE_IDR:
        pkt->flags |= AV_PKT_FLAG_KEY;
#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    case NV_ENC_PIC_TYPE_I:
        avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;
        picType = 'I';
        break;
    case NV_ENC_PIC_TYPE_P:
        avctx->coded_frame->pict_type = AV_PICTURE_TYPE_P;
        picType = 'P';
        break;
    case NV_ENC_PIC_TYPE_B:
        avctx->coded_frame->pict_type = AV_PICTURE_TYPE_B;
        picType = 'B';
        break;
    case NV_ENC_PIC_TYPE_BI:
        avctx->coded_frame->pict_type = AV_PICTURE_TYPE_BI;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unknown picture type encountered, expect the output to be broken.\n");
        av_log(avctx, AV_LOG_ERROR, "Please report this error and include as much information on how to reproduce it as possible.\n");
        res = AVERROR_EXTERNAL;
        goto error;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    }

    av_log(avctx, AV_LOG_VERBOSE, "FRAME STATISTICS: Frame No. %d  PicType %c Frame AvgQP %d  SATD Cost %d  Size %d bytes\r", lock_params.frameIdx, picType, lock_params.frameAvgQP, lock_params.frameSatd, lock_params.bitstreamSizeInBytes);

    pkt->pts = lock_params.outputTimeStamp;
    pkt->dts = timestamp_queue_dequeue(&ctx->timestamp_list);

    /* when there're b frame(s), set dts offset */
    if (ctx->encode_config.frameIntervalP >= 2)
        pkt->dts -= 1;

    if (pkt->dts > pkt->pts)
        pkt->dts = pkt->pts;

    if (ctx->last_dts != AV_NOPTS_VALUE && pkt->dts <= ctx->last_dts)
        pkt->dts = ctx->last_dts + 1;

    ctx->last_dts = pkt->dts;

    av_free(slice_offsets);

    return 0;

error:

    av_free(slice_offsets);
    timestamp_queue_dequeue(&ctx->timestamp_list);

    return res;
}


static int call_interleavechroma_kernel(CudaDynLoadFunctions* dl_func, CUfunction func,
    CUdeviceptr cb_dptr, CUdeviceptr cr_dptr, CUdeviceptr nv12chroma_dptr, int width, int height, int srcStride, int dstStride)
{
    void *args_uchar[] = { &cb_dptr, &cr_dptr, &nv12chroma_dptr, &width, &height, &srcStride, &dstStride};
    __cu(dl_func->cu_launch_kernel(func, BLOCKSX, 1, 1, THREADSX, 1, 1, 0, NULL, args_uchar, NULL));

    return 0;
}

static int nvenc_copy_to_inputbuffer(NvencContext *ctx, const AVFrame* frame, NvencInputSurface *inSurf)
{
    CudaDynLoadFunctions *p_cuda = get_cuda_dl_func();
    if (frame->format == AV_PIX_FMT_NV12) {

        // check opaque field, if there's already a deviceptr
        if (frame->opaque && check_nvinfo(frame->opaque) &&
            ((ffnvinfo*)(frame->opaque))->dptr[0]) {
            ffnvinfo* info = (ffnvinfo*)frame->opaque;

            __cu(cuMemCpy2d(NULL, info->dptr[0], info->linesize[0], NULL, inSurf->dptr, inSurf->pitch, frame->width, frame->height, CU_MEMORYTYPE_DEVICE, CU_MEMORYTYPE_DEVICE));
            __cu(cuMemCpy2d(NULL, info->dptr[1], info->linesize[1], NULL, inSurf->dptr + inSurf->pitch*inSurf->height, inSurf->pitch, frame->width, frame->height/2, CU_MEMORYTYPE_DEVICE, CU_MEMORYTYPE_DEVICE));
        }
        else
        {
            __cu(cuMemCpy2d(frame->data[0], (CUdeviceptr)NULL, frame->linesize[0], NULL, inSurf->dptr, inSurf->pitch, frame->width, frame->height, CU_MEMORYTYPE_HOST, CU_MEMORYTYPE_DEVICE));
            __cu(cuMemCpy2d(frame->data[1], (CUdeviceptr)NULL, frame->linesize[1], NULL, inSurf->dptr + inSurf->pitch*inSurf->height, inSurf->pitch, frame->width, frame->height / 2, CU_MEMORYTYPE_HOST, CU_MEMORYTYPE_DEVICE));
        }
    }
    else if (frame->format == AV_PIX_FMT_YUV420P) {
        // check opaque field, if there's already a deviceptr
        if (frame->opaque && check_nvinfo(frame->opaque) &&
            ((ffnvinfo*)(frame->opaque))->dptr[0]) {
            ffnvinfo* info = (ffnvinfo*)frame->opaque;

            __cu(cuMemCpy2d(NULL, info->dptr[0], info->linesize[0], NULL, inSurf->dptr, inSurf->pitch, frame->width, frame->height, CU_MEMORYTYPE_DEVICE, CU_MEMORYTYPE_DEVICE));
            call_interleavechroma_kernel(p_cuda, ctx->cu_func_interleaveChroma, info->dptr[1], info->dptr[2], inSurf->dptr + inSurf->pitch*inSurf->height, (frame->width+31) & ~31 , frame->height, (info->linesize[1]<<1), inSurf->pitch);
        }
        else
        {
            __cu(cuMemCpy2d(frame->data[0], (CUdeviceptr)NULL, frame->linesize[0], NULL, inSurf->dptr, inSurf->pitch, frame->width, frame->height, CU_MEMORYTYPE_HOST, CU_MEMORYTYPE_DEVICE));
            __cu(cuMemCpy2d(frame->data[1], (CUdeviceptr)NULL, frame->linesize[1], NULL, ctx->transferSurf.dptr, ctx->transferSurf.pitch / 2, ctx->transferSurf.width / 2, frame->height / 2, CU_MEMORYTYPE_HOST, CU_MEMORYTYPE_DEVICE));
            __cu(cuMemCpy2d(frame->data[2], (CUdeviceptr)NULL, frame->linesize[2], NULL, ctx->transferSurf.dptr + ctx->transferSurf.pitch*ctx->transferSurf.height / 4, ctx->transferSurf.pitch / 2, ctx->transferSurf.width / 2, frame->height / 2, CU_MEMORYTYPE_HOST, CU_MEMORYTYPE_DEVICE));

            call_interleavechroma_kernel(p_cuda, ctx->cu_func_interleaveChroma, ctx->transferSurf.dptr, ctx->transferSurf.dptr + ctx->transferSurf.pitch*ctx->transferSurf.height/4, inSurf->dptr + inSurf->pitch*inSurf->height, (frame->width + 31) & ~31, frame->height, ctx->transferSurf.pitch, inSurf->pitch);
        }
    }
    else {
        av_log(NULL, AV_LOG_FATAL, "Invalid pixel format!\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int nvenc_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
    const AVFrame *frame, int *got_packet)
{
    NVENCSTATUS nv_status;
    NvencOutputSurface *tmpoutsurf;
    int res, i = 0;

    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    NV_ENC_PIC_PARAMS pic_params = { 0 };
    pic_params.version = NV_ENC_PIC_PARAMS_VER;

    if (frame) {
        NV_ENC_MAP_INPUT_RESOURCE mapParams = { 0 };
        NvencInputSurface *inSurf = NULL;

        for (i = 0; i < ctx->max_surface_count; ++i) {
            if (!ctx->input_surfaces[i].lockCount) {
                inSurf = &ctx->input_surfaces[i];
                break;
            }
        }

        av_assert0(inSurf);

        inSurf->lockCount = 1;

        mapParams.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapParams.registeredResource = inSurf->hRes;
        nv_status = p_nvenc->nvEncMapInputResource(ctx->nvencoder, &mapParams);
        if (nv_status != NV_ENC_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed mapping nvenc input buffer\n");
            return 0;
        }

        inSurf->input_surface = mapParams.mappedResource;
        if (inSurf->format != mapParams.mappedBufferFmt) {
            av_log(avctx, AV_LOG_ERROR, "Incompatible buffer format!\n");
            return 0;
        }

        if (nvenc_copy_to_inputbuffer(ctx, frame, inSurf) != 0) {
            p_nvenc->nvEncUnmapInputResource(ctx->nvencoder, inSurf->input_surface);
            av_log(avctx, AV_LOG_ERROR, "Failed to copy data to NVENC input buffer!\n");
            return 0;
        }

        nv_status = p_nvenc->nvEncUnmapInputResource(ctx->nvencoder, inSurf->input_surface);
        if (nv_status != NV_ENC_SUCCESS) {
            av_log(avctx, AV_LOG_FATAL, "Failed unlocking input buffer!\n");
            return AVERROR_EXTERNAL;
        }

        for (i = 0; i < ctx->max_surface_count; ++i)
            if (!ctx->output_surfaces[i].busy)
                break;

        if (i == ctx->max_surface_count) {
            inSurf->lockCount = 0;
            av_log(avctx, AV_LOG_FATAL, "No free output surface found!\n");
            return AVERROR_EXTERNAL;
        }

        ctx->output_surfaces[i].input_surface = inSurf;

        pic_params.inputBuffer = inSurf->input_surface;
        pic_params.bufferFmt = inSurf->format;
        pic_params.inputWidth = avctx->width;
        pic_params.inputHeight = avctx->height;
        pic_params.outputBitstream = ctx->output_surfaces[i].output_surface;
        pic_params.completionEvent = 0;

        if (avctx->flags & AV_CODEC_FLAG_INTERLACED_DCT) {
            if (frame->top_field_first) {
                pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM;
            } else {
                pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP;
            }
        } else {
            pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        }

        pic_params.encodePicFlags = 0;
        pic_params.inputTimeStamp = frame->pts;
        pic_params.inputDuration = 0;
        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
          pic_params.codecPicParams.h264PicParams.sliceMode = ctx->encode_config.encodeCodecConfig.h264Config.sliceMode;
          pic_params.codecPicParams.h264PicParams.sliceModeData = ctx->encode_config.encodeCodecConfig.h264Config.sliceModeData;
          break;
        case AV_CODEC_ID_H265:
          pic_params.codecPicParams.hevcPicParams.sliceMode = ctx->encode_config.encodeCodecConfig.hevcConfig.sliceMode;
          pic_params.codecPicParams.hevcPicParams.sliceModeData = ctx->encode_config.encodeCodecConfig.hevcConfig.sliceModeData;
          break;
        default:
          av_log(avctx, AV_LOG_ERROR, "Unknown codec name\n");
          return AVERROR(EINVAL);
        }

        res = timestamp_queue_enqueue(&ctx->timestamp_list, frame->pts);

        if (res)
            return res;
    } else {
        pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    }

    nv_status = p_nvenc->nvEncEncodePicture(ctx->nvencoder, &pic_params);

    if (frame && nv_status == NV_ENC_ERR_NEED_MORE_INPUT) {
        res = out_surf_queue_enqueue(&ctx->output_surface_queue, &ctx->output_surfaces[i]);

        if (res)
            return res;

        ctx->output_surfaces[i].busy = 1;
    }

    if (nv_status != NV_ENC_SUCCESS && nv_status != NV_ENC_ERR_NEED_MORE_INPUT) {
        av_log(avctx, AV_LOG_ERROR, "EncodePicture failed!\n");
        return AVERROR_EXTERNAL;
    }

    if (nv_status != NV_ENC_ERR_NEED_MORE_INPUT) {
        while (ctx->output_surface_queue.count) {
            tmpoutsurf = out_surf_queue_dequeue(&ctx->output_surface_queue);
            res = out_surf_queue_enqueue(&ctx->output_surface_ready_queue, tmpoutsurf);

            if (res)
                return res;
        }

        if (frame) {
            res = out_surf_queue_enqueue(&ctx->output_surface_ready_queue, &ctx->output_surfaces[i]);

            if (res)
                return res;

            ctx->output_surfaces[i].busy = 1;
        }
    }

    if (ctx->output_surface_ready_queue.count && (!frame || ctx->output_surface_ready_queue.count + ctx->output_surface_queue.count >= ctx->buffer_delay)) {
        tmpoutsurf = out_surf_queue_dequeue(&ctx->output_surface_ready_queue);

        res = process_output_surface(avctx, pkt, tmpoutsurf);

        if (res)
            return res;

        tmpoutsurf->busy = 0;
        av_assert0(tmpoutsurf->input_surface->lockCount);
        tmpoutsurf->input_surface->lockCount--;

        *got_packet = 1;
    } else {
        *got_packet = 0;
    }

    return 0;
}

static const enum AVPixelFormat pix_fmts_nvenc[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NONE
};

#define OFFSET(x) offsetof(NvencContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "preset", "Set the encoding preset (one of slow = hq 2pass, medium = hq, fast = hp, hq, hp, bd, ll, llhq, llhp, default)", OFFSET(preset), AV_OPT_TYPE_STRING, { .str = "hq" }, 0, 0, VE },
    { "profile", "Set the encoding profile (high, main, baseline)", OFFSET(profile), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
    { "level", "Set the encoding level restriction (auto, 1.0, 1.0b, 1.1, 1.2, ..., 4.2, 5.0, 5.1)", OFFSET(level), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
    { "tier", "Set the encoding tier (main or high)", OFFSET(tier), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
    { "cbr", "Use cbr encoding mode", OFFSET(cbr), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "2pass", "Use 2pass encoding mode", OFFSET(twopass), AV_OPT_TYPE_BOOL, { .i64 = -1 }, -1, 1, VE },
    { "gpu", "Selects which NVENC capable GPU to use. First GPU is 0, second is 1, and so on.", OFFSET(gpu), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "delay", "Delays frame output by the given amount of frames.", OFFSET(buffer_delay), AV_OPT_TYPE_INT, { .i64 = INT_MAX }, 0, INT_MAX, VE },
    { "enableaq", "set to 1 to enable AQ ", OFFSET(aq), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { NULL }
};

static const AVCodecDefault nvenc_defaults[] = {
    { "b", "0" },
    { "qmin", "-1" },
    { "qmax", "-1" },
    { "qdiff", "-1" },
    { "qblur", "-1" },
    { "qcomp", "-1" },
    { NULL },
};

#if CONFIG_NVENC_ENCODER
static const AVClass nvenc_class = {
    .class_name = "nvenc",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_nvenc_encoder = {
    .name = "nvenc",
    .long_name = NULL_IF_CONFIG_SMALL("NVIDIA NVENC h264 encoder"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(NvencContext),
    .init = nvenc_encode_init,
    .encode2 = nvenc_encode_frame,
    .close = nvenc_encode_close,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class = &nvenc_class,
    .defaults = nvenc_defaults,
    .pix_fmts = pix_fmts_nvenc,
};
#endif

/* Add an alias for nvenc_h264 */
#if CONFIG_NVENC_H264_ENCODER
static const AVClass nvenc_h264_class = {
    .class_name = "nvenc_h264",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_nvenc_h264_encoder = {
    .name = "nvenc_h264",
    .long_name = NULL_IF_CONFIG_SMALL("NVIDIA NVENC h264 encoder"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(NvencContext),
    .init = nvenc_encode_init,
    .encode2 = nvenc_encode_frame,
    .close = nvenc_encode_close,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class = &nvenc_h264_class,
    .defaults = nvenc_defaults,
    .pix_fmts = pix_fmts_nvenc,
};
#endif

#if CONFIG_NVENC_HEVC_ENCODER
static const AVClass nvenc_hevc_class = {
    .class_name = "nvenc_hevc",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_nvenc_hevc_encoder = {
    .name = "nvenc_hevc",
    .long_name = NULL_IF_CONFIG_SMALL("NVIDIA NVENC hevc encoder"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H265,
    .priv_data_size = sizeof(NvencContext),
    .init = nvenc_encode_init,
    .encode2 = nvenc_encode_frame,
    .close = nvenc_encode_close,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class = &nvenc_hevc_class,
    .defaults = nvenc_defaults,
    .pix_fmts = pix_fmts_nvenc,
};
#endif
