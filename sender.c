/*
 * gst-rtpbin-stream-sender-loop.c
 *
 *  quick'n'dirty test application for creating/destroying video stream on i.MX6
 *  in loop/multiple iterations
 */
#include <gst/gst.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct _options_st
{
    gchar *sender_host;
    gint video_rtp_port;
    gint width_video;
    gint height_video;
    gboolean no_dma_buf;

} options_st;

typedef struct
{
    GstPad *rtp_sinkpad;
    GstPad *rtcp_srcpad;
    GstPad *rtcp_sinkpad;
    GstElement *rtpbin;
    GstElement *rtcpsink;
    GstElement *rtcpsrc;
    GstElement *rtpsink;
} tstRtpBin;

typedef struct
{
    GstElement *encoder;
    GstElement *encoder_caps_filter;
    GstElement *parser;
    GstElement *payload;
    GstElement *bin;
    GstPad *ghost_pad;
    tstRtpBin stRtpBin;
} tstVideoStream;

#define PRINT_REFCOUNT(element) g_print("%s: refcount of element %s: %d\n", \
                                __func__, \
                                GST_OBJECT_NAME(element), \
                                GST_OBJECT_REFCOUNT_VALUE(element))

#define REMOVE_FLOATING_REF(element) if (g_object_is_floating(G_OBJECT(element))) \
                                     { \
                                       gst_object_ref_sink(GST_OBJECT(element)); \
                                     }

static options_st options = {
    "127.0.0.1",
    9997,
    320,
    240,
    FALSE
};


static void _parse_cmdline(int *argc, char **argv, options_st *options)
{
  GOptionEntry entries[] = {
        { "sender-host", 0, 0, G_OPTION_ARG_STRING, &options->sender_host,
          "IP address of the sender host (used to send feedback RTCP to)", "127.0.0.1"},

        { "rtp-port", 0, 0, G_OPTION_ARG_INT, &options->video_rtp_port,
          "UDP port for RTP video.  Should be even (RTCP will take the next following it)", "9997" },

        { "video-width", 0, 0, G_OPTION_ARG_INT, &options->width_video,
          "width of the video to send", "320" },

        { "video-height", 0, 0, G_OPTION_ARG_INT, &options->height_video,
          "height of the video to send", "240" },

        { "no-dma-buf", 0, 0, G_OPTION_ARG_NONE, &options->no_dma_buf,
          "Disable usage of DMABUF", NULL},

        { NULL }
    };

    GError *gerror = NULL;
    GOptionContext *context;
    GOptionGroup *gstreamer_group;

    /* We must initialise the threading system before using any
       other Glib function, such as g_option_context_new() ??? */
    if ( ! g_thread_supported() )
    {
        g_thread_init(NULL);
    }

    context = g_option_context_new("- Send video streaming over RTP");

    //Init gstreamer
    gstreamer_group = gst_init_get_option_group();  // This is a way to chain gst_init with other
                                                    // _init() functions

    // We add our options and the gstreamer specific ones
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gstreamer_group);

    //Parse context
    g_option_context_parse(context, argc, &argv, &gerror);
    if (gerror != NULL)
    {
        g_printerr("Error initialising: %s\n", gerror->message);
        g_error_free(gerror);
        exit(-1);
    }
    g_option_context_free(context);
}

