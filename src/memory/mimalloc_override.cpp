// This file is used to override the global new and delete operators with mimalloc.
// It should be included in the build only when USE_MIMALLOC is enabled.
#if defined(USE_MIMALLOC)
// ReSharper disable once CppUnusedIncludeDirective
#include <mimalloc-new-delete.h>
#endif
