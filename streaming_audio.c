#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// Definições padrão para arquivos e rede
#define DEFAULT_ARQUIVO_ORIGEM "Cradles.mp4"
#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORTA 5000
#define DEFAULT_LATENCIA 200

// Flag de controle para encerramento gracioso via sinal
static volatile gboolean g_rodando = TRUE;

static void sigint_handler(int sig)
{
    (void)sig;
    g_rodando = FALSE;
}

// Dados do ramo de áudio no transmissor
typedef struct _AudioTxData
{
    GstElement *pipeline;
    GstElement *queue;
    GstElement *convert;
} AudioTxData;

// Dados do ramo de vídeo no transmissor
typedef struct _VideoTxData
{
    GstElement *pipeline;
    GstElement *queue;
    GstElement *convert;
} VideoTxData;

// Conexão dinâmica do pad de áudio decodificado
static void audio_pad_added_handler(GstElement *src, GstPad *new_pad, AudioTxData *data)
{
    (void)src;
    GstPad *sink_pad = gst_element_get_static_pad(data->queue, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    if (!sink_pad || gst_pad_is_linked(sink_pad))
        goto exit;

    new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps)
        new_pad_caps = gst_pad_query_caps(new_pad, NULL);

    if (!new_pad_caps || gst_caps_is_empty(new_pad_caps))
        goto exit;

    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    if (g_str_has_prefix(new_pad_type, "audio/x-raw"))
    {
        ret = gst_pad_link(new_pad, sink_pad);
        if (GST_PAD_LINK_FAILED(ret))
            g_printerr("Erro ao conectar pad de audio.\n");
    }

    exit:
    if (new_pad_caps != NULL)
        gst_caps_unref(new_pad_caps);
    if (sink_pad != NULL)
        gst_object_unref(sink_pad);
}

// Conexão dinâmica do pad de vídeo decodificado
static void video_pad_added_handler(GstElement *src, GstPad *new_pad, VideoTxData *data)
{
    (void)src;
    GstPad *sink_pad = gst_element_get_static_pad(data->queue, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    if (!sink_pad || gst_pad_is_linked(sink_pad))
        goto exit;

    new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps)
        new_pad_caps = gst_pad_query_caps(new_pad, NULL);

    if (!new_pad_caps || gst_caps_is_empty(new_pad_caps))
        goto exit;

    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    if (g_str_has_prefix(new_pad_type, "video/x-raw"))
    {
        ret = gst_pad_link(new_pad, sink_pad);
        if (GST_PAD_LINK_FAILED(ret))
            g_printerr("Erro ao conectar pad de video.\n");
    }

    exit:
    if (new_pad_caps != NULL)
        gst_caps_unref(new_pad_caps);
    if (sink_pad != NULL)
        gst_object_unref(sink_pad);
}

// Criação do elemento de saída de áudio compatível com o sistema
static GstElement *criar_audio_sink(void)
{
    GstElement *sink = NULL;
    const char *sinks[] = {"alsasink", "pulsesink", "autoaudiosink", NULL};

    for (int i = 0; sinks[i] != NULL; i++)
    {
        sink = gst_element_factory_make(sinks[i], "audio_sink");
        if (sink)
        {
            if (gst_element_set_state(sink, GST_STATE_READY) == GST_STATE_CHANGE_SUCCESS)
            {
                gst_element_set_state(sink, GST_STATE_NULL);
                return sink;
            }
            gst_object_unref(sink);
            sink = NULL;
        }
    }
    return gst_element_factory_make("autoaudiosink", "audio_sink");
}

// Criação do elemento de saída de vídeo compatível com o sistema
static GstElement *criar_video_sink(void)
{
    GstElement *sink = NULL;
    const char *sinks[] = {"autovideosink", "xvimagesink", "ximagesink", "glimagesink", NULL};

    for (int i = 0; sinks[i] != NULL; i++)
    {
        sink = gst_element_factory_make(sinks[i], "video_sink");
        if (sink)
        {
            if (gst_element_set_state(sink, GST_STATE_READY) == GST_STATE_CHANGE_SUCCESS)
            {
                gst_element_set_state(sink, GST_STATE_NULL);
                return sink;
            }
            gst_object_unref(sink);
            sink = NULL;
        }
    }
    return gst_element_factory_make("autovideosink", "video_sink");
}

