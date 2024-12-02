#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_DURATION 10.0
#define STREAM_FRAME_RATE 25              /* 25 images/s */
#define STREAM_PIX_FMT AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

// a wrapper around a single output AVStream
typedef struct InputStream {
  int stream_idx;
  AVStream *st;

  AVCodecParameters *codecpar;
  AVCodec *codec;
  AVCodecContext *codec_ctx;
} InputStream;

// a wrapper around a single output AVStream
typedef struct OutputStream {
  AVStream *st;
  AVCodecContext *enc;

  /* pts of the next frame that will be generated */
  int64_t next_pts;
  int samples_count;

  AVFrame *frame;
  AVFrame *tmp_frame;

  AVPacket *tmp_pkt;

  float t, tincr, tincr2;

  struct SwsContext *sws_ctx;
  struct SwrContext *swr_ctx;
} OutputStream;

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt) {
  AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

  printf(
      "pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s "
      "stream_index:%d\n",
      av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
      av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
      av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
      pkt->stream_index);
}

static int initialize_input_streams(AVFormatContext *format_ctx,
                                    InputStream *ivs, InputStream *ias) {
  // Retrieve stream information
  if (avformat_find_stream_info(format_ctx, NULL) < 0) {
    fprintf(stderr, "Could not find stream information\n");
    return 3;
  }

  ivs->stream_idx = -1;
  ias->stream_idx = -1;
  for (unsigned int i = 0; i < format_ctx->nb_streams; ++i) {
    if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      ivs->stream_idx = i;
      ivs->st = format_ctx->streams[i];

      ivs->codecpar = format_ctx->streams[ivs->stream_idx]->codecpar;
      ivs->codec = avcodec_find_decoder(ivs->codecpar->codec_id);
      ivs->codec_ctx = avcodec_alloc_context3(ivs->codec);
      avcodec_parameters_to_context(ivs->codec_ctx, ivs->codecpar);

      // Open codec
      if (avcodec_open2(ivs->codec_ctx, ivs->codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 5;
      }
      continue;
    }
    if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      ias->stream_idx = i;
      ivs->st = format_ctx->streams[i];

      ias->codecpar = format_ctx->streams[ias->stream_idx]->codecpar;
      ias->codec = avcodec_find_decoder(ias->codecpar->codec_id);
      ias->codec_ctx = avcodec_alloc_context3(ias->codec);
      avcodec_parameters_to_context(ias->codec_ctx, ias->codecpar);

      // Open codec
      if (avcodec_open2(ias->codec_ctx, ias->codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 5;
      }
      continue;
    }
  }

  if (ivs->stream_idx == -1 || ias->stream_idx == -1) {
    fprintf(stderr, "Could not find video or audio stream\n");
    return 4;
  }
}

static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame, AVPacket *pkt) {
  int ret;

  // send the frame to the encoder
  ret = avcodec_send_frame(c, frame);
  if (ret < 0) {
    fprintf(stderr, "Error sending a frame to the encoder: %s\n",
            av_err2str(ret));
    exit(1);
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(c, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    else if (ret < 0) {
      fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
      exit(1);
    }

    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, c->time_base, st->time_base);
    pkt->stream_index = st->index;

    /* Write the compressed frame to the media file. */
    log_packet(fmt_ctx, pkt);
    ret = av_interleaved_write_frame(fmt_ctx, pkt);
    /* pkt is now blank (av_interleaved_write_frame() takes ownership of
     * its contents and resets pkt), so that no unreferencing is necessary.
     * This would be different if one used av_write_frame(). */
    if (ret < 0) {
      fprintf(stderr, "Error while writing output packet: %s\n",
              av_err2str(ret));
      exit(1);
    }
  }

  return ret == AVERROR_EOF ? 1 : 0;
}

