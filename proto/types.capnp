@0xb2c3d4e5f6071829;

struct AuthSnapshot {
  apiKeyId @0 :Int64;
  userId @1 :Int64;
  groupId @2 :Int64;
  name @3 :Text;
  status @4 :Text;
  ipWhitelist @5 :List(Text);
  ipBlacklist @6 :List(Text);
  user @7 :UserSnapshot;
  group @8 :GroupSnapshot;
  quota @9 :Float64;
  quotaUsed @10 :Float64;
  expiresAt @11 :Int64;
  rateLimit5h @12 :Float64;
  rateLimit1d @13 :Float64;
  rateLimit7d @14 :Float64;
  version @15 :Int64;
}

struct UserSnapshot {
  id @0 :Int64;
  status @1 :Text;
  role @2 :Text;
  balance @3 :Float64;
  concurrency @4 :Int32;
  allowedGroups @5 :List(Int64);
  rpmLimit @6 :Int32;
}

struct GroupSnapshot {
  id @0 :Int64;
  platform @1 :Text;
  isExclusive @2 :Bool;
  status @3 :Text;
  rateMultiplier @4 :Float64;
  dailyLimitUsd @5 :Float64;
  claudeCodeOnly @6 :Bool;
  fallbackGroupId @7 :Int64;
  modelRoutingEnabled @8 :Bool;
}

struct UpstreamTarget {
  accountId @0 :Int64;
  platform @1 :Text;
  baseUrl @2 :Text;
  upstreamPath @3 :Text;
  authHeaders @4 :List(Header);
  mappedModel @5 :Text;
  proxy @6 :ProxyConfig;
  userId @7 :Int64;
  groupId @8 :Int64;
  billing @9 :BillingContext;
  tlsFingerprint @10 :Bool;

  struct Header {
    key @0 :Text;
    value @1 :Text;
  }
}

struct ProxyConfig {
  enabled @0 :Bool;
  url @1 :Text;
  username @2 :Text;
  password @3 :Text;
}

struct BillingContext {
  rateMultiplier @0 :Float64;
  holdAmount @1 :Float64;
  holdHandle @2 :Text;
  model @3 :Text;
  upstreamModel @4 :Text;
  inboundEndpoint @5 :Text;
}

struct AccountProjection {
  id @0 :Int64;
  name @1 :Text;
  platform @2 :Text;
  priority @3 :Int32;
  concurrency @4 :Int32;
  currentLoad @5 :Int32;
  schedulable @6 :Bool;
  rateMultiplier @7 :Float64;
  loadFactor @8 :Int32;
  status @9 :Text;
  rateLimitResetAt @10 :Int64;
  overloadUntil @11 :Int64;
  tempUnschedulableUntil @12 :Int64;
  supportedModels @13 :List(Text);
  groupIds @14 :List(Int64);
}

struct UsageReport {
  leaseToken @0 :Text;
  requestId @1 :Text;
  apiKeyId @2 :Int64;
  userId @3 :Int64;
  accountId @4 :Int64;
  groupId @5 :Int64;
  model @6 :Text;
  upstreamModel @7 :Text;
  inboundEndpoint @8 :Text;
  inputTokens @9 :Int32;
  outputTokens @10 :Int32;
  cacheCreateTokens @11 :Int32;
  cacheReadTokens @12 :Int32;
  durationMs @13 :Int32;
  firstTokenMs @14 :Int32;
  stream @15 :Bool;
  clientDisconnect @16 :Bool;
  forceCacheBilling @17 :Bool;
  ipAddress @18 :Text;
  userAgent @19 :Text;
}

struct ErrorReport {
  accountId @0 :Int64;
  statusCode @1 :Int32;
  retryAfterMs @2 :Int32;
  requestId @3 :Text;
  errorMessage @4 :Text;
}

struct ModelRoute {
  pattern @0 :Text;
  accountIds @1 :List(Int64);
}

struct GroupConfig {
  id @0 :Int64;
  platform @1 :Text;
  rateMultiplier @2 :Float64;
  modelRoutingEnabled @3 :Bool;
  modelRoutes @4 :List(ModelRoute);
  claudeCodeOnly @5 :Bool;
  fallbackGroupId @6 :Int64;
  rpmLimit @7 :Int32;
  peakMultiplier @8 :Float64;
  peakStartHour @9 :Int32;
  peakEndHour @10 :Int32;
}
