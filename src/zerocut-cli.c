#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double start;
    double dur;
} CutSegment;

int main(int argc, char *argv[]) {
    const char *in_file = NULL;
    const char *out_file = NULL;
    CutSegment segs[128];
    int seg_count = 0;

    /* Парсинг аргументов (совместимый с интерфейсом AsfCut: -i, -o, -start, -dur) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            in_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_file = argv[++i];
        } else if (strcmp(argv[i], "-start") == 0 && i + 1 < argc) {
            segs[seg_count].start = atof(argv[++i]);
            segs[seg_count].dur = 0.0;
            if (i + 2 < argc && strcmp(argv[i + 1], "-dur") == 0) {
                i++;
                segs[seg_count].dur = atof(argv[++i]);
            }
            seg_count++;
        }
    }

    if (!in_file || !out_file || seg_count == 0) {
        fprintf(stderr, "Использование: %s -i input.wmv -o output.wmv -start SEC -dur SEC [...]\n", argv[0]);
        return 1;
    }

    AVFormatContext *ifmt_ctx = NULL;
    if (avformat_open_input(&ifmt_ctx, in_file, NULL, NULL) < 0) {
        fprintf(stderr, "Не удалось открыть входной файл: %s\n", in_file);
        return 1;
    }
    if (avformat_find_stream_info(ifmt_ctx, NULL) < 0) {
        fprintf(stderr, "Не удалось прочитать потоки файла\n");
        avformat_close_input(&ifmt_ctx);
        return 1;
    }

    AVFormatContext *ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_file);
    if (!ofmt_ctx) {
        fprintf(stderr, "Не удалось создать выходной контекст для: %s\n", out_file);
        avformat_close_input(&ifmt_ctx);
        return 1;
    }

    int nb_streams = ifmt_ctx->nb_streams;
    int *stream_mapping = av_calloc(nb_streams, sizeof(int));
    int out_stream_idx = 0;

    for (int i = 0; i < nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_par = in_stream->codecpar;
        if (in_par->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_par->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_par->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }
        stream_mapping[i] = out_stream_idx++;
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "Ошибка создания выходного потока\n");
            return 1;
        }
        avcodec_parameters_copy(out_stream->codecpar, in_par);
        out_stream->codecpar->codec_tag = 0;
    }

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, out_file, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Не удалось открыть выходной файл для записи: %s\n", out_file);
            return 1;
        }
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) {
        fprintf(stderr, "Ошибка записи заголовка WMV/ASF\n");
        return 1;
    }

    AVPacket *pkt = av_packet_alloc();
    int64_t *timeline_offset = av_calloc(nb_streams, sizeof(int64_t));
    int64_t *last_dts = av_calloc(nb_streams, sizeof(int64_t));
    for (int i = 0; i < nb_streams; i++) last_dts[i] = AV_NOPTS_VALUE;

    /* Обработка каждого указанного отрезка */
    for (int s = 0; s < seg_count; s++) {
        double start_sec = segs[s].start;
        double end_sec = (segs[s].dur > 0.0) ? (start_sec + segs[s].dur) : 1e12;

        int64_t seek_ts = (int64_t)(start_sec * AV_TIME_BASE);
        av_seek_frame(ifmt_ctx, -1, seek_ts, AVSEEK_FLAG_BACKWARD);

        int64_t *seg_start_pts = av_calloc(nb_streams, sizeof(int64_t));
        int64_t *seg_max_dts = av_calloc(nb_streams, sizeof(int64_t));
        for (int i = 0; i < nb_streams; i++) {
            seg_start_pts[i] = AV_NOPTS_VALUE;
            seg_max_dts[i] = 0;
        }

        while (av_read_frame(ifmt_ctx, pkt) >= 0) {
            int idx = pkt->stream_index;
            if (idx >= nb_streams || stream_mapping[idx] < 0) {
                av_packet_unref(pkt);
                continue;
            }

            AVStream *in_stream = ifmt_ctx->streams[idx];
            AVStream *out_stream = ofmt_ctx->streams[stream_mapping[idx]];

            double pkt_time = (pkt->pts != AV_NOPTS_VALUE)
                ? pkt->pts * av_q2d(in_stream->time_base)
                : pkt->dts * av_q2d(in_stream->time_base);

            if (pkt_time > end_sec) {
                av_packet_unref(pkt);
                break;
            }

            int64_t raw_ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            if (raw_ts == AV_NOPTS_VALUE) {
                av_packet_unref(pkt);
                continue;
            }

            if (seg_start_pts[idx] == AV_NOPTS_VALUE) {
                seg_start_pts[idx] = raw_ts;
            }

            int64_t rel_pts = ((pkt->pts != AV_NOPTS_VALUE ? pkt->pts : raw_ts) - seg_start_pts[idx]) + timeline_offset[idx];
            int64_t rel_dts = (raw_ts - seg_start_pts[idx]) + timeline_offset[idx];
            if (rel_pts < 0) rel_pts = 0;
            if (rel_dts < 0) rel_dts = 0;

            pkt->pts = av_rescale_q_rnd(rel_pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
            pkt->dts = av_rescale_q_rnd(rel_dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
            pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
            pkt->pos = -1;
            pkt->stream_index = stream_mapping[idx];

            /* Защита от неупорядоченных DTS при склейке */
            if (last_dts[idx] != AV_NOPTS_VALUE && pkt->dts <= last_dts[idx]) {
                pkt->dts = last_dts[idx] + 1;
                if (pkt->pts < pkt->dts) pkt->pts = pkt->dts;
            }
            last_dts[idx] = pkt->dts;

            if (rel_dts + pkt->duration > seg_max_dts[idx]) {
                seg_max_dts[idx] = rel_dts + pkt->duration;
            }

            av_interleaved_write_frame(ofmt_ctx, pkt);
            av_packet_unref(pkt);
        }

        for (int i = 0; i < nb_streams; i++) {
            timeline_offset[i] += seg_max_dts[i];
        }
        av_free(seg_start_pts);
        av_free(seg_max_dts);
    }

    av_write_trailer(ofmt_ctx);

    av_packet_free(&pkt);
    av_free(timeline_offset);
    av_free(last_dts);
    av_free(stream_mapping);
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmt_ctx->pb);
    }
    avformat_free_context(ofmt_ctx);
    avformat_close_input(&ifmt_ctx);
    return 0;
}
