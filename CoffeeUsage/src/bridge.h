// Plain-C bridge over the C++ core (core.hpp) for the Swift shell.
// String getters return pointers valid until the owning object is freed,
// except ub_*_copy functions which return malloc'd strings the caller frees.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Providers (indices match ub::PID order: Codex, Claude, DeepSeek, Grok, Muse, OpenRouter).
int ub_provider_count(void);
const char* ub_info_id(int p);
const char* ub_info_name(int p);
const char* ub_info_symbol(int p);
unsigned ub_info_rgb(int p);
int ub_info_needs_key(int p);
const char* ub_info_key_help(int p);

// Fetch plan. Opaque to Swift (only ever passed back as a pointer);
// freed with ub_plan_free. The dummy body makes the type importable.
typedef struct UBPlan { int _opaque; } UBPlan;
UBPlan* ub_plan_build(int pid, int interactive);
void ub_plan_free(UBPlan* plan);
int ub_plan_req_count(const UBPlan* plan);
const char* ub_plan_req_method(const UBPlan* plan, int i);
const char* ub_plan_req_url(const UBPlan* plan, int i);
const char* ub_plan_req_body(const UBPlan* plan, int i);
int ub_plan_req_header_count(const UBPlan* plan, int i);
const char* ub_plan_req_header_key(const UBPlan* plan, int i, int h);
const char* ub_plan_req_header_value(const UBPlan* plan, int i, int h);
const char* ub_plan_error(const UBPlan* plan);
int ub_plan_auth_problem(const UBPlan* plan);
const char* ub_plan_seed_email(const UBPlan* plan);
const char* ub_plan_seed_plan(const UBPlan* plan);

// One HTTP response. Body/err point to caller-owned bytes valid during ub_parse_plan.
typedef struct {
  int status;  // 0 = transport failure
  const char* body;
  size_t body_len;
  const char* err;
  size_t err_len;
} UBResponse;

// Parsed snapshot. Opaque to Swift (only ever passed back as a pointer);
// freed with ub_snapshot_free. The dummy body makes the type importable.
typedef struct UBSnapshot { int _opaque; } UBSnapshot;
UBSnapshot* ub_parse_plan(const UBPlan* plan, const UBResponse* rs, int n, int64_t now);
void ub_snapshot_free(UBSnapshot* s);
int ub_snapshot_ok(const UBSnapshot* s);
int ub_snapshot_has_data(const UBSnapshot* s);
const char* ub_snapshot_error(const UBSnapshot* s);
const char* ub_snapshot_email(const UBSnapshot* s);
const char* ub_snapshot_plan(const UBSnapshot* s);
const char* ub_snapshot_bar_text(const UBSnapshot* s);
int64_t ub_snapshot_updated(const UBSnapshot* s);
int ub_snapshot_window_count(const UBSnapshot* s);
const char* ub_snapshot_window_label(const UBSnapshot* s, int i);
double ub_snapshot_window_used(const UBSnapshot* s, int i);
int64_t ub_snapshot_window_reset(const UBSnapshot* s, int i);
int64_t ub_snapshot_window_duration(const UBSnapshot* s, int i);
int ub_snapshot_window_in_bar(const UBSnapshot* s, int i);
int ub_snapshot_note_count(const UBSnapshot* s);
const char* ub_snapshot_note(const UBSnapshot* s, int i);

// Credentials, learning, formatting. ub_*_copy return malloc'd strings.
int64_t ub_now_sec(void);
const char* ub_api_key(int pid);
int ub_set_api_key(int pid, const char* key);
void ub_invalidate_credential_cache(int pid);
void ub_learn_sample(int pid, const UBSnapshot* s);
double ub_weekly_per_session(int pid);
char* ub_bar_text_copy(const UBSnapshot* s);
double ub_remaining_copy(const UBSnapshot* s);
char* ub_fmt_duration_copy(int64_t secs);
char* ub_fmt_updated_copy(int64_t age);
char* ub_fmt_money_copy(double v);
char* ub_pace_line_copy(int pid, const UBSnapshot* s, int windex, int64_t now);

#ifdef __cplusplus
}
#endif
