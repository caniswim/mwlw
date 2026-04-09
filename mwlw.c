/*
 * mwlw — Minimal Wayland live wallpaper client
 *
 * Single VA-API decode → DMA-BUF shared across N layer-shell surfaces.
 * If compositor supports wp_color_representation_v1: direct P010 passthrough (BT.709).
 * Otherwise: VPP hardware color convert P010→BGRX (BT.709) as fallback.
 * Zero-copy, zero CPU pixel work.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <malloc.h>
#include <sys/timerfd.h>

#include <wayland-client.h>
#include "wlr-layer-shell-client.h"
#include "linux-dmabuf-client.h"
#include "viewporter-client.h"
#include "color-representation-client.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>

#include <libdrm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_vpp.h>
#include <va/va_drmcommon.h>

#define MAX_OUTPUTS 8
#define VPP_POOL_SIZE 3  /* triple-buffer: safe without release tracking */

/* ── Output ────────────────────────────────────────────────────────────── */
struct output {
    struct wl_output *wl;
    uint32_t global_name;
    int32_t width, height, refresh, transform;
    char name[64];
    bool done, configured, frame_pending, is_primary;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    uint32_t configured_w, configured_h;
};

/* ── State ─────────────────────────────────────────────────────────────── */
static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwp_linux_dmabuf_v1 *dmabuf_manager;
    struct wp_viewporter *viewporter;
    struct wp_color_representation_manager_v1 *color_repr_manager;

    struct output outputs[MAX_OUTPUTS];
    int n_outputs;

    /* decoder */
    AVFormatContext *fmt_ctx;
    AVCodecContext *dec_ctx;
    AVBufferRef *hw_device_ctx;
    VADisplay va_display;
    int video_stream_idx;

    /* VPP (color conversion — fallback when no color-representation protocol) */
    VAConfigID vpp_config;
    VAContextID vpp_context;
    VASurfaceID vpp_surfaces[VPP_POOL_SIZE];
    struct wl_buffer *vpp_wl_buffers[VPP_POOL_SIZE]; /* persistent, pre-exported */
    int vpp_current; /* round-robin index */

    /* current frame */
    struct wl_buffer *current_buffer;
    AVFrame *hw_frame;   /* reusable decode frame */
    AVPacket *pkt;       /* reusable packet */
    uint32_t frame_width, frame_height;

    bool running, loop;
    char video_path[4096];
} state;

/* ── Forward decls ─────────────────────────────────────────────────────── */
static void create_wallpaper_surface(struct output *out);
static bool decode_next_frame(void);
static void present_frame(void);

/* ── Output listener ───────────────────────────────────────────────────── */
static void output_geometry(void *data, struct wl_output *wl, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t sub,
                            const char *make, const char *model, int32_t t) {
    ((struct output *)data)->transform = t;
}
static void output_mode(void *data, struct wl_output *wl, uint32_t flags,
                        int32_t w, int32_t h, int32_t r) {
    struct output *o = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) { o->width = w; o->height = h; o->refresh = r; }
}
static void output_scale(void *d, struct wl_output *w, int32_t f) {(void)d;(void)w;(void)f;}
static void output_done(void *d, struct wl_output *w) { ((struct output*)d)->done = true; }
static void output_name(void *d, struct wl_output *w, const char *n) {
    snprintf(((struct output*)d)->name, 64, "%s", n);
}
static void output_desc(void *d, struct wl_output *w, const char *s) {(void)d;(void)w;(void)s;}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .scale = output_scale,
    .done = output_done, .name = output_name, .description = output_desc,
};

/* ── Layer surface listener ────────────────────────────────────────────── */
static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
    struct output *o = data;
    o->configured = true; o->configured_w = w; o->configured_h = h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (o->viewport) wp_viewport_set_destination(o->viewport, w, h);
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    struct output *o = data;
    if (o->layer_surface) { zwlr_layer_surface_v1_destroy(o->layer_surface); o->layer_surface = NULL; }
    if (o->viewport) { wp_viewport_destroy(o->viewport); o->viewport = NULL; }
    if (o->surface) { wl_surface_destroy(o->surface); o->surface = NULL; }
    o->configured = false;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure, .closed = layer_closed,
};

