#include <gst/gst.h>
#include <unistd.h>
#include <webkit/webkit-web-process-extension.h>

static gboolean g_hw_only_video = TRUE;
static gboolean g_block_dgpu_plugins = TRUE;

static void set_feature_rank(GstRegistry *registry, const char *name, guint rank) {
    GstPluginFeature *feature = gst_registry_lookup_feature(registry, name);
    if (feature) {
        gst_plugin_feature_set_rank(feature, rank);
        gst_object_unref(feature);
    }
}

static void apply_gstreamer_ranks(gboolean init_if_needed) {
    if (!gst_is_initialized()) {
        if (!init_if_needed) {
            return;
        }
        GError *err = NULL;
        if (!gst_init_check(NULL, NULL, &err)) {
            if (err) g_error_free(err);
            return;
        }
    }

    GstRegistry *registry = gst_registry_get();
    if (!registry) return;

    /* Prefer VA-API hardware decoders cleanly; missing plugins on future distros are ignored safely. */
    set_feature_rank(registry, "vavp9dec", GST_RANK_PRIMARY + 200);
    set_feature_rank(registry, "vah264dec", GST_RANK_PRIMARY + 200);
    set_feature_rank(registry, "vah265dec", GST_RANK_PRIMARY + 200);
    set_feature_rank(registry, "vaav1dec", GST_RANK_PRIMARY + 200);

    /* Avoid waking discrete GPU decoders/CUDA uploaders when iGPU zero-copy mode is active. */
    const guint dgpu_rank = g_block_dgpu_plugins ? GST_RANK_NONE : GST_RANK_MARGINAL;
    static const char *const nv_features[] = {
        "nvav1dec", "nvvp9dec", "nvh264dec", "nvh265dec",
        "nvvp8dec", "nvjpegdec", "cudaupload", "cudadownload",
        "cudaconvert", "cudaconvertscale", "cudascale", "cudacompositor"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(nv_features); i++) {
        set_feature_rank(registry, nv_features[i], dgpu_rank);
    }

    /* Demote software AV1/VP9 decoders below hardware VA-API decoders. */
    if (g_hw_only_video) {
        set_feature_rank(registry, "dav1ddec", GST_RANK_NONE);
        set_feature_rank(registry, "av1dec", GST_RANK_NONE);
        set_feature_rank(registry, "vp9dec", GST_RANK_MARGINAL);
        set_feature_rank(registry, "avdec_vp9", GST_RANK_MARGINAL);
        set_feature_rank(registry, "avdec_h264", GST_RANK_MARGINAL);
    } else {
        set_feature_rank(registry, "dav1ddec", GST_RANK_MARGINAL);
        set_feature_rank(registry, "vp9dec", GST_RANK_SECONDARY);
    }
}

static void on_page_created(WebKitWebProcessExtension *extension,
                            WebKitWebPage             *web_page,
                            gpointer                   user_data) {
    (void)extension;
    (void)user_data;

    guint64 page_id = webkit_web_page_get_id(web_page);
    gint32 pid = (gint32)getpid();
    GVariant *params = g_variant_new("(ti)", page_id, pid);
    WebKitUserMessage *msg = webkit_user_message_new("explorer.process-pid", params);
    webkit_web_page_send_message_to_view(web_page, msg, NULL, NULL, NULL);
}

static gboolean on_user_message_received(WebKitWebProcessExtension *extension,
                                         WebKitUserMessage         *message,
                                         gpointer                   user_data) {
    (void)extension;
    (void)user_data;

    const char *name = webkit_user_message_get_name(message);
    if (g_strcmp0(name, "explorer.power-policy") == 0) {
        GVariant *params = webkit_user_message_get_parameters(message);
        if (params && g_variant_is_of_type(params, G_VARIANT_TYPE("(bb)"))) {
            g_variant_get(params, "(bb)", &g_hw_only_video, &g_block_dgpu_plugins);
            apply_gstreamer_ranks(TRUE);
        }
        return TRUE;
    }
    return FALSE;
}

G_MODULE_EXPORT void
webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *extension,
                                                       const GVariant            *user_data) {
    if (user_data && g_variant_is_of_type(user_data, G_VARIANT_TYPE("(bb)"))) {
        g_variant_get((GVariant *)user_data, "(bb)", &g_hw_only_video, &g_block_dgpu_plugins);
    }

    apply_gstreamer_ranks(FALSE);

    g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
    g_signal_connect(extension, "user-message-received", G_CALLBACK(on_user_message_received), NULL);
}
