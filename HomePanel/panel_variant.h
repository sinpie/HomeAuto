#pragma once
// 0 = manufacturer Old Panel, 1 = manufacturer New Panel.
// build.ps1 writes panel_select.h (-Panel New / Old). Without it (e.g. Arduino IDE),
// the default is New, the variant confirmed on this project's board.
#if __has_include("panel_select.h")
#include "panel_select.h"
#endif
#ifndef HOME_PANEL_NEW
#define HOME_PANEL_NEW 1
#endif
