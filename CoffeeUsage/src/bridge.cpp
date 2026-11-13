// Plain-C wrappers over the C++ core for the Swift shell.
#include "bridge.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "core.hpp"

namespace {

const ub::Snapshot& snapOf(const UBSnapshot* s) { return *reinterpret_cast<const ub::Snapshot*>(s); }
const ub::Plan& planOf(const UBPlan* p) { return *reinterpret_cast<const ub::Plan*>(p); }

char* copyOf(const std::string& s) {
  char* out = (char*)malloc(s.size() + 1);
  if (out) memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

}  // namespace

extern "C" {

int ub_provider_count(void) { return ub::kProviderCount; }
const char* ub_info_id(int p) { return ub::info((ub::PID)p).id; }
const char* ub_info_name(int p) { return ub::info((ub::PID)p).name; }
const char* ub_info_symbol(int p) { return ub::info((ub::PID)p).symbol; }
unsigned ub_info_rgb(int p) { return ub::info((ub::PID)p).rgb; }
int ub_info_needs_key(int p) { return ub::info((ub::PID)p).needsApiKey ? 1 : 0; }
const char* ub_info_key_help(int p) { return ub::info((ub::PID)p).keyHelp ? ub::info((ub::PID)p).keyHelp : ""; }

UBPlan* ub_plan_build(int pid, int interactive) {
  return reinterpret_cast<UBPlan*>(new ub::Plan(ub::buildPlan((ub::PID)pid, interactive != 0)));
}
void ub_plan_free(UBPlan* plan) { delete reinterpret_cast<ub::Plan*>(plan); }
int ub_plan_req_count(const UBPlan* plan) { return (int)planOf(plan).reqs.size(); }
const char* ub_plan_req_method(const UBPlan* plan, int i) { return planOf(plan).reqs[(size_t)i].method.c_str(); }
const char* ub_plan_req_url(const UBPlan* plan, int i) { return planOf(plan).reqs[(size_t)i].url.c_str(); }
const char* ub_plan_req_body(const UBPlan* plan, int i) { return planOf(plan).reqs[(size_t)i].body.c_str(); }
int ub_plan_req_header_count(const UBPlan* plan, int i) {
  return (int)planOf(plan).reqs[(size_t)i].headers.size();
}
const char* ub_plan_req_header_key(const UBPlan* plan, int i, int h) {
  return planOf(plan).reqs[(size_t)i].headers[(size_t)h].first.c_str();
}
const char* ub_plan_req_header_value(const UBPlan* plan, int i, int h) {
  return planOf(plan).reqs[(size_t)i].headers[(size_t)h].second.c_str();
}
const char* ub_plan_error(const UBPlan* plan) { return planOf(plan).error.c_str(); }
int ub_plan_auth_problem(const UBPlan* plan) { return planOf(plan).authProblem ? 1 : 0; }
const char* ub_plan_seed_email(const UBPlan* plan) { return planOf(plan).seed.email.c_str(); }
const char* ub_plan_seed_plan(const UBPlan* plan) { return planOf(plan).seed.plan.c_str(); }

UBSnapshot* ub_parse_plan(const UBPlan* plan, const UBResponse* rs, int n, int64_t now) {
  std::vector<ub::Response> v;
  v.reserve((size_t)(n < 0 ? 0 : n));
  for (int i = 0; i < n; ++i) {
    ub::Response r;
    r.status = rs[i].status;
    if (rs[i].body && rs[i].body_len) r.body.assign(rs[i].body, rs[i].body_len);
    if (rs[i].err && rs[i].err_len) r.netError.assign(rs[i].err, rs[i].err_len);
    v.push_back(std::move(r));
  }
  return reinterpret_cast<UBSnapshot*>(new ub::Snapshot(ub::parse(planOf(plan), v, now)));
}
void ub_snapshot_free(UBSnapshot* s) { delete reinterpret_cast<ub::Snapshot*>(s); }
int ub_snapshot_ok(const UBSnapshot* s) { return snapOf(s).ok ? 1 : 0; }
int ub_snapshot_has_data(const UBSnapshot* s) { return snapOf(s).hasData ? 1 : 0; }
const char* ub_snapshot_error(const UBSnapshot* s) { return snapOf(s).error.c_str(); }
const char* ub_snapshot_email(const UBSnapshot* s) { return snapOf(s).email.c_str(); }
const char* ub_snapshot_plan(const UBSnapshot* s) { return snapOf(s).plan.c_str(); }
const char* ub_snapshot_bar_text(const UBSnapshot* s) { return snapOf(s).barText.c_str(); }
int64_t ub_snapshot_updated(const UBSnapshot* s) { return snapOf(s).updatedAt; }
int ub_snapshot_window_count(const UBSnapshot* s) { return (int)snapOf(s).windows.size(); }
const char* ub_snapshot_window_label(const UBSnapshot* s, int i) {
  return snapOf(s).windows[(size_t)i].label.c_str();
}
double ub_snapshot_window_used(const UBSnapshot* s, int i) { return snapOf(s).windows[(size_t)i].usedPct; }
int64_t ub_snapshot_window_reset(const UBSnapshot* s, int i) { return snapOf(s).windows[(size_t)i].resetAt; }
int64_t ub_snapshot_window_duration(const UBSnapshot* s, int i) {
  return snapOf(s).windows[(size_t)i].durationSec;
}
int ub_snapshot_window_in_bar(const UBSnapshot* s, int i) {
  return snapOf(s).windows[(size_t)i].inBar ? 1 : 0;
}
int ub_snapshot_note_count(const UBSnapshot* s) { return (int)snapOf(s).notes.size(); }
const char* ub_snapshot_note(const UBSnapshot* s, int i) { return snapOf(s).notes[(size_t)i].c_str(); }

int64_t ub_now_sec(void) { return ub::nowSec(); }
const char* ub_api_key(int pid) {
  static thread_local std::string buf;
  buf = ub::apiKey((ub::PID)pid);
  return buf.c_str();
}
int ub_set_api_key(int pid, const char* key) { return ub::setApiKey((ub::PID)pid, key ? key : "") ? 1 : 0; }
void ub_invalidate_credential_cache(int pid) { ub::invalidateCredentialCache((ub::PID)pid); }
void ub_learn_sample(int pid, const UBSnapshot* s) { ub::learnSample((ub::PID)pid, snapOf(s)); }
double ub_weekly_per_session(int pid) { return ub::weeklyPerSession((ub::PID)pid); }
char* ub_bar_text_copy(const UBSnapshot* s) { return copyOf(ub::barText(snapOf(s))); }
double ub_remaining_copy(const UBSnapshot* s) { return ub::remainingPct(snapOf(s)); }
char* ub_fmt_duration_copy(int64_t secs) { return copyOf(ub::fmtDuration(secs)); }
char* ub_fmt_updated_copy(int64_t age) { return copyOf(ub::fmtUpdated(age)); }
char* ub_fmt_money_copy(double v) { return copyOf(ub::fmtMoney(v)); }
char* ub_pace_line_copy(int pid, const UBSnapshot* s, int windex, int64_t now) {
  const auto& ws = snapOf(s).windows;
  if (windex < 0 || (size_t)windex >= ws.size()) return copyOf(std::string());
  return copyOf(ub::paceLine((ub::PID)pid, snapOf(s), ws[(size_t)windex], now, nullptr, nullptr));
}

}  // extern "C"