/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
                       const AVCodec **codec, enum AVCodecID codec_id,
                       InputStream *ist, AVFormatContext *input_format_ctx) {
  AVCodecContext *c;
  int i;

  /* find the encoder */
  *codec = avcodec_find_encoder(codec_id);
  if (!(*codec)) {
    fprintf(stderr, "Could not find encoder for '%s'\n",
            avcodec_get_name(codec_id));
    exit(1);
  }

  ost->tmp_pkt = av_packet_alloc();
  if (!ost->tmp_pkt) {
    fprintf(stderr, "Could not allocate AVPacket\n");
    exit(1);
  }

  ost->st = avformat_new_stream(oc, NULL);
  if (!ost->st) {
    fprintf(stderr, "Could not allocate stream\n");
    exit(1);
  }
  ost->st->id = oc->nb_streams - 1;
  c = avcodec_alloc_context3(*codec);
  if (!c) {
    fprintf(stderr, "Could not alloc an encoding context\n");
    exit(1);
  }
  ost->enc = c;

  switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:

      // c->sample_fmt =
      //     (*codec)->sample_fmts ? (*codec)->sample_fmts[0] :
      //     AV_SAMPLE_FMT_FLTP;

      c->bit_rate = 64000;

      // c->sample_rate = 44100;
      const enum AVSampleFormat *sample_fmts = NULL;
      int r = avcodec_get_supported_config(ist->codec_ctx, NULL,
                                           AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                           (const void **)&sample_fmts, NULL);
      if (!r) {
        printf("No supported configurations found\n");
        return AVERROR(EINVAL);
      }
      c->sample_fmt = sample_fmts;

      c->sample_rate = ist->codec_ctx->sample_rate;

      // c->ch_layout = ist->codec_ctx->ch_layout;

      // if ((*codec)->supported_samplerates) {
      //   c->sample_rate = (*codec)->supported_samplerates[0];
      //   for (i = 0; (*codec)->supported_samplerates[i]; i++) {
      //     if ((*codec)->supported_samplerates[i] == 44100)
      //       c->sample_rate = 44100;
      //   }
      // }

      // av_channel_layout_copy(&c->ch_layout,
      //                        &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);

      // av_channel_layout_copy(&c->ch_layout, &ist->codec_ctx->ch_layout);

      av_channel_layout_default(&c->ch_layout, 2);

      ost->st->time_base = (AVRational){1, c->sample_rate};
      break;

    case AVMEDIA_TYPE_VIDEO:
      c->codec_id = codec_id;

      c->bit_rate = ist->codec_ctx->bit_rate;
      /* Resolution must be a multiple of two. */
      c->width = ist->codec_ctx->width;
      c->height = ist->codec_ctx->height;
      /* timebase: This is the fundamental unit of time (in seconds) in terms
       * of which frame timestamps are represented. For fixed-fps content,
       * timebase should be 1/framerate and timestamp increments should be
       * identical to 1. */
      ost->st->time_base = (AVRational){1, 30};
      c->time_base = ost->st->time_base;

      // c->gop_size = 12; /* emit one intra frame every twelve frames at most
      // */
      c->gop_size = ist->codec_ctx->gop_size;
      c->pix_fmt = ist->codec_ctx->pix_fmt;
      if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        /* just for testing, we also add B-frames */
        c->max_b_frames = 2;
      }
      if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        /* Needed to avoid using macroblocks in which some coeffs overflow.
         * This does not happen with normal video, it just happens here as
         * the motion of the chroma plane does not match the luma plane. */
        c->mb_decision = 2;
      }
      break;

    default:
      break;
  }

  /* Some formats want stream headers to be separate. */
  if (oc->oformat->flags & AVFMT_GLOBALHEADER)
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/**************************************************************/
/* audio output */

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  const AVChannelLayout *channel_layout,
                                  int sample_rate, int nb_samples) {
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Error allocating an audio frame\n");
    exit(1);
  }

  frame->format = sample_fmt;
  av_channel_layout_copy(&frame->ch_layout, channel_layout);
  frame->sample_rate = sample_rate;
  frame->nb_samples = nb_samples;

  if (nb_samples) {
    if (av_frame_get_buffer(frame, 0) < 0) {
      fprintf(stderr, "Error allocating an audio buffer\n");
      exit(1);
    }
  }

  return frame;
}

