#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/mainloop.h>
#include <pulse/mainloop-api.h>
#include <pulse/mainloop-signal.h>
#include <pulse/operation.h>
#include <pulse/proplist.h>
#include <pulse/sample.h>
#include <pulse/stream.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

pa_mainloop_api *g_ml_api;
pa_context *g_pa_ctx;
AVCodecContext* pCodecCtx;
pa_sample_spec spec;
char *filename;

typedef struct PacketQueue
{
	AVPacketList *first_pkt; // 队头
	AVPacketList *last_pkt; // 队尾
	int nb_packets; //包的个数
	int size; // 占用空间的字节数
	pthread_mutex_t mutex; // 互斥信号量
	pthread_cond_t cond; // 条件变量
}PacketQueue;

PacketQueue audioq;

// 包队列初始化
void packet_queue_init(PacketQueue* q)
{
	//memset(q, 0, sizeof(PacketQueue));
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	pthread_mutex_init(&q->mutex,NULL); 
	pthread_cond_init(&q->cond,NULL); 
}

// 放入packet到队列中，不带头指针的队列
int packet_queue_put(PacketQueue*q, AVPacket *pkt)
{
	AVPacketList *pktl;
	if (av_dup_packet(pkt) < 0)
		return -1;

	pktl = (AVPacketList*)av_malloc(sizeof(AVPacketList));
	if (!pktl)
		return -1;

	pktl->pkt = *pkt;
	pktl->next = NULL;

	pthread_mutex_lock(&q->mutex);

	if (!q->last_pkt) // 队列为空，新插入元素为第一个元素
		q->first_pkt = pktl;
	else // 插入队尾
		q->last_pkt->next = pktl;

	q->last_pkt = pktl;

	q->nb_packets++;
	q->size += pkt->size;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);

	return 0;
}

// 从队列中取出packet
static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block)
{
	AVPacketList* pktl;
	int ret;

	pthread_mutex_lock(&q->mutex);

	while (1)
	{
		pktl = q->first_pkt;
		if (pktl)
		{
			q->first_pkt = pktl->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;

			q->nb_packets--;
			q->size -= pktl->pkt.size;

			*pkt = pktl->pkt;
			av_free(pktl);
			ret = 1;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
      printf("-------------------------sleep, cannot get packet\n");
			pthread_cond_wait(&q->cond, &q->mutex);
      printf("-------------------------packet ok,sleep over\n");
		}
	}

	pthread_mutex_unlock(&q->mutex);

	return ret;
}

// 解码音频数据
int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t* audio_buf, int buf_size)
{
	AVPacket pkt;
	int data_size = 0;
	SwrContext *swr_ctx = NULL;
  AVFrame *frame = av_frame_alloc();

  if (packet_queue_get(&audioq, &pkt, 1) < 0)
    return -1;

	int ret = avcodec_send_packet(aCodecCtx, &pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return -1;

	ret = avcodec_receive_frame(aCodecCtx, frame);
	if (ret < 0 && ret != AVERROR_EOF)
		return -1;

	// 设置通道数或channel_layout
	if (frame->channels > 0 && frame->channel_layout == 0)
		frame->channel_layout = av_get_default_channel_layout(frame->channels);
	else if (frame->channels == 0 && frame->channel_layout > 0)
		frame->channels = av_get_channel_layout_nb_channels(frame->channel_layout);

	enum AVSampleFormat dst_format = AV_SAMPLE_FMT_S16;//av_get_packed_sample_fmt((AVSampleFormat)frame->format);
	unsigned long long dst_layout = av_get_default_channel_layout(frame->channels);
	// 设置转换参数
	swr_ctx = swr_alloc_set_opts(NULL, dst_layout, dst_format, frame->sample_rate,
                               frame->channel_layout, (enum AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);
	if (!swr_ctx || swr_init(swr_ctx) < 0)
		return -1;

	// 计算转换后的sample个数 a * b / c
	int dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, frame->sample_rate) + frame->nb_samples, frame->sample_rate, frame->sample_rate, (enum AVRounding)1);
	// 转换，返回值为转换后的sample个数
	int nb = swr_convert(swr_ctx, &audio_buf, dst_nb_samples, (const uint8_t**)frame->data, frame->nb_samples);
	data_size = frame->channels * nb * av_get_bytes_per_sample(dst_format);

	av_frame_free(&frame);
	swr_free(&swr_ctx);
	return data_size;
}

