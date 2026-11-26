#include "types.h"
#include "ctrace/trace.h"

void do_action_a(int val) {
    const char *msg = "from_module_a";
    TRACE_EVENT(shared_event, val, msg);
    TRACE_EVENT(event_only_in_a, val);
}
