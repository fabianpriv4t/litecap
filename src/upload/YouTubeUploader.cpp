#include "upload/YouTubeUploader.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

#include "output/FileNamer.h"
#include "upload/GoogleAuth.h"
#include "upload/Http.h"
#include "util/Log.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace litecap {

using nlohmann::json;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(UploadJob, id, file, title, description, privacy,
                                                tags, categoryId, deleteAfter, sessionUri, state,
                                                bytesSent, total, videoId, error, attempts, nextTry)

namespace {

constexpr uint64_t kChunk = 8ull * 1024 * 1024;  // Vielfaches von 256 KiB (Vorgabe von Google)
constexpr int kMaxAttempts = 12;

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

std::filesystem::path QueueFile() { return paths::LocalDataDir() / L"uploads.json"; }

// "bytes=0-12345" → 12346 (nächstes Byte); kein Range-Header → 0
uint64_t NextOffsetFromRange(const std::wstring& range) {
  size_t dash = range.find(L'-');
  if (dash == std::wstring::npos) return 0;
  return std::stoull(range.substr(dash + 1)) + 1;
}

// YouTube: Titel max. 100 Zeichen, keine spitzen Klammern
std::string CleanTitle(std::string t) {
  std::erase_if(t, [](char c) { return c == '<' || c == '>'; });
  std::wstring w = Utf8ToWide(t);
  if (w.size() > 100) w.resize(100);
  return w.empty() ? "LiteCap" : WideToUtf8(w);
}

// In den Papierkorb statt endgültig löschen (versehentliche Löschung bleibt umkehrbar)
bool MoveToRecycleBin(const std::filesystem::path& file) {
  std::wstring from = file.wstring();
  from.push_back(L'\0');  // doppelt nullterminiert
  SHFILEOPSTRUCTW op = {};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  return SHFileOperationW(&op) == 0;
}

struct RetryLater : std::runtime_error {
  RetryLater(const std::string& m, int64_t delaySec) : std::runtime_error(m), delay(delaySec) {}
  int64_t delay;
};
struct Permanent : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string GoogleError(const std::string& body) {
  json j = json::parse(body, nullptr, false);
  if (j.is_object() && j.contains("error") && j["error"].is_object()) {
    std::string reason;
    if (j["error"].contains("errors") && !j["error"]["errors"].empty())
      reason = j["error"]["errors"][0].value("reason", "");
    return reason.empty() ? j["error"].value("message", "") : reason;
  }
  return body.substr(0, 200);
}

} // namespace

void YouTubeUploader::Start(NoticeFn onNotice) {
  Stop();
  onNotice_ = std::move(onNotice);
  Load();
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread([this] { Run(); });
}

void YouTubeUploader::Stop() {
  if (stop_) SetEvent(stop_);
  if (thread_.joinable()) thread_.join();
  if (wake_) CloseHandle(wake_), wake_ = nullptr;
  if (stop_) CloseHandle(stop_), stop_ = nullptr;
}

void YouTubeUploader::UpdateConfig(const YouTubeConfig& cfg) {
  std::lock_guard lock(mutex_);
  cfg_ = cfg;
}

void YouTubeUploader::Load() {
  std::lock_guard lock(mutex_);
  jobs_.clear();
  std::ifstream in(QueueFile(), std::ios::binary);
  if (!in) return;
  try {
    jobs_ = json::parse(in).get<std::vector<UploadJob>>();
    for (auto& j : jobs_)
      if (j.state == "uploading") j.state = "pending";  // wurde unterbrochen → fortsetzen
    // Erledigtes nur 7 Tage behalten
    std::erase_if(jobs_, [](const UploadJob& j) {
      return (j.state == "done" || j.state == "failed") && Now() - j.nextTry > 7 * 86400;
    });
  } catch (const std::exception& e) {
    log::Warn("Upload-Warteschlange unlesbar, wird neu angelegt: {}", e.what());
  }
}

void YouTubeUploader::SaveLocked() const {
  auto file = QueueFile();
  auto tmp = file;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json(jobs_).dump(2);
  }
  MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void YouTubeUploader::Save() const {
  std::lock_guard lock(mutex_);
  SaveLocked();
}

void YouTubeUploader::Enqueue(const std::filesystem::path& file, const std::wstring& source,
                              const std::wstring& type, bool deleteAfter) {
  {
    std::lock_guard lock(mutex_);
    UploadJob j;
    j.id = std::to_string(Now()) + "-" + std::to_string(jobs_.size());
    j.file = WideToUtf8(file.wstring());
    j.title = CleanTitle(ExpandTemplate(cfg_.titleTemplate, source, type));
    j.description = ExpandTemplate(cfg_.description, source, type);
    j.privacy = cfg_.privacy;
    j.tags = cfg_.tags;
    j.categoryId = cfg_.categoryId;
    j.deleteAfter = deleteAfter;
    jobs_.push_back(std::move(j));
    SaveLocked();
  }
  log::Info("Upload eingereiht: {}", WideToUtf8(file.filename().wstring()));
  if (wake_) SetEvent(wake_);
}