static void quit(int ret) {
    g_ml_api->quit(g_ml_api, ret);
    exit(ret);
}

static void stream_state_callback(pa_stream *stream, void *userdata) {
    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_CREATING:
    case PA_STREAM_TERMINATED:
        break;
    case PA_STREAM_READY:
        printf("Playback stream succesfully created\n");
        break;
    case PA_STREAM_FAILED:
    default:
        printf("Playback stream error: %s", pa_strerror(pa_context_errno(g_pa_ctx)));
        goto fail;
    }
    return;

fail:
    quit(EXIT_FAILURE);
}

static void stream_drain_complete(pa_stream*s, int success, void *userdata) {
    printf("Playback stream fully drained.. Exiting application");
    quit(EXIT_SUCCESS);
}

static void stream_cork_success(pa_stream*s, int success, void *userdata) {
    printf("stream corked\n");
}

static void stream_write_callback(pa_stream *stream, size_t len, void *userdata);
static void tcb(pa_mainloop_api*a, pa_time_event *e, const struct timeval *tv, void *userdata) {
    printf("TIME EVENT\n");
    pa_stream *stream = (pa_stream*)userdata;
    pa_stream_cork(stream,0, stream_cork_success, NULL);
}

#define  MAX_AUDIO_FRAME_SIZE  192000
#define  min(a,b) ((a)<(b)?(a):(b))
static void stream_write_callback(pa_stream *stream, size_t len, void *userdata) {
    static uint8_t audio_buff[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;
    struct pa_operation *operation;
    size_t to_write, write_unit;
    int ret, audio_size;

    static int c=0;
    if(c==20){
        pa_stream_cork(stream,1, stream_cork_success, NULL);
	struct timeval tv;
	gettimeofday(&tv,NULL);
	tv.tv_sec+=10;
    	pa_time_event *te = g_ml_api->time_new(g_ml_api,&tv, tcb, stream);
	}
    printf("c==========%d\n",c);
    c++;
    while (len > 0)
      {
        if (audio_buf_index >= audio_buf_size)
          {
            audio_size = audio_decode_frame(pCodecCtx, audio_buff, sizeof(audio_buff));
            if (audio_size < 0)
              {
                audio_buf_size = 1024;
                memset(audio_buff, 0, audio_buf_size);
              }
            else
              audio_buf_size = audio_size;

            audio_buf_index = 0;
          }

        write_unit = pa_frame_size(&spec);
        to_write = audio_buf_size - audio_buf_index;
        to_write = min(len, to_write);
        to_write -= (to_write % write_unit);
        ret = pa_stream_write(stream, (uint8_t*)(audio_buff + audio_buf_index), to_write, NULL, 0, PA_SEEK_RELATIVE);
        if (ret < 0) {
          printf("Failed writing audio data to stream: %s", pa_strerror(pa_context_errno(g_pa_ctx)));
          goto fail;
        }

        len -= to_write;
        audio_buf_index += to_write;
      }

    /* if ((file->size - file->readi) < write_unit) { */
    /*     printf("Success! - Reached end of file"); */
    /*     printf("Draining playback stream before exit"); */
    /*     pa_stream_set_write_callback(stream, NULL, NULL); */
    /*     operation = pa_stream_drain(stream, stream_drain_complete, NULL); */
    /*     if (!operation) { */
    /*         printf("Could not drain playback stream: %s", pa_strerror(pa_context_errno(g_pa_ctx))); */
    /*         goto fail; */
    /*     } */
    /* } */
    return;

fail:
    quit(EXIT_FAILURE);
}

static void context_state_callback(pa_context *context, void *userdata) {
    pa_stream *stream;
    int ret;

    switch (pa_context_get_state(g_pa_ctx)) {
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_SETTING_NAME:
        break;
    case PA_CONTEXT_READY:
        printf("Connection established with PulseAudio sound server\n");
        stream = pa_stream_new(context, "playback stream", &spec, NULL);
        if (!stream){
          printf("pa_stream_connect_playback() failed: %s\n", pa_strerror(pa_context_errno(g_pa_ctx)));
          goto fail;
          
        }
        pa_stream_set_state_callback(stream, stream_state_callback, userdata);
        pa_stream_set_write_callback(stream, stream_write_callback, userdata);
        ret = pa_stream_connect_playback(stream, NULL, NULL, 0, NULL, NULL);
        if (ret < 0) {
            printf("pa_stream_connect_playback() failed: %s", pa_strerror(pa_context_errno(g_pa_ctx)));
            goto fail;
        }
        break;
    case PA_CONTEXT_TERMINATED:
        exit(EXIT_SUCCESS);
        break;
    case PA_CONTEXT_FAILED:
    default:
        printf("PulseAudio context connection failure: %s", pa_strerror(pa_context_errno(g_pa_ctx)));
        goto fail;
    }

    return;

fail:
    quit(EXIT_FAILURE);
}

void* play_thread(void *data)
{
  int ret = 0;
  pa_proplist *proplist = pa_proplist_new();
  if (!proplist) {
    printf("Couldn't create a PulseAudio property list");
    goto quit;
  }
  pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "pa-ffmpeg");
  pa_proplist_sets(proplist, PA_PROP_MEDIA_NAME, filename);

  pa_mainloop *m = pa_mainloop_new();
  if (!m) {
    printf("Couldn't create PulseAudio mainloop");
    goto quit;
  }

  g_ml_api = pa_mainloop_get_api(m);
  g_pa_ctx = pa_context_new_with_proplist(g_ml_api, NULL, proplist);
  if (!g_pa_ctx) {
    printf("Couldn't create client context");
    goto quit;
  }

  pa_context_set_state_callback(g_pa_ctx, context_state_callback, NULL);
  ret = pa_context_connect(g_pa_ctx, NULL, 0, NULL);
  if (ret < 0) {
    printf ("Couldn't connect to PulseAudio server: %s", pa_strerror(pa_context_errno(g_pa_ctx)));
    goto quit;
  }

  pa_mainloop_run(m, &ret);

  return NULL;
quit:
  exit(EXIT_FAILURE);
  return NULL;
}

