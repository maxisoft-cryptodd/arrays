#include "buffer.h"

#include <hwy/aligned_allocator.h>
// All Buffer methods are defined in the header to allow for inlining.

static_assert(cryptodd::details::_DEFAULT_HWY_ALIGNMENT >= HWY_ALIGNMENT, "_DEFAULT_HWY_ALIGNMENT must remain aligned with HWY_ALIGNMENT (from highway google lib)");
static_assert(cryptodd::details::_DEFAULT_HWY_ALIGNMENT % HWY_ALIGNMENT == 0);