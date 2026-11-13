#include "core.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include "json.hpp"

namespace ub {

// ---------------------------------------------------------------- registry

static const ProviderInfo kInfo[kProviderCount] = {
    {"codex", "Codex", "hexagon", 0x49A3B0, false, nullptr},
    {"claude", "Claude", "asterisk", 0xCC7D5E, false, nullptr},
    {"deepseek", "DeepSeek", "fish", 0x4D6BFE, true, "platform.deepseek.com/api_keys"},
    {"grok", "Grok", "slash.circle", 0x2E9E7E, false, nullptr},
    {"muse", "Muse Code", "infinity", 0x0866FF, false, nullptr},
    {"openrouter", "OpenRouter", "arrow.triangle.branch", 0x6467F2, true, "openrouter.ai/settings/keys"},
};
const ProviderInfo& info(PID p) { return kInfo[(int)p]; }

int64_t nowSec() { return (int64_t)time(nullptr); }

// ---------------------------------------------------------------- utilities

static std::string home() {
  const char* h = getenv("HOME");
  return h ? h : "";
}

static bool readFile(const std::string& path, std::string& out, size_t maxBytes = 8 << 20) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  struct stat st {};
  if (fstat(fileno(f), &st) != 0 || (size_t)st.st_size > maxBytes) { fclose(f); return false; }
  out.resize((size_t)st.st_size);
  size_t n = st.st_size ? fread(out.data(), 1, out.size(), f) : 0;
  fclose(f);
  out.resize(n);
  return true;
}

static int64_t mtimeOf(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec : -1;
}

static std::string b64urlDecode(std::string_view in) {
  static int8_t tbl[256];
  static bool init = false;
  if (!init) {
    memset(tbl, -1, sizeof tbl);
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 62; ++i) tbl[(uint8_t)a[i]] = (int8_t)i;
    tbl['+'] = tbl['-'] = 62;
    tbl['/'] = tbl['_'] = 63;
    init = true;
  }
  std::string out;
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    int8_t v = tbl[(uint8_t)c];
    if (v < 0) continue;
    buf = (buf << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
  }
  return out;
}

static std::string jwtPayload(const std::string& tok) {
  size_t a = tok.find('.');
  if (a == std::string::npos) return {};
  size_t b = tok.find('.', a + 1);
  if (b == std::string::npos) return {};
  return b64urlDecode(std::string_view(tok).substr(a + 1, b - a - 1));
}

int64_t parseISO8601(const std::string& s) {
  int Y, M, D, h = 0, m = 0, sec = 0;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3) return 0;
  struct tm t {};
  t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
  t.tm_hour = h; t.tm_min = m; t.tm_sec = sec;
  int64_t v = (int64_t)timegm(&t);
  size_t tpos = s.find('T');
  if (tpos != std::string::npos) {
    size_t z = s.find_first_of("Z+-", tpos);
    if (z != std::string::npos && s[z] != 'Z') {
      int oh = 0, om = 0;
      sscanf(s.c_str() + z + 1, "%d:%d", &oh, &om);
      int off = oh * 3600 + om * 60;
      v -= (s[z] == '+') ? off : -off;
    }
  }
  return v;
}

// Accepts unix seconds, unix ms, or ISO-8601 strings.
static int64_t timeValue(json::V v) {
  if (v.isNum()) {
    double d = v.num();
    return (int64_t)(d > 1e12 ? d / 1000 : d);
  }
  if (v.isStr()) {
    std::string s = v.str();
    if (!s.empty() && isdigit((unsigned char)s[0]) && s.find('-') == std::string::npos) {
      double d = atof(s.c_str());
      return (int64_t)(d > 1e12 ? d / 1000 : d);
    }
    return parseISO8601(s);
  }
  return 0;
}

static std::string titleCase(std::string s) {
  bool up = true;
  for (char& c : s) {
    if (c == '_' || c == '-') { c = ' '; up = true; continue; }
    if (up) c = (char)toupper((unsigned char)c), up = false;
  }
  return s;
}

std::string fmtMoney(double v) {
  char b[48];
  snprintf(b, sizeof b, "$%.2f", v);
  return b;
}

std::string fmtDuration(int64_t s) {
  if (s < 60) return s <= 0 ? "now" : "<1m";
  int64_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  char b[48];
  if (d > 0) snprintf(b, sizeof b, "%lldd %lldh", (long long)d, (long long)h);
  else if (h > 0) snprintf(b, sizeof b, "%lldh %lldm", (long long)h, (long long)m);
  else snprintf(b, sizeof b, "%lldm", (long long)m);
  return b;
}

std::string fmtUpdated(int64_t age) {
  if (age < 60) return "Updated just now";
  return "Updated " + fmtDuration(age) + " ago";
}

