#include "tools/SelfTest.h"

#include <windows.h>
#include <psapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11_4.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "capture/CaptureTarget.h"
#include "capture/ColorConverter.h"
#include "capture/GpuDevice.h"
#include "capture/WgcCapture.h"
#include "config/Config.h"
#include "encode/FFmpeg.h"
extern "C" {
#include <libswscale/swscale.h>
}
#include "output/FileNamer.h"
#include "pipeline/CaptureSession.h"
#include "pipeline/Recorder.h"
#include "pipeline/ReplayBuffer.h"
#include "util/Log.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace litecap {

namespace {

void Out(const std::string& s) {
  std::string line = s + "\n";
  DWORD written;
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h && h != INVALID_HANDLE_VALUE)
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  log::Info("{}", s);
}

// NV12 (Studio-Range BT.709) → BGR-Bitmap, nur für das Testbild.
bool SaveNv12AsBmp(const std::filesystem::path& file, const uint8_t* y, const uint8_t* uv,
                   UINT pitch, UINT w, UINT h) {
  const UINT rowBytes = (w * 3 + 3) & ~3u;
  std::vector<uint8_t> bgr(static_cast<size_t>(rowBytes) * h);
  for (UINT row = 0; row < h; ++row) {
    uint8_t* dst = &bgr[static_cast<size_t>(h - 1 - row) * rowBytes];  // BMP ist bottom-up
    for (UINT x = 0; x < w; ++x) {
      float Y = (y[row * pitch + x] - 16) * 1.164f;
      const uint8_t* c = &uv[(row / 2) * pitch + (x & ~1u)];
      float U = c[0] - 128.0f, V = c[1] - 128.0f;
      auto clip = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f)); };
      dst[x * 3 + 0] = clip(Y + 2.112f * U);
      dst[x * 3 + 1] = clip(Y - 0.213f * U - 0.533f * V);
      dst[x * 3 + 2] = clip(Y + 1.793f * V);
    }
  }
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  ih.biSize = sizeof(ih);
  ih.biWidth = static_cast<LONG>(w);
  ih.biHeight = static_cast<LONG>(h);
  ih.biPlanes = 1;
  ih.biBitCount = 24;
  ih.biSizeImage = static_cast<DWORD>(bgr.size());
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + ih.biSizeImage;
  std::ofstream f(file, std::ios::binary);
  f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
  f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
  f.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
  return static_cast<bool>(f);
}

} // namespace

