/**
 * @file src/platform/android/publish.cpp
 * @brief Android mDNS service advertising for GameStream discovery.
 * @details Not yet implemented. Returning null skips advertising; Moonlight clients can still
 *          connect by entering the host IP manually. The real backend will register the
 *          `_nvstream._tcp` service via NsdManager (JNI) or an embedded mDNS responder.
 */
// local includes
#include "src/platform/common.h"

namespace platf::publish {

  std::unique_ptr<deinit_t> start() {
    // TODO: register SERVICE_NAME / SERVICE_TYPE via NsdManager. Null == discovery disabled.
    return nullptr;
  }

}  // namespace platf::publish
