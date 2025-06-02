/* 
 * Copyright (C)2014 Teluu Inc. (http://www.teluu.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#include <pjmedia-codec/passthrough_h264.h>
#include <pjmedia-codec/h264_packetizer.h>
#include <pjmedia/vid_codec_util.h>
#include <pjmedia/errno.h>
#include <pj/log.h>
#include "venc_wrapper.h"

#if defined(PJMEDIA_HAS_PASSTHROUGH_H264_CODEC) && \
PJMEDIA_HAS_PASSTHROUGH_H264_CODEC != 0 && \
    defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

#ifdef _MSC_VER
#   include <stdint.h>
#   pragma comment( lib, "openh264.lib")
#endif

/* OpenH264: */
//#include <wels/codec_api.h>
//#include <wels/codec_app_def.h>

/*
 * Constants
 */
#define THIS_FILE               "passthrough_h264.cpp"

#if (defined(PJ_DARWINOS) && PJ_DARWINOS != 0 && TARGET_OS_IPHONE) || \
     defined(__ANDROID__)
#  define DEFAULT_WIDTH         352
#  define DEFAULT_HEIGHT        288
#else
#  define DEFAULT_WIDTH         640
#  define DEFAULT_HEIGHT        480
#endif

#define DEFAULT_FPS             20
#define DEFAULT_AVG_BITRATE     256000
#define DEFAULT_MAX_BITRATE     256000

#define MAX_RX_WIDTH            1200
#define MAX_RX_HEIGHT           800

/* OpenH264 default PT */
#define ss_h264_PT                PJMEDIA_RTP_PT_H264

/* Minimum interval (in msec) between generating two missing keyframe events.
 * This is to avoid sending too many events during consecutive decode
 * failures.
 */
#define MISSING_KEYFRAME_EV_MIN_INTERVAL        1000

/*
 * Factory operations.
 */
static pj_status_t ss_h264_test_alloc(pjmedia_vid_codec_factory *factory,
                                    const pjmedia_vid_codec_info *info );
static pj_status_t ss_h264_default_attr(pjmedia_vid_codec_factory *factory,
                                      const pjmedia_vid_codec_info *info,
                                      pjmedia_vid_codec_param *attr );
static pj_status_t ss_h264_enum_info(pjmedia_vid_codec_factory *factory,
                                   unsigned *count,
                                   pjmedia_vid_codec_info codecs[]);
static pj_status_t ss_h264_alloc_codec(pjmedia_vid_codec_factory *factory,
                                     const pjmedia_vid_codec_info *info,
                                     pjmedia_vid_codec **p_codec);
static pj_status_t ss_h264_dealloc_codec(pjmedia_vid_codec_factory *factory,
                                       pjmedia_vid_codec *codec );


/*
 * Codec operations
 */
static pj_status_t ss_h264_codec_init(pjmedia_vid_codec *codec,
                                    pj_pool_t *pool );
static pj_status_t ss_h264_codec_open(pjmedia_vid_codec *codec,
                                    pjmedia_vid_codec_param *param );
static pj_status_t ss_h264_codec_close(pjmedia_vid_codec *codec);
static pj_status_t ss_h264_codec_modify(pjmedia_vid_codec *codec,
                                      const pjmedia_vid_codec_param *param);
static pj_status_t ss_h264_codec_get_param(pjmedia_vid_codec *codec,
                                         pjmedia_vid_codec_param *param);
static pj_status_t ss_h264_codec_encode_begin(pjmedia_vid_codec *codec,
                                            const pjmedia_vid_encode_opt *opt,
                                            const pjmedia_frame *input,
                                            unsigned out_size,
                                            pjmedia_frame *output,
                                            pj_bool_t *has_more);
static pj_status_t ss_h264_codec_encode_more(pjmedia_vid_codec *codec,
                                           unsigned out_size,
                                           pjmedia_frame *output,
                                           pj_bool_t *has_more);
static pj_status_t ss_h264_codec_decode(pjmedia_vid_codec *codec,
                                      pj_size_t count,
                                      pjmedia_frame packets[],
                                      unsigned out_size,
                                      pjmedia_frame *output);