int RunCaptureSelfTest(int seconds) {
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) AllocConsole();
  SetConsoleOutputCP(CP_UTF8);
  try {
    Config cfg = Config::Load(paths::ConfigFile());
    cfg.Validate();

    std::string err;
    auto target = ResolveTarget(cfg.capture, err);
    if (!target) {
      Out("FEHLER: " + err);
      return 2;
    }
    Out("Ziel: " + WideToUtf8(target->name));

    GpuDevice gpu;
    gpu.Create(GpuVendor::Unknown, target->HostMonitor());
    Out("GPU: " + WideToUtf8(gpu.AdapterName()) + " (" + VendorName(gpu.Vendor()) + ")");

    WgcCapture capture;
    ColorConverter conv;
    winrt::com_ptr<ID3D11Texture2D> nv12;
    std::atomic<uint64_t> frames{0};
    std::mutex statsMutex;
    std::vector<double> convertMs;
    std::atomic<bool> closed{false};

    auto start = [&](UINT outW, UINT outH) {
      conv.Init(gpu, outW, outH, cfg.video.fps);
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = conv.OutWidth();
      td.Height = conv.OutHeight();
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_NV12;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      winrt::check_hresult(gpu.Device()->CreateTexture2D(&td, nullptr, nv12.put()));
    };

    capture.Start(
        gpu, *target, cfg.capture.captureCursor, cfg.video.fps,
        [&](const CapturedFrame& f) {
          auto t0 = std::chrono::steady_clock::now();
          GpuLock lock(gpu.Multithread());
          if (!nv12) return;  // Konverter noch nicht initialisiert
          conv.Convert(f.texture, f.width, f.height, nv12.get());
          double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          std::lock_guard l(statsMutex);
          convertMs.push_back(ms);
          ++frames;
        },
        [&] { closed = true; });

    // Ausgabegröße: konfiguriert oder native Größe der Quelle
    auto src = capture.SourceSize();
    UINT outW = cfg.video.width ? cfg.video.width : static_cast<UINT>(src.Width);
    UINT outH = cfg.video.height ? cfg.video.height : static_cast<UINT>(src.Height);
    {
      GpuLock lock(gpu.Multithread());
      start(outW, outH);
    }

    Out("Aufnahme läuft " + std::to_string(seconds) + " s – bewege etwas auf dem Bildschirm…");
    for (int i = 0; i < seconds * 10 && !closed; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint64_t dropped = capture.FramesDropped();
    capture.Stop();

    // Ergebnis
    std::vector<double> ms;
    {
      std::lock_guard l(statsMutex);
      ms = convertMs;
    }
    std::sort(ms.begin(), ms.end());
    double avg = 0;
    for (double v : ms) avg += v;
    if (!ms.empty()) avg /= ms.size();
    Out(std::format("Frames: {} ({:.1f} fps), verworfen (über FPS-Limit): {}", frames.load(),
                    frames.load() / static_cast<double>(seconds), dropped));
    if (!ms.empty())
      Out(std::format("Konvertierung (CPU-Zeit für GPU-Befehle): Ø {:.3f} ms, p95 {:.3f} ms",
                      avg, ms[ms.size() * 95 / 100]));
    if (frames == 0) {
      Out("HINWEIS: Keine Frames – WGC liefert nur bei Bildänderungen. Bewege die Maus und "
          "wiederhole den Test.");
      return 3;
    }

    // Testbild: NV12 → Staging → BMP
    D3D11_TEXTURE2D_DESC td;
    nv12->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(gpu.Device()->CreateTexture2D(&td, nullptr, staging.put()));
    GpuLock lock(gpu.Multithread());
    gpu.Context()->CopyResource(staging.get(), nv12.get());
    D3D11_MAPPED_SUBRESOURCE map;
    winrt::check_hresult(gpu.Context()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map));
    auto* base = static_cast<const uint8_t*>(map.pData);
    auto file = paths::LocalDataDir() / L"selftest-capture.bmp";
    bool ok = SaveNv12AsBmp(file, base, base + static_cast<size_t>(map.RowPitch) * td.Height,
                            map.RowPitch, td.Width, td.Height);
    gpu.Context()->Unmap(staging.get(), 0);
    Out(std::string(ok ? "Testbild gespeichert: " : "Testbild FEHLER: ") + WideToUtf8(file.wstring()));
    Out("OK");
    return 0;
  } catch (const winrt::hresult_error& e) {
    Out("FEHLER: " + WideToUtf8(e.message()) + " (" + HResultMessage(e.code()) + ")");
  } catch (const std::exception& e) {
    Out(std::string("FEHLER: ") + e.what());
  }
  return 1;
}

