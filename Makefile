all:sw sf sfq

sw:
	gcc -o sw	sdl-wave.c -lSDL2 
sf:
	gcc -o sf	sdl-ffmpeg.c -lSDL2 -lavformat -lavcodec -lavutil -lswresample
sfq:
	gcc -o sfq	sdl-ffmpeg-queue.c -lSDL2 -lavformat -lavcodec -lavutil -lswresample