// TODO: should return state
void create_and_link_rtpbin(tstRtpBin *pRtpBin, GstElement *pipe, GstElement *src,
                       gint port, gchar* ip)
{
  GstPad *srcpad;
  GstPad *sinkpad;

  pRtpBin->rtpbin = gst_element_factory_make ("rtpbin", "rtpbin");
  pRtpBin->rtpsink = gst_element_factory_make("udpsink", "udpsink0");
  pRtpBin->rtcpsink = gst_element_factory_make ("udpsink", "rtcpsink0");
  pRtpBin->rtcpsrc = gst_element_factory_make ("udpsrc", "rtcpsrc0");
  gst_bin_add_many (GST_BIN (pipe), pRtpBin->rtpbin, pRtpBin->rtpsink,
                    pRtpBin->rtcpsink, pRtpBin->rtcpsrc, NULL);

  // RTP
  srcpad = gst_element_get_static_pad (src, "src");
  pRtpBin->rtp_sinkpad = gst_element_get_request_pad (pRtpBin->rtpbin, "send_rtp_sink_0");
  if (gst_pad_link (srcpad, pRtpBin->rtp_sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link audio payloader to rtpbin");
  gst_object_unref (srcpad);

  srcpad = gst_element_get_static_pad (pRtpBin->rtpbin, "send_rtp_src_0");
  sinkpad = gst_element_get_static_pad (pRtpBin->rtpsink, "sink");
  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link rtpbin to rtpsink");
  gst_object_unref (srcpad);
  gst_object_unref (sinkpad);

  // RTCP
  pRtpBin->rtcp_srcpad = gst_element_get_request_pad (pRtpBin->rtpbin, "send_rtcp_src_0");
  sinkpad = gst_element_get_static_pad (pRtpBin->rtcpsink, "sink");
  if (gst_pad_link (pRtpBin->rtcp_srcpad, sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link rtpbin to rtcpsink");
  gst_object_unref (sinkpad);

  srcpad = gst_element_get_static_pad (pRtpBin->rtcpsrc, "src");
  pRtpBin->rtcp_sinkpad = gst_element_get_request_pad (pRtpBin->rtpbin, "recv_rtcp_sink_0");
  if (gst_pad_link (srcpad, pRtpBin->rtcp_sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link rtcpsrc to rtpbin");
  gst_object_unref (srcpad);

  // Set properties to rtpbin
  g_object_set(G_OBJECT(pRtpBin->rtpsink), "port",  port, "host", ip, NULL);
  g_object_set (pRtpBin->rtcpsink, "port", port+1, "host", ip, NULL);
  g_object_set (pRtpBin->rtcpsink, "async", FALSE, "sync", FALSE, NULL);
  g_object_set (pRtpBin->rtcpsrc, "port", port+5, NULL);
}

GstElement *create_video_stream(tstVideoStream *stream, gint nodmabuf,
                                gint port, gchar* ip)
{
  GstPad *pad = NULL;
  gboolean status;
  gchar *format;


  if (!stream)
  {
    g_printerr ("VideoStream: invalid stream object.\n");
    return NULL;
  }


  stream->payload = gst_element_factory_make("rtph264pay", "rtph264pay0");
  stream->encoder = gst_element_factory_make("v4l2video2h264enc", "v4l2video2h264enc");
  stream->parser  = gst_element_factory_make("h264parse","h264parse0");
  stream->encoder_caps_filter = gst_element_factory_make("capsfilter", "enc_caps");
  if (!stream->payload || !stream->encoder || !stream->parser  || !stream->encoder_caps_filter)
  {
    g_printerr ("VideoStream: can't create all elements.\n");
    return NULL;
  }

  if (nodmabuf == FALSE)
  {
    g_object_set(G_OBJECT(stream->encoder), "output-io-mode", 5, NULL); // set io-mode to dmabuf-import
    //g_object_set(G_OBJECT(stream->encoder), "tune", 4, NULL);  //Set tune to zerolatency
  }
  format="I420\0";


  GstCaps *caps_enc = gst_caps_new_simple ("video/x-h264",
      "stream-format", G_TYPE_STRING, "byte-stream",
      "alignment", G_TYPE_STRING, "au",
      "profile", G_TYPE_STRING, "constrained-baseline",
      NULL);

  if (!caps_enc )
  {
    g_printerr ("VideoStream: Caps for Encoder could not be created.\n");
    return NULL;
  }

  g_object_set(G_OBJECT(stream->encoder_caps_filter), "caps", caps_enc, NULL);


  stream->bin = gst_bin_new("VideoStream");
  gst_bin_add_many (GST_BIN (stream->bin),
                    stream->encoder,
                    stream->encoder_caps_filter,
                    stream->parser,
                    stream->payload,
                    NULL);
  status = gst_element_link_many (stream->encoder, stream->encoder_caps_filter,
      stream->parser, stream->payload, NULL);


  if ( status != TRUE){
    g_printerr ("VideoStream: elements could not be linked.\n");
    gst_object_unref (stream->bin);
    return NULL;
  }

  // TODO: correctly check return..
  create_and_link_rtpbin(&stream->stRtpBin, stream->bin, stream->payload,
                         port, ip);

  pad = gst_element_get_static_pad(stream->encoder, "sink");
  stream->ghost_pad = gst_ghost_pad_new("sink", pad);
  gst_pad_set_active(stream->ghost_pad, TRUE);
  gst_element_add_pad(stream->bin, stream->ghost_pad);
  gst_object_unref(GST_OBJECT(pad));

  REMOVE_FLOATING_REF(stream->bin);
  PRINT_REFCOUNT(stream->encoder);
  PRINT_REFCOUNT(stream->bin);
  return stream->bin;
}

void destroy_video_stream(tstVideoStream *stream)
{
  if (stream != NULL && stream->bin != NULL)
  {
    PRINT_REFCOUNT(stream->bin);
    PRINT_REFCOUNT(stream->encoder);
    gst_bin_remove_many(GST_BIN(stream->bin),
        stream->encoder,
        stream->encoder_caps_filter,
        stream->parser,
        stream->payload,
        NULL);

    PRINT_REFCOUNT(stream->bin);
    /* destroy bin container */
    gst_object_unref(GST_OBJECT(stream->bin));
  }
}

int main(int argc, char *argv[]) {
  GstElement *pipeline = NULL;
  GstElement *source = NULL;
  GstElement *source_caps_filter = NULL;
  GstElement *stream = NULL;
  tstVideoStream stVideoStream;
  GstStateChangeReturn ret;
  gboolean status;
  int cnt;

  //Initialisation gstream and argument list check
  _parse_cmdline(&argc, argv, &options);

  // There should only remain the file name
  if (argc != 1)
  {
    g_printerr("Extra unknown arguments present.  See --help\n");
    exit(-1);
  }

  /* Create the VideoDevice */
  source = gst_element_factory_make ("v4l2src", "source0");
  // remove floating reference as this should element stay even after being removed from bin
  REMOVE_FLOATING_REF(source);
  if (options.no_dma_buf == FALSE)
  {
    g_print("Using DMABUF\n");
    g_object_set(G_OBJECT(source), "io-mode", 4, NULL); // set io-mode to dmabuf
  }
  else
  {
    g_print("Not using DMABUF\n");
  }
  // Capsfilter
  source_caps_filter = gst_element_factory_make("capsfilter", "source_caps");
  // remove floating reference as this should element stay even after being removed from bin
  REMOVE_FLOATING_REF(source_caps_filter);
  // Create caps
  GstCaps *caps_src = gst_caps_new_simple ("video/x-raw",
      "framerate", GST_TYPE_FRACTION, 30, 1,
      "format", G_TYPE_STRING, "I420",
      "width", G_TYPE_INT, options.width_video,
      "height", G_TYPE_INT, options.height_video,
      NULL);
  if (!caps_src )
  {
    g_printerr ("Caps for Source could not be created.\n");
    return -1;
  }
  // Set caps on cap filter
  g_object_set(G_OBJECT(source_caps_filter), "caps", caps_src, NULL);

  /* Create VideoStream and connect it with VideoDevice several times */
  for (cnt=0; cnt < 10; cnt++)
  {
    g_print("Build pipeline (iteration %d)\n", cnt);

    /* Build the pipeline */
    pipeline = gst_pipeline_new ("test-pipeline");

    // add VideoDevice
    gst_bin_add_many (GST_BIN (pipeline), source,  source_caps_filter, NULL);
    status = gst_element_link(source, source_caps_filter);
    if ( status != TRUE){
      g_printerr ("VideoDevice could not be linked.\n");
      gst_object_unref (pipeline);
      return -1;
    }

    // add VideoStream
    stream = create_video_stream(&stVideoStream, options.no_dma_buf,
        options.video_rtp_port, options.sender_host);
    if (stream == NULL)
    {
      g_printerr ("VideoStream could not be created.\n");
      gst_object_unref (pipeline);
      return -1;
    }
    gst_bin_add(GST_BIN (pipeline), stream);

    status = gst_element_link(source_caps_filter, stream);
    if ( status != TRUE){
      g_printerr ("VideoDevice could not be linked with VideoStream.\n");
      gst_object_unref (pipeline);
      return -1;
    }
    /* Start playing */
    g_print("Start playing pipeline (iteration %d)\n", cnt);
    ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      g_printerr ("Unable to set the pipeline to the playing state.\n");
      gst_object_unref (pipeline);
      return -1;
    }

    sleep(4);

    /* stop playing, remove elements/destroy stream */
    g_print("Stop playing pipeline (iteration %d)\n", cnt);
    gst_element_set_state (pipeline, GST_STATE_NULL);

    gst_element_unlink(source_caps_filter, stream);
    gst_bin_remove_many(GST_BIN (pipeline), source,  source_caps_filter,
                        stream, NULL);
    destroy_video_stream(&stVideoStream);

    g_print("Delete pipeline (iteration %d)\n", cnt);
    gst_object_unref (pipeline);
  }

  gst_object_unref(source);
  gst_object_unref(source_caps_filter);

  return 0;
}