/* Definition for OpenH264 codecs operations. */
static pjmedia_vid_codec_op ss_h264_codec_op =
{
    &ss_h264_codec_init,
    &ss_h264_codec_open,
    &ss_h264_codec_close,
    &ss_h264_codec_modify,
    &ss_h264_codec_get_param,
    &ss_h264_codec_encode_begin,
    &ss_h264_codec_encode_more,
    &ss_h264_codec_decode,
    NULL
};

/* Definition for OpenH264 codecs factory operations. */
static pjmedia_vid_codec_factory_op ss_h264_factory_op =
{
    &ss_h264_test_alloc,
    &ss_h264_default_attr,
    &ss_h264_enum_info,
    &ss_h264_alloc_codec,
    &ss_h264_dealloc_codec
};


static struct ss_h264_factory
{
    pjmedia_vid_codec_factory    base;
    pjmedia_vid_codec_mgr       *mgr;
    pj_pool_factory             *pf;
    pj_pool_t                   *pool;
} ss_h264_factory;


typedef struct ss_h264_codec_data
{
    pj_pool_t                   *pool;
    pjmedia_vid_codec_param     *prm;
    pj_bool_t                    whole;
    pjmedia_h264_packetizer     *pktz;

    /* Encoder state */
    //ISVCEncoder                 *enc;
    //SSourcePicture              *esrc_pic;
    unsigned                     enc_input_size;
    pj_uint8_t                  *enc_frame_whole;
    unsigned                     enc_frame_size;
    unsigned                     enc_processed;
    pj_timestamp                 ets;
    //SFrameBSInfo                 bsi;
    int                          ilayer;

    /* Decoder state */
    //ISVCDecoder                 *dec;
    pj_uint8_t                  *dec_buf;
    unsigned                     dec_buf_size;
    unsigned                     missing_kf_interval;
    unsigned                     last_missing_kf_event;
} ss_h264_codec_data;

struct SLayerPEncCtx
{
    pj_int32_t                  iDLayerQp;
    //SSliceArgument              sSliceArgument;
};

static void log_print(void* ctx, int level, const char* string) {
    PJ_UNUSED_ARG(ctx);
    PJ_LOG(4,("[OPENH264_LOG]", "[L%d] %s", level, string));
}

PJ_DEF(pj_status_t) pjmedia_codec_passthrough_h264_vid_init(pjmedia_vid_codec_mgr *mgr,
                                                    pj_pool_factory *pf)
{
    const pj_str_t h264_name = { (char*)"H264", 4};
    pj_status_t status;

    if (ss_h264_factory.pool != NULL) {
        /* Already initialized. */
        return PJ_SUCCESS;
    }

    if (!mgr) mgr = pjmedia_vid_codec_mgr_instance();
    PJ_ASSERT_RETURN(mgr, PJ_EINVAL);

    /* Create OpenH264 codec factory. */
    ss_h264_factory.base.op = &ss_h264_factory_op;
    ss_h264_factory.base.factory_data = NULL;
    ss_h264_factory.mgr = mgr;
    ss_h264_factory.pf = pf;
    ss_h264_factory.pool = pj_pool_create(pf, "ss_h264factory", 256, 256, NULL);
    if (!ss_h264_factory.pool)
        return PJ_ENOMEM;

    /* Registering format match for SDP negotiation */
    status = pjmedia_sdp_neg_register_fmt_match_cb(
                                        &h264_name,
                                        &pjmedia_vid_codec_h264_match_sdp);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Register codec factory to codec manager. */
    status = pjmedia_vid_codec_mgr_register_factory(mgr,
                                                    &ss_h264_factory.base);
    if (status != PJ_SUCCESS)
        goto on_error;

    PJ_LOG(4,(THIS_FILE, "OpenH264 codec initialized"));

    /* Done. */
    return PJ_SUCCESS;

on_error:
    pj_pool_release(ss_h264_factory.pool);
    ss_h264_factory.pool = NULL;
    return status;
}

/*
 * Unregister OpenH264 codecs factory from pjmedia endpoint.
 */
PJ_DEF(pj_status_t) pjmedia_codec_passthrough_h264_vid_deinit(void)
{
    pj_status_t status = PJ_SUCCESS;

    if (ss_h264_factory.pool == NULL) {
        /* Already deinitialized */
        return PJ_SUCCESS;
    }

    /* Unregister OpenH264 codecs factory. */
    status = pjmedia_vid_codec_mgr_unregister_factory(ss_h264_factory.mgr,
                                                      &ss_h264_factory.base);

    /* Destroy pool. */
    pj_pool_release(ss_h264_factory.pool);
    ss_h264_factory.pool = NULL;

    return status;
}