namespace {

// Liest die Datei ein und fasst Streams, Dauer und Keyframes zusammen; Tonspuren werden
// dekodiert, um Pegel und Länge zu prüfen.
bool VerifyFile(const std::filesystem::path& file) {
  AVFormatContext* fmt = nullptr;
  std::string path = WideToUtf8(file.wstring());
  int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
  if (ret < 0) {
    Out("PRÜFUNG FEHLGESCHLAGEN: Datei nicht lesbar – " + AvError(ret));
    return false;
  }
  avformat_find_stream_info(fmt, nullptr);

  struct StreamStat {
    int64_t packets = 0, keyframes = 0, bytes = 0;
    int64_t firstPtsMs = INT64_MIN, lastEndMs = 0;
    int64_t prevDts = INT64_MIN;
    bool monotonic = true;
    AVCodecContext* dec = nullptr;
    double peak = 0, sumSq = 0;
    int64_t samples = 0;
  };
  std::vector<StreamStat> st(fmt->nb_streams);
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    auto* par = fmt->streams[i]->codecpar;
    const char* title = nullptr;
    if (auto* t = av_dict_get(fmt->streams[i]->metadata, "title", nullptr, 0)) title = t->value;
    else if (auto* h = av_dict_get(fmt->streams[i]->metadata, "handler_name", nullptr, 0)) title = h->value;
    Out(std::format("  Stream {}: {} {} {}", i, par->codec_type == AVMEDIA_TYPE_VIDEO ? "Video" : "Audio",
                    avcodec_get_name(par->codec_id),
                    par->codec_type == AVMEDIA_TYPE_VIDEO
                        ? std::format("{}x{}", par->width, par->height)
                        : std::format("{} Hz, \"{}\"", par->sample_rate, title ? title : "")));
    if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
      const AVCodec* dc = avcodec_find_decoder(par->codec_id);
      if (dc) {
        st[i].dec = avcodec_alloc_context3(dc);
        avcodec_parameters_to_context(st[i].dec, par);
        if (avcodec_open2(st[i].dec, dc, nullptr) < 0) avcodec_free_context(&st[i].dec);
      }
    }
  }

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frm = av_frame_alloc();
  while (av_read_frame(fmt, pkt) >= 0) {
    auto& s = st[static_cast<size_t>(pkt->stream_index)];
    AVRational tb = fmt->streams[pkt->stream_index]->time_base;
    ++s.packets;
    s.bytes += pkt->size;
    if (pkt->flags & AV_PKT_FLAG_KEY) ++s.keyframes;
    if (pkt->dts != AV_NOPTS_VALUE) {
      if (pkt->dts <= s.prevDts) s.monotonic = false;
      s.prevDts = pkt->dts;
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
      int64_t ms = av_rescale_q(pkt->pts, tb, {1, 1000});
      if (s.firstPtsMs == INT64_MIN) s.firstPtsMs = ms;
      s.lastEndMs = std::max(s.lastEndMs, ms + av_rescale_q(pkt->duration, tb, {1, 1000}));
    }
    if (s.dec && avcodec_send_packet(s.dec, pkt) >= 0) {
      while (avcodec_receive_frame(s.dec, frm) >= 0) {
        if (frm->format == AV_SAMPLE_FMT_FLTP) {
          for (int c = 0; c < frm->ch_layout.nb_channels; ++c) {
            auto* d = reinterpret_cast<const float*>(frm->data[c]);
            for (int n = 0; n < frm->nb_samples; ++n) {
              s.peak = std::max(s.peak, static_cast<double>(std::abs(d[n])));
              s.sumSq += d[n] * d[n];
            }
          }
          s.samples += static_cast<int64_t>(frm->nb_samples) * frm->ch_layout.nb_channels;
        }
        av_frame_unref(frm);
      }
    }
    av_packet_unref(pkt);
  }

  bool ok = true;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    auto& s = st[i];
    double secs = (s.lastEndMs - (s.firstPtsMs == INT64_MIN ? 0 : s.firstPtsMs)) / 1000.0;
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      Out(std::format("  Video: {} Frames, {} Keyframes, Start {} ms, {:.2f} s, Ø {:.1f} Mbit/s, "
                      "Zeitstempel {}",
                      s.packets, s.keyframes, s.firstPtsMs, secs,
                      secs > 0 ? s.bytes * 8 / secs / 1e6 : 0.0, s.monotonic ? "OK" : "FEHLERHAFT"));
      ok = ok && s.packets > 0 && s.keyframes > 0 && s.monotonic;
    } else {
      auto db = [](double v) { return v > 0 ? 20 * std::log10(v) : -120.0; };
      double rms = s.samples ? std::sqrt(s.sumSq / s.samples) : 0;
      Out(std::format("  Audio {}: {} Pakete, Start {} ms, {:.2f} s, Spitze {:.1f} dBFS, RMS {:.1f} dBFS{}",
                      i, s.packets, s.firstPtsMs, secs, db(s.peak), db(rms),
                      s.peak == 0 ? " (Stille)" : ""));
      ok = ok && s.packets > 0 && s.monotonic;
    }
    if (s.dec) avcodec_free_context(&s.dec);
  }
  av_frame_free(&frm);
  av_packet_free(&pkt);
  avformat_close_input(&fmt);
  return ok;
}

} // namespace

