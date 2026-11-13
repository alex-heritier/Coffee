// Platform-independent core: provider request building, response parsing,
// credential discovery, pace math, and formatting. No Objective-C here.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ub {

enum class PID : int { Codex = 0, Claude, DeepSeek, Grok, Muse, OpenRouter, Count };
constexpr int kProviderCount = (int)PID::Count;

struct ProviderInfo {
  const char* id;        // stable id, used for persistence + keychain account
  const char* name;      // display name
  const char* symbol;    // SF Symbol name
  uint32_t rgb;          // brand color 0xRRGGBB
  bool needsApiKey;      // true when credentials come from a user-entered API key
  const char* keyHelp;   // where to get the key
};
const ProviderInfo& info(PID p);

struct Window {
  std::string label;        // "Session", "Weekly", ...
  double usedPct = -1;      // 0..100, <0 unknown
  int64_t resetAt = 0;      // unix seconds, 0 unknown
  int64_t durationSec = 0;  // full window length, 0 unknown
  bool inBar = true;        // participates in menu bar % (most constrained wins)
};

struct Snapshot {
  bool ok = false;
  bool hasData = false;      // at least one successful fetch happened
  std::string error;         // shown when !ok
  std::string email, plan;
  std::vector<Window> windows;
  std::vector<std::string> notes;  // extra detail lines
  std::string barText;             // menu bar text override when no windows (e.g. "$12.30")
  int64_t updatedAt = 0;
};

struct Request {
  std::string method = "GET";
  std::string url;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct Response {
  int status = 0;            // 0 = transport failure
  std::string body;
  std::string netError;
};

// A fetch plan: requests to perform plus context gathered while building them.
struct Plan {
  PID pid{};
  std::vector<Request> reqs;
  std::string error;     // non-empty: no requests, show this
  bool authProblem = false;
  Snapshot seed;         // identity fields known locally (email/plan)
};

// Build requests. `interactive` allows Keychain prompts for third-party items.
Plan buildPlan(PID p, bool interactive);
// Parse responses (same order as plan.reqs).
Snapshot parse(const Plan& plan, const std::vector<Response>& rs, int64_t now);

// API key storage (our own Keychain items; never prompts).
std::string apiKey(PID p);
bool setApiKey(PID p, const std::string& key);  // empty key deletes
void invalidateCredentialCache(PID p);

// Learned "weekly % consumed per full session" estimator, persisted on disk.
void learnSample(PID p, const Snapshot& s);
double weeklyPerSession(PID p);  // <0 unknown

// Formatting / derived text.
std::string fmtDuration(int64_t secs);           // "13h 28m", "2d 4h", "5m"
std::string fmtUpdated(int64_t age);             // "Updated just now"
std::string fmtMoney(double v);                  // "$12.30"
std::string barText(const Snapshot& s);          // "21%" / "$12.30" / "—"
double remainingPct(const Snapshot& s);          // most constrained remaining, <0 none
std::string paceLine(PID p, const Snapshot& s, const Window& w, int64_t now, double* expectedUsedOut,
                     bool* reserveOut);

int64_t parseISO8601(const std::string& s);      // 0 on failure
int64_t nowSec();

}  // namespace ub
