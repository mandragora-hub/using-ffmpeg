#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
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

// a wrapper around a single input AVStream
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
  AVFrame *frame;

  AVAudioFifo *fifo;

  struct SwsContext *sws_ctx;
  struct SwrContext *swr_ctx;
} OutputStream;

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt) {
  AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

  printf(
      "pts: %s pts_time: %s  dts: %s  dts_time: %s duration: %s  "
      "duration_time: %s "
      "stream_index: %d\n",
      av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
      av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
      av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
      pkt->stream_index);
}

static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context) {
  /* Create the FIFO buffer based on the specified output sample format. */
  if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                    output_codec_context->ch_layout.nb_channels,
                                    1))) {
    fprintf(stderr, "Could not allocate FIFO\n");
    return AVERROR(ENOMEM);
  }
  return 0;
}

static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context) {
  int error;

  /*
   * Create a resampler context for the conversion.
   * Set the conversion parameters.
   */
  error = swr_alloc_set_opts2(
      resample_context, &output_codec_context->ch_layout,
      output_codec_context->sample_fmt, output_codec_context->sample_rate,
      &input_codec_context->ch_layout, input_codec_context->sample_fmt,
      input_codec_context->sample_rate, 0, NULL);
  if (error < 0) {
    fprintf(stderr, "Could not allocate resample context\n");
    return error;
  }
  /*
   * Perform a sanity check so that the number of converted samples is
   * not greater than the number of samples to be converted.
   * If the sample rates differ, this case has to be handled differently
   */
  av_assert0(output_codec_context->sample_rate ==
             input_codec_context->sample_rate);

  /* Open the resampler with the specified parameters. */
  if ((error = swr_init(*resample_context)) < 0) {
    fprintf(stderr, "Could not open resample context\n");
    swr_free(resample_context);
    return error;
  }
  return 0;
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
      ias->st = format_ctx->streams[i];

      ias->codecpar = format_ctx->streams[ias->stream_idx]->codecpar;
      ias->codec = avcodec_find_decoder(ias->codecpar->codec_id);
      ias->codec_ctx = avcodec_alloc_context3(ias->codec);
      avcodec_parameters_to_context(ias->codec_ctx, ias->codecpar);

      // Open codec
      if (avcodec_open2(ias->codec_ctx, ias->codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 5;
      }

      ias->codec_ctx->pkt_timebase = ias->st->time_base;

      continue;
    }
  }

  if (ivs->stream_idx == -1 && ias->stream_idx == -1) {
    fprintf(stderr, "Could not find video or audio stream\n");
    return 4;
  }
  return 0;
}