int RunRecordSelfTest(int seconds, const std::string& codec, const std::string& encoder) {
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) AllocConsole();
  SetConsoleOutputCP(CP_UTF8);
  try {
    Config cfg = Config::Load(paths::ConfigFile());
    if (!codec.empty()) cfg.video.codec = codec;
    if (!encoder.empty()) cfg.video.encoder = encoder;
    for (const auto& fix : cfg.Validate()) Out("Config: " + fix);

    FILETIME c0, e0, k0, u0, c1, e1, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    auto t0 = std::chrono::steady_clock::now();

    CaptureSession session;
    std::atomic<bool> failed{false};
    session.Start(cfg, {[&](const std::string& e) {
                          Out("FEHLER (Encoder): " + e);
                          failed = true;
                        },
                        [&] { Out("Quelle geschlossen"); }});
    Out("Encoder: " + session.Description() +
        (session.Video().Choice().transcodeToAv1 ? " (AV1 nachträglich)" : ""));

    Recorder rec;
    auto file = MakeOutputPath(cfg, session.SourceName(), L"Selbsttest");
    rec.Start(session, file);
    Out("Aufnahme läuft " + std::to_string(seconds) + " s …");
    for (int i = 0; i < seconds * 10 && !failed; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rec.Stop();
    session.Stop();

    GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
    auto toMs = [](FILETIME a, FILETIME b) {
      auto v = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
      return (v(b) - v(a)) / 10000.0;
    };
    double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    double cpuMs = toMs(k0, k1) + toMs(u0, u1);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    Out(std::format("CPU-Last des Prozesses: {:.2f} % (über alle {} Kerne), {:.1f} % eines Kerns",
                    cpuMs / wallMs / si.dwNumberOfProcessors * 100, si.dwNumberOfProcessors,
                    cpuMs / wallMs * 100));

    Out("Datei: " + WideToUtf8(file.wstring()));
    bool ok = VerifyFile(file) && !failed;
    Out(ok ? "OK" : "FEHLER");
    return ok ? 0 : 1;
  } catch (const winrt::hresult_error& e) {
    Out("FEHLER: " + WideToUtf8(e.message()) + " (" + HResultMessage(e.code()) + ")");
  } catch (const std::exception& e) {
    Out(std::string("FEHLER: ") + e.what());
  }
  return 1;
}

