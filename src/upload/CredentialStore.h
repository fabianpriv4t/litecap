#pragma once

#include <optional>
#include <string>

namespace litecap::credentials {

// Geheimnisse im Windows-Anmeldeinformationsverwaltung (Credential Manager), verschlüsselt
// mit dem Benutzerkonto (DPAPI). Nie im Klartext auf der Festplatte.
// Sichtbar unter: Systemsteuerung → Anmeldeinformationsverwaltung → Windows-Anmeldeinformationen.
inline constexpr wchar_t kYouTubeRefreshToken[] = L"LiteCap/YouTube/RefreshToken";
inline constexpr wchar_t kYouTubeClientSecret[] = L"LiteCap/YouTube/ClientSecret";

bool Write(const wchar_t* target, const std::string& secret);
std::optional<std::string> Read(const wchar_t* target);
void Remove(const wchar_t* target);

} // namespace litecap::credentials
