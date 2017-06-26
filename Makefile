all:sw sf sfq pf

sw:
	gcc -o sw	sdl-wave.c -lSDL2 
sf:
	gcc -o sf	sdl-ffmpeg.c -lSDL2 -lavformat -lavcodec -lavutil -lswresample
sfq:
	gcc -o sfq	sdl-ffmpeg-queue.c -lSDL2 -lavformat -lavcodec -lavutil -lswresample
pf:
	gcc -o pf	pa-ffmpeg-queue.c -lpulse -lavformat -lavcodec -lavutil -lpthread -lswresample

clean:
	rm sw sf sfq pf
