#pragma once

namespace litecap {

// Hilfsbefehle für die Einstellungs-GUI (eigene kurzlebige Prozesse, kein Tray):
//   litecap.exe --youtube-login    Browser-Anmeldung, speichert Refresh-Token im Credential Manager
//   litecap.exe --youtube-logout   Token widerrufen und löschen
//   litecap.exe --list-devices     JSON auf stdout: Monitore, Fenster, Mikrofone, Encoder, YouTube-Status
int RunYouTubeLogin();
int RunYouTubeLogout();
int RunListDevices();

} // namespace litecap
