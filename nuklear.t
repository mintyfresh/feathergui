
local nuklear = terralib.includecstring([[
//#include "nuklear.h"
#include "nuklear_cross.h"
]],
  {--[["-DNKC_IMPLEMENATION",]] "-DNKCD=NKC_XLIB"})

--terralib.linklibrary "./libnkc.so"
--terralib.linklibrary "X11"

return nuklear
