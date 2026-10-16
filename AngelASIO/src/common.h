#pragma once
#include <windows.h>
#include <cstdint>
#include "asio_defs.h"

// CLSID unico de este driver (tambien se usa como IID al crear la instancia, como exige ASIO).
static const GUID CLSID_AngelAsio =
    { 0x6e1f4a52, 0x8c3b, 0x4d27, { 0x9a, 0x5e, 0x1b, 0x7d, 0x03, 0xc4, 0xf8, 0x62 } };
#define ANGEL_CLSID_STR L"{6E1F4A52-8C3B-4D27-9A5E-1B7D03C4F862}"
#define ANGEL_DRIVER_NAME "AMR Angel ASIO"
#define ANGEL_DRIVER_NAME_W L"AMR Angel ASIO"

extern HINSTANCE g_hInst;
extern LONG g_objCount;