static pj_status_t ss_h264_test_alloc(pjmedia_vid_codec_factory *factory,
                                    const pjmedia_vid_codec_info *info )
{
    PJ_ASSERT_RETURN(factory == &ss_h264_factory.base, PJ_EINVAL);

    if (info->fmt_id == PJMEDIA_FORMAT_H264 &&
        info->pt == ss_h264_PT)
    {
        return PJ_SUCCESS;
    }

    return PJMEDIA_CODEC_EUNSUP;
}

static pj_status_t ss_h264_default_attr(pjmedia_vid_codec_factory *factory,
                                      const pjmedia_vid_codec_info *info,
                                      pjmedia_vid_codec_param *attr )
{
    PJ_ASSERT_RETURN(factory == &ss_h264_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info && attr, PJ_EINVAL);

    pj_bzero(attr, sizeof(pjmedia_vid_codec_param));

    attr->dir = PJMEDIA_DIR_ENCODING_DECODING;
    attr->packing = PJMEDIA_VID_PACKING_PACKETS;

    /* Encoded format */
    pjmedia_format_init_video(&attr->enc_fmt, PJMEDIA_FORMAT_H264,
                              DEFAULT_WIDTH, DEFAULT_HEIGHT,
                              DEFAULT_FPS, 1);

    /* Decoded format */
    pjmedia_format_init_video(&attr->dec_fmt, PJMEDIA_FORMAT_I420,
                              DEFAULT_WIDTH, DEFAULT_HEIGHT,
                              DEFAULT_FPS, 1);

    /* Decoding fmtp */
    attr->dec_fmtp.cnt = 2;
    attr->dec_fmtp.param[0].name = pj_str((char*)"profile-level-id");
    attr->dec_fmtp.param[0].val = pj_str((char*)"42e01e");
    attr->dec_fmtp.param[1].name = pj_str((char*)" packetization-mode");
    attr->dec_fmtp.param[1].val = pj_str((char*)"1");

    /* Bitrate */
    attr->enc_fmt.det.vid.avg_bps = DEFAULT_AVG_BITRATE;
    attr->enc_fmt.det.vid.max_bps = DEFAULT_MAX_BITRATE;

    /* Encoding MTU */
    attr->enc_mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

    return PJ_SUCCESS;
}

static pj_status_t ss_h264_enum_info(pjmedia_vid_codec_factory *factory,
                                   unsigned *count,
                                   pjmedia_vid_codec_info info[])
{
    PJ_ASSERT_RETURN(info && *count > 0, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &ss_h264_factory.base, PJ_EINVAL);

    *count = 1;
    info->fmt_id = PJMEDIA_FORMAT_H264;
    info->pt = ss_h264_PT;
    info->encoding_name = pj_str((char*)"H264");
    info->encoding_desc = pj_str((char*)"OpenH264 codec");
    info->clock_rate = 90000;
    info->dir = PJMEDIA_DIR_ENCODING_DECODING;
    info->dec_fmt_id_cnt = 1;
    info->dec_fmt_id[0] = PJMEDIA_FORMAT_I420;
    info->packings = PJMEDIA_VID_PACKING_PACKETS |
                     PJMEDIA_VID_PACKING_WHOLE;
    info->fps_cnt = 3;
    info->fps[0].num = 15;
    info->fps[0].denum = 1;
    info->fps[1].num = 25;
    info->fps[1].denum = 1;
    info->fps[2].num = 30;
    info->fps[2].denum = 1;

    return PJ_SUCCESS;

}

