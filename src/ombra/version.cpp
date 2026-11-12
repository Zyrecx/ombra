#include "ombra/version.h"

#include "ombra/build_info.h"    // generated at build time: OMBRA_BUILD_COMMIT
#include "ombra/version_defs.h"  // generated at configure time: OMBRA_VERSION

namespace ombra {

const char* AppVersion() { return OMBRA_VERSION; }
const char* BuildCommit() { return OMBRA_BUILD_COMMIT; }
const char* SentryDsn() { return OMBRA_SENTRY_DSN; }

}  // namespace ombra
