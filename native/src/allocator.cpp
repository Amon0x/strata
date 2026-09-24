// strata_c's own C++ allocations (operator new and delete for everything linked into it) come
// from mimalloc. Only this module uses them: the C ABI never hands over memory to be freed by the
// host, and malloc/free stay the C runtime's.
#if defined(_MSC_VER)
// The replacements declare their results unaliased (__declspec(restrict)), which they are.
#pragma warning(push)
#pragma warning(disable : 4559)
#endif
#include <mimalloc-new-delete.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