double remainingPct(const Snapshot& s) {
  double r = -1;
  for (auto& w : s.windows)
    if (w.inBar && w.usedPct >= 0) {
      double left = std::max(0.0, 100.0 - w.usedPct);
      if (r < 0 || left < r) r = left;
    }
  return r;
}

std::string barText(const Snapshot& s) {
  double r = remainingPct(s);
  if (r >= 0) {
    char b[16];
    snprintf(b, sizeof b, "%d%%", (int)std::floor(r + 0.0001));
    return b;
  }
  if (!s.barText.empty()) return s.barText;
  return s.hasData ? "—" : "…";
}

// ---------------------------------------------------------------- keychain

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// Reads a generic password. status out: errSecSuccess, errSecItemNotFound,
// errSecInteractionNotAllowed (would prompt), errSecUserCanceled / errSecAuthFailed.
static std::string keychainRead(const char* service, const char* account, bool allowUI, OSStatus* status) {
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
  CFStringRef svc = CFStringCreateWithCString(nullptr, service, kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrService, svc);
  CFStringRef acc = nullptr;
  if (account) {
    acc = CFStringCreateWithCString(nullptr, account, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecAttrAccount, acc);
  }
  CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
  if (!allowUI) CFDictionarySetValue(q, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
  CFTypeRef res = nullptr;
  OSStatus st = SecItemCopyMatching(q, &res);
  if (status) *status = st;
  std::string out;
  if (st == errSecSuccess && res && CFGetTypeID(res) == CFDataGetTypeID()) {
    CFDataRef d = (CFDataRef)res;
    out.assign((const char*)CFDataGetBytePtr(d), (size_t)CFDataGetLength(d));
  }
  if (res) CFRelease(res);
  CFRelease(svc);
  if (acc) CFRelease(acc);
  CFRelease(q);
  return out;
}
#pragma clang diagnostic pop

static const char* kOwnService = "UsageBar API keys";

static std::mutex gKeyMu;
static std::string gKeyCache[kProviderCount];
static bool gKeyLoaded[kProviderCount];

// Returns the key; sets *needsUI when the item exists but reading it requires a prompt
// (happens after rebuilding the ad-hoc-signed binary).
static std::string apiKeyImpl(PID p, bool interactive, bool* needsUI) {
  std::lock_guard<std::mutex> l(gKeyMu);
  int i = (int)p;
  if (!gKeyLoaded[i]) {
    OSStatus st = 0;
    gKeyCache[i] = keychainRead(kOwnService, info(p).id, interactive, &st);
    if (st == errSecInteractionNotAllowed) { if (needsUI) *needsUI = true; return {}; }
    gKeyLoaded[i] = true;
  }
  return gKeyCache[i];
}
std::string apiKey(PID p) { return apiKeyImpl(p, false, nullptr); }

bool setApiKey(PID p, const std::string& key) {
  CFStringRef svc = CFStringCreateWithCString(nullptr, kOwnService, kCFStringEncodingUTF8);
  CFStringRef acc = CFStringCreateWithCString(nullptr, info(p).id, kCFStringEncodingUTF8);
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrService, svc);
  CFDictionarySetValue(q, kSecAttrAccount, acc);
  SecItemDelete(q);
  OSStatus st = errSecSuccess;
  if (!key.empty()) {
    CFDataRef data = CFDataCreate(nullptr, (const UInt8*)key.data(), (CFIndex)key.size());
    CFDictionarySetValue(q, kSecValueData, data);
    st = SecItemAdd(q, nullptr);
    CFRelease(data);
  }
  CFRelease(q);
  CFRelease(svc);
  CFRelease(acc);
  std::lock_guard<std::mutex> l(gKeyMu);
  gKeyCache[(int)p] = (st == errSecSuccess) ? key : std::string();
  gKeyLoaded[(int)p] = true;
  return st == errSecSuccess;
}

// ---------------------------------------------------------------- credential caches

struct ClaudeCred {
  std::string token, subType, tier;
  int64_t expiresAt = 0;  // seconds
};
static std::mutex gCredMu;
static ClaudeCred gClaude;
static int64_t gClaudeKeychainDeniedUntil = 0;
static std::string gMuseToken;
static int64_t gMuseKeychainDeniedUntil = 0;

void invalidateCredentialCache(PID p) {
  std::lock_guard<std::mutex> l(gCredMu);
  if (p == PID::Claude) gClaude = {};
  if (p == PID::Muse) gMuseToken.clear();
  if (p == PID::DeepSeek || p == PID::OpenRouter) {
    std::lock_guard<std::mutex> k(gKeyMu);
    gKeyLoaded[(int)p] = false;
  }
}

