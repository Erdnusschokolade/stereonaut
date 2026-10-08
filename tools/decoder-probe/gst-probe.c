/* Explicit decoder; no decodebin and no software fallback. */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <stdio.h>
#include <string.h>

static gboolean dump_frame(GstSample *sample, const char *path) {
    GstVideoInfo info;
    GstVideoFrame frame;
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample))) return FALSE;
    GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&info);
    if (fmt != GST_VIDEO_FORMAT_NV12 && fmt != GST_VIDEO_FORMAT_P010_10LE) {
        g_printerr("dump unsupported format=%s\n", gst_video_format_to_string(fmt));
        return FALSE;
    }
    if (!gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ))
        return FALSE;
    FILE *f = fopen(path, "wb");
    gboolean ok = f != NULL;
    guint bytes = GST_VIDEO_INFO_WIDTH(&info) * (fmt == GST_VIDEO_FORMAT_NV12 ? 1 : 2);
    for (guint p = 0; ok && p < 2; p++) {
        guint rows = GST_VIDEO_INFO_HEIGHT(&info) / (p ? 2 : 1);
        gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, p);
        if (stride < (gint)bytes) { ok = FALSE; break; }
        for (guint y = 0; ok && y < rows; y++)
            ok = fwrite((guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame,p) + y*stride,
                        1, bytes, f) == bytes;
    }
    if (f && fclose(f)) ok = FALSE;
    gst_video_frame_unmap(&frame);
    return ok;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc != 6) {
        g_printerr("usage: gst-probe INPUT {auto|NV12|P010_10LE} {auto|mmap|dmabuf} EXPECTED_FRAMES {DUMP_FIRST_FRAME|-}\n");
        return 2;
    }
    const char *fmt = argv[2], *mode = argv[3];
    if (strcmp(fmt,"auto") && strcmp(fmt,"NV12") && strcmp(fmt,"P010_10LE")) return 2;
    int io = !strcmp(mode,"auto") ? 0 : !strcmp(mode,"mmap") ? 2 : !strcmp(mode,"dmabuf") ? 4 : -1;
    if (io < 0) return 2;
    guint expected = (guint)g_ascii_strtoull(argv[4], NULL, 10);
    if (!expected) return 2;
    GstElement *pipeline = gst_pipeline_new("iris-probe");
    GstElement *src = gst_element_factory_make("filesrc", "source");
    GstElement *parse = gst_element_factory_make("h265parse", "parser");
    GstElement *dec = gst_element_factory_make("v4l2h265dec", "hardware_decoder");
    GstElement *filter = gst_element_factory_make("capsfilter", "output_format");
    GstElement *sink = gst_element_factory_make("appsink", "inspect");
    if (!pipeline || !src || !parse || !dec || !filter || !sink) {
        g_printerr("missing required element; no fallback\n"); return 2;
    }
    g_object_set(src, "location", argv[1], NULL);
    g_object_set(dec, "capture-io-mode", io, NULL);
    gchar *device = NULL;
    g_object_get(dec, "device", &device, NULL);
    g_print("decoder=v4l2h265dec device=%s capture_io_mode=%s\n",device,mode);
    if (g_strcmp0(device,"/dev/video22") != 0) {
        g_printerr("unexpected device; re-investigate before streaming\n"); g_free(device); return 2;
    }
    g_free(device);
    if (strcmp(fmt,"auto")) {
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, fmt, NULL);
        g_object_set(filter, "caps", caps, NULL); gst_caps_unref(caps);
    }
    g_object_set(sink, "sync", FALSE, "max-buffers", 4u, "drop", FALSE,
                 "enable-last-sample", FALSE, NULL);
    gst_bin_add_many(GST_BIN(pipeline),src,parse,dec,filter,sink,NULL);
    if (!gst_element_link_many(src,parse,dec,filter,sink,NULL)) {
        g_printerr("link failed; no fallback\n"); gst_object_unref(pipeline); return 3;
    }
    GstBus *bus = gst_element_get_bus(pipeline);
    guint frames=0, dma_frames=0;
    gboolean failed=FALSE, eos=FALSE;
    gint64 deadline=g_get_monotonic_time()+20*G_TIME_SPAN_SECOND;
    if (gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE) failed=TRUE;
    while (!failed && g_get_monotonic_time()<deadline) {
        GstSample *s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),100*GST_MSECOND);
        if (s) {
            GstBuffer *b=gst_sample_get_buffer(s);
            gboolean all_dma=gst_buffer_n_memory(b)>0;
            for (guint i=0;i<gst_buffer_n_memory(b);i++)
                all_dma &= gst_is_dmabuf_memory(gst_buffer_peek_memory(b,i));
            frames++; dma_frames += all_dma;
            if (frames==1) {
                gchar *caps=gst_caps_to_string(gst_sample_get_caps(s));
                g_print("first_frame caps=%s size=%zu memories=%u pts=%" G_GUINT64_FORMAT "\n",
                        caps,gst_buffer_get_size(b),gst_buffer_n_memory(b),GST_BUFFER_PTS(b));
                g_free(caps);
                GstVideoInfo vi;
                if (gst_video_info_from_caps(&vi,gst_sample_get_caps(s))) {
                    g_print("video_info format=%s depth=%u planes=%u\n",
                        gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&vi)),
                        GST_VIDEO_INFO_COMP_DEPTH(&vi,0),GST_VIDEO_INFO_N_PLANES(&vi));
                    for (guint p=0;p<GST_VIDEO_INFO_N_PLANES(&vi);p++)
                        g_print("video_info plane=%u offset=%zu stride=%d\n",p,vi.offset[p],vi.stride[p]);
                }
                GstVideoMeta *vm=gst_buffer_get_video_meta(b);
                if (vm) for (guint p=0;p<vm->n_planes;p++)
                    g_print("video_meta plane=%u offset=%zu stride=%d\n",p,vm->offset[p],vm->stride[p]);
                for (guint i=0;i<gst_buffer_n_memory(b);i++) {
                    GstMemory *m=gst_buffer_peek_memory(b,i);
                    gsize offset,maxsize,size=gst_memory_get_sizes(m,&offset,&maxsize);
                    g_print("memory=%u dmabuf=%d size=%zu offset=%zu maxsize=%zu fd=%d\n",i,
                        gst_is_dmabuf_memory(m),size,offset,maxsize,
                        gst_is_dmabuf_memory(m)?gst_dmabuf_memory_get_fd(m):-1);
                }
                if (strcmp(argv[5],"-")) {
                    gboolean ok=dump_frame(s,argv[5]);
                    g_print("first_frame_dump=%s success=%d\n",argv[5],ok);
                    if (!ok) failed=TRUE;
                }
            }
            gst_sample_unref(s);
        }
        GstMessage *msg;
        while ((msg=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR|GST_MESSAGE_WARNING))!=NULL) {
            GError *err=NULL; gchar *debug=NULL;
            if (GST_MESSAGE_TYPE(msg)==GST_MESSAGE_ERROR) {
                gst_message_parse_error(msg,&err,&debug); failed=TRUE;
            } else gst_message_parse_warning(msg,&err,&debug);
            g_printerr("%s source=%s message=%s debug=%s\n",GST_MESSAGE_TYPE_NAME(msg),
                GST_OBJECT_NAME(msg->src),err?err->message:"",debug?debug:"");
            g_clear_error(&err); g_free(debug); gst_message_unref(msg);
        }
        if (!s && gst_app_sink_is_eos(GST_APP_SINK(sink))) {eos=TRUE; break;}
    }
    if (!eos || frames!=expected) failed=TRUE;
    g_print("result frames=%u expected=%u dmabuf_frames=%u eos=%d failed=%d\n",
            frames,expected,dma_frames,eos,failed);
    gst_element_set_state(pipeline,GST_STATE_NULL);
    gst_object_unref(bus); gst_object_unref(pipeline);
    return failed?1:0;
}