int main(int argc, char **argv)
{
  pthread_t tid;
  if (argc != 2) {
    printf("usage: %s audiofile or url\n", argv[0]);
    return -1;
  }

  filename = argv[1];

  av_register_all();
  avformat_network_init();

	AVFormatContext* pFormatCtx = NULL;
	if (avformat_open_input(&pFormatCtx, filename, NULL, NULL) != 0)
		return -1; // 打开失败

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
		return -1; // 没有检测到流信息 stream infomation

	int audioStream = -1;
	for (int i = 0; i < pFormatCtx->nb_streams; i++)
    {
      if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
          audioStream = i;
          break;
        }
    }

	if (audioStream == -1)
		return -1; // 没有查找到视频流audio stream

	AVCodecContext* pCodecCtxOrg = pFormatCtx->streams[audioStream]->codec; // codec context
	AVCodec*pCodec = avcodec_find_decoder(pCodecCtxOrg->codec_id);
	if (!pCodec)
    {
      printf("Unsupported codec!\n");
      return -1;
    }

	pCodecCtx = avcodec_alloc_context3(pCodec);
	if (avcodec_copy_context(pCodecCtx, pCodecCtxOrg) != 0)
    {
      printf("Could not copy codec context!\n");
      return -1;
    }

  spec.format = PA_SAMPLE_S16LE;
  spec.rate = pCodecCtx->sample_rate;
  spec.channels = pCodecCtx->channels;

	avcodec_open2(pCodecCtx, pCodec, NULL);

  pthread_create(&tid,NULL,play_thread,NULL);

	AVPacket packet;
	packet_queue_init(&audioq);
	while (av_read_frame(pFormatCtx, &packet) >= 0)
    {
      if (packet.stream_index == audioStream)
        packet_queue_put(&audioq, &packet);
      else
        av_free_packet(&packet);

    }

  printf("produce packets over\n");
  getchar();
	return 0;
}