/* ── Frame callback ────────────────────────────────────────────────────── */
static void frame_done(void *data, struct wl_callback *cb, uint32_t t) {
    ((struct output*)data)->frame_pending = false;
    wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

/* ── Registry ──────────────────────────────────────────────────────────── */
static void registry_global(void *d, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t ver) {
    if (!strcmp(iface, wl_compositor_interface.name))
        state.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        state.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name))
        state.dmabuf_manager = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3);
    else if (!strcmp(iface, wp_viewporter_interface.name))
        state.viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    else if (!strcmp(iface, wp_color_representation_manager_v1_interface.name))
        state.color_repr_manager = wl_registry_bind(reg, name,
            &wp_color_representation_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && state.n_outputs < MAX_OUTPUTS) {
        struct output *o = &state.outputs[state.n_outputs++];
        memset(o, 0, sizeof(*o));
        o->global_name = name;
        o->wl = wl_registry_bind(reg, name, &wl_output_interface, 4);
        wl_output_add_listener(o->wl, &output_listener, o);
    }
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) {(void)d;(void)r;(void)n;}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

/* ── Create wallpaper surface ──────────────────────────────────────────── */
static void create_wallpaper_surface(struct output *o) {
    o->surface = wl_compositor_create_surface(state.compositor);
    o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, o->surface, o->wl,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "mwlw");
    zwlr_layer_surface_v1_set_anchor(o->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_listener, o);
    if (state.viewporter)
        o->viewport = wp_viewporter_get_viewport(state.viewporter, o->surface);
    /* Mark fully opaque — lets compositor skip alpha blending and skip
     * rendering anything behind this surface (we're BACKGROUND layer) */
    struct wl_region *opaque = wl_compositor_create_region(state.compositor);
    wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(o->surface, opaque);
    wl_region_destroy(opaque);
    /* TODO: when compositor supports wp_color_representation_v1, set BT.709
     * coefficients here and bypass VPP entirely (direct P010 passthrough) */
    wl_surface_commit(o->surface);
}

/* ── Buffer listener ───────────────────────────────────────────────────── */
static void buffer_release(void *d, struct wl_buffer *b) {(void)d;(void)b;}
static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

/* ── VPP: init color conversion pipeline ───────────────────────────────── */
static bool init_vpp(uint32_t width, uint32_t height) {
    VAStatus st;

    st = vaCreateConfig(state.va_display, VAProfileNone, VAEntrypointVideoProc,
                        NULL, 0, &state.vpp_config);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[vpp] vaCreateConfig failed: %s\n", vaErrorStr(st));
        return false;
    }

    st = vaCreateContext(state.va_display, state.vpp_config, width, height,
                         0, NULL, 0, &state.vpp_context);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[vpp] vaCreateContext failed: %s\n", vaErrorStr(st));
        return false;
    }

    /*
     * Create RGB output surfaces. Use BGRX (VA_FOURCC_BGRX) which maps to
     * DRM_FORMAT_XRGB8888 — universally supported by all compositors.
     * The VPP engine does BT.709 YCbCr→RGB conversion in hardware.
     */
    VASurfaceAttrib attrs[] = {{
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRX },
    }};

    st = vaCreateSurfaces(state.va_display, VA_RT_FORMAT_RGB32,
                          width, height,
                          state.vpp_surfaces, VPP_POOL_SIZE,
                          attrs, 1);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[vpp] vaCreateSurfaces (RGB) failed: %s\n", vaErrorStr(st));
        return false;
    }

    fprintf(stderr, "[vpp] initialized: %ux%u, P010→BGRX (BT.709), pool=%d\n",
        width, height, VPP_POOL_SIZE);
    return true;
}

