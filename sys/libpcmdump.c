
/**
 * @file
 * API example for audio decoding and filtering
 * @example filtering_audio.c
 */

#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

// We want to achieve:
// - format: f32le
// - codec: pcm_f32le
// - channels: 1
// - rate: 44100

static const char* filter_descr =
    "aresample=44100,aformat=sample_fmts=flt:channel_layouts=mono";
static const char* player = "ffplay -f f32le -ar 44100 -ac 1 -";

// static AVFormatContext* state->fmt_ctx;
// static AVCodecContext* state->dec_ctx;
// AVFilterContext* state->buffersink_ctx;
// AVFilterContext* state->buffersrc_ctx;
// AVFilterGraph* state->filter_graph;
// static int state->audio_stream_index = -1;

typedef struct {
  AVFormatContext* fmt_ctx;
  AVCodecContext* dec_ctx;
  AVFilterContext* buffersink_ctx;
  AVFilterContext* buffersrc_ctx;
  AVFilterGraph* filter_graph;
  int audio_stream_index;

  // decoding state
  AVPacket packet;
  AVFrame* frame;
  AVFrame* filt_frame;

} PgState;

static int open_input_file(const char* filename, PgState* state) {
  int ret;
  AVCodec* dec;

  if ((ret = avformat_open_input(&state->fmt_ctx, filename, NULL, NULL)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
    return ret;
  }

  if ((ret = avformat_find_stream_info(state->fmt_ctx, NULL)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
    return ret;
  }

  /* select the audio stream */
  ret =
      av_find_best_stream(state->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR,
           "Cannot find an audio stream in the input file\n");
    return ret;
  }
  state->audio_stream_index = ret;

  /* create decoding context */
  state->dec_ctx = avcodec_alloc_context3(dec);
  if (!state->dec_ctx)
    return AVERROR(ENOMEM);
  avcodec_parameters_to_context(
      state->dec_ctx,
      state->fmt_ctx->streams[state->audio_stream_index]->codecpar);
  av_opt_set_int(state->dec_ctx, "refcounted_frames", 1, 0);

  /* init the audio decoder */
  if ((ret = avcodec_open2(state->dec_ctx, dec, NULL)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
    return ret;
  }

  return 0;
}

static int init_filters(const char* filters_descr, PgState* state) {
  char args[512];
  int ret = 0;
  AVFilter* abuffersrc = avfilter_get_by_name("abuffer");
  AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  static const enum AVSampleFormat out_sample_fmts[] = {AV_SAMPLE_FMT_FLT, -1};
  static const int64_t out_channel_layouts[] = {AV_CH_LAYOUT_MONO, -1};
  static const int out_sample_rates[] = {44100, -1};
  const AVFilterLink* outlink;
  AVRational time_base =
      state->fmt_ctx->streams[state->audio_stream_index]->time_base;

  state->filter_graph = avfilter_graph_alloc();
  if (!outputs || !inputs || !state->filter_graph) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  /* buffer audio source: the decoded frames from the decoder will be inserted
   * here. */
  if (!state->dec_ctx->channel_layout)
    state->dec_ctx->channel_layout =
        av_get_default_channel_layout(state->dec_ctx->channels);
  snprintf(
      args, sizeof(args),
      "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
      time_base.num, time_base.den, state->dec_ctx->sample_rate,
      av_get_sample_fmt_name(state->dec_ctx->sample_fmt),
      state->dec_ctx->channel_layout);
  ret = avfilter_graph_create_filter(&state->buffersrc_ctx, abuffersrc, "in",
                                     args, NULL, state->filter_graph);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
    goto end;
  }

  /* buffer audio sink: to terminate the filter chain. */
  ret = avfilter_graph_create_filter(&state->buffersink_ctx, abuffersink, "out",
                                     NULL, NULL, state->filter_graph);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
    goto end;
  }

  ret = av_opt_set_int_list(state->buffersink_ctx, "sample_fmts",
                            out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
    goto end;
  }

  ret = av_opt_set_int_list(state->buffersink_ctx, "channel_layouts",
                            out_channel_layouts, -1, AV_OPT_SEARCH_CHILDREN);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
    goto end;
  }

  ret = av_opt_set_int_list(state->buffersink_ctx, "sample_rates",
                            out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
    goto end;
  }

  /*
   * Set the endpoints for the filter graph. The state->filter_graph will
   * be linked to the graph described by filters_descr.
   */

  /*
   * The buffer source output must be connected to the input pad of
   * the first filter described by filters_descr; since the first
   * filter input label is not specified, it is set to "in" by
   * default.
   */
  outputs->name = av_strdup("in");
  outputs->filter_ctx = state->buffersrc_ctx;
  outputs->pad_idx = 0;
  outputs->next = NULL;

  /*
   * The buffer sink input must be connected to the output pad of
   * the last filter described by filters_descr; since the last
   * filter output label is not specified, it is set to "out" by
   * default.
   */
  inputs->name = av_strdup("out");
  inputs->filter_ctx = state->buffersink_ctx;
  inputs->pad_idx = 0;
  inputs->next = NULL;

  if ((ret = avfilter_graph_parse_ptr(state->filter_graph, filters_descr,
                                      &inputs, &outputs, NULL)) < 0)
    goto end;

  if ((ret = avfilter_graph_config(state->filter_graph, NULL)) < 0)
    goto end;

  /* Print summary of the sink buffer
   * Note: args buffer is reused to store channel layout string */
  outlink = state->buffersink_ctx->inputs[0];
  av_get_channel_layout_string(args, sizeof(args), -1, outlink->channel_layout);
  //   av_log(NULL, AV_LOG_INFO, "Output: srate:%dHz fmt:%s chlayout:%s\n",
  //          (int)outlink->sample_rate,
  //          (char*)av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
  //          args);

