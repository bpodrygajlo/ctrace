#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

void do_action_a(int val);
void do_action_b(int val);

int main(void) {
    ctrace_init("test_multi.log");

    do_action_a(100);
    do_action_b(200);

    ctrace_flush();
    ctrace_shutdown();

    FILE *f = fopen("test_multi.log", "r");
    assert(f != NULL);
    char buf[1024];
    int shared_count = 0;
    int a_count = 0;
    int b_count = 0;

    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "shared_event")) shared_count++;
        if (strstr(buf, "event_only_in_a")) a_count++;
        if (strstr(buf, "event_only_in_b")) b_count++;
    }
    fclose(f);

    assert(shared_count == 2);
    assert(a_count == 1);
    assert(b_count == 1);

    printf("test_multi passed successfully!\n");
    return 0;
}