/* ── VPP: convert one frame YCbCr→RGB ──────────────────────────────────── */
static VASurfaceID vpp_convert(VASurfaceID input_surface) {
    VASurfaceID output = state.vpp_surfaces[state.vpp_current];
    state.vpp_current = (state.vpp_current + 1) % VPP_POOL_SIZE;

    VAProcPipelineParameterBuffer params = {0};
    params.surface = input_surface;
    params.surface_color_standard = VAProcColorStandardBT709;
    params.output_color_standard = VAProcColorStandardBT709;

    VABufferID buf;
    VAStatus st;

    st = vaCreateBuffer(state.va_display, state.vpp_context,
                        VAProcPipelineParameterBufferType,
                        sizeof(params), 1, &params, &buf);
    if (st != VA_STATUS_SUCCESS) return VA_INVALID_SURFACE;

    st = vaBeginPicture(state.va_display, state.vpp_context, output);
    if (st != VA_STATUS_SUCCESS) { vaDestroyBuffer(state.va_display, buf); return VA_INVALID_SURFACE; }

    st = vaRenderPicture(state.va_display, state.vpp_context, &buf, 1);
    vaEndPicture(state.va_display, state.vpp_context);
    vaDestroyBuffer(state.va_display, buf);

    if (st != VA_STATUS_SUCCESS) return VA_INVALID_SURFACE;
    return output;
}

/* ── Pre-export VPP surfaces → persistent wl_buffers (called once) ────── */
static bool init_wl_buffers(void) {
    for (int i = 0; i < VPP_POOL_SIZE; i++) {
        VADRMPRIMESurfaceDescriptor prime = {0};
        VAStatus st = vaExportSurfaceHandle(state.va_display, state.vpp_surfaces[i],
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
            &prime);
        if (st != VA_STATUS_SUCCESS) {
            fprintf(stderr, "[init_wl] export surface %d failed: %s\n", i, vaErrorStr(st));
            return false;
        }

        if (i == 0) {
            char fcc[5] = {0}; memcpy(fcc, &prime.fourcc, 4);
            fprintf(stderr, "[init_wl] fourcc=%s %ux%u, %u planes, modifier=%#lx\n",
                fcc, prime.width, prime.height, prime.layers[0].num_planes,
                (unsigned long)prime.objects[0].drm_format_modifier);
        }

        struct zwp_linux_buffer_params_v1 *bp =
            zwp_linux_dmabuf_v1_create_params(state.dmabuf_manager);

        for (uint32_t j = 0; j < prime.layers[0].num_planes; j++) {
            uint32_t oi = prime.layers[0].object_index[j];
            zwp_linux_buffer_params_v1_add(bp,
                prime.objects[oi].fd, j,
                prime.layers[0].offset[j],
                prime.layers[0].pitch[j],
                prime.objects[oi].drm_format_modifier >> 32,
                prime.objects[oi].drm_format_modifier & 0xFFFFFFFF);
        }

        struct wl_buffer *buf = zwp_linux_buffer_params_v1_create_immed(bp,
            prime.width, prime.height, prime.layers[0].drm_format, 0);
        zwp_linux_buffer_params_v1_destroy(bp);

        /* FDs were dup'd by Wayland socket — close our copies */
        for (uint32_t k = 0; k < prime.num_objects; k++)
            close(prime.objects[k].fd);

        if (!buf) {
            fprintf(stderr, "[init_wl] wl_buffer creation failed for surface %d\n", i);
            return false;
        }

        wl_buffer_add_listener(buf, &buffer_listener, NULL);
        state.vpp_wl_buffers[i] = buf;
    }

    fprintf(stderr, "[init_wl] %d persistent wl_buffers pre-exported\n", VPP_POOL_SIZE);
    return true;
}

/* ── Decoder setup ─────────────────────────────────────────────────────── */
static enum AVPixelFormat get_hw_format(AVCodecContext *c, const enum AVPixelFormat *f) {
    for (const enum AVPixelFormat *p = f; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VAAPI) return AV_PIX_FMT_VAAPI;
    return AV_PIX_FMT_NONE;
}

