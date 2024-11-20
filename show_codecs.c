#include <libavcodec/avcodec.h>
#include <stdio.h>

void show_codecs() {
	void *iter = NULL;
	const AVCodec *codec = NULL;

	printf("Available decoders:\n");
	// Iterate over decoders
	while ((codec = av_codec_iterate(&iter)) != NULL) {
		if (av_codec_is_decoder(codec)) {
			printf("Decoder Name: %s\n", codec->name);
			printf("Decoder Description: %s\n", codec->long_name);
			printf("Decoder Type: %s\n", codec->type == AVMEDIA_TYPE_VIDEO ? "Video" : "Audio");
			printf("\n");
		}
	}

	printf("\nAvailable encoders:\n");
	// Iterate over encoders
	codec = NULL;
	iter = NULL;
	while ((codec = av_codec_iterate(&iter)) != NULL) {
		if (av_codec_is_encoder(codec)) {
			printf("Encoder Name: %s\n", codec->name); 
			printf("Encoder Description: %s\n", codec->long_name); 
			printf("Encoder Type: %s\n", codec->type == AVMEDIA_TYPE_VIDEO ? "Video" : "Audio"); 
			printf("\n");
		}
	}
}

int main() {
	// Show all available codecs
	show_codecs();

	return 0;
}