int RunReplaySelfTest(int bufferSeconds, int runSeconds) {
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) AllocConsole();
  SetConsoleOutputCP(CP_UTF8);
  try {
    Config cfg = Config::Load(paths::ConfigFile());
    cfg.Validate();
    CaptureSession session;
    std::atomic<bool> failed{false};
    session.Start(cfg, {[&](const std::string& e) {
                          Out("FEHLER (Encoder): " + e);
                          failed = true;
                        },
                        {}});
    Out("Encoder: " + session.Description());

    ReplayBuffer replay;
    replay.Start(session, bufferSeconds, cfg.video.bitrateKbps, cfg.audio.bitrateKbps * 2,
                 cfg.ReplayBufferDir());
    Out(std::format("Replay-Puffer {} s, Ringdatei {:.0f} MB – läuft {} s …", bufferSeconds,
                    replay.FileCapacity() / 1048576.0, runSeconds));

    HANDLE proc = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    size_t peakPrivate = 0;
    for (int i = 0; i < runSeconds * 10 && !failed; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (GetProcessMemoryInfo(proc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        peakPrivate = std::max(peakPrivate, static_cast<size_t>(pmc.PrivateUsage));
    }
    Out(std::format("Gepuffert: {:.1f} s; RAM: Working Set {:.1f} MB, privat (Spitze) {:.1f} MB",
                    replay.BufferedSeconds(), pmc.WorkingSetSize / 1048576.0,
                    peakPrivate / 1048576.0));

    auto file = MakeOutputPath(cfg, session.SourceName(), L"Clip");
    auto t0 = std::chrono::steady_clock::now();
    double secs = replay.SaveClip(file);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Out(std::format("Clip gespeichert in {:.0f} ms: {:.1f} s", ms, secs));
    replay.Stop();
    session.Stop();

    Out("Datei: " + WideToUtf8(file.wstring()));
    bool ok = VerifyFile(file) && !failed && secs >= bufferSeconds - 3 && secs <= bufferSeconds + 3;
    Out(ok ? "OK" : "FEHLER");
    return ok ? 0 : 1;
  } catch (const winrt::hresult_error& e) {
    Out("FEHLER: " + WideToUtf8(e.message()));
  } catch (const std::exception& e) {
    Out(std::string("FEHLER: ") + e.what());
  }
  return 1;
}

int RunDecodeTest(const std::wstring& file, double snapSec) {
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) AllocConsole();
  SetConsoleOutputCP(CP_UTF8);
  av_log_set_level(AV_LOG_ERROR);
  std::string path = WideToUtf8(file);
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
    Out("Datei nicht lesbar");
    return 1;
  }
  avformat_find_stream_info(fmt, nullptr);
  int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vi < 0) {
    Out("Kein Videostream");
    return 1;
  }
  AVStream* st = fmt->streams[vi];
  Out(std::format("Codec {}, {}x{}, extradata {} Bytes, Dauer {:.1f} s", avcodec_get_name(st->codecpar->codec_id),
                  st->codecpar->width, st->codecpar->height, st->codecpar->extradata_size,
                  fmt->duration / 1e6));
  const AVCodec* dc = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext* dec = avcodec_alloc_context3(dc);
  avcodec_parameters_to_context(dec, st->codecpar);
  avcodec_open2(dec, dc, nullptr);
  Out(std::format("Profil {} ({}), Level {}, Farbraum {}/{}/{}, Range {}, Bildrate {}/{}, Zeitbasis {}/{}",
                  st->codecpar->profile, avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile) ?
                  avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile) : "?",
                  st->codecpar->level, static_cast<int>(st->codecpar->color_primaries),
                  static_cast<int>(st->codecpar->color_trc), static_cast<int>(st->codecpar->color_space),
                  static_cast<int>(st->codecpar->color_range), st->avg_frame_rate.num, st->avg_frame_rate.den,
                  st->time_base.num, st->time_base.den));
  for (unsigned i = 0; i < fmt->nb_streams; ++i)
    Out(std::format("  Stream {}: start_time {} ({:.3f} s), Dauer {:.3f} s", i, fmt->streams[i]->start_time,
                    fmt->streams[i]->start_time * av_q2d(fmt->streams[i]->time_base),
                    fmt->streams[i]->duration * av_q2d(fmt->streams[i]->time_base)));

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frm = av_frame_alloc();
  int64_t packets = 0, frames = 0, errors = 0, firstKeyIdx = -1;
  int64_t lastPts = AV_NOPTS_VALUE, maxGap = 0;  // größter Abstand zweier Frames
  std::vector<std::string> samples;
  AVFrame* keep = av_frame_alloc();
  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vi) {
      if (firstKeyIdx < 0 && (pkt->flags & AV_PKT_FLAG_KEY)) firstKeyIdx = packets;
      if (lastPts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) maxGap = std::max(maxGap, pkt->pts - lastPts);
      lastPts = pkt->pts;
      if (packets < 3 || (pkt->flags & AV_PKT_FLAG_KEY && packets < 400)) {
        // AVCC: 4-Byte-Längenpräfix + NAL; Typ in den unteren 5 Bits (H.264)
        std::string nals;
        for (int off = 0; off + 4 < pkt->size;) {
          uint32_t len = (pkt->data[off] << 24) | (pkt->data[off + 1] << 16) | (pkt->data[off + 2] << 8) | pkt->data[off + 3];
          nals += std::to_string(pkt->data[off + 4] & 0x1F) + " ";
          off += 4 + static_cast<int>(len);
        }
        Out(std::format("  Paket {} key={} pts={} NAL-Typen: {}", packets, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0, pkt->pts, nals));
      }
      ++packets;
      if (avcodec_send_packet(dec, pkt) < 0) ++errors;
      while (avcodec_receive_frame(dec, frm) == 0) {
        if (frm->decode_error_flags) ++errors;
        // Helligkeit (Y) und Streuung: grau/einfarbig → Streuung ≈ 0
        if (frames % 120 == 0 && frm->format == AV_PIX_FMT_YUV420P || (frames % 120 == 0 && frm->format == AV_PIX_FMT_NV12)) {
          double sum = 0, sq = 0;
          int n = 0;
          for (int y = 0; y < frm->height; y += 8)
            for (int x = 0; x < frm->width; x += 8) {
              double v = frm->data[0][y * frm->linesize[0] + x];
              sum += v;
              sq += v * v;
              ++n;
            }
          double mean = sum / n, sd = std::sqrt(std::max(0.0, sq / n - mean * mean));
          samples.push_back(std::format("t={:.1f}s Y-Mittel {:.0f} Streuung {:.1f}{}", frm->pts * av_q2d(st->time_base), mean, sd,
                                        sd < 2 ? " ← EINFARBIG" : ""));
        }
        if (frames == 0 || (snapSec < 0 ? frames == 300
                                         : std::abs(frm->pts * av_q2d(st->time_base) - snapSec) < 0.01)) {
          av_frame_unref(keep);
          av_frame_ref(keep, frm);
        }
        ++frames;
        av_frame_unref(frm);
      }
    }
    av_packet_unref(pkt);
  }
  Out(std::format("Pakete {}, dekodierte Frames {}, Decoderfehler {}, erster Keyframe bei Paket {}, "
                  "größte Lücke {:.1f} ms",
                  packets, frames, errors, firstKeyIdx, maxGap * av_q2d(st->time_base) * 1000));
  for (auto& l : samples) Out("  " + l);

  if (keep->data[0]) {
    // Bild als BMP speichern (YUV → BGR über libswscale)
    SwsContext* sws = sws_getContext(keep->width, keep->height, static_cast<AVPixelFormat>(keep->format),
                                     keep->width, keep->height, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
    const int w = keep->width, h = keep->height, rowBytes = (w * 3 + 3) & ~3;
    std::vector<uint8_t> bgr(static_cast<size_t>(rowBytes) * h);
    uint8_t* dst[1] = {bgr.data() + static_cast<size_t>(rowBytes) * (h - 1)};
    int dstStride[1] = {-rowBytes};  // BMP ist bottom-up
    sws_scale(sws, keep->data, keep->linesize, 0, h, dst, dstStride);
    sws_freeContext(sws);
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = static_cast<DWORD>(bgr.size());
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    auto out = paths::LocalDataDir() / L"decode-test.bmp";
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    f.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
    Out("Bild gespeichert: " + WideToUtf8(out.wstring()));
  }
  av_frame_free(&keep);
  av_frame_free(&frm);
  av_packet_free(&pkt);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  return 0;
}