static void open_audio(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg) {
  AVCodecContext *c;
  int nb_samples;
  int ret;
  AVDictionary *opt = NULL;

  c = ost->enc;

  /* open it */
  av_dict_copy(&opt, opt_arg, 0);
  ret = avcodec_open2(c, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
    exit(1);
  }

  // /* init signal generator */
  // ost->t = 0;
  // ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
  // /* increment frequency by 110 Hz per second */
  // ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

  if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
    nb_samples = 10000;
  else
    nb_samples = c->frame_size;

  ost->frame = alloc_audio_frame(c->sample_fmt, &c->ch_layout, c->sample_rate,
                                 nb_samples);
  // ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &c->ch_layout,
  //                                    c->sample_rate, nb_samples);

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(ost->st->codecpar, c);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }

  /* create resampler context */
  ost->swr_ctx = swr_alloc();
  if (!ost->swr_ctx) {
    fprintf(stderr, "Could not allocate resampler context\n");
    exit(1);
  }

  /* set options */
  av_opt_set_chlayout(ost->swr_ctx, "in_chlayout", &c->ch_layout, 0);
  av_opt_set_int(ost->swr_ctx, "in_sample_rate", c->sample_rate, 0);
  av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  av_opt_set_chlayout(ost->swr_ctx, "out_chlayout", &c->ch_layout, 0);
  av_opt_set_int(ost->swr_ctx, "out_sample_rate", c->sample_rate, 0);
  av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt", c->sample_fmt, 0);

  /* initialize the resampling context */
  if ((ret = swr_init(ost->swr_ctx)) < 0) {
    fprintf(stderr, "Failed to initialize the resampling context\n");
    exit(1);
  }
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
// static int write_audio_frame(AVFormatContext *oc, OutputStream *ost) {
//   AVCodecContext *c;
//   AVFrame *frame;
//   int ret;
//   int dst_nb_samples;

//   c = ost->enc;

//   frame = get_audio_frame(ost);

//   if (frame) {
//     /* convert samples from native format to destination codec format, using
//     the
//      * resampler */
//     /* compute destination number of samples */
//     dst_nb_samples =
//         swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples;
//     av_assert0(dst_nb_samples == frame->nb_samples);

//     /* when we pass a frame to the encoder, it may keep a reference to it
//      * internally;
//      * make sure we do not overwrite it here
//      */
//     ret = av_frame_make_writable(ost->frame);
//     if (ret < 0) exit(1);

//     /* convert to destination format */
//     ret = swr_convert(ost->swr_ctx, ost->frame->data, dst_nb_samples,
//                       (const uint8_t **)frame->data, frame->nb_samples);
//     if (ret < 0) {
//       fprintf(stderr, "Error while converting\n");
//       exit(1);
//     }
//     frame = ost->frame;

//     frame->pts = av_rescale_q(ost->samples_count,
//                               (AVRational){1, c->sample_rate}, c->time_base);
//     ost->samples_count += dst_nb_samples;
//   }

//   return write_frame(oc, c, ost->st, frame, ost->tmp_pkt);
// }

/**************************************************************/
/* video output */

static AVFrame *alloc_frame(enum AVPixelFormat pix_fmt, int width, int height) {
  AVFrame *frame;
  int ret;

  frame = av_frame_alloc();
  if (!frame) return NULL;

  frame->format = pix_fmt;
  frame->width = width;
  frame->height = height;

  /* allocate the buffers for the frame data */
  ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate frame data.\n");
    exit(1);
  }

  return frame;
}

static void open_video(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg) {
  int ret;
  AVCodecContext *c = ost->enc;
  AVDictionary *opt = NULL;

  av_dict_copy(&opt, opt_arg, 0);

  /* open the codec */
  ret = avcodec_open2(c, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
    exit(1);
  }

  /* allocate and init a re-usable frame */
  ost->frame = alloc_frame(c->pix_fmt, c->width, c->height);
  if (!ost->frame) {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }

  /* If the output format is not YUV420P, then a temporary YUV420P
   * picture is needed too. It is then converted to the required
   * output format. */
  ost->tmp_frame = NULL;
  if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
    ost->tmp_frame = alloc_frame(AV_PIX_FMT_YUV420P, c->width, c->height);
    if (!ost->tmp_frame) {
      fprintf(stderr, "Could not allocate temporary video frame\n");
      exit(1);
    }
  }

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(ost->st->codecpar, c);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }
}