static int verificar_arquivo(const char *arquivo)
{
    FILE *f = fopen(arquivo, "rb");
    if (!f)
    {
        g_printerr("Erro: Arquivo %s nao encontrado.\n", arquivo);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    if (fsize <= 0)
    {
        g_printerr("Erro: O arquivo %s esta vazio (0 bytes).\n", arquivo);
        return -1;
    }
    return 0;
}

// Transmissor separado em duas pipelines: áudio em `port` e vídeo em `port + 2`
int executar_transmissor(const char *ip, int port, const char *arquivo)
{
    if (verificar_arquivo(arquivo) != 0)
        return -1;

    AudioTxData audio_data = {0};
    VideoTxData video_data = {0};

    GstElement *a_source, *a_resample, *a_filter, *a_payloader, *a_sink;
    GstElement *v_source, *v_enc, *v_payloader, *v_sink;
    GstCaps *a_caps;
    gchar *uri = gst_filename_to_uri(arquivo, NULL);
    if (!uri)
    {
        g_printerr("Erro ao converter caminho do arquivo para URI.\n");
        return -1;
    }

    //Pipeline de Áudio
    audio_data.pipeline = gst_pipeline_new("pipeline-tx-audio");
    a_source = gst_element_factory_make("uridecodebin", "a_source");
    audio_data.queue = gst_element_factory_make("queue", "a_queue");
    audio_data.convert = gst_element_factory_make("audioconvert", "a_convert");
    a_resample = gst_element_factory_make("audioresample", "a_resample");
    a_filter = gst_element_factory_make("capsfilter", "a_filter");
    a_payloader = gst_element_factory_make("rtpL16pay", "a_payloader");
    a_sink = gst_element_factory_make("udpsink", "a_sink");

    if (!audio_data.pipeline || !a_source || !audio_data.queue || !audio_data.convert ||
        !a_resample || !a_filter || !a_payloader || !a_sink)
    {
        g_printerr("Erro: Falha ao criar elementos da pipeline de audio do transmissor.\n");
        g_free(uri);
        if (audio_data.pipeline) gst_object_unref(audio_data.pipeline);
        return -1;
    }

    g_object_set(a_source, "uri", uri, NULL);
    a_caps = gst_caps_from_string("audio/x-raw, format=S16BE, channels=2, rate=44100");
    g_object_set(a_filter, "caps", a_caps, NULL);
    gst_caps_unref(a_caps);
    g_object_set(a_sink, "host", ip, "port", port, "sync", TRUE, NULL);

    gst_bin_add_many(GST_BIN(audio_data.pipeline),
                     a_source, audio_data.queue, audio_data.convert,
                     a_resample, a_filter, a_payloader, a_sink, NULL);

    if (!gst_element_link_many(audio_data.queue, audio_data.convert, a_resample, a_filter, a_payloader, a_sink, NULL))
    {
        g_printerr("Erro ao linkar elementos da pipeline de audio do transmissor.\n");
        g_free(uri);
        gst_object_unref(audio_data.pipeline);
        return -1;
    }
    g_signal_connect(a_source, "pad-added", G_CALLBACK(audio_pad_added_handler), &audio_data);

    //Pipeline de Vídeo
    video_data.pipeline = gst_pipeline_new("pipeline-tx-video");
    v_source = gst_element_factory_make("uridecodebin", "v_source");
    video_data.queue = gst_element_factory_make("queue", "v_queue");
    video_data.convert = gst_element_factory_make("videoconvert", "v_convert");
    v_enc = gst_element_factory_make("x264enc", "v_enc");
    v_payloader = gst_element_factory_make("rtph264pay", "v_payloader");
    v_sink = gst_element_factory_make("udpsink", "v_sink");

    if (!video_data.pipeline || !v_source || !video_data.queue || !video_data.convert ||
        !v_enc || !v_payloader || !v_sink)
    {
        g_printerr("Erro: Falha ao criar elementos da pipeline de video do transmissor.\n");
        g_free(uri);
        gst_object_unref(audio_data.pipeline);
        if (video_data.pipeline) gst_object_unref(video_data.pipeline);
        return -1;
    }

    g_object_set(v_source, "uri", uri, NULL);
    g_free(uri);

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(v_enc), "tune"))
        g_object_set(v_enc, "tune", 0x04 /* zerolatency */, NULL);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(v_enc), "speed-preset"))
        g_object_set(v_enc, "speed-preset", 1 /* ultrafast */, NULL);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(v_payloader), "config-interval"))
        g_object_set(v_payloader, "config-interval", -1, NULL);

    g_object_set(v_sink, "host", ip, "port", port + 2, "sync", TRUE, NULL);

    gst_bin_add_many(GST_BIN(video_data.pipeline),
                     v_source, video_data.queue, video_data.convert,
                     v_enc, v_payloader, v_sink, NULL);

    if (!gst_element_link_many(video_data.queue, video_data.convert, v_enc, v_payloader, v_sink, NULL))
    {
        g_printerr("Erro ao linkar elementos da pipeline de video do transmissor.\n");
        gst_object_unref(audio_data.pipeline);
        gst_object_unref(video_data.pipeline);
        return -1;
    }
    g_signal_connect(v_source, "pad-added", G_CALLBACK(video_pad_added_handler), &video_data);

    // Inicia ambas as pipelines
    printf("Transmitindo %s para %s (audio: %d | video: %d)...\n", arquivo, ip, port, port + 2);
    fflush(stdout);

    if (gst_element_set_state(audio_data.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
        gst_element_set_state(video_data.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Erro ao iniciar transmissao das pipelines.\n");
        gst_element_set_state(audio_data.pipeline, GST_STATE_NULL);
        gst_element_set_state(video_data.pipeline, GST_STATE_NULL);
        gst_object_unref(audio_data.pipeline);
        gst_object_unref(video_data.pipeline);
        return -1;
    }

    g_rodando = TRUE;
    signal(SIGINT, sigint_handler);

    GstBus *bus_audio = gst_element_get_bus(audio_data.pipeline);
    GstBus *bus_video = gst_element_get_bus(video_data.pipeline);
    gboolean audio_ativo = TRUE, video_ativo = TRUE;

    while (g_rodando && (audio_ativo || video_ativo))
    {
        GstMessage *msg = NULL;

        if (audio_ativo)
        {
            msg = gst_bus_timed_pop_filtered(bus_audio, 250 * GST_MSECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
            if (msg)
            {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                {
                    GError *err;
                    gchar *debug_info;
                    gst_message_parse_error(msg, &err, &debug_info);
                    g_printerr("\nErro no transmissor (audio): %s\n", err->message);
                    g_clear_error(&err);
                    g_free(debug_info);
                    audio_ativo = FALSE;
                }
                else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
                {
                    audio_ativo = FALSE;
                }
                gst_message_unref(msg);
            }
        }

        if (video_ativo)
        {
            msg = gst_bus_timed_pop_filtered(bus_video, 250 * GST_MSECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
            if (msg)
            {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                {
                    GError *err;
                    gchar *debug_info;
                    gst_message_parse_error(msg, &err, &debug_info);
                    g_printerr("\nErro no transmissor (video): %s\n", err->message);
                    g_clear_error(&err);
                    g_free(debug_info);
                    video_ativo = FALSE;
                }
                else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
                {
                    video_ativo = FALSE;
                }
                gst_message_unref(msg);
            }
        }

        gint64 pos = 0, dur = 0;
        if (gst_element_query_position(audio_data.pipeline, GST_FORMAT_TIME, &pos) &&
            gst_element_query_duration(audio_data.pipeline, GST_FORMAT_TIME, &dur) && dur > 0)
        {
            printf("\rTransmitindo: %02ld:%02ld / %02ld:%02ld",
                   (long)((pos / GST_SECOND) / 60), (long)((pos / GST_SECOND) % 60),
                   (long)((dur / GST_SECOND) / 60), (long)((dur / GST_SECOND) % 60));
            fflush(stdout);
        }
    }

    printf("\nTransmissao concluida.\n");

    gst_object_unref(bus_audio);
    gst_object_unref(bus_video);
    gst_element_set_state(audio_data.pipeline, GST_STATE_NULL);
    gst_element_set_state(video_data.pipeline, GST_STATE_NULL);
    gst_object_unref(audio_data.pipeline);
    gst_object_unref(video_data.pipeline);
    return 0;
}

// Receptor separado em duas pipelines: áudio em `port` e vídeo em `port + 2`
int executar_receptor(int port, int latency)
{
    GstElement *pipe_audio, *a_src, *a_jitter, *a_depay, *a_convert, *a_resample, *a_sink;
    GstElement *pipe_video, *v_src, *v_jitter, *v_depay, *v_dec, *v_convert, *v_sink;
    GstCaps *a_caps, *v_caps;

    //Pipeline de Recepção de Áudio
    pipe_audio = gst_pipeline_new("pipeline-rx-audio");
    a_src = gst_element_factory_make("udpsrc", "a_src");
    a_jitter = gst_element_factory_make("rtpjitterbuffer", "a_jitter");
    a_depay = gst_element_factory_make("rtpL16depay", "a_depay");
    a_convert = gst_element_factory_make("audioconvert", "a_convert");
    a_resample = gst_element_factory_make("audioresample", "a_resample");
    a_sink = criar_audio_sink();

    if (!pipe_audio || !a_src || !a_jitter || !a_depay || !a_convert || !a_resample || !a_sink)
    {
        g_printerr("Erro: Falha ao criar elementos da pipeline de audio do receptor.\n");
        if (pipe_audio) gst_object_unref(pipe_audio);
        return -1;
    }

    a_caps = gst_caps_from_string("application/x-rtp, media=audio, clock-rate=44100, encoding-name=L16, channels=(int)2, payload=96");
    g_object_set(a_src, "port", port, "address", "0.0.0.0", "caps", a_caps, "timeout", (guint64)3000000000ULL, NULL);
    gst_caps_unref(a_caps);
    g_object_set(a_jitter, "latency", latency, "do-lost", TRUE, NULL);

    gst_bin_add_many(GST_BIN(pipe_audio), a_src, a_jitter, a_depay, a_convert, a_resample, a_sink, NULL);
    if (!gst_element_link_many(a_src, a_jitter, a_depay, a_convert, a_resample, a_sink, NULL))
    {
        g_printerr("Erro ao linkar elementos da pipeline de audio do receptor.\n");
        gst_object_unref(pipe_audio);
        return -1;
    }

    // --- Pipeline de Recepção de Vídeo ---
    pipe_video = gst_pipeline_new("pipeline-rx-video");
    v_src = gst_element_factory_make("udpsrc", "v_src");
    v_jitter = gst_element_factory_make("rtpjitterbuffer", "v_jitter");
    v_depay = gst_element_factory_make("rtph264depay", "v_depay");
    v_dec = gst_element_factory_make("avdec_h264", "v_dec");
    v_convert = gst_element_factory_make("videoconvert", "v_convert");
    v_sink = criar_video_sink();

    if (!pipe_video || !v_src || !v_jitter || !v_depay || !v_dec || !v_convert || !v_sink)
    {
        g_printerr("Erro: Falha ao criar elementos da pipeline de video do receptor.\n");
        gst_object_unref(pipe_audio);
        if (pipe_video) gst_object_unref(pipe_video);
        return -1;
    }

    v_caps = gst_caps_from_string("application/x-rtp, media=video, clock-rate=90000, encoding-name=H264, payload=96");
    g_object_set(v_src, "port", port + 2, "address", "0.0.0.0", "caps", v_caps, "timeout", (guint64)3000000000ULL, NULL);
    gst_caps_unref(v_caps);
    g_object_set(v_jitter, "latency", latency, "do-lost", TRUE, NULL);

    gst_bin_add_many(GST_BIN(pipe_video), v_src, v_jitter, v_depay, v_dec, v_convert, v_sink, NULL);
    if (!gst_element_link_many(v_src, v_jitter, v_depay, v_dec, v_convert, v_sink, NULL))
    {
        g_printerr("Erro ao linkar elementos da pipeline de video do receptor.\n");
        gst_object_unref(pipe_audio);
        gst_object_unref(pipe_video);
        return -1;
    }

    g_rodando = TRUE;
    signal(SIGINT, sigint_handler);

    printf("Aguardando transmissao (audio: %d | video: %d | latencia: %d ms)...\n", port, port + 2, latency);
    fflush(stdout);

    if (gst_element_set_state(pipe_audio, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
        gst_element_set_state(pipe_video, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("Erro ao iniciar reproducao no receptor.\n");
        gst_element_set_state(pipe_audio, GST_STATE_NULL);
        gst_element_set_state(pipe_video, GST_STATE_NULL);
        gst_object_unref(pipe_audio);
        gst_object_unref(pipe_video);
        return -1;
    }

    GstBus *bus_audio = gst_element_get_bus(pipe_audio);
    GstBus *bus_video = gst_element_get_bus(pipe_video);
    gboolean recebendo = FALSE;
    guint64 pushed = 0, lost = 0, late = 0, duplicates = 0, jitter = 0;

    while (g_rodando)
    {
        GstMessage *msg = gst_bus_timed_pop_filtered(bus_audio, 250 * GST_MSECOND,
                                                     GST_MESSAGE_ELEMENT | GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

        GstStructure *stats = NULL;
        g_object_get(a_jitter, "stats", &stats, NULL);
        if (stats)
        {
            gst_structure_get_uint64(stats, "num-pushed", &pushed);
            gst_structure_get_uint64(stats, "num-lost", &lost);
            gst_structure_get_uint64(stats, "num-late", &late);
            gst_structure_get_uint64(stats, "num-duplicates", &duplicates);
            gst_structure_get_uint64(stats, "avg-jitter", &jitter);
            gst_structure_free(stats);
        }

        if (pushed > 0)
        {
            if (!recebendo)
            {
                recebendo = TRUE;
                printf("Recebendo e reproduzindo midia em tempo real...\n");
            }
            printf("\r[Audio] Recebidos: %lu | Perdidos: %lu | Atrasados: %lu | Jitter: %lu ns",
                   (unsigned long)pushed, (unsigned long)lost, (unsigned long)late, (unsigned long)jitter);
            fflush(stdout);
        }

        if (msg != NULL)
        {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
            {
                GError *err;
                gchar *debug_info;
                gst_message_parse_error(msg, &err, &debug_info);
                g_printerr("\nErro no receptor (audio): %s\n", err->message);
                g_clear_error(&err);
                g_free(debug_info);
                g_rodando = FALSE;
            }
            else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
            {
                g_rodando = FALSE;
            }
            else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT)
            {
                const GstStructure *s = gst_message_get_structure(msg);
                if (gst_structure_has_name(s, "GstUDPSrcTimeout") && recebendo)
                {
                    g_rodando = FALSE;
                }
            }
            gst_message_unref(msg);
        }

        GstMessage *vmsg = gst_bus_timed_pop_filtered(bus_video, 250 * GST_MSECOND,
                                                      GST_MESSAGE_ELEMENT | GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (vmsg != NULL)
        {
            if (GST_MESSAGE_TYPE(vmsg) == GST_MESSAGE_ERROR)
            {
                GError *err;
                gchar *debug_info;
                gst_message_parse_error(vmsg, &err, &debug_info);
                g_printerr("\nErro no receptor (video): %s\n", err->message);
                g_clear_error(&err);
                g_free(debug_info);
            }
            else if (GST_MESSAGE_TYPE(vmsg) == GST_MESSAGE_ELEMENT)
            {
                const GstStructure *s = gst_message_get_structure(vmsg);
                if (gst_structure_has_name(s, "GstUDPSrcTimeout") && recebendo)
                {
                    g_rodando = FALSE;
                }
            }
            gst_message_unref(vmsg);
        }
    }

    printf("\nTransmissao concluida.\n");

    gst_object_unref(bus_audio);
    gst_object_unref(bus_video);
    gst_element_set_state(pipe_audio, GST_STATE_NULL);
    gst_element_set_state(pipe_video, GST_STATE_NULL);
    gst_object_unref(pipe_audio);
    gst_object_unref(pipe_video);

    char nome_log[128];
    snprintf(nome_log, sizeof(nome_log), "log_receptor_%dms.txt", latency);

    const char *arquivos_log[] = {nome_log, "log_receptor.txt", NULL};
    time_t agora = time(NULL);

    for (int i = 0; arquivos_log[i] != NULL; i++)
    {
        FILE *log = fopen(arquivos_log[i], "w");
        if (log)
        {
            fprintf(log, "=== RELATORIO DE STREAMING RTP/UDP ===\n");
            fprintf(log, "Data/Hora: %s", ctime(&agora));
            fprintf(log, "Porta Audio: %d | Porta Video: %d\n", port, port + 2);
            fprintf(log, "Configuracao de Latencia (rtpjitterbuffer): %d ms\n", latency);
            fprintf(log, "Pacotes recebidos (num-pushed): %lu\n", (unsigned long)pushed);
            fprintf(log, "Pacotes perdidos (num-lost): %lu\n", (unsigned long)lost);
            fprintf(log, "Pacotes atrasados (num-late): %lu\n", (unsigned long)late);
            fprintf(log, "Pacotes duplicados: %lu\n", (unsigned long)duplicates);
            fprintf(log, "Jitter medio calculado: %lu ns\n", (unsigned long)jitter);
            fprintf(log, "Status da transmissao: Concluido com sucesso\n");
            fclose(log);
        }
    }

    printf("Relatorio salvo em %s\n", nome_log);
    return 0;
}

static void exibir_ajuda(const char *prog)
{
    printf("Uso: %s [opcoes]\n", prog);
    printf("Modo Transmissor:\n");
    printf("  %s -t <IP_DESTINO> [PORTA_BASE] [ARQUIVO_MIDIA]\n", prog);
    printf("  Exemplo: %s -t 192.168.1.15 5000 Cradles.mp4\n\n", prog);
    printf("Modo Receptor:\n");
    printf("  %s -r [PORTA_BASE] [LATENCIA_MS]\n", prog);
    printf("  Exemplo: %s -r 5000 200\n\n", prog);
    printf("Modo Interativo (sem argumentos):\n");
    printf("  %s\n", prog);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    if (argc > 1)
    {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
            exibir_ajuda(argv[0]);
            return 0;
        }

        if (strcmp(argv[1], "-t") == 0 || strcmp(argv[1], "1") == 0 || strcmp(argv[1], "--transmissor") == 0)
        {
            if (argc < 3)
            {
                g_printerr("Erro: Informe o IP de destino para o transmissor.\n");
                exibir_ajuda(argv[0]);
                return -1;
            }
            const char *ip = argv[2];
            int port = (argc >= 4) ? atoi(argv[3]) : DEFAULT_PORTA;
            const char *arquivo = (argc >= 5) ? argv[4] : DEFAULT_ARQUIVO_ORIGEM;
            if (port <= 0) port = DEFAULT_PORTA;
            return executar_transmissor(ip, port, arquivo);
        }

        if (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "2") == 0 || strcmp(argv[1], "--receptor") == 0)
        {
            int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORTA;
            int latency = (argc >= 4) ? atoi(argv[3]) : DEFAULT_LATENCIA;
            if (port <= 0) port = DEFAULT_PORTA;
            if (latency <= 0) latency = DEFAULT_LATENCIA;
            return executar_receptor(port, latency);
        }

        g_printerr("Opcao invalida: %s\n", argv[1]);
        exibir_ajuda(argv[0]);
        return -1;
    }

    char opcao[16];
    char buffer[128];
    char ip[64] = DEFAULT_IP;
    int port = DEFAULT_PORTA;
    int latency = DEFAULT_LATENCIA;

    printf("1 - Transmissor\n");
    printf("2 - Receptor\n");
    printf("Escolha: ");
    fflush(stdout);

    if (fgets(opcao, sizeof(opcao), stdin) != NULL)
    {
        if (opcao[0] == '1')
        {
            printf("IP do receptor [127.0.0.1]: ");
            fflush(stdout);
            if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
            {
                buffer[strcspn(buffer, "\r\n")] = 0;
                if (strlen(buffer) > 0)
                    strncpy(ip, buffer, sizeof(ip) - 1);
            }

            printf("Porta base [5000]: ");
            fflush(stdout);
            if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
            {
                int p = atoi(buffer);
                if (p > 0) port = p;
            }

            return executar_transmissor(ip, port, DEFAULT_ARQUIVO_ORIGEM);
        }
        else if (opcao[0] == '2')
        {
            printf("Porta base [5000]: ");
            fflush(stdout);
            if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
            {
                int p = atoi(buffer);
                if (p > 0) port = p;
            }

            printf("Latencia (ms) [200]: ");
            fflush(stdout);
            if (fgets(buffer, sizeof(buffer), stdin) && buffer[0] != '\n')
            {
                int l = atoi(buffer);
                if (l > 0) latency = l;
            }

            return executar_receptor(port, latency);
        }
    }

    printf("Opcao invalida.\n");
    return 0;
}
