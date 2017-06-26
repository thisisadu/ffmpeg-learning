// 3_wav_play.cpp : 定义控制台应用程序的入口点。
#include <stdio.h>
#include <SDL2/SDL.h>

typedef struct  
{
	Uint8 * data; // pcm buffer
	int offset; // current read pos
	int data_len; // left data length
	int is_exit; // is audio play buffer empty?
}AudioPlayContext;

#define min(a,b) (a<b?a:b)
void MyAudioCallback(void* userdata, Uint8* stream, int len)
{
	AudioPlayContext * context = (AudioPlayContext*)(userdata);
	int copy_len = min(len, context->data_len);
	memcpy(stream, context->data+context->offset, copy_len);
	context->data_len -= copy_len;
	context->offset += copy_len;
	if (context->data_len <= 0)
	{
		context->is_exit = 1;
	}

	if (copy_len < len)
	{
		memset(stream+copy_len, 0, len-copy_len);
	}
}

int main(int argc, char* argv[])
{
	SDL_Init(SDL_INIT_AUDIO);

	SDL_AudioSpec wav_spec;

	Uint32 wav_length;
	Uint8 *wav_buffer;

	/* Load the WAV */
	if (SDL_LoadWAV("nf.wav", &wav_spec, &wav_buffer, &wav_length) == NULL) 
	{
		fprintf(stderr, "Could not open test.wav: %s\n", SDL_GetError());
		return -1;
	}
	
	AudioPlayContext context;
	context.data = wav_buffer;
	context.data_len = wav_length;
	context.offset = 0;
	context.is_exit = 0;

	// open audio device
	SDL_AudioSpec want, have;
	want = wav_spec;
	want.callback = MyAudioCallback;  // you wrote this function elsewhere.
	want.userdata = &context;

	if (SDL_OpenAudio(&want, &have) < 0) 
	{
		printf("Failed to open audio: %s\n", SDL_GetError());
		return -2;
	}
	
	if (have.format != want.format)
		printf("We didn't get Float32 audio format.\n");

	// start audio playing.
	SDL_PauseAudio(0);  

	// wait for the end
	while(!context.is_exit){
		SDL_Delay(1000);
	}

	SDL_CloseAudio();
	/* Do stuff with the WAV data, and then... */
	SDL_FreeWAV(wav_buffer);
	SDL_Quit();
	return 0;
}