static int write_video_frame(OutputStream *ost, AVFrame *frame,
                             AVFormatContext *oc) {
  /* check if we want to generate more frames */
  // if (av_compare_ts(video_st.next_pts, c->time_base, STREAM_DURATION,
  //                   (AVRational){1, 1}) > 0)
  //   return NULL;

  /* when we pass a frame to the encoder, it may keep a reference to it
   * internally; make sure we do not overwrite it here */
  //   if (av_frame_make_writable(video_st.frame) < 0) exit(1);

  // if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
  /* as we only generate a YUV420P picture, we must convert it
   * to the codec pixel format if needed */
  if (!ost->sws_ctx) {
    ost->sws_ctx = sws_getContext(
        ost->enc->width, ost->enc->height, AV_PIX_FMT_YUV420P, ost->enc->width,
        ost->enc->height, ost->enc->pix_fmt, SCALE_FLAGS, NULL, NULL, NULL);
    if (!ost->sws_ctx) {
      fprintf(stderr, "Could not initialize the conversion context\n");
      exit(1);
    }
  }
  ost->tmp_frame = frame;
  sws_scale(ost->sws_ctx, (const uint8_t *const *)ost->tmp_frame->data,
            ost->tmp_frame->linesize, 0, ost->enc->height, ost->frame->data,
            ost->frame->linesize);
  // }

  ost->frame->pts = ost->next_pts++;

  return write_frame(oc, ost->enc, ost->st, ost->frame, ost->tmp_pkt);

  // Print basic frame properties
  // printf("Frame: %ld (type=%d, size=%d bytes) pts %ld key_frame %d\n",
  // 		codec_ctx->frame_num,
  // 		av_get_picture_type_char(frame->pict_type),
  // 		frame->pkt_size,
  // 		frame->pts,
  // 		frame->key_frame);
}

static void close_stream(AVFormatContext *oc, OutputStream *ost) {
  avcodec_free_context(&ost->enc);
  av_frame_free(&ost->frame);
  av_frame_free(&ost->tmp_frame);
  av_packet_free(&ost->tmp_pkt);
  sws_freeContext(ost->sws_ctx);
  swr_free(&ost->swr_ctx);
}

static void close_input_stream(InputStream *ist) {
  // Free allocated resources
  // av_frame_free(&input_st->frame);
  avcodec_free_context(&ist->codec_ctx);
}

