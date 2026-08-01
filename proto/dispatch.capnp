@0xa1b2c3d4e5f60718;

using Types = import "types.capnp";

interface GatewayDispatch {
  dispatch @0 (request: DispatchRequest) -> (response: DispatchResponse);
  reportUsage @1 (report: Types.UsageReport) -> ();
  abort @2 (leaseToken: Text, reason: Text) -> ();
  reportUpstreamError @3 (report: Types.ErrorReport) -> ();
}

struct DispatchRequest {
  apiKeyHash @0 :Text;
  requestedModel @1 :Text;
  sessionHash @2 :Text;
  clientIp @3 :Text;
  requestId @4 :Text;
  excludedAccounts @5 :List(Int64);
  cachedAuthVersion @6 :Int64;
  endpoint @7 :EndpointKind;
  metadataUserId @8 :Text;

  enum EndpointKind {
    messages @0;
    chatCompletions @1;
    responses @2;
    embeddings @3;
    images @4;
    gemini @5;
  }
}

struct DispatchResponse {
  outcome @0 :Outcome;
  authVersion @1 :Int64;
  auth @2 :Types.AuthSnapshot;
  upstream @3 :Types.UpstreamTarget;
  waitPlan @4 :WaitPlan;
  reject @5 :RejectInfo;
  leaseToken @6 :Text;

  enum Outcome {
    ok @0;
    wait @1;
    rejected @2;
    reauth @3;
  }
}

struct WaitPlan {
  accountId @0 :Int64;
  maxConcurrency @1 :Int32;
  timeoutMs @2 :Int32;
  maxWaiting @3 :Int32;
}

struct RejectInfo {
  code @0 :RejectCode;
  message @1 :Text;
  retryAfterMs @2 :Int32;

  enum RejectCode {
    invalidKey @0;
    expired @1;
    noBalance @2;
    rateLimited @3;
    noAccount @4;
    concurrencyExceeded @5;
    ipBlocked @6;
    quotaExhausted @7;
  }
}

struct AbortRequest {
  leaseToken @0 :Text;
  reason @1 :Text;
}