static bool parseClaudeCred(const std::string& raw, ClaudeCred& c) {
  json::Doc d(raw);
  auto o = d.root()["claudeAiOauth"];
  if (!o.isObj()) return false;
  c.token = o["accessToken"].str();
  c.expiresAt = (int64_t)(o["expiresAt"].num() / 1000);
  c.subType = o["subscriptionType"].str();
  c.tier = o["rateLimitTier"].str();
  return !c.token.empty();
}

// Small mtime-keyed cache for ~/.claude.json account info (file can be large).
static std::string gClaudeEmail, gClaudeOrgType;
static int64_t gClaudeJsonMtime = -2;

static void loadClaudeAccount() {
  std::string path = home() + "/.claude.json";
  int64_t mt = mtimeOf(path);
  if (mt == gClaudeJsonMtime) return;
  gClaudeJsonMtime = mt;
  std::string raw;
  if (!readFile(path, raw, 32 << 20)) return;
  json::Doc d(raw);
  auto a = d.root()["oauthAccount"];
  gClaudeEmail = a["emailAddress"].str();
  gClaudeOrgType = a["organizationType"].str();
}

static std::string claudePlanName(const ClaudeCred& c) {
  std::string t = c.subType;
  if (t.empty()) {
    t = gClaudeOrgType;  // e.g. "claude_pro"
    if (t.rfind("claude_", 0) == 0) t = t.substr(7);
  }
  if (t.empty()) return {};
  std::string name = titleCase(t);
  if (c.tier.find("20x") != std::string::npos) name += " 20x";
  else if (c.tier.find("5x") != std::string::npos) name += " 5x";
  return name;
}

// ---------------------------------------------------------------- plan builders

static void auth(Request& r, const std::string& tok) { r.headers.push_back({"Authorization", "Bearer " + tok}); }

static Plan planClaude(bool interactive) {
  Plan p;
  std::lock_guard<std::mutex> l(gCredMu);
  loadClaudeAccount();
  int64_t now = nowSec();
  if (gClaude.token.empty() || gClaude.expiresAt <= now + 60) {
    ClaudeCred best;
    std::string raw;
    if (readFile(home() + "/.claude/.credentials.json", raw)) parseClaudeCred(raw, best);
    if (best.expiresAt <= now + 60 && (interactive || now >= gClaudeKeychainDeniedUntil)) {
      OSStatus st = 0;
      std::string kc = keychainRead("Claude Code-credentials", nullptr, interactive, &st);
      ClaudeCred k;
      if (!kc.empty() && parseClaudeCred(kc, k) && k.expiresAt > best.expiresAt) best = k;
      if (st == errSecInteractionNotAllowed) {
        if (best.token.empty()) p.error = "Keychain access needed — open the menu and click Refresh, then choose “Always Allow”.";
      } else if (st == errSecUserCanceled || st == errSecAuthFailed) {
        gClaudeKeychainDeniedUntil = now + 6 * 3600;
      }
    }
    gClaude = best;
  }
  p.seed.email = gClaudeEmail;
  p.seed.plan = claudePlanName(gClaude);
  if (gClaude.token.empty()) {
    if (p.error.empty()) p.error = "Not signed in. Run `claude` and log in.";
    p.authProblem = true;
    return p;
  }
  if (gClaude.expiresAt && gClaude.expiresAt <= now) {
    p.error = "Claude login expired. Run `claude` once to refresh it.";
    p.authProblem = true;
    gClaude = {};
    return p;
  }
  Request r;
  r.url = "https://api.anthropic.com/api/oauth/usage";
  auth(r, gClaude.token);
  r.headers.push_back({"anthropic-beta", "oauth-2025-04-20"});
  r.headers.push_back({"Accept", "application/json"});
  r.headers.push_back({"User-Agent", "claude-cli/2.1.0 (external, cli)"});
  p.reqs.push_back(std::move(r));
  return p;
}

static Plan planCodex() {
  Plan p;
  const char* ch = getenv("CODEX_HOME");
  std::string raw, path = (ch && *ch ? std::string(ch) : home() + "/.codex") + "/auth.json";
  if (!readFile(path, raw)) { p.error = "Not signed in. Run `codex` and log in."; p.authProblem = true; return p; }
  json::Doc d(raw);
  auto t = d.root()["tokens"];
  std::string tok = t["access_token"].str(), acct = t["account_id"].str();
  if (tok.empty()) {
    p.error = d.root()["OPENAI_API_KEY"].isStr() ? "Codex is using an API key; plan usage needs a ChatGPT login."
                                                 : "Not signed in. Run `codex` and log in.";
    p.authProblem = true;
    return p;
  }
  std::string idp = jwtPayload(t["id_token"].str());
  json::Doc id(idp);
  p.seed.email = id.root()["email"].str();
  std::string pt = id.root()["https://api.openai.com/auth"]["chatgpt_plan_type"].str();
  if (!pt.empty()) p.seed.plan = titleCase(pt);
  std::string ap = jwtPayload(tok);
  json::Doc at(ap);
  int64_t exp = (int64_t)at.root()["exp"].num();
  if (exp && exp <= nowSec()) {
    p.error = "Codex login expired. Run `codex` once to refresh it.";
    p.authProblem = true;
    return p;
  }
  Request r;
  r.url = "https://chatgpt.com/backend-api/wham/usage";
  auth(r, tok);
  if (!acct.empty()) r.headers.push_back({"ChatGPT-Account-Id", acct});
  r.headers.push_back({"User-Agent", "codex-cli"});
  r.headers.push_back({"Accept", "application/json"});
  p.reqs.push_back(std::move(r));
  return p;
}

