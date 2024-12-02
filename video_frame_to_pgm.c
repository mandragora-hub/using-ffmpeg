#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdio.h>

void save_gray_frame(unsigned char* buf, int wrap, int xsize, int ysize,
                     char* filename) {
  FILE* f;
  int i;
  f = fopen(filename, "w");
  // writing the minimal required header for a pgm file format
  // portable graymap format ->
  // https://en.wikipedia.org/wiki/Netpbm_format#PGM_example
  fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);

  // writing line by line
  for (i = 0; i < ysize; i++) fwrite(buf + i * wrap, 1, xsize, f);
  fclose(f);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <input_file> <output_folder>\n", argv[0]);
    return 1;
  }

  const char* input_file = argv[1];

  // Open input file and allocate format context
  AVFormatContext* format_ctx = NULL;
  if (avformat_open_input(&format_ctx, input_file, NULL, NULL) < 0) {
    fprintf(stderr, "Could not open input file: %s\n", input_file);
    return 2;
  }

  printf("format %s, duration %ld us, bit_rate %ld\n",
         format_ctx->iformat->name, format_ctx->duration, format_ctx->bit_rate);

  // Retrieve stream information
  if (avformat_find_stream_info(format_ctx, NULL) < 0) {
    fprintf(stderr, "Could not find stream information\n");
    return 3;
  }

  // Find video stream
  int video_stream_idx = -1;
  for (unsigned int i = 0; i < format_ctx->nb_streams; ++i) {
    if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_idx = i;
      break;
    }
  }

  if (video_stream_idx == -1) {
    fprintf(stderr, "Could not find video stream\n");
    return 4;
  }

  // Show info about video stream
  printf("AVStream->time_base before open coded %d/%d\n",
         format_ctx->streams[video_stream_idx]->time_base.num,
         format_ctx->streams[video_stream_idx]->time_base.den);
  printf("AVStream->r_frame_rate before open coded %d/%d\n",
         format_ctx->streams[video_stream_idx]->r_frame_rate.num,
         format_ctx->streams[video_stream_idx]->r_frame_rate.den);
  printf("AVStream->start_time %ld\n",
         format_ctx->streams[video_stream_idx]->start_time);
  printf("AVStream->duration %ld\n",
         format_ctx->streams[video_stream_idx]->duration);

  // Get codec parameters and codec context
  AVCodecParameters* codecpar = format_ctx->streams[video_stream_idx]->codecpar;
  AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(codec_ctx, codecpar);

  // Open codec
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
    fprintf(stderr, "Could not open codec\n");
    return 5;
  }

  // Allocate frame and packet
  AVFrame* frame = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();

  // Read frames from the video stream
  while (av_read_frame(format_ctx, packet) >= 0) {
    if (packet->stream_index == video_stream_idx) {
      // Decode video frame
      int ret = avcodec_send_packet(codec_ctx, packet);
      if (ret < 0) {
        fprintf(stderr, "Error sending packet for decoding\n");
        break;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          fprintf(stderr, "Error during decoding\n");
          break;
        }

        // Print basic frame properties
        printf("Frame: %ld (type=%d, size=%d bytes) pts %ld key_frame %d\n",
               codec_ctx->frame_num, av_get_picture_type_char(frame->pict_type),
               frame->pkt_size, frame->pts, frame->key_frame);

        // Save to pgm
        char frame_filename[1024];
        snprintf(frame_filename, sizeof(frame_filename), "%s/%s-%ld.pgm",
                 argv[2], "frame", codec_ctx->frame_num);

        // Check if the frame is a planar YUV 4:2:0, 12bpp
        // That is the format of the provided .mp4 file
        // RGB formats will definitely not give a gray image
        // Other YUV image may do so, but untested, so give a warning
        if (frame->format != AV_PIX_FMT_YUV420P) {
          printf(
              "Warning: the generated file may not be a grayscale image, but "
              "could e.g. be just the R component if the video format is RGB");
        }
        // save a grayscale frame into a .pgm file
        save_gray_frame(frame->data[0], frame->linesize[0], frame->width,
                        frame->height, frame_filename);
      }
    }

    av_packet_unref(packet);
  }

  // Free allocated resources
  av_frame_free(&frame);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&format_ctx);

  return 0;
}
