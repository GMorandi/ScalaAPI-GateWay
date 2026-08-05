@0xa1b2c3d4e5f60718;

using Types = import "types.capnp";

interface GatewayDispatch {
  dispatch @0 (request: DispatchRequest) -> (response: DispatchResponse);
  reportUsage @1 (report: Types.UsageReport) -> (ack: WriteAck);
  abort @2 (leaseToken: Text, reason: Text) -> (ack: WriteAck);
  reportUpstreamError @3 (report: Types.ErrorReport) -> (ack: WriteAck);
  mediaOperation @4 (request: MediaOperationRequest) -> (response: MediaOperationResponse);
}

struct WriteAck {
  accepted @0 :Bool;
  duplicate @1 :Bool;
  retryable @2 :Bool;
  errorCode @3 :Text;
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
  protocolVersion @9 :UInt16;
  stream @10 :Bool;
  # v2 extension fields.  Existing field numbers are intentionally immutable.
  operation @11 :Text;
  inboundFormat @12 :Text;
  httpMethod @13 :Text;
  requestPath @14 :Text;
  contentType @15 :Text;
  capability @16 :Text;
  idempotencyKey @17 :Text;
  realtimeSession @18 :Bool;
  forcePlatform @19 :Text;
  requestFingerprint @20 :Text;
  requestQuery @21 :Text;

  enum EndpointKind {
    messages @0;
    chatCompletions @1;
    responses @2;
    embeddings @3;
    images @4;
    gemini @5;
    videos @6;
    countTokens @7;
    models @8;
    alphaSearch @9;
    realtime @10;
    antigravity @11;
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
  protocolVersion @7 :UInt16;

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
    idempotencyConflict @8;
    unsupportedCapability @9;
    idempotencyReplay @10;
    pricingUnavailable @11;
  }
}

struct AbortRequest {
  leaseToken @0 :Text;
  reason @1 :Text;
}

struct MediaOperationRequest {
  apiKeyHash @0 :Text;
  operationId @1 :Text;
  action @2 :Text;
  requestId @3 :Text;
  clientIp @4 :Text;
  idempotencyKey @5 :Text;
  requestFingerprint @6 :Text;
  status @7 :Text;
  upstreamTaskId @8 :Text;
  outputMetadata @9 :Text;
  outputUrl @10 :Text;
  contentType @11 :Text;
  progress @12 :Int32;
}

struct MediaOperationResponse {
  accepted @0 :Bool;
  statusCode @1 :Int32;
  operationId @2 :Text;
  operationType @3 :Text;
  status @4 :Text;
  progress @5 :Int32;
  upstreamTaskId @6 :Text;
  outputMetadata @7 :Text;
  outputUrl @8 :Text;
  contentType @9 :Text;
  errorCode @10 :Text;
  errorMessage @11 :Text;
}
