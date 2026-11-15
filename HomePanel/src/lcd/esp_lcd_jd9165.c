// Vendor implementations selected explicitly by build.ps1.
#include "../../panel_variant.h"
#if HOME_PANEL_NEW
#include "jd9165_new_impl.h"
#else
#include "jd9165_old_impl.h"
#endif
