#include "retcheck_bypass.h"

#include <dolos/pipe_log.h>

namespace d2r {

template <typename R, typename... Args>
inline void RetcheckFunction<R, Args...>::DoCall(R* result, Args... args) {
  FuncPtr cur_fn = dummy;
  do {
    bool swap_active = false;
    if (call_site != nullptr) {
      cur_fn = real_fn;

      if (real_call_site == nullptr) {
        real_call_site = ProbeCallInstruction(call_site);
        if (real_call_site == nullptr) {
          PIPE_LOG("Call failed: Could not find call site");
          return;
        }
      }
      if (!RetcheckBypass::AddAddress(reinterpret_cast<uintptr_t>(real_call_site))) {
        PIPE_LOG("Call failed: Could not add return address");
        return;
      }
      if (!RetcheckBypass::SwapIn()) {
        PIPE_LOG("Call failed: Could not swap in retcheck bypass");
        return;
      }
      swap_active = true;
    }

#if defined(_MSC_VER)
    __try {
#else
    try {
#endif
    if constexpr (!std::is_void_v<R>) {
      *result = cur_fn(args...);
    } else {
      cur_fn(args...);
    }
    call_site = GetCallSite();

#if defined(_MSC_VER)
    } __finally {
      if (swap_active) {
        if (!RetcheckBypass::SwapOut()) {
          PIPE_LOG_ERROR("RetcheckBypass: SwapOut failed in finally");
        }
      }
    }
#else
    } catch (...) {
      if (swap_active && !RetcheckBypass::SwapOut()) {
        PIPE_LOG_ERROR("RetcheckBypass: SwapOut failed in catch");
      }
      throw;
    }
    if (swap_active && !RetcheckBypass::SwapOut()) {
      PIPE_LOG_ERROR("RetcheckBypass: SwapOut failed");
    }
#endif
  } while (cur_fn != real_fn);
}

}  // namespace d2r
