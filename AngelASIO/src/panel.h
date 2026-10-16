#pragma once
#include <windows.h>
#include <string>
#include "config.h"

// Panel de control modal. Devuelve true si el usuario pulso "Aplicar" (la config ya queda guardada).
bool showPanel(HWND owner, const std::string& status, Config& cfg);
