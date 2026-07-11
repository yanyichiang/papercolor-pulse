#include <cassert>
#include <cstdint>

#include "../main/apps/scheduled_push/scheduled_push_policy.h"

int main()
{
    using papercolor::ScheduledJobWindow;

    ScheduledJobWindow jobs[] = {
        {.display_at = 900, .expires_at = 0, .displayed = false},
        {.display_at = 1020, .expires_at = 1300, .displayed = false},
        {.display_at = 1010, .expires_at = 1025, .displayed = false},
    };

    // Future jobs are never due, even when the RTC woke early for display preparation.
    assert(papercolor::select_due_job(jobs, 3, 1000) == 0);
    assert(papercolor::select_imminent_job(jobs, 3, 1000, 20) == 2);

    // Once the newest due job is displayed, older due work is suppressed locally.
    assert(papercolor::select_due_job(jobs, 3, 1020) == 1);
    assert(papercolor::suppress_older_due_jobs(jobs, 3, 1, 1020) == 2);
    assert(jobs[0].displayed);
    assert(jobs[2].displayed);
    assert(!jobs[1].displayed);

    jobs[0] = {.display_at = 1000, .expires_at = 1000, .displayed = false};
    assert(papercolor::select_due_job(jobs, 1, 1000) == -1);
    jobs[0] = {.display_at = 1001, .expires_at = 0, .displayed = false};
    assert(papercolor::select_due_job(jobs, 1, 1000) == -1);

    // The local poll deadline is a ceiling for a later server-provided next-sync time.
    assert(papercolor::select_next_sync_deadline(1000, 1200, 15) == 1200);
    assert(papercolor::select_next_sync_deadline(1000, 3000, 15) == 1900);
    assert(papercolor::select_next_sync_deadline(1000, 900, 15) == 1900);

    ScheduledJobWindow future[] = {
        {.display_at = 2000, .expires_at = 0, .displayed = false},
    };
    assert(papercolor::select_next_wake(future, 1, 1000, 1900, 20, 300) == 1900);
    assert(papercolor::select_next_wake(future, 1, 1000, 2500, 20, 300) == 1980);
    assert(papercolor::select_next_wake(future, 1, 1000, 2500, 0, 300) == 2000);

    ScheduledJobWindow overdue[] = {
        {.display_at = 900, .expires_at = 0, .displayed = false},
    };
    assert(papercolor::select_next_wake(overdue, 1, 1000, 1900, 20, 300) == 1300);
    assert(papercolor::select_next_wake(nullptr, 0, 1000, 1900, 20, 300) == 1900);
    return 0;
}
