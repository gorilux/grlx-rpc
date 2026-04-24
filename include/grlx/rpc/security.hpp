#pragma once

// Security configuration for grlx-rpc sessions and servers.
//
// These settings are the knobs you reach for when exposing an RPC server
// to untrusted networks (the public internet). Each field documents the
// attack it defends against and what a safe default looks like.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace grlx::rpc {

// Classic leaky-token-bucket rate limiter.
//
// `capacity` is the largest burst the limiter will allow before throttling
// kicks in; `refill_per_sec` is the steady-state rate. Tokens are real-
// valued so refill math is monotone even at very low rates (e.g. 0.5/s).
//
// Not thread-safe on its own — callers either own it exclusively (per-
// session) or protect it with an external mutex (per-IP).
struct token_bucket {
  double                                capacity       = 0.0;
  double                                refill_per_sec = 0.0;
  double                                tokens         = 0.0;
  std::chrono::steady_clock::time_point last_refill{};

  // Deduct `cost` tokens from the bucket, refilling first based on time
  // elapsed since the previous call. Returns true when the caller may
  // proceed, false when the bucket is empty.
  bool try_take(double cost = 1.0) {
    auto now = std::chrono::steady_clock::now();
    if (last_refill.time_since_epoch().count() == 0) {
      last_refill = now;
      tokens      = capacity;
    } else {
      double elapsed = std::chrono::duration<double>(now - last_refill).count();
      tokens         = std::min(capacity, tokens + elapsed * refill_per_sec);
      last_refill    = now;
    }
    // A non-positive capacity means "limiting disabled" — always allow.
    if (capacity <= 0.0) return true;
    if (tokens >= cost) {
      tokens -= cost;
      return true;
    }
    return false;
  }
};

// Per-session limits. Applied in session::msg_reader before allocating any
// buffer derived from wire-supplied sizes.
struct session_limits {
  // Hard cap on a single RPC payload (request body, response body, or
  // notification body). Defends against a peer that crafts a header with a
  // giant MSG_SIZE to force the server into allocating unbounded memory.
  //
  // The default, 8 MiB, accommodates video keyframes broadcast via
  // notifications (a 1080p H.264 I-frame routinely exceeds 1 MiB and a
  // 4K frame can approach 4 MiB). Apps that only push small control-plane
  // messages should tighten this to 1 MiB; apps with dedicated blob-upload
  // endpoints may need to raise it further and pair that with a matching
  // change to the Cereal decode archive settings.
  std::uint64_t max_message_bytes = 8ull << 20;  // 8 MiB

  // Maximum time a session may sit with no bytes arriving on the wire. When
  // exceeded, the session is closed — this is how zombie mobile sockets
  // behind NAT eventually get cleaned up (the OS often fails to surface the
  // dead peer, so we rely on an application-level deadline).
  //
  // The default (5 minutes) is generous enough for flaky cellular links and
  // for idle Nexus clients that only see sync traffic every few minutes.
  // Apps with long quiet periods should either raise this or implement a
  // keepalive (msg_type::nop is already supported and bypasses dispatch).
  std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);

  // Once a valid header has been read, how long the peer has to deliver the
  // declared body. Defends against slow-loris attacks that complete the
  // handshake and header but then drip-feed body bytes forever.
  //
  // 60 s accommodates a ~1 MiB payload over a moderate mobile link. Apps
  // that know their clients push big uploads on slow connections should
  // raise this; apps with small bounded payloads can lower it.
  std::chrono::milliseconds message_read_timeout = std::chrono::seconds(60);

  // Per-session request-rate limiter. Applied in the msg_reader before
  // dispatch. A breach sends a msg_type::error back and does NOT close the
  // session — apps that need stricter behavior can close in their auth
  // callback after seeing a burst of errors. Set capacity <= 0 to disable.
  double request_burst   = 200.0;
  double request_rate_ps = 1000.0;
};