static Plan planKey(PID pid, bool interactive) {
  Plan p;
  bool needsUI = false;
  std::string key = apiKeyImpl(pid, interactive, &needsUI);
  if (needsUI) {
    p.error = "Keychain access needed — click Refresh, then choose “Always Allow”.";
    p.authProblem = true;
    return p;
  }
  if (key.empty()) {
    const char* env = getenv(pid == PID::DeepSeek ? "DEEPSEEK_API_KEY" : "OPENROUTER_API_KEY");
    if (env) key = env;
  }
  if (key.empty()) {
    p.error = std::string("No API key. Click “Set API Key…” below (") + info(pid).keyHelp + ").";
    p.authProblem = true;
    return p;
  }
  auto mk = [&](const char* url) {
    Request r;
    r.url = url;
    auth(r, key);
    r.headers.push_back({"Accept", "application/json"});
    return r;
  };
  if (pid == PID::DeepSeek) {
    p.reqs.push_back(mk("https://api.deepseek.com/user/balance"));
  } else {
    p.reqs.push_back(mk("https://openrouter.ai/api/v1/key"));
    p.reqs.push_back(mk("https://openrouter.ai/api/v1/credits"));
  }
  return p;
}

static Plan planGrok() {
  Plan p;
  std::string raw;
  if (!readFile(home() + "/.grok/auth.json", raw)) { p.error = "Not signed in. Run `grok` and log in."; p.authProblem = true; return p; }
  json::Doc d(raw);
  std::string tok;
  int64_t exp = 0;
  for (auto e : d.root()) {
    std::string k = e["key"].str();
    if (k.empty()) continue;
    tok = k;
    p.seed.email = e["email"].str();
    exp = timeValue(e["expires_at"]);
    break;
  }
  if (tok.empty()) { p.error = "Not signed in. Run `grok` and log in."; p.authProblem = true; return p; }
  if (exp && exp <= nowSec()) {
    p.error = "Grok login expired. Run `grok` once to refresh it.";
    p.authProblem = true;
    return p;
  }
  auto mk = [&](const char* url) {
    Request r;
    r.url = url;
    auth(r, tok);
    r.headers.push_back({"x-xai-token-auth", "xai-grok-cli"});
    r.headers.push_back({"Accept", "application/json"});
    return r;
  };
  p.reqs.push_back(mk("https://cli-chat-proxy.grok.com/v1/billing?format=credits"));
  p.reqs.push_back(mk("https://cli-chat-proxy.grok.com/v1/settings"));
  return p;
}

static Plan planMuse(bool interactive) {
  Plan p;
  const char* envPath = getenv("MUSE_AUTH_PATH");
  std::string raw, path = envPath ? envPath : home() + "/.config/muse/auth.json";
  bool loggedIn = false;
  std::string inlineTok;
  if (readFile(path, raw)) {
    json::Doc d(raw);
    auto m = d.root()["providers"]["meta"];
    loggedIn = m.isObj();
    p.seed.email = m["user_email"].str();
    inlineTok = m["access_token"].str();
  }
  std::lock_guard<std::mutex> l(gCredMu);
  std::string tok = inlineTok.empty() ? gMuseToken : inlineTok;
  int64_t now = nowSec();
  if (tok.empty() && (interactive || now >= gMuseKeychainDeniedUntil)) {
    OSStatus st = 0;
    std::string kc = keychainRead("ai.meta.dev.credentials", "meta", interactive, &st);
    if (!kc.empty()) {
      json::Doc k(kc);
      tok = k.root()["access_token"].str();
      if (tok.empty() && kc.rfind("dca:", 0) == 0) tok = kc;  // raw token form
      gMuseToken = tok;
    } else if (st == errSecInteractionNotAllowed) {
      p.error = "Keychain access needed — open the menu and click Refresh, then choose “Always Allow”.";
      p.authProblem = true;
      return p;
    } else if (st == errSecUserCanceled || st == errSecAuthFailed) {
      gMuseKeychainDeniedUntil = now + 6 * 3600;
    }
  }
  if (tok.rfind("dca:", 0) != 0) {
    p.error = loggedIn ? "Muse needs a device-code login. Run `muse login` again." : "Not signed in. Run `muse login`.";
    p.authProblem = true;
    return p;
  }
  Request r;
  r.method = "POST";
  r.url = "https://api.meta.ai/muse-code/key";
  r.body = "{}";
  auth(r, tok);
  r.headers.push_back({"x-api-version", "1.0.0"});
  r.headers.push_back({"Content-Type", "application/json"});
  r.headers.push_back({"Accept", "application/json"});
  p.reqs.push_back(std::move(r));
  return p;
}