static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame) {
  int ret;

  // send the frame to the encoder
  ret = avcodec_send_frame(c, frame);
  if (ret < 0) {
    fprintf(stderr, "Error sending a frame to the encoder: %s\n",
            av_err2str(ret));
    exit(1);
  }

  AVPacket *pkt = av_packet_alloc();
  if (!pkt) {
    fprintf(stderr, "Could not allocate AVPacket\n");
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

  ost->st = avformat_new_stream(oc, NULL);
  if (!ost->st) {
    fprintf(stderr, "Could not allocate stream\n");
    exit(1);
  }
  // ost->st->id = oc->nb_streams - 1;
  // ost->st->id = ist->st->id;
  c = avcodec_alloc_context3(*codec);
  if (!c) {
    fprintf(stderr, "Could not alloc an encoding context\n");
    exit(1);
  }
  ost->enc = c;

  switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:

      const enum AVSampleFormat *sample_fmts = NULL;
      int r =
          avcodec_get_supported_config(c, NULL, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                       0, (const void **)&sample_fmts, NULL);

      if (r < 0) {
        printf("No supported configurations found\n");
        return exit(1);
        ;
      }
      /* Set the basic encoder parameters.
       * The input file's sample rate is used to avoid a sample rate conversion.
       */
      av_channel_layout_default(&c->ch_layout, 2);
      c->sample_rate = ist->codec_ctx->sample_rate;
      c->sample_fmt =
          (r >= 0 && sample_fmts) ? sample_fmts[0] : (*codec)->sample_fmts[0];
      c->bit_rate = 96000;

      /* Set the sample rate for the container. */
      ost->st->time_base.den = c->sample_rate;
      ost->st->time_base.num = 1;

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
                       InputStream *ist, OutputStream *ost,
                       AVDictionary *opt_arg) {
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

  // if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
  //   nb_samples = 10000;
  // else
  //   nb_samples = c->frame_size;

  // ost->frame = alloc_audio_frame(c->sample_fmt, &c->ch_layout,
  // c->sample_rate,
  //                                nb_samples);
  // ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &c->ch_layout,
  //                                    c->sample_rate, nb_samples);

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(ost->st->codecpar, c);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }

  ret = init_fifo(&ost->fifo, c);
  if (ret < 0) {
    fprintf(stderr, "Could not init audio fifo\n");
    exit(1);
  }

  ret = init_resampler(ist->codec_ctx, c, &ost->swr_ctx);
  if (ret < 0) {
    fprintf(stderr, "Could not init resampler fifo\n");
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

  AVFrame *tmp_frame = av_frame_alloc();

  sws_scale(ost->sws_ctx, (const uint8_t *const *)frame->data, frame->linesize,
            0, ost->enc->height, ost->frame->data, ost->frame->linesize);

  return write_frame(oc, ost->enc, ost->st, ost->frame);
}

static void close_stream(AVFormatContext *oc, OutputStream *ost) {
  if (ost->fifo) av_audio_fifo_free(ost->fifo);
  avcodec_free_context(&ost->enc);
  av_frame_free(&ost->frame);
  sws_freeContext(ost->sws_ctx);
  swr_free(&ost->swr_ctx);
}

/* Global timestamp for the audio frames. */
static int64_t pts = 0;

static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size) {
  int error;

  /* Allocate as many pointers as there are audio channels.
   * Each pointer will point to the audio samples of the corresponding
   * channels (although it may be NULL for interleaved formats).
   * Allocate memory for the samples of all channels in one consecutive
   * block for convenience. */
  if ((error = av_samples_alloc_array_and_samples(
           converted_input_samples, NULL,
           output_codec_context->ch_layout.nb_channels, frame_size,
           output_codec_context->sample_fmt, 0)) < 0) {
    fprintf(stderr, "Could not allocate converted input samples (error '%s')\n",
            av_err2str(error));
    return error;
  }
  return 0;
}

static int convert_samples(const uint8_t **input_data, uint8_t **converted_data,
                           const int frame_size, SwrContext *resample_context) {
  int error;

  /* Convert the samples using the resampler. */
  if ((error = swr_convert(resample_context, converted_data, frame_size,
                           input_data, frame_size)) < 0) {
    fprintf(stderr, "Could not convert input samples (error '%s')\n",
            av_err2str(error));
    return error;
  }

  return 0;
}