int RunMfDecodeTest(const std::wstring& file, bool hardware) {
  constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) AllocConsole();
  SetConsoleOutputCP(CP_UTF8);
  MFStartup(MF_VERSION);
  winrt::com_ptr<IMFAttributes> attr;
  MFCreateAttributes(attr.put(), 4);
  winrt::com_ptr<ID3D11Device> dev;
  winrt::com_ptr<IMFDXGIDeviceManager> mgr;
  if (hardware) {
    // Wie die Windows-Medienwiedergabe: DXVA-Hardware-Decoder auf der GPU
    UINT token = 0;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                      D3D11_SDK_VERSION, dev.put(), nullptr, nullptr);
    dev.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    MFCreateDXGIDeviceManager(&token, mgr.put());
    mgr->ResetDevice(dev.get(), token);
    attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr.get());
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    Out("MF mit Hardware-Decoder (DXVA)");
  } else {
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // → RGB32
    Out("MF mit Software-Decoder");
  }
  winrt::com_ptr<IMFSourceReader> reader;
  HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), attr.get(), reader.put());
  if (FAILED(hr)) {
    Out("MF: Datei nicht lesbar " + HResultMessage(hr));
    return 1;
  }
  winrt::com_ptr<IMFMediaType> type;
  MFCreateMediaType(type.put());
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  hr = reader->SetCurrentMediaType(kVideoStream, nullptr, type.get());
  if (FAILED(hr)) {
    Out("MF: kein RGB-Ausgabeformat " + HResultMessage(hr));
    return 1;
  }
  winrt::com_ptr<IMFMediaType> cur;
  reader->GetCurrentMediaType(kVideoStream, cur.put());
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(cur.get(), MF_MT_FRAME_SIZE, &w, &h);
  int frames = 0;
  for (int i = 0; i < 2000; ++i) {
    DWORD flags = 0;
    LONGLONG ts = 0;
    winrt::com_ptr<IMFSample> sample;
    hr = reader->ReadSample(kVideoStream, 0, nullptr, &flags, &ts, sample.put());
    if (FAILED(hr)) {
      Out("MF: ReadSample-Fehler " + HResultMessage(hr));
      break;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    if (!sample) continue;
    if (frames % 120 == 0) {
      winrt::com_ptr<IMFMediaBuffer> buf;
      sample->ConvertToContiguousBuffer(buf.put());
      BYTE* data = nullptr;
      DWORD len = 0;
      if (FAILED(buf->Lock(&data, nullptr, &len))) {
        Out("MF: Puffer nicht lesbar");
        continue;
      }
      double sum = 0, sq = 0;
      int n = 0;
      for (DWORD y = 0; y < h; y += 8)
        for (DWORD x = 0; x < w; x += 8) {
          size_t o = (static_cast<size_t>(y) * w + x) * 4;
          if (o + 2 >= len) continue;
          double v = 0.299 * data[o + 2] + 0.587 * data[o + 1] + 0.114 * data[o];
          sum += v;
          sq += v * v;
          ++n;
        }
      buf->Unlock();
      double mean = n ? sum / n : 0, sd = n ? std::sqrt(std::max(0.0, sq / n - mean * mean)) : 0;
      Out(std::format("  MF t={:.1f}s Helligkeit {:.0f} Streuung {:.1f}{}", ts / 1e7, mean, sd,
                      sd < 2 ? " ← EINFARBIG/GRAU" : ""));
    }
    ++frames;
  }
  Out(std::format("MF: {} Frames gelesen ({}x{})", frames, w, h));
  reader = nullptr;
  MFShutdown();
  return 0;
}

} // namespace litecap
