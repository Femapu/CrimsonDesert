// QuattroStagioni - shared plugin state (see qs_state.h).
#include "qs_state.h"

namespace qs {

SharedState& shared() {
    static SharedState s;
    return s;
}

}  // namespace qs