Plan buildPlan(PID pid, bool interactive) {
  Plan p;
  switch (pid) {
    case PID::Claude: p = planClaude(interactive); break;
    case PID::Codex: p = planCodex(); break;
    case PID::DeepSeek:
    case PID::OpenRouter: p = planKey(pid, interactive); break;
    case PID::Grok: p = planGrok(); break;
    case PID::Muse: p = planMuse(interactive); break;
    default: p.error = "Unknown provider";
  }
  p.pid = pid;
  return p;
}

// ---------------------------------------------------------------- parsers

static std::string httpError(const Response& r, PID pid) {
  if (r.status == 0) return "Network error" + (r.netError.empty() ? std::string() : ": " + r.netError);
  if (r.status == 401 || r.status == 403) {
    switch (pid) {
      case PID::Claude: return "Claude rejected the login (HTTP " + std::to_string(r.status) + "). Run `claude` to refresh it.";
      case PID::Codex: return "Codex rejected the login. Run `codex` to refresh it.";
      case PID::Grok: return "Grok rejected the login. Run `grok` to refresh it.";
      case PID::Muse: return "Muse rejected the login. Run `muse login` again.";
      default: return "API key rejected (HTTP " + std::to_string(r.status) + ").";
    }
  }
  if (r.status == 429) return "Rate limited (HTTP 429). Will retry later.";
  return "HTTP " + std::to_string(r.status);
}

static void addWindow(Snapshot& s, std::string label, json::V w, const char* pctKey, const char* resetKey,
                      int64_t dur, bool inBar) {
  if (!w.isObj()) return;
  json::V pv = w[pctKey];
  if (!pv.isNum() && !pv.isStr()) return;
  Window x;
  x.label = std::move(label);
  x.usedPct = std::clamp(pv.num(), 0.0, 100.0);
  x.resetAt = timeValue(w[resetKey]);
  x.durationSec = dur;
  x.inBar = inBar;
  s.windows.push_back(std::move(x));
}

static std::string windowLabel(int64_t dur) {
  if (dur <= 0) return "Usage";
  if (dur <= 6 * 3600) return "Session";
  if (dur >= 6 * 86400 && dur <= 8 * 86400) return "Weekly";
  if (dur >= 27 * 86400 && dur <= 32 * 86400) return "Monthly";
  if (dur % 86400 == 0) return std::to_string(dur / 86400) + "-day";
  return std::to_string(dur / 3600) + "h";
}

static void parseClaude(Snapshot& s, const json::V& r) {
  addWindow(s, "Session", r["five_hour"], "utilization", "resets_at", 5 * 3600, true);
  json::V wk = r["seven_day"];
  if (!wk["utilization"].isNum()) wk = r["seven_day_oauth_apps"];
  addWindow(s, "Weekly", wk, "utilization", "resets_at", 7 * 86400, true);
  addWindow(s, "Opus weekly", r["seven_day_opus"], "utilization", "resets_at", 7 * 86400, false);
  addWindow(s, "Sonnet weekly", r["seven_day_sonnet"], "utilization", "resets_at", 7 * 86400, false);
  json::V ex = r["extra_usage"];
  if (ex.isObj() && ex["is_enabled"].boolean()) {
    double used = ex["used_credits"].num() / 100.0, lim = ex["monthly_limit"].num() / 100.0;
    s.notes.push_back("Extra usage: " + fmtMoney(used) + (lim > 0 ? " / " + fmtMoney(lim) : std::string()) +
                      " this month");
  }
  if (s.windows.empty()) s.notes.push_back("No plan limits reported for this account.");
}

static void parseCodex(Snapshot& s, const json::V& r) {
  std::string pt = r["plan_type"].str();
  if (!pt.empty()) s.plan = titleCase(pt);
  json::V rl = r["rate_limit"];
  int64_t now = nowSec();
  for (const char* k : {"primary_window", "secondary_window"}) {
    json::V w = rl[k];
    if (!w.isObj()) continue;
    Window x;
    x.durationSec = (int64_t)w["limit_window_seconds"].num();
    x.label = windowLabel(x.durationSec);
    x.usedPct = std::clamp(w["used_percent"].num(), 0.0, 100.0);
    x.resetAt = timeValue(w["reset_at"]);
    if (!x.resetAt && w["reset_after_seconds"].isNum()) x.resetAt = now + (int64_t)w["reset_after_seconds"].num();
    s.windows.push_back(std::move(x));
  }
  json::V cr = r["credits"];
  if (cr.isObj() && cr["has_credits"].boolean()) {
    if (cr["unlimited"].boolean()) s.notes.push_back("Credits: unlimited");
    else {
      char b[64];
      snprintf(b, sizeof b, "Credits: %.0f", cr["balance"].num());
      s.notes.push_back(b);
    }
  }
}

