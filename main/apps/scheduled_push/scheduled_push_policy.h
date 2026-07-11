#pragma once

#include <cstddef>
#include <cstdint>

namespace papercolor {

struct ScheduledJobWindow {
    int64_t display_at = 0;
    int64_t expires_at = 0;
    bool displayed     = false;
};

inline int select_due_job(const ScheduledJobWindow* jobs, size_t count, int64_t now)
{
    int selected = -1;
    for (size_t index = 0; index < count; ++index) {
        const ScheduledJobWindow& job = jobs[index];
        if (job.displayed || job.display_at <= 0 || job.display_at > now) continue;
        if (job.expires_at > 0 && job.expires_at <= now) continue;
        if (selected < 0 || job.display_at >= jobs[selected].display_at) {
            selected = static_cast<int>(index);
        }
    }
    return selected;
}

inline int select_imminent_job(const ScheduledJobWindow* jobs, size_t count, int64_t now, int64_t maximum_wait_seconds)
{
    int selected = -1;
    for (size_t index = 0; index < count; ++index) {
        const ScheduledJobWindow& job = jobs[index];
        if (job.displayed || job.display_at <= now || job.display_at > now + maximum_wait_seconds) continue;
        if (job.expires_at > 0 && job.expires_at <= job.display_at) continue;
        if (selected < 0 || job.display_at < jobs[selected].display_at) {
            selected = static_cast<int>(index);
        }
    }
    return selected;
}

inline size_t suppress_older_due_jobs(ScheduledJobWindow* jobs, size_t count, size_t selected, int64_t now)
{
    if (!jobs || selected >= count) return 0;
    size_t suppressed           = 0;
    const int64_t selected_time = jobs[selected].display_at;
    for (size_t index = 0; index < count; ++index) {
        ScheduledJobWindow& job = jobs[index];
        if (index == selected || job.displayed || job.display_at <= 0 || job.display_at > now ||
            job.display_at > selected_time) {
            continue;
        }
        if (job.expires_at > 0 && job.expires_at <= now) continue;
        job.displayed = true;
        ++suppressed;
    }
    return suppressed;
}

inline int64_t select_next_sync_deadline(int64_t now, int64_t server_next_sync_at, uint16_t poll_minutes)
{
    int64_t local_deadline = now + static_cast<int64_t>(poll_minutes > 0 ? poll_minutes : 1) * 60;
    if (server_next_sync_at > now && server_next_sync_at < local_deadline) return server_next_sync_at;
    return local_deadline;
}

inline int64_t select_next_wake(const ScheduledJobWindow* jobs, size_t count, int64_t now, int64_t next_sync_at,
                                int64_t display_wake_lead_seconds = 20, int64_t overdue_retry_seconds = 300)
{
    int64_t next_wake = next_sync_at > now ? next_sync_at : 0;
    for (size_t index = 0; index < count; ++index) {
        const ScheduledJobWindow& job = jobs[index];
        if (job.displayed || job.display_at <= 0) continue;
        if (job.expires_at > 0 && job.expires_at <= now) continue;
        int64_t candidate =
            job.display_at <= now ? now + overdue_retry_seconds : job.display_at - display_wake_lead_seconds;
        if (candidate <= now) candidate = now + 1;
        if (next_wake == 0 || candidate < next_wake) next_wake = candidate;
    }
    return next_wake;
}

}  // namespace papercolor