std::wstring YouTubeUploader::StatusText() const {
  std::lock_guard lock(mutex_);
  int pending = 0;
  for (const auto& j : jobs_)
    if (j.state == "pending" || j.state == "uploading") ++pending;
  if (!pending) return {};
  int pct = activePercent_;
  if (pct < 0) return std::format(L"{} Upload(s) wartend", pending);
  return pending > 1 ? std::format(L"Upload {} % ({} weitere)", pct, pending - 1)
                     : std::format(L"Upload {} %", pct);
}

void YouTubeUploader::Run() {
  // Hintergrundmodus: niedrige CPU- und I/O-Priorität für diesen Thread
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  HANDLE waits[2] = {stop_, wake_};
  for (;;) {
    // Nächsten fälligen Auftrag suchen
    int next = -1;
    int64_t soonest = INT64_MAX;
    {
      std::lock_guard lock(mutex_);
      for (size_t i = 0; i < jobs_.size(); ++i) {
        const auto& j = jobs_[i];
        if (j.state != "pending") continue;
        if (j.nextTry <= Now()) {
          next = static_cast<int>(i);
          break;
        }
        soonest = std::min(soonest, j.nextTry);
      }
    }
    if (next < 0) {
      DWORD timeout = soonest == INT64_MAX ? INFINITE
                                           : static_cast<DWORD>(std::clamp<int64_t>(soonest - Now(), 1, 3600) * 1000);
      if (WaitForMultipleObjects(2, waits, FALSE, timeout) == WAIT_OBJECT_0) break;
      continue;
    }

    UploadJob job;
    {
      std::lock_guard lock(mutex_);
      jobs_[static_cast<size_t>(next)].state = "uploading";
      job = jobs_[static_cast<size_t>(next)];
      SaveLocked();
    }
    Process(job);
    activePercent_ = -1;
    {
      std::lock_guard lock(mutex_);
      for (auto& j : jobs_)
        if (j.id == job.id) j = job;
      SaveLocked();
    }
    if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
  }
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

void YouTubeUploader::Process(UploadJob& job) {
  YouTubeConfig cfg;
  {
    std::lock_guard lock(mutex_);
    cfg = cfg_;
  }
  const std::filesystem::path file = Utf8ToWide(job.file);
  const std::wstring name = file.filename().wstring();

  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    job.state = "failed";
    job.error = "Datei nicht gefunden";
    job.nextTry = Now();
    onNotice_({L"Upload fehlgeschlagen", name + L": Datei nicht gefunden", true});
    return;
  }
  LARGE_INTEGER size;
  GetFileSizeEx(h, &size);
  job.total = static_cast<uint64_t>(size.QuadPart);

  try {
    Http http;
    GoogleAuth auth(cfg.clientId);
    auto bearer = [&] { return L"Bearer " + Utf8ToWide(auth.AccessToken(http)); };

    // Antwort auswerten, die keinen Erfolg bedeutet
    auto handleError = [&](const HttpResponse& r, const char* what) {
      std::string reason = GoogleError(r.body);
      if (r.status == 401) {
        auth.Invalidate();
        throw RetryLater(std::string(what) + ": Anmeldung ungültig", 5);
      }
      if (r.status == 403 && (reason == "quotaExceeded" || reason == "uploadLimitExceeded" ||
                              reason == "rateLimitExceeded"))
        throw RetryLater("YouTube-Tageskontingent erschöpft – neuer Versuch in 1 Stunde", 3600);
      if (r.status >= 500 || r.status == 429) throw RetryLater(std::string(what) + ": Serverfehler", 30);
      throw Permanent(std::string(what) + " abgelehnt (HTTP " + std::to_string(r.status) + "): " + reason);
    };

    // 1) Upload-Sitzung anlegen (einmalig, URI wird gespeichert)
    if (job.sessionUri.empty()) {
      json meta = {{"snippet",
                    {{"title", job.title},
                     {"description", job.description},
                     {"tags", job.tags},
                     {"categoryId", std::to_string(job.categoryId)}}},
                   {"status", {{"privacyStatus", job.privacy}, {"selfDeclaredMadeForKids", false}}}};
      std::string body = meta.dump();
      HttpResponse r = http.Send(
          L"POST",
          L"https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet,status",
          {{L"Authorization", bearer()},
           {L"Content-Type", L"application/json; charset=UTF-8"},
           {L"X-Upload-Content-Type", L"video/*"},
           {L"X-Upload-Content-Length", std::to_wstring(job.total)}},
          body);
      if (r.status != 200) handleError(r, "Upload-Sitzung");
      job.sessionUri = WideToUtf8(r.Header(L"location"));
      if (job.sessionUri.empty()) throw RetryLater("Keine Upload-Sitzung erhalten", 60);
      job.bytesSent = 0;
      Save();
    }
    const std::wstring session = Utf8ToWide(job.sessionUri);

    // 2) Stand beim Server erfragen (nach Abbruch/Neustart)
    {
      HttpResponse r = http.Send(L"PUT", session,
                                 {{L"Authorization", bearer()},
                                  {L"Content-Range", L"bytes */" + std::to_wstring(job.total)}});
      if (r.status == 200 || r.status == 201) {
        job.videoId = json::parse(r.body, nullptr, false).value("id", "");
        job.bytesSent = job.total;
      } else if (r.status == 308) {
        job.bytesSent = NextOffsetFromRange(r.Header(L"range"));
      } else if (r.status == 404 || r.status == 410) {
        job.sessionUri.clear();  // Sitzung abgelaufen → von vorn
        throw RetryLater("Upload-Sitzung abgelaufen, starte neu", 5);
      } else {
        handleError(r, "Upload-Status");
      }
    }
    if (job.bytesSent > 0 && job.bytesSent < job.total)
      log::Info("Upload {}: setze bei {:.1f} MB fort", WideToUtf8(name), job.bytesSent / 1048576.0);

    // 3) In Stücken hochladen
    auto lastSave = std::chrono::steady_clock::now();
    while (job.videoId.empty()) {
      if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) {
        job.state = "pending";  // beim nächsten Start fortsetzen
        CloseHandle(h);
        return;
      }
      const uint64_t start = job.bytesSent;
      const uint64_t len = std::min(kChunk, job.total - start);
      HttpResponse r = http.SendFile(
          L"PUT", session,
          {{L"Authorization", bearer()},
           {L"Content-Type", L"video/*"},
           {L"Content-Range", std::format(L"bytes {}-{}/{}", start, start + len - 1, job.total)}},
          h, start, len,
          [&](uint64_t sent) {
            activePercent_ = static_cast<int>((start + sent) * 100 / std::max<uint64_t>(job.total, 1));
            return WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0;
          },
          cfg.maxUploadKbps);
      if (r.status == 308) {
        job.bytesSent = NextOffsetFromRange(r.Header(L"range"));
      } else if (r.status == 200 || r.status == 201) {
        job.videoId = json::parse(r.body, nullptr, false).value("id", "");
        job.bytesSent = job.total;
        if (job.videoId.empty()) throw Permanent("Upload beendet, aber keine Video-ID erhalten");
      } else {
        handleError(r, "Upload");
      }
      job.attempts = 0;  // Fortschritt → Fehlerzähler zurücksetzen
      if (std::chrono::steady_clock::now() - lastSave > std::chrono::seconds(10)) {
        {
          std::lock_guard lock(mutex_);
          for (auto& j : jobs_)
            if (j.id == job.id) j = job;
          SaveLocked();
        }
        lastSave = std::chrono::steady_clock::now();
      }
    }
    CloseHandle(h);
    h = INVALID_HANDLE_VALUE;

    job.state = "done";
    job.error.clear();
    job.nextTry = Now();
    log::Info("Hochgeladen: {} → https://youtu.be/{}", WideToUtf8(name), job.videoId);
    std::wstring text = Utf8ToWide(job.title) + L"\nhttps://youtu.be/" + Utf8ToWide(job.videoId);
    if (job.deleteAfter) {
      if (MoveToRecycleBin(file)) text += L"\nLokale Datei in den Papierkorb verschoben.";
      else log::Warn("Lokale Datei konnte nicht gelöscht werden: {}", WideToUtf8(name));
    }
    onNotice_({L"YouTube-Upload fertig", text, false});
  } catch (const AuthError& e) {
    // Ohne Anmeldung kein Sinn, es ständig zu versuchen: in 1 Stunde erneut
    job.state = "pending";
    job.error = e.what();
    job.nextTry = Now() + 3600;
    log::Warn("Upload {}: {}", WideToUtf8(name), e.what());
    onNotice_({L"YouTube-Upload wartet", Utf8ToWide(e.what()), true});
  } catch (const Permanent& e) {
    job.state = "failed";
    job.error = e.what();
    job.nextTry = Now();
    log::Error("Upload {}: {}", WideToUtf8(name), e.what());
    onNotice_({L"Upload fehlgeschlagen", name + L"\n" + Utf8ToWide(e.what()), true});
  } catch (const std::exception& e) {
    // Netzwerkfehler, Serverfehler, Quota: später erneut (exponentiell, max. 30 min)
    const auto* later = dynamic_cast<const RetryLater*>(&e);
    ++job.attempts;
    job.error = e.what();
    if (job.attempts > kMaxAttempts) {
      job.state = "failed";
      job.nextTry = Now();
      onNotice_({L"Upload fehlgeschlagen", name + L"\n" + Utf8ToWide(e.what()), true});
    } else {
      job.state = "pending";
      int64_t backoff = std::min<int64_t>(1800, 5ll << std::min(job.attempts, 9));
      job.nextTry = Now() + (later ? std::max(later->delay, backoff / 4) : backoff);
      log::Warn("Upload {}: {} – neuer Versuch in {} s", WideToUtf8(name), e.what(),
                job.nextTry - Now());
      if (job.attempts == 1 || (later && later->delay >= 3600))
        onNotice_({L"Upload unterbrochen", Utf8ToWide(e.what()) + L"\nWird automatisch fortgesetzt.", true});
    }
  }
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

} // namespace litecap