static pj_status_t ss_h264_alloc_codec(pjmedia_vid_codec_factory *factory,
                                     const pjmedia_vid_codec_info *info,
                                     pjmedia_vid_codec **p_codec)
{
    pj_pool_t *pool;
    pjmedia_vid_codec *codec;
    ss_h264_codec_data *ss_h264_data;
    int rc;

    PJ_ASSERT_RETURN(factory == &ss_h264_factory.base && info && p_codec,
                     PJ_EINVAL);

    *p_codec = NULL;

    pool = pj_pool_create(ss_h264_factory.pf, "ss_h264%p", 512, 512, NULL);
    if (!pool)
        return PJ_ENOMEM;

    /* codec instance */
    codec = PJ_POOL_ZALLOC_T(pool, pjmedia_vid_codec);
    codec->factory = factory;
    codec->op = &ss_h264_codec_op;

    /* codec data */
    ss_h264_data = PJ_POOL_ZALLOC_T(pool, ss_h264_codec_data);
    ss_h264_data->pool = pool;
    codec->codec_data = ss_h264_data;

    *p_codec = codec;
    return PJ_SUCCESS;

on_error:
    ss_h264_dealloc_codec(factory, codec);
    return PJMEDIA_CODEC_EFAILED;
}

static pj_status_t ss_h264_dealloc_codec(pjmedia_vid_codec_factory *factory,
                                       pjmedia_vid_codec *codec )
{
    ss_h264_codec_data *ss_h264_data;

    PJ_ASSERT_RETURN(codec, PJ_EINVAL);

    PJ_UNUSED_ARG(factory);

    ss_h264_data = (ss_h264_codec_data*) codec->codec_data;
    pj_pool_release(ss_h264_data->pool);
    return PJ_SUCCESS;
}

static pj_status_t ss_h264_codec_init(pjmedia_vid_codec *codec,
                                    pj_pool_t *pool )
{
    PJ_ASSERT_RETURN(codec && pool, PJ_EINVAL);
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    return PJ_SUCCESS;
}

static pj_status_t ss_h264_codec_open(pjmedia_vid_codec *codec,
                                    pjmedia_vid_codec_param *codec_param )
{
    ss_h264_codec_data    *ss_h264_data;
    pjmedia_vid_codec_param     *param;
    pjmedia_h264_packetizer_cfg  pktz_cfg;
    pjmedia_vid_codec_h264_fmtp  h264_fmtp;
    int                  rc;
    pj_status_t          status;

    PJ_ASSERT_RETURN(codec && codec_param, PJ_EINVAL);

    PJ_LOG(5,(THIS_FILE, "Opening codec.."));

    ss_h264_data = (ss_h264_codec_data*) codec->codec_data;
    ss_h264_data->prm = pjmedia_vid_codec_param_clone( ss_h264_data->pool,
                                                     codec_param);
    param = ss_h264_data->prm;

    /* Parse remote fmtp */
    pj_bzero(&h264_fmtp, sizeof(h264_fmtp));
    status = pjmedia_vid_codec_h264_parse_fmtp(&param->enc_fmtp, &h264_fmtp);
    if (status != PJ_SUCCESS)
        return status;

    /* Apply SDP fmtp to format in codec param */
    if (!param->ignore_fmtp) {
        status = pjmedia_vid_codec_h264_apply_fmtp(param);
        if (status != PJ_SUCCESS)
            return status;
    }

    pj_bzero(&pktz_cfg, sizeof(pktz_cfg));
    pktz_cfg.mtu = param->enc_mtu;
    /* Packetization mode */
#if 0
    if (h264_fmtp.packetization_mode == 0)
        pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL;
    else if (h264_fmtp.packetization_mode == 1)
        pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED;
    else
        return PJ_ENOTSUP;
#else
    if (h264_fmtp.packetization_mode!=
                                PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL &&
        h264_fmtp.packetization_mode!=
                                PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED)
    {
        return PJ_ENOTSUP;
    }
    /* Better always send in single NAL mode for better compatibility */
    pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED;
#endif

    status = pjmedia_h264_packetizer_create(ss_h264_data->pool, &pktz_cfg,
                                            &ss_h264_data->pktz);
    if (status != PJ_SUCCESS)
        return status;

    ss_h264_data->whole = (param->packing == PJMEDIA_VID_PACKING_WHOLE);

    ss_h264_data->dec_buf_size = (MAX_RX_WIDTH * MAX_RX_HEIGHT * 3 >> 1) +
                               (MAX_RX_WIDTH);
    ss_h264_data->dec_buf = (pj_uint8_t*)pj_pool_alloc(ss_h264_data->pool,
                                                     ss_h264_data->dec_buf_size);

    /* Need to update param back after values are negotiated */
    pj_memcpy(codec_param, param, sizeof(*codec_param));

    VencOpenStream(SIP_VENC_CHANNEL_NUM);

    return PJ_SUCCESS;
}