// Knobs for TLS/mTLS context construction. Defaults are locked-down because
// these helpers are the on-ramp for internet-exposed services; opt-out is a
// conscious choice.
//
// The two peer-verification flags used to be a single `require_peer_cert`
// knob, but its meaning differed between server and client side, which
// confused callers. They are now split:
//   - require_client_cert applies only to server contexts and controls
//     whether mTLS is enforced.
//   - verify_server_cert applies only to client contexts and controls
//     whether the server's cert must chain to a trust anchor.
// apply_tls_config reads the flag relevant to the context's side.
struct tls_config {
  // Server side: enforce mTLS — every connecting client must present a
  // certificate signed by a CA added via add_certificate_authority.
  // When false, the server runs one-way TLS (server cert only).
  bool require_client_cert = true;

  // Client side: verify the server's certificate against the trust
  // anchors loaded into the context. When false, any cert is accepted
  // (do not use on the public internet — MITM becomes trivial).
  bool verify_server_cert = true;

  // Forbid every protocol older than TLS 1.3. Keep true on anything facing
  // the internet; drop to TLS 1.2 only if you must support legacy clients
  // and you've audited the cipher list.
  bool tls13_only = true;
};

// Per-session metadata surfaced to the application when a new session opens.
// Apps can use it for logging, metrics, and binding auth tokens to the
// client-cert fingerprint that was negotiated during the TLS handshake.
struct session_info {
  // SHA-256 fingerprint of the peer's X.509 certificate in DER form,
  // rendered as lowercase hex (no colons, no prefix). Empty if no peer
  // cert was presented (only possible when the server runs with
  // tls_config::require_client_cert = false or the channel is plain TCP).
  std::string peer_fingerprint;

  // Remote endpoint in "host:port" form for logging / rate-limit buckets.
  std::string peer_address;
};

// Who's making this RPC call? Passed to the auth callback before the request
// body has been decoded — identity here comes purely from the transport.
struct client_context {
  std::string peer_fingerprint;
  std::string peer_address;
};

// How strictly should the dispatcher gate a method?
enum class visibility {
  // Callable by anyone who completed the handshake. Reserved for endpoints
  // that intentionally run pre-auth: login, version probe, cert challenges.
  public_,

  // Default. The auth callback must approve the call before the handler
  // runs. Use for anything that reads or mutates per-user state.
  authenticated,

  // Callback must both approve AND mark the caller as admin. Use for
  // privileged operations (user creation, role changes, destructive ops).
  admin,
};

// Outcome of an auth callback invocation. `deny_reason` is included only for
// logging at the server — the wire error to the peer is intentionally generic
// so attackers can't probe the auth policy by watching responses.
struct auth_result {
  bool        allow    = false;
  bool        is_admin = false;
  std::string deny_reason;
};

// Signature the application implements to gate dispatch. The callback MUST
// be fast (runs in the session's read path) and MUST NOT throw.
using auth_callback = std::function<auth_result(client_context const&,
                                                std::string const& method_name)>;

// Server-wide connection limits. Enforced at the channel layer *before* the
// TLS handshake so a peer attempting to exhaust the server wastes only a
// TCP SYN/ACK per rejection, never a full handshake.
struct server_limits {
  // Hard cap on concurrent sessions across all sources. The internet-facing
  // default (1000) is intentionally generous for a small LAN deployment and
  // restrictive enough to cap memory growth on a successful handshake flood.
  std::size_t max_concurrent_sessions = 1000;

  // Cap on concurrent sessions originating from the same IPv4/IPv6 address.
  // A single client should never legitimately exceed this — mobile clients
  // that reconnect after network blips will free their old session well
  // before they hit 8. Raise cautiously; the defense is all in the small n.
  std::size_t max_sessions_per_ip = 8;

  // Per-IP accept-rate limiter. Defends against a single source opening
  // handshakes faster than we can complete them. A legitimate mobile client
  // that reconnects aggressively during a flapping network averages well
  // under 1/sec; 60/min with a burst of 20 leaves that comfortable.
  // Set capacity <= 0 to disable.
  double accept_burst_per_ip         = 20.0;  // token bucket capacity
  double accept_refill_per_ip_per_s  = 1.0;   // == 60/min
};

}  // namespace grlx::rpc