static void parseDeepSeek(Snapshot& s, const json::V& r) {
  json::V pick;
  for (auto b : r["balance_infos"]) {
    if (!pick.ok()) pick = b;
    if (b["currency"].str() == "USD") { pick = b; break; }
  }
  if (!pick.ok()) { s.ok = false; s.error = "Unexpected balance response"; return; }
  std::string cur = pick["currency"].str();
  double total = pick["total_balance"].num(), paid = pick["topped_up_balance"].num(),
         granted = pick["granted_balance"].num();
  auto money = [&](double v) {
    if (cur == "USD") return fmtMoney(v);
    char b[48];
    snprintf(b, sizeof b, cur == "CNY" ? "¥%.2f" : "%.2f", v);
    return std::string(b);
  };
  s.barText = money(total);
  s.notes.push_back("Balance: " + money(total) + " (Paid " + money(paid) + " · Granted " + money(granted) + ")");
  if (total <= 0) s.notes.push_back("Add credits at platform.deepseek.com");
  else if (!r["is_available"].boolean(true)) s.notes.push_back("Balance unavailable for API calls");
}

static void parseOpenRouter(Snapshot& s, const json::V& key, const json::V& credits) {
  json::V k = key["data"];
  if (k.isObj()) {
    double lim = k["limit"].num(-1);
    if (lim > 0) {
      double rem = k["limit_remaining"].num(-1);
      double used = rem >= 0 ? lim - rem : k["usage"].num();
      Window w;
      std::string rs = k["limit_reset"].str();
      w.label = "Key limit" + (rs.empty() ? std::string() : " (" + rs + ")");
      w.usedPct = std::clamp(used / lim * 100.0, 0.0, 100.0);
      // OpenRouter resets key limits at UTC boundaries (weekly = Monday 00:00 UTC).
      time_t now = (time_t)nowSec();
      struct tm t {};
      gmtime_r(&now, &t);
      t.tm_hour = t.tm_min = t.tm_sec = 0;
      int64_t midnight = (int64_t)timegm(&t);
      if (rs == "daily") { w.resetAt = midnight + 86400; w.durationSec = 86400; }
      else if (rs == "weekly") {
        int daysToMon = (8 - t.tm_wday) % 7;
        w.resetAt = midnight + (int64_t)(daysToMon ? daysToMon : 7) * 86400;
        w.durationSec = 7 * 86400;
      } else if (rs == "monthly") {
        struct tm m = t;
        m.tm_mday = 1;
        int64_t start = (int64_t)timegm(&m);
        m.tm_mon += 1;
        w.resetAt = (int64_t)timegm(&m);
        w.durationSec = w.resetAt - start;
      }
      s.windows.push_back(w);
      s.notes.push_back("Spending cap " + fmtMoney(lim) + ", " + fmtMoney(std::max(0.0, lim - used)) + " left");
    }
    if (k["usage_daily"].isNum())
      s.notes.push_back("Key spend: today " + fmtMoney(k["usage_daily"].num()) + " · week " +
                        fmtMoney(k["usage_weekly"].num()) + " · month " + fmtMoney(k["usage_monthly"].num()));
    std::string label = k["label"].str();
    if (!label.empty() && label.rfind("sk-", 0) != 0) s.email = label;
  }
  json::V c = credits["data"];
  if (c.isObj()) {
    double bal = c["total_credits"].num() - c["total_usage"].num();
    s.barText = fmtMoney(bal);
    s.notes.insert(s.notes.begin(), "Balance: " + fmtMoney(bal) + " (of " + fmtMoney(c["total_credits"].num()) +
                                        " purchased)");
  }
}

static double valOf(json::V v) { return v.isObj() ? v["val"].num(-1) : v.num(-1); }