int main(int argc, char **argv) {
  // input
  InputStream input_video_st = {0}, input_audio_st = {0};
  AVFormatContext *input_format_ctx = NULL;
  const char *input_filename;

  //   output
  OutputStream video_st = {0}, audio_st = {0};
  const AVOutputFormat *output_fmt;
  const char *output_filename;
  AVFormatContext *output_format_ctx;
  const AVCodec *audio_codec, *video_codec;
  int ret;
  int have_video = 0, have_audio = 0;
  int encode_video = 0, encode_audio = 0;
  AVDictionary *opt = NULL;
  int i;

  if (argc < 3) {
    printf(
        "usage: %s output_file\n"
        "API example program to output a media file with libavformat.\n"
        "This program generates a synthetic audio and video stream, encodes "
        "and\n"
        "muxes them into a file named output_file.\n"
        "The output format is automatically guessed according to the file "
        "extension.\n"
        "Raw images can also be output by using '%%d' in the filename.\n"
        "\n",
        argv[0]);
    return 1;
  }

  input_filename = argv[1];
  output_filename = argv[2];
  for (i = 2; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "-flags") || !strcmp(argv[i], "-fflags"))
      av_dict_set(&opt, argv[i] + 1, argv[i + 1], 0);
  }

  if (avformat_open_input(&input_format_ctx, input_filename, NULL, NULL) < 0) {
    fprintf(stderr, "Could not open input file: %s\n", input_filename);
    return 2;
  }

  if (initialize_input_streams(input_format_ctx, &input_video_st,
                               &input_audio_st) < 0) {
    fprintf(stderr, "Could not open initialize input streams\n");
    return 2;
  }

  /* allocate the output media context */
  avformat_alloc_output_context2(&output_format_ctx, NULL, NULL,
                                 output_filename);
  if (!output_format_ctx) {
    printf("Could not deduce output format from file extension: using MPEG.\n");
    avformat_alloc_output_context2(&output_format_ctx, NULL, "mpeg",
                                   output_filename);
  }
  if (!output_format_ctx) return 1;

  output_fmt = output_format_ctx->oformat;

  /* Add the audio and video streams using the default format codecs
   * and initialize the codecs. */
  if (output_fmt->video_codec != AV_CODEC_ID_NONE) {
    add_stream(&video_st, output_format_ctx, &video_codec,
               output_fmt->video_codec, &input_video_st, input_format_ctx);
    have_video = 1;
    encode_video = 1;
  }
  // if (output_fmt->audio_codec != AV_CODEC_ID_NONE) {
  //   add_stream(&audio_st, output_format_ctx, &audio_codec,
  //              output_fmt->audio_codec, &input_audio_st, input_format_ctx);
  //   have_audio = 1;
  //   encode_audio = 1;
  // }

  /* Now that all the parameters are set, we can open the audio and
   * video codecs and allocate the necessary encode buffers. */
  if (have_video) open_video(output_format_ctx, video_codec, &video_st, opt);
  // if (have_audio) open_audio(output_format_ctx, audio_codec, &audio_st, opt);

  printf("Show input format context dumb information:\n");
  av_dump_format(input_format_ctx, 0, input_filename, 0);
  printf("----------------\n");

  printf("Show output format context dumb information:\n");
  av_dump_format(output_format_ctx, 0, output_filename, 1);
  printf("----------------\n");

  /* open the output file, if needed */
  if (!(output_fmt->flags & AVFMT_NOFILE)) {
    ret = avio_open(&output_format_ctx->pb, output_filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      fprintf(stderr, "Could not open '%s': %s\n", output_filename,
              av_err2str(ret));
      return 1;
    }
  }

  /* Write the stream header, if any. */
  ret = avformat_write_header(output_format_ctx, &opt);
  if (ret < 0) {
    fprintf(stderr, "Error occurred when opening output file: %s\n",
            av_err2str(ret));
    return 1;
  }

  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_packet_alloc();
  while (av_read_frame(input_format_ctx, packet) >= 0) {
    if (packet->stream_index == input_video_st.stream_idx) {
      // Decode video frame
      int ret = avcodec_send_packet(input_video_st.codec_ctx, packet);
      if (ret < 0) {
        fprintf(stderr, "Error sending packet for decoding\n");
        break;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(input_video_st.codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          fprintf(stderr, "Error during decoding\n");
          break;
        }
      }
      write_video_frame(&video_st, frame, output_format_ctx);
    }

    // if (packet->stream_index == input_audio_st.stream_idx) {
    //   int ret = avcodec_send_packet(input_audio_st.codec_ctx, packet);
    //   if (ret < 0) {
    //     fprintf(stderr, "Error sending packet for decoding\n");
    //     break;
    //   }

    //   while (ret >= 0) {
    //     ret = avcodec_receive_frame(input_audio_st.codec_ctx,
    //                                 input_audio_st.frame);
    //     if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    //       break;
    //     } else if (ret < 0) {
    //       fprintf(stderr, "Error during decoding\n");
    //       break;
    //     }

    //     AVCodecContext *c;
    //     AVFrame *frame;
    //     int dst_nb_samples;

    //     c = audio_st.enc;

    //     frame = input_audio_st.frame;
    //     frame->pts = audio_st.next_pts;
    //     audio_st.next_pts += frame->nb_samples;

    //     if (frame) {
    //       /* convert samples from native format to destination codec format,
    //        * using the resampler */
    //       /* compute destination number of samples */
    //       dst_nb_samples = swr_get_delay(audio_st.swr_ctx, c->sample_rate) +
    //                        frame->nb_samples;
    //       av_assert0(dst_nb_samples == frame->nb_samples);

    //       /* when we pass a frame to the encoder, it may keep a reference to
    //       it
    //        * internally;
    //        * make sure we do not overwrite it here
    //        */
    //       ret = av_frame_make_writable(audio_st.frame);
    //       if (ret < 0) exit(1);

    //       /* convert to destination format */
    //       ret = swr_convert(audio_st.swr_ctx, audio_st.frame->data,
    //                         dst_nb_samples, (const uint8_t **)frame->data,
    //                         frame->nb_samples);
    //       if (ret < 0) {
    //         fprintf(stderr, "Error while converting\n");
    //         exit(1);
    //       }
    //       frame = audio_st.frame;

    //       frame->pts =
    //           av_rescale_q(audio_st.samples_count,
    //                        (AVRational){1, c->sample_rate}, c->time_base);
    //       audio_st.samples_count += dst_nb_samples;
    //     }

    //     write_frame(oc, c, audio_st.st, frame, audio_st.tmp_pkt);
    //   }
    // }

    av_packet_unref(packet);
  }

  av_frame_free(&frame);

  av_write_trailer(output_format_ctx);

  /* Close each codec. */
  if (have_video) close_stream(output_format_ctx, &video_st);
  if (have_audio) close_stream(output_format_ctx, &audio_st);

  if (!(output_fmt->flags & AVFMT_NOFILE)) /* Close the output file. */
    avio_closep(&output_format_ctx->pb);

  /* free the stream */
  avformat_free_context(output_format_ctx);

  // close input streams
  close_input_stream(&input_video_st);
  close_input_stream(&input_audio_st);

  // TODO check if input_format_ctx was deleted
  avformat_close_input(&input_format_ctx);

  return 0;
}
