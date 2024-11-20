#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

#include <stdio.h>

int main(int argc, char* argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
		return 1;
	}

	const char* input_file = argv[1];

	// Open input file and allocate format context
	AVFormatContext* format_ctx = NULL;
	if (avformat_open_input(&format_ctx, input_file, NULL, NULL) < 0) {
		fprintf(stderr, "Could not open input file: %s\n", input_file);
		return 2;
	}

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
						codec_ctx->frame_num,
						av_get_picture_type_char(frame->pict_type),
						frame->pkt_size,
						frame->pts,
						frame->key_frame);
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