static void parseGrok(Snapshot& s, const json::V& billing, const json::V& settings) {
  json::V c = billing["config"];
  if (!c.isObj()) c = billing;
  double pct = c["creditUsagePercent"].num(-1);
  if (pct < 0) {
    double cap = valOf(c["onDemandCap"]), used = valOf(c["onDemandUsed"]);
    if (cap > 0 && used >= 0) pct = used / cap * 100;
  }
  int64_t st = timeValue(c["currentPeriod"]["start"]), en = timeValue(c["currentPeriod"]["end"]);
  if (!en) { st = timeValue(c["billingPeriodStart"]); en = timeValue(c["billingPeriodEnd"]); }
  if (pct >= 0) {
    Window w;
    w.durationSec = (st && en > st) ? en - st : 0;
    w.label = w.durationSec ? windowLabel(w.durationSec) : "Credits";
    if (w.label == "Usage") w.label = "Credits";
    w.usedPct = std::clamp(pct, 0.0, 100.0);
    w.resetAt = en;
    s.windows.push_back(w);
  } else if (en > nowSec() && st && st <= nowSec()) {
    // Proto3 omits zero values: an active period with no usage fields means nothing used yet.
    Window w;
    w.durationSec = en - st;
    w.label = windowLabel(w.durationSec);
    w.usedPct = 0;
    w.resetAt = en;
    s.windows.push_back(w);
    s.notes.push_back("Grok reports no usage this period (0% assumed).");
  } else if (en) {
    s.notes.push_back("Usage not reported yet this period");
  }
  json::V pb = c["prepaidBalance"];
  double usd = pb.isObj() ? valOf(pb["usd"]) : -1;
  if (usd == 0) usd = -1;
  if (usd >= 0) {
    s.notes.push_back("Prepaid balance: " + fmtMoney(usd));
    if (s.windows.empty()) s.barText = fmtMoney(usd);
  }
  std::string tier = settings["subscription_tier_display"].str();
  if (tier.empty()) tier = billing["subscription_tier"].str();
  if (!tier.empty()) s.plan = tier;
}

static void parseMuse(Snapshot& s, const json::V& r) {
  if (r["require_payment"].boolean()) { s.ok = false; s.error = "Muse needs a payment method — finish billing at dev.meta.ai"; return; }
  std::string tier = r["subs_tier_name"].str();
  if (!tier.empty()) s.plan = tier;
  if (!r["is_subs_active"].boolean()) { s.notes.push_back("No active Muse Code subscription (pay-as-you-go)."); return; }
  json::V u = r["subs_usage"];
  if (!u.isObj()) {
    Window w;
    w.label = "Session";
    w.usedPct = 0;
    w.durationSec = 5 * 3600;
    s.windows.push_back(w);
    s.notes.push_back("5h window idle — weekly quota not included in this response.");
    return;
  }
  json::V win = u["window"], wk = u["weekly"];
  int64_t mins = (int64_t)win["window_duration_mins"].num(300);
  addWindow(s, windowLabel(mins * 60), win, "used_percent", "resets_at", mins * 60, true);
  addWindow(s, "Weekly", wk, "used_percent", "resets_at", 7 * 86400, true);
}

Snapshot parse(const Plan& plan, const std::vector<Response>& rs, int64_t now) {
  Snapshot s = plan.seed;
  s.updatedAt = now;
  if (!plan.error.empty()) { s.error = plan.error; return s; }
  // Optional secondary requests (OpenRouter credits, Grok settings) may fail without failing the fetch.
  const Response& r0 = rs.at(0);
  if (r0.status != 200) { s.error = httpError(r0, plan.pid); return s; }
  json::Doc d0(r0.body);
  if (!d0.ok()) { s.error = "Unexpected response from server"; return s; }
  s.ok = true;
  s.hasData = true;
  std::string empty = "{}";
  json::Doc none(empty);
  auto second = [&](json::Doc& holder) -> json::V {
    return (rs.size() > 1 && rs[1].status == 200 && holder.ok()) ? holder.root() : none.root();
  };
  switch (plan.pid) {
    case PID::Claude: parseClaude(s, d0.root()); break;
    case PID::Codex: parseCodex(s, d0.root()); break;
    case PID::DeepSeek: parseDeepSeek(s, d0.root()); break;
    case PID::Muse: parseMuse(s, d0.root()); break;
    case PID::OpenRouter: {
      json::Doc d1(rs.size() > 1 ? std::string_view(rs[1].body) : std::string_view());
      parseOpenRouter(s, d0.root(), second(d1));
      if (rs.size() > 1 && rs[1].status != 200) s.notes.push_back("Balance unavailable for this key (needs account access).");
      break;
    }
    case PID::Grok: {
      json::Doc d1(rs.size() > 1 ? std::string_view(rs[1].body) : std::string_view());
      parseGrok(s, d0.root(), second(d1));
      break;
    }
    default: break;
  }
  return s;
}

// ---------------------------------------------------------------- pace + learning

struct Learn {
  double sumW = 0, sumS = 0, lastS = -1, lastW = -1;
  int64_t lastReset = 0;
};
static std::mutex gLearnMu;
static Learn gLearn[kProviderCount];
static bool gLearnLoaded = false;

static std::string learnPath() { return home() + "/Library/Application Support/UsageBar/learn.txt"; }

