#include "output/Transcoder.h"

#include <windows.h>

#include <vector>

#include "encode/FFmpeg.h"
#include "output/Muxer.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

int RunTranscodeAv1(const std::filesystem::path& input, const std::filesystem::path& output) {
  // Ganzer Prozess im Hintergrundmodus: niedrigste CPU-, I/O- und Speicherpriorität.
  // Auch die Worker-Threads von SVT-AV1 erben das.
  SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
  SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);

  AVFormatContext* in = nullptr;
  AVCodecContext* dec = nullptr;
  AvCodecCtxPtr enc;
  Muxer mux;
  int rc = 1;
  try {
    std::string inPath = WideToUtf8(input.wstring());
    AvCheck(avformat_open_input(&in, inPath.c_str(), nullptr, nullptr), "Eingabe öffnen");
    AvCheck(avformat_find_stream_info(in, nullptr), "Streams lesen");
    int vIndex = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AvCheck(vIndex, "Videostream suchen");
    AVStream* vs = in->streams[vIndex];

    const AVCodec* dc = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!dc) throw std::runtime_error("Kein Decoder für das Eingangsvideo");
    dec = avcodec_alloc_context3(dc);
    avcodec_parameters_to_context(dec, vs->codecpar);
    dec->thread_count = 2;
    AvCheck(avcodec_open2(dec, dc, nullptr), "Decoder öffnen");

    const AVCodec* ec = avcodec_find_encoder_by_name("libsvtav1");
    if (!ec) throw std::runtime_error("SVT-AV1 ist nicht in FFmpeg enthalten");
    enc.reset(avcodec_alloc_context3(ec));
    AVRational fr = vs->avg_frame_rate.num ? vs->avg_frame_rate : AVRational{60, 1};
    enc->width = dec->width;
    enc->height = dec->height;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = vs->time_base;
    enc->framerate = fr;
    enc->gop_size = static_cast<int>(av_q2d(fr) * 5);
    enc->color_range = dec->color_range;
    enc->colorspace = dec->colorspace;
    enc->color_primaries = dec->color_primaries;
    enc->color_trc = dec->color_trc;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "8", 0);  // schnell; Qualität etwa wie HEVC bei ~60 % Größe
    av_dict_set(&opts, "crf", "32", 0);
    // Begrenzte Parallelität, damit nicht alle Kerne belegt werden
    av_dict_set(&opts, "svtav1-params", "lp=2", 0);
    int ret = avcodec_open2(enc.get(), ec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {  // ältere/neuere SVT-Versionen kennen "lp" evtl. nicht
      AVDictionary* o2 = nullptr;
      av_dict_set(&o2, "preset", "8", 0);
      av_dict_set(&o2, "crf", "32", 0);
      enc.reset(avcodec_alloc_context3(ec));
      enc->width = dec->width;
      enc->height = dec->height;
      enc->pix_fmt = AV_PIX_FMT_YUV420P;
      enc->time_base = vs->time_base;
      enc->framerate = fr;
      enc->gop_size = static_cast<int>(av_q2d(fr) * 5);
      enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      ret = avcodec_open2(enc.get(), ec, &o2);
      av_dict_free(&o2);
    }
    AvCheck(ret, "SVT-AV1 öffnen");

    // Ausgabe: Video neu, alle anderen Streams kopiert
    mux.Open(output, Muxer::Mode::Clip);
    std::vector<int> map(in->nb_streams, -1);
    AVCodecParameters* vpar = avcodec_parameters_alloc();
    avcodec_parameters_from_context(vpar, enc.get());
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      AVStream* s = in->streams[i];
      const AVDictionaryEntry* t = av_dict_get(s->metadata, "title", nullptr, 0);
      if (!t) t = av_dict_get(s->metadata, "handler_name", nullptr, 0);
      std::string title = t ? t->value : "";
      if (static_cast<int>(i) == vIndex)
        map[i] = mux.AddStream(vpar, enc->time_base, title);
      else if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        map[i] = mux.AddStream(s->codecpar, s->time_base, title);
    }
    avcodec_parameters_free(&vpar);
    mux.WriteHeader();

    AvPacketPtr pkt = MakePacket(), out = MakePacket();
    AvFramePtr frame = MakeFrame();
    auto drainEncoder = [&] {
      while (avcodec_receive_packet(enc.get(), out.get()) == 0) {
        out->stream_index = map[vIndex];
        mux.Write(out.get());
        av_packet_unref(out.get());
      }
    };
    auto drainDecoder = [&] {
      while (avcodec_receive_frame(dec, frame.get()) == 0) {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        AvCheck(avcodec_send_frame(enc.get(), frame.get()), "AV1 kodieren");
        av_frame_unref(frame.get());
        drainEncoder();
      }
    };

    while (av_read_frame(in, pkt.get()) >= 0) {
      const int si = pkt->stream_index;
      if (si == vIndex) {
        AvCheck(avcodec_send_packet(dec, pkt.get()), "Dekodieren");
        drainDecoder();
      } else if (map[static_cast<size_t>(si)] >= 0) {
        pkt->stream_index = map[static_cast<size_t>(si)];
        mux.Write(pkt.get());
      }
      av_packet_unref(pkt.get());
    }
    avcodec_send_packet(dec, nullptr);
    drainDecoder();
    avcodec_send_frame(enc.get(), nullptr);
    drainEncoder();
    mux.Close();
    rc = 0;
  } catch (const std::exception& e) {
    log::Error("AV1-Umwandlung: {}", e.what());
    mux.Abort();
    std::error_code ec;
    std::filesystem::remove(output, ec);
  }
  if (dec) avcodec_free_context(&dec);
  if (in) avformat_close_input(&in);
  return rc;
}

} // namespace litecap
