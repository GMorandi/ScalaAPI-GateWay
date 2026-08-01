@0xc3d4e5f60718293a;

interface InvalidationStream {
  subscribe @0 (gatewayId: Text) -> (stream: InvalidationEvent);
  resync @1 (versions: List(VersionEntry)) -> (stale: List(VersionEntry));

  struct VersionEntry {
    entityType @0 :Text;
    entityKey @1 :Text;
    version @2 :Int64;
  }
}

struct InvalidationEvent {
  entityType @0 :EntityType;
  entityKey @1 :Text;
  version @2 :Int64;
  kind @3 :Kind;

  enum EntityType {
    apiKey @0;
    account @1;
    group @2;
    user @3;
    config @4;
  }

  enum Kind {
    evict @0;
    upsert @1;
    delete @2;
  }
}