static void loadLearn() {
  if (gLearnLoaded) return;
  gLearnLoaded = true;
  std::ifstream f(learnPath());
  std::string id;
  Learn l;
  while (f >> id >> l.sumW >> l.sumS >> l.lastS >> l.lastW >> l.lastReset)
    for (int i = 0; i < kProviderCount; ++i)
      if (id == kInfo[i].id) gLearn[i] = l;
}

static void saveLearn() {
  std::string dir = home() + "/Library/Application Support/UsageBar";
  mkdir(dir.c_str(), 0700);
  std::string tmp = learnPath() + ".tmp";
  {
    std::ofstream f(tmp);
    for (int i = 0; i < kProviderCount; ++i) {
      const Learn& l = gLearn[i];
      f << kInfo[i].id << ' ' << l.sumW << ' ' << l.sumS << ' ' << l.lastS << ' ' << l.lastW << ' ' << l.lastReset
        << '\n';
    }
  }
  rename(tmp.c_str(), learnPath().c_str());
}

static const Window* findWin(const Snapshot& s, bool session) {
  for (auto& w : s.windows) {
    if (!w.inBar || w.usedPct < 0) continue;
    if (session && w.durationSec > 0 && w.durationSec <= 6 * 3600) return &w;
    if (!session && w.durationSec >= 6 * 86400 && w.durationSec <= 8 * 86400) return &w;
  }
  return nullptr;
}

void learnSample(PID p, const Snapshot& s) {
  const Window* se = findWin(s, true);
  const Window* wk = findWin(s, false);
  if (!se || !wk) return;
  std::lock_guard<std::mutex> lk(gLearnMu);
  loadLearn();
  Learn& l = gLearn[(int)p];
  bool sameSession = l.lastS >= 0 && se->resetAt && std::llabs(se->resetAt - l.lastReset) < 120;
  bool changed = false;
  if (sameSession) {
    double dS = se->usedPct - l.lastS, dW = wk->usedPct - l.lastW;
    if (dS > 0 && dW >= 0 && dW < 50) {
      l.sumS += dS;
      l.sumW += dW;
      if (l.sumS > 400) { l.sumS *= 0.5; l.sumW *= 0.5; }  // exponential forgetting
      changed = true;
    }
  }
  if (l.lastS != se->usedPct || l.lastW != wk->usedPct || l.lastReset != se->resetAt) changed = true;
  l.lastS = se->usedPct;
  l.lastW = wk->usedPct;
  l.lastReset = se->resetAt;
  if (changed) saveLearn();
}

double weeklyPerSession(PID p) {
  std::lock_guard<std::mutex> lk(gLearnMu);
  loadLearn();
  const Learn& l = gLearn[(int)p];
  if (l.sumS < 10 || l.sumW <= 0) return -1;
  return l.sumW / l.sumS * 100.0;  // weekly % burned by one full session
}

std::string paceLine(PID p, const Snapshot& s, const Window& w, int64_t now, double* expectedOut, bool* reserveOut) {
  if (expectedOut) *expectedOut = -1;
  if (w.usedPct < 0 || !w.resetAt || w.durationSec < 86400) return {};
  int64_t left = w.resetAt - now;
  if (left <= 0) return {};
  int64_t elapsed = std::clamp<int64_t>(w.durationSec - left, 0, w.durationSec);
  double expected = (double)elapsed / (double)w.durationSec * 100.0;
  if (expectedOut) *expectedOut = expected;
  double delta = w.usedPct - expected;
  if (reserveOut) *reserveOut = delta <= 0;
  std::vector<std::string> parts;
  char b[96];
  int dv = (int)std::lround(std::fabs(delta));
  if (dv == 0) parts.push_back("On pace");
  else {
    snprintf(b, sizeof b, "%d%% in %s", dv, delta < 0 ? "reserve" : "deficit");
    parts.push_back(b);
  }
  if (w.usedPct <= 0 || elapsed <= 0) parts.push_back("Lasts until reset");
  else {
    double rate = w.usedPct / (double)elapsed;
    double eta = (100.0 - w.usedPct) / rate;
    if (eta >= (double)left) parts.push_back("Lasts until reset");
    else parts.push_back("Runs out in " + fmtDuration((int64_t)eta));
  }
  const Window* wk = findWin(s, false);
  if (wk == &w && findWin(s, true)) {
    double per = weeklyPerSession(p);
    if (per > 0) {
      snprintf(b, sizeof b, "Est. %.1f session quotas left", std::max(0.0, 100.0 - w.usedPct) / per);
      parts.push_back(b);
    }
    int64_t windows = (left + 5 * 3600 - 1) / (5 * 3600);
    snprintf(b, sizeof b, "%lld window%s until reset", (long long)windows, windows == 1 ? "" : "s");
    parts.push_back(b);
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) out += (i ? " · " : "") + parts[i];
  return out;
}

}  // namespace ub
