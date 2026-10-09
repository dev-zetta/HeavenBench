#ifndef HEAVENBENCH_SRC_ENGINE_LOADING_PROGRESS_H_
#define HEAVENBENCH_SRC_ENGINE_LOADING_PROGRESS_H_

#include <functional>
#include <string_view>

namespace heaven {
// A negative fraction denotes an indeterminate stage. CPU preparation reports
// through a mailbox; GPU upload callbacks run only on the rendering owner.
// Throwing from a callback cancels at a safe work boundary and unwinds owners.
using LoadingProgressCallback = std::function<void(std::string_view, float)>;
}
#endif