static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size) {
  int error;

  /* Make the FIFO as large as it needs to be to hold both,
   * the old and the new samples. */
  if ((error = av_audio_fifo_realloc(
           fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
    fprintf(stderr, "Could not reallocate FIFO\n");
    return error;
  }

  /* Store the new samples in the FIFO buffer. */
  if (av_audio_fifo_write(fifo, (void **)converted_input_samples, frame_size) <
      frame_size) {
    fprintf(stderr, "Could not write data to FIFO\n");
    return AVERROR_EXIT;
  }
  return 0;
}

static int init_packet(AVPacket **packet) {
  if (!(*packet = av_packet_alloc())) {
    fprintf(stderr, "Could not allocate packet\n");
    return AVERROR(ENOMEM);
  }
  return 0;
}

static int init_input_frame(AVFrame **frame) {
  if (!(*frame = av_frame_alloc())) {
    fprintf(stderr, "Could not allocate input frame\n");
    return AVERROR(ENOMEM);
  }
  return 0;
}

static int decode_audio_frame(AVFrame *frame,
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished) {
  /* Packet used for temporary storage. */
  AVPacket *input_packet;
  int error;

  error = init_packet(&input_packet);
  if (error < 0) return error;

  *data_present = 0;
  *finished = 0;
  /* Read one audio frame from the input file into a temporary packet. */
  if ((error = av_read_frame(input_format_context, input_packet)) < 0) {
    /* If we are at the end of the file, flush the decoder below. */
    if (error == AVERROR_EOF)
      *finished = 1;
    else {
      fprintf(stderr, "Could not read frame (error '%s')\n", av_err2str(error));
      goto cleanup;
    }
  }

  /* Send the audio frame stored in the temporary packet to the decoder.
   * The input audio stream decoder is used to do this. */
  if ((error = avcodec_send_packet(input_codec_context, input_packet)) < 0) {
    fprintf(stderr, "Could not send packet for decoding (error '%s')\n",
            av_err2str(error));
    goto cleanup;
  }

  /* Receive one frame from the decoder. */
  error = avcodec_receive_frame(input_codec_context, frame);
  /* If the decoder asks for more data to be able to decode a frame,
   * return indicating that no data is present. */
  if (error == AVERROR(EAGAIN)) {
    error = 0;
    goto cleanup;
    /* If the end of the input file is reached, stop decoding. */
  } else if (error == AVERROR_EOF) {
    *finished = 1;
    error = 0;
    goto cleanup;
  } else if (error < 0) {
    fprintf(stderr, "Could not decode frame (error '%s')\n", av_err2str(error));
    goto cleanup;
    /* Default case: Return decoded data. */
  } else {
    *data_present = 1;
    goto cleanup;
  }

cleanup:
  av_packet_free(&input_packet);
  return error;
}

static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         AVCodecContext *output_codec_context,
                                         SwrContext *resampler_context,
                                         int *finished) {
  /* Temporary storage of the input samples of the frame read from the file. */
  AVFrame *input_frame = NULL;
  /* Temporary storage for the converted input samples. */
  uint8_t **converted_input_samples = NULL;
  int data_present;
  int ret = AVERROR_EXIT;

  /* Initialize temporary storage for one input frame. */
  if (init_input_frame(&input_frame)) goto cleanup;
  /* Decode one frame worth of audio samples. */
  if (decode_audio_frame(input_frame, input_format_context, input_codec_context,
                         &data_present, finished))
    goto cleanup;
  /* If we are at the end of the file and there are no more samples
   * in the decoder which are delayed, we are actually finished.
   * This must not be treated as an error. */
  if (*finished) {
    ret = 0;
    goto cleanup;
  }
  /* If there is decoded data, convert and store it. */
  if (data_present) {
    /* Initialize the temporary storage for the converted input samples. */
    if (init_converted_samples(&converted_input_samples, output_codec_context,
                               input_frame->nb_samples))
      goto cleanup;

    /* Convert the input samples to the desired output sample format.
     * This requires a temporary storage provided by converted_input_samples. */
    if (convert_samples((const uint8_t **)input_frame->extended_data,
                        converted_input_samples, input_frame->nb_samples,
                        resampler_context))
      goto cleanup;

    /* Add the converted input samples to the FIFO buffer for later processing.
     */
    if (add_samples_to_fifo(fifo, converted_input_samples,
                            input_frame->nb_samples))
      goto cleanup;
    ret = 0;
  }
  ret = 0;

cleanup:
  if (converted_input_samples) av_freep(&converted_input_samples[0]);
  av_freep(&converted_input_samples);
  av_frame_free(&input_frame);

  return ret;
}

static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present) {
  /* Packet used for temporary storage. */
  AVPacket *output_packet;
  int error;

  error = init_packet(&output_packet);
  if (error < 0) return error;

  /* Set a timestamp based on the sample rate for the container. */
  if (frame) {
    frame->pts = pts;
    pts += frame->nb_samples;
  }

  *data_present = 0;
  /* Send the audio frame stored in the temporary packet to the encoder.
   * The output audio stream encoder is used to do this. */
  error = avcodec_send_frame(output_codec_context, frame);
  /* Check for errors, but proceed with fetching encoded samples if the
   *  encoder signals that it has nothing more to encode. */
  if (error < 0 && error != AVERROR_EOF) {
    fprintf(stderr, "Could not send packet for encoding (error '%s')\n",
            av_err2str(error));
    goto cleanup;
  }

  /* Receive one encoded frame from the encoder. */
  error = avcodec_receive_packet(output_codec_context, output_packet);
  /* If the encoder asks for more data to be able to provide an
   * encoded frame, return indicating that no data is present. */
  if (error == AVERROR(EAGAIN)) {
    error = 0;
    goto cleanup;
    /* If the last frame has been encoded, stop encoding. */
  } else if (error == AVERROR_EOF) {
    error = 0;
    goto cleanup;
  } else if (error < 0) {
    fprintf(stderr, "Could not encode frame (error '%s')\n", av_err2str(error));
    goto cleanup;
    /* Default case: Return encoded data. */
  } else {
    *data_present = 1;
  }

  /* Write one audio frame from the temporary packet to the output file. */
  if (*data_present &&
      (error = av_write_frame(output_format_context, output_packet)) < 0) {
    fprintf(stderr, "Could not write frame (error '%s')\n", av_err2str(error));
    goto cleanup;
  }

cleanup:
  av_packet_free(&output_packet);
  return error;
}