end:
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  return ret;
}

static void print_frame(const AVFrame* frame) {
  const int n = frame->nb_samples *
                av_get_channel_layout_nb_channels(frame->channel_layout);

  typedef float output_t;

  const output_t* p = (output_t*)frame->data[0];
  const output_t* p_end = p + n;

  while (p < p_end) {
    char* cp = (char*)p;

    for (int i = 0; i < sizeof(output_t); i++) {
      fputc(*(cp + i), stdout);
    }

    p++;
  }

  fflush(stdout);
}

void cleanup(int ret, PgState* state) {
  avfilter_graph_free(&state->filter_graph);
  avcodec_free_context(&state->dec_ctx);
  avformat_close_input(&state->fmt_ctx);
  av_frame_free(&state->frame);
  av_frame_free(&state->filt_frame);

  if (ret < 0 && ret != AVERROR_EOF) {
    fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
    exit(1);
  }

  free(state);

  exit(0);
}

PgState* init_state(const char* filename) {
  int ret;
  PgState* state = (PgState*)calloc(1, sizeof(PgState));

  state->frame = av_frame_alloc();
  state->filt_frame = av_frame_alloc();

  if (!state->frame || !state->filt_frame) {
    perror("Could not allocate frame");
    exit(1);
  }

  av_register_all();
  avfilter_register_all();

  if ((ret = open_input_file(filename, state)) < 0)
    cleanup(ret, state);
  if ((ret = init_filters(filter_descr, state)) < 0)
    cleanup(ret, state);

  return state;
}

int main(int argc, char** argv) {
  int ret;

  if (argc != 2) {
    fprintf(stderr, "Usage: %s file | %s\n", argv[0], player);
    exit(1);
  }

  PgState* state = init_state(argv[1]);

  /* read all packets */
  while (1) {
    if ((ret = av_read_frame(state->fmt_ctx, &(state->packet))) < 0)
      break;

    if (state->packet.stream_index == state->audio_stream_index) {
      ret = avcodec_send_packet(state->dec_ctx, &(state->packet));
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Error while sending a packet to the decoder\n");
        break;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(state->dec_ctx, state->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          av_log(NULL, AV_LOG_ERROR,
                 "Error while receiving a frame from the decoder\n");
          cleanup(ret, state);
        }

        if (ret >= 0) {
          /* push the audio data from decoded frame into the filtergraph */
          if (av_buffersrc_add_frame_flags(state->buffersrc_ctx, state->frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Error while feeding the audio filtergraph\n");
            break;
          }

          /* pull filtered audio from the filtergraph */
          while (1) {
            ret = av_buffersink_get_frame(state->buffersink_ctx,
                                          state->filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
              break;
            if (ret < 0)
              cleanup(ret, state);

            // Actually print a frame!
            print_frame(state->filt_frame);

            ///
            av_frame_unref(state->filt_frame);
          }
          av_frame_unref(state->frame);
        }
      }
    }
    av_packet_unref(&(state->packet));
  }
  cleanup(0, state);
}