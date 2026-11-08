#include "upload/CredentialStore.h"

#include <windows.h>
#include <wincred.h>

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap::credentials {

bool Write(const wchar_t* target, const std::string& secret) {
  CREDENTIALW c = {};
  c.Type = CRED_TYPE_GENERIC;
  c.TargetName = const_cast<wchar_t*>(target);
  c.CredentialBlobSize = static_cast<DWORD>(secret.size());
  c.CredentialBlob = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));
  c.Persist = CRED_PERSIST_LOCAL_MACHINE;  // nur dieser PC, nicht ins Roaming-Profil
  c.UserName = const_cast<wchar_t*>(L"LiteCap");
  if (!CredWriteW(&c, 0)) {
    log::Error("Credential Manager: Schreiben fehlgeschlagen ({})", LastErrorMessage());
    return false;
  }
  return true;
}

std::optional<std::string> Read(const wchar_t* target) {
  PCREDENTIALW c = nullptr;
  if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &c)) return std::nullopt;
  std::string s(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
  SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
  CredFree(c);
  return s;
}

void Remove(const wchar_t* target) { CredDeleteW(target, CRED_TYPE_GENERIC, 0); }

} // namespace litecap::credentials