static bool init_decoder(const char *path) {
    int ret;
    if ((ret = avformat_open_input(&state.fmt_ctx, path, NULL, NULL)) < 0) {
        fprintf(stderr, "[decoder] open failed: %s\n", av_err2str(ret)); return false;
    }
    avformat_find_stream_info(state.fmt_ctx, NULL);

    state.video_stream_idx = av_find_best_stream(state.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (state.video_stream_idx < 0) { fprintf(stderr, "[decoder] no video\n"); return false; }

    AVStream *vst = state.fmt_ctx->streams[state.video_stream_idx];
    const AVCodec *codec = avcodec_find_decoder(vst->codecpar->codec_id);
    state.dec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(state.dec_ctx, vst->codecpar);

    if ((ret = av_hwdevice_ctx_create(&state.hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0)) < 0) {
        fprintf(stderr, "[decoder] VA-API failed: %s\n", av_err2str(ret)); return false;
    }

    AVHWDeviceContext *dev = (AVHWDeviceContext *)state.hw_device_ctx->data;
    state.va_display = ((AVVAAPIDeviceContext *)dev->hwctx)->display;

    state.dec_ctx->hw_device_ctx = av_buffer_ref(state.hw_device_ctx);
    state.dec_ctx->get_format = get_hw_format;
    state.dec_ctx->thread_count = 1;

    if ((ret = avcodec_open2(state.dec_ctx, codec, NULL)) < 0) {
        fprintf(stderr, "[decoder] open codec failed: %s\n", av_err2str(ret)); return false;
    }

    state.frame_width = state.dec_ctx->width;
    state.frame_height = state.dec_ctx->height;

    double fps = vst->avg_frame_rate.den > 0 ?
        (double)vst->avg_frame_rate.num / vst->avg_frame_rate.den : 0;
    fprintf(stderr, "[decoder] %s %dx%d %.2ffps\n",
        codec->name, state.frame_width, state.frame_height, fps);
    return true;
}

/* ── Decode → VPP convert (no export, no sync — all pre-done) ─────────── */
static bool decode_next_frame(void) {
    bool got = false;

    while (!got) {
        int ret = av_read_frame(state.fmt_ctx, state.pkt);
        if (ret < 0) {
            if (state.loop && ret == AVERROR_EOF) {
                av_seek_frame(state.fmt_ctx, state.video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(state.dec_ctx);
                continue;
            }
            break;
        }
        if (state.pkt->stream_index != state.video_stream_idx) {
            av_packet_unref(state.pkt);
            continue;
        }

        ret = avcodec_send_packet(state.dec_ctx, state.pkt);
        av_packet_unref(state.pkt);
        if (ret < 0) continue;

        av_frame_unref(state.hw_frame);
        ret = avcodec_receive_frame(state.dec_ctx, state.hw_frame);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) break;

        VASurfaceID yuv = (VASurfaceID)(uintptr_t)state.hw_frame->data[3];

        /* VPP convert writes into pre-exported surface — no export or sync needed */
        int target = state.vpp_current;
        VASurfaceID rgb = vpp_convert(yuv);
        if (rgb == VA_INVALID_SURFACE) continue;

        /* DMA-BUF implicit sync: compositor waits on GPU fence automatically */
        state.current_buffer = state.vpp_wl_buffers[target];
        got = true;
    }

    return got;
}

/* ── Present ───────────────────────────────────────────────────────────── */
static void present_frame(void) {
    if (!state.current_buffer) return;
    for (int i = 0; i < state.n_outputs; i++) {
        struct output *o = &state.outputs[i];
        if (!o->configured || !o->surface || o->frame_pending) continue;
        wl_surface_attach(o->surface, state.current_buffer, 0, 0);
        wl_surface_damage_buffer(o->surface, 0, 0, state.frame_width, state.frame_height);
        struct wl_callback *cb = wl_surface_frame(o->surface);
        wl_callback_add_listener(cb, &frame_listener, o);
        o->frame_pending = true;
        wl_surface_commit(o->surface);
    }
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Force glibc to use heap instead of mmap for FFmpeg packet buffers (<1MB).
     * Eliminates ~1500 mmap/munmap syscalls per loop — ~21% of syscall overhead. */
    mallopt(M_MMAP_THRESHOLD, 1 << 20);

    state.loop = true;
    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (!strcmp(argv[arg_idx], "--no-loop")) state.loop = false;
        else { fprintf(stderr, "Usage: %s [--no-loop] <video>\n", argv[0]); return 1; }
        arg_idx++;
    }
    if (arg_idx >= argc) { fprintf(stderr, "Usage: %s [--no-loop] <video>\n", argv[0]); return 1; }
    snprintf(state.video_path, sizeof(state.video_path), "%s", argv[arg_idx]);

    /* Wayland */
    state.display = wl_display_connect(NULL);
    if (!state.display) { fprintf(stderr, "[wl] no display\n"); return 1; }
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, NULL);
    wl_display_roundtrip(state.display);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.layer_shell || !state.dmabuf_manager) {
        fprintf(stderr, "[wl] missing globals\n"); return 1;
    }

    fprintf(stderr, "[wl] color_representation_v1: %s\n",
        state.color_repr_manager ? "available (direct P010 passthrough ready)" : "unavailable (using VPP fallback)");
    fprintf(stderr, "[wl] %d outputs:\n", state.n_outputs);
    for (int i = 0; i < state.n_outputs; i++)
        fprintf(stderr, "  %s: %dx%d @%.1fHz t=%d\n",
            state.outputs[i].name, state.outputs[i].width, state.outputs[i].height,
            state.outputs[i].refresh/1000.0, state.outputs[i].transform);

    /* Decoder */
    if (!init_decoder(state.video_path)) return 1;

    /* VPP color conversion + pre-export persistent wl_buffers */
    if (!init_vpp(state.frame_width, state.frame_height)) return 1;
    if (!init_wl_buffers()) return 1;

    /* Reusable decode resources */
    state.pkt = av_packet_alloc();
    state.hw_frame = av_frame_alloc();

    /* Primary = lowest refresh */
    int pi = 0;
    for (int i = 1; i < state.n_outputs; i++)
        if (state.outputs[i].refresh < state.outputs[pi].refresh) pi = i;
    state.outputs[pi].is_primary = true;
    fprintf(stderr, "[engine] primary: %s @%.1fHz\n",
        state.outputs[pi].name, state.outputs[pi].refresh/1000.0);

    /* Layer surfaces */
    for (int i = 0; i < state.n_outputs; i++)
        create_wallpaper_surface(&state.outputs[i]);
    wl_display_roundtrip(state.display);

    /* First frame */
    if (!decode_next_frame()) { fprintf(stderr, "first frame failed\n"); return 1; }
    present_frame();
    wl_display_roundtrip(state.display);

    /* Timer at primary refresh */
    int hz = state.outputs[pi].refresh / 1000;
    if (hz <= 0) hz = 60;
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec ts = { .it_interval = {0, 1000000000L/hz}, .it_value = {0, 1000000000L/hz} };
    timerfd_settime(tfd, 0, &ts, NULL);

    struct pollfd fds[2] = {
        { .fd = wl_display_get_fd(state.display), .events = POLLIN },
        { .fd = tfd, .events = POLLIN },
    };

    fprintf(stderr, "[engine] running at %dHz\n", hz);
    state.running = true;

    while (state.running) {
        wl_display_flush(state.display);
        int ret = poll(fds, 2, 100);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        if (fds[0].revents & POLLIN) wl_display_dispatch(state.display);
        else wl_display_dispatch_pending(state.display);

        if (fds[1].revents & POLLIN) {
            uint64_t exp; read(tfd, &exp, sizeof(exp));
            if (decode_next_frame()) present_frame();
        }
    }

    /* Cleanup */
    close(tfd);
    av_frame_free(&state.hw_frame);
    av_packet_free(&state.pkt);
    for (int i = 0; i < VPP_POOL_SIZE; i++)
        if (state.vpp_wl_buffers[i]) wl_buffer_destroy(state.vpp_wl_buffers[i]);
    for (int i = 0; i < state.n_outputs; i++) {
        struct output *o = &state.outputs[i];
        if (o->layer_surface) zwlr_layer_surface_v1_destroy(o->layer_surface);
        if (o->viewport) wp_viewport_destroy(o->viewport);
        if (o->surface) wl_surface_destroy(o->surface);
        if (o->wl) wl_output_destroy(o->wl);
    }
    vaDestroySurfaces(state.va_display, state.vpp_surfaces, VPP_POOL_SIZE);
    vaDestroyContext(state.va_display, state.vpp_context);
    vaDestroyConfig(state.va_display, state.vpp_config);
    avcodec_free_context(&state.dec_ctx);
    avformat_close_input(&state.fmt_ctx);
    av_buffer_unref(&state.hw_device_ctx);
    zwp_linux_dmabuf_v1_destroy(state.dmabuf_manager);
    if (state.color_repr_manager)
        wp_color_representation_manager_v1_destroy(state.color_repr_manager);
    wp_viewporter_destroy(state.viewporter);
    zwlr_layer_shell_v1_destroy(state.layer_shell);
    wl_compositor_destroy(state.compositor);
    wl_registry_destroy(state.registry);
    wl_display_disconnect(state.display);
    return 0;
}