static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size) {
  int error;

  /* Create a new frame to store the audio samples. */
  if (!(*frame = av_frame_alloc())) {
    fprintf(stderr, "Could not allocate output frame\n");
    return AVERROR_EXIT;
  }

  /* Set the frame's parameters, especially its size and format.
   * av_frame_get_buffer needs this to allocate memory for the
   * audio samples of the frame.
   * Default channel layouts based on the number of channels
   * are assumed for simplicity. */
  (*frame)->nb_samples = frame_size;
  av_channel_layout_copy(&(*frame)->ch_layout,
                         &output_codec_context->ch_layout);
  (*frame)->format = output_codec_context->sample_fmt;
  (*frame)->sample_rate = output_codec_context->sample_rate;

  /* Allocate the samples of the created frame. This call will make
   * sure that the audio frame can hold as many samples as specified. */
  if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
    fprintf(stderr, "Could not allocate output frame samples (error '%s')\n",
            av_err2str(error));
    av_frame_free(frame);
    return error;
  }

  return 0;
}

static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context) {
  /* Temporary storage of the output samples of the frame written to the file.
   */
  AVFrame *output_frame;
  /* Use the maximum number of possible samples per frame.
   * If there is less than the maximum possible frame size in the FIFO
   * buffer use this number. Otherwise, use the maximum possible frame size. */
  const int frame_size =
      FFMIN(av_audio_fifo_size(fifo), output_codec_context->frame_size);
  int data_written;

  /* Initialize temporary storage for one output frame. */
  if (init_output_frame(&output_frame, output_codec_context, frame_size))
    return AVERROR_EXIT;

  /* Read as many samples from the FIFO buffer as required to fill the frame.
   * The samples are stored in the frame temporarily. */
  if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) <
      frame_size) {
    fprintf(stderr, "Could not read data from FIFO\n");
    av_frame_free(&output_frame);
    return AVERROR_EXIT;
  }

  /* Encode one frame worth of audio samples. */
  if (encode_audio_frame(output_frame, output_format_context,
                         output_codec_context, &data_written)) {
    av_frame_free(&output_frame);
    return AVERROR_EXIT;
  }
  av_frame_free(&output_frame);
  return 0;
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
  if (output_fmt->audio_codec != AV_CODEC_ID_NONE) {
    add_stream(&audio_st, output_format_ctx, &audio_codec,
               output_fmt->audio_codec, &input_audio_st, input_format_ctx);
    have_audio = 1;
    encode_audio = 1;
  }

  /* Now that all the parameters are set, we can open the audio and
   * video codecs and allocate the necessary encode buffers. */
  if (have_video) open_video(output_format_ctx, video_codec, &video_st, opt);
  if (have_audio)
    open_audio(output_format_ctx, audio_codec, &input_audio_st, &audio_st, opt);

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
  AVFrame *frame = av_frame_alloc();
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

        write_video_frame(&video_st, frame, output_format_ctx);
      }
    }

    if (packet->stream_index == input_audio_st.stream_idx) {
      // Decode video frame
      int ret = avcodec_send_packet(input_audio_st.codec_ctx, packet);
      if (ret < 0) {
        fprintf(stderr, "Error sending packet for decoding\n");
        break;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(input_audio_st.codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          fprintf(stderr, "Error during decoding\n");
          break;
        }

        uint8_t **converted_input_samples = NULL;

        init_converted_samples(&converted_input_samples, video_st.enc,
                               frame->nb_samples);
        convert_samples((const uint8_t **)frame->extended_data,
                        converted_input_samples, frame->nb_samples,
                        audio_st.swr_ctx);
        add_samples_to_fifo(audio_st.fifo, converted_input_samples,
                            frame->nb_samples);

        if (converted_input_samples) av_freep(&converted_input_samples[0]);
        av_freep(&converted_input_samples);

        const int output_frame_size = audio_st.enc->frame_size;
        while (av_audio_fifo_size(audio_st.fifo) >= output_frame_size ||
               av_audio_fifo_size(audio_st.fifo) > 0) {
          /* Take one frame worth of audio samples from the FIFO buffer,
           * encode it and write it to the output file. */
          if (load_encode_and_write(audio_st.fifo, output_format_ctx,
                                    audio_st.enc))
            break;
        }

        /* If we are at the end of the input file and have encoded
         * all remaining samples, we can exit this loop and finish. */
        // if (finished) {
        //   int data_written;
        //   /* Flush the encoder as it may have delayed frames. */
        //   do {
        //     if (encode_audio_frame(NULL, output_format_ctx, audio_st.enc,
        //                            &data_written))
        //       break;
        //   } while (data_written);
        //   break;
        // }
      }
    }

    av_packet_unref(packet);
  }

  /* Loop as long as we have input samples to read or output samples
   * to write; abort as soon as we have neither. */
  // while (1) {
  //   /* Use the encoder's desired frame size for processing. */
  //   const int output_frame_size = audio_st.enc->frame_size;
  //   int finished = 0;

  //   printf("current number of sample = %d, frame size = %d\n",
  //          av_audio_fifo_size(audio_st.fifo), output_frame_size);

  //   /* Make sure that there is one frame worth of samples in the FIFO
  //    * buffer so that the encoder can do its work.
  //    * Since the decoder's and the encoder's frame size may differ, we
  //    * need to FIFO buffer to store as many frames worth of input samples
  //    * that they make up at least one frame worth of output samples. */
  //   while (av_audio_fifo_size(audio_st.fifo) < output_frame_size) {
  //     /* Decode one frame worth of audio samples, convert it to the
  //      * output sample format and put it into the FIFO buffer. */
  //     if (read_decode_convert_and_store(audio_st.fifo, input_format_ctx,
  //                                       input_audio_st.codec_ctx, audio_st.enc,
  //                                       audio_st.swr_ctx, &finished))
  //       break;

  //     /* If we are at the end of the input file, we continue
  //      * encoding the remaining audio samples to the output file. */
  //     if (finished) break;
  //   }

  //   /* If we have enough samples for the encoder, we encode them.
  //    * At the end of the file, we pass the remaining samples to
  //    * the encoder. */
  //   while (av_audio_fifo_size(audio_st.fifo) >= output_frame_size ||
  //          (finished && av_audio_fifo_size(audio_st.fifo) > 0))
  //     /* Take one frame worth of audio samples from the FIFO buffer,
  //      * encode it and write it to the output file. */
  //     if (load_encode_and_write(audio_st.fifo, output_format_ctx, audio_st.enc))
  //       break;

  //   /* If we are at the end of the input file and have encoded
  //    * all remaining samples, we can exit this loop and finish. */
  //   if (finished) {
  //     int data_written;
  //     /* Flush the encoder as it may have delayed frames. */
  //     do {
  //       if (encode_audio_frame(NULL, output_format_ctx, audio_st.enc,
  //                              &data_written))
  //         break;
  //     } while (data_written);
  //     break;
  //   }
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