static pj_status_t ss_h264_codec_close(pjmedia_vid_codec *codec)
{
    PJ_ASSERT_RETURN(codec, PJ_EINVAL);
    PJ_UNUSED_ARG(codec);
    return PJ_SUCCESS;
}

static pj_status_t ss_h264_codec_modify(pjmedia_vid_codec *codec,
                                      const pjmedia_vid_codec_param *param)
{
    return PJ_SUCCESS;
}

static pj_status_t ss_h264_codec_get_param(pjmedia_vid_codec *codec,
                                         pjmedia_vid_codec_param *param)
{
    struct ss_h264_codec_data *ss_h264_data;

    PJ_ASSERT_RETURN(codec && param, PJ_EINVAL);

    ss_h264_data = (ss_h264_codec_data*) codec->codec_data;
    pj_memcpy(param, ss_h264_data->prm, sizeof(*param));

    return PJ_SUCCESS;
}

static pj_status_t ss_h264_codec_encode_begin(pjmedia_vid_codec *codec,
                                            const pjmedia_vid_encode_opt *opt,
                                            const pjmedia_frame *input,
                                            unsigned out_size,
                                            pjmedia_frame *output,
                                            pj_bool_t *has_more)
{
    
    struct ss_h264_codec_data *ss_h264_data;
    int rc;

    PJ_ASSERT_RETURN(codec && input && out_size && output && has_more,
                     PJ_EINVAL);

    

    ss_h264_data = (ss_h264_codec_data*) codec->codec_data;

    ss_h264_data->ets = input->timestamp;
    ss_h264_data->ilayer = 0;
    //ss_h264_data->enc_frame_size = input->size;
    ss_h264_data->enc_frame_whole = (pj_uint8_t*)input->buf;
    ss_h264_data->enc_frame_size = VencGetDataDirect(SIP_VENC_CHANNEL_NUM, input->buf, input->size);
    if(ss_h264_data->enc_frame_size == 0) return PJ_SUCCESS;
    ss_h264_data->enc_processed = 0;
    
    return ss_h264_codec_encode_more(codec, out_size, output, has_more);
}


static pj_status_t ss_h264_codec_encode_more(pjmedia_vid_codec *codec,
                                           unsigned out_size,
                                           pjmedia_frame *output,
                                           pj_bool_t *has_more)
{
    struct ss_h264_codec_data *ss_h264_data;
    const pj_uint8_t *payload;
    pj_size_t payload_len;
    pj_status_t status;

    PJ_ASSERT_RETURN(codec && out_size && output && has_more,
                     PJ_EINVAL);

    ss_h264_data = (ss_h264_codec_data*) codec->codec_data;

    status = pjmedia_h264_packetize(ss_h264_data->pktz,
                                    ss_h264_data->enc_frame_whole,
                                    ss_h264_data->enc_frame_size,
                                    &ss_h264_data->enc_processed,
                                    &payload, &payload_len);

    if (status != PJ_SUCCESS) {
        /* Reset */
        #if 0
        ss_h264_data->enc_frame_size = ss_h264_data->enc_processed = 0;
        *has_more = (ss_h264_data->ilayer < ss_h264_data->bsi.iLayerNum);
        #endif
        PJ_PERROR(3,(THIS_FILE, status, "pjmedia_h264_packetize() error [2]"));
        return status;
    }   

    PJ_ASSERT_RETURN(payload_len <= out_size, PJMEDIA_CODEC_EFRMTOOSHORT);

    output->type = PJMEDIA_FRAME_TYPE_VIDEO;
    pj_memcpy(output->buf, payload, payload_len);
    output->size = payload_len;

    *has_more = (ss_h264_data->enc_processed < ss_h264_data->enc_frame_size);
    return PJ_SUCCESS;

no_frame:
    *has_more = PJ_FALSE;
    output->size = 0;
    output->type = PJMEDIA_FRAME_TYPE_NONE;
    return PJ_SUCCESS;
}

static pj_status_t ss_h264_codec_decode(pjmedia_vid_codec *codec,
                                      pj_size_t count,
                                      pjmedia_frame packets[],
                                      unsigned out_size,
                                      pjmedia_frame *output)
{
    return PJ_SUCCESS;
}

#endif  /* PJMEDIA_HAS_OPENH264_CODEC */
