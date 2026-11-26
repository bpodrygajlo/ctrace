#include "types.h"
#include "ctrace/trace.h"

void do_action_b(int val) {
    const char *msg = "from_module_b";
    TRACE_EVENT(shared_event, val, msg);
    TRACE_EVENT(event_only_in_b, val);
}
