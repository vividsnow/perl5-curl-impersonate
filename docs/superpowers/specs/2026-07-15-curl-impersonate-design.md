# Curl::Impersonate -- design

Date: 2026-07-15
Status: approved, ready for an implementation plan

## Problem

Perl HTTP clients -- and EV::WebKit's network stack -- use OpenSSL or GnuTLS,
which cannot reproduce a real browser's TLS ClientHello (BoringSSL with GREASE
and Chrome's exact cipher/curve/extension ordering) or its HTTP/2 SETTINGS. So a
server fingerprints them (JA3/JA4, the Akamai HTTP/2 fingerprint) as
not-a-browser, regardless of how faithfully the JS layer is spoofed.

`curl-impersonate` is the proven engine for this: a patched libcurl built
against BoringSSL that emits Chrome/Safari/Firefox/Edge's exact TLS + HTTP/2
handshake. This distribution wraps its libcurl via XS so Perl can make HTTP
requests that carry a chosen browser's *connection* fingerprint.

It is the foundation for the planned EV::WebKit companion impersonation proxy
(a MITM forward proxy re-issues WebKit's requests through this client), and it is
independently useful as a fingerprint-resistant Perl HTTP client. This spec
covers ONLY this client + its build; the proxy and the EV::WebKit integration are
separate sub-projects.

## The linchpin -- a build spike is the plan's first task

Building `curl-impersonate` (BoringSSL + nghttp2 + brotli + zstd + the patched
curl) from source, in userspace, with NO sudo, is make-or-break. Go and Rust are
available in this environment but BoringSSL/curl-impersonate are not prebuilt.
The implementation plan's FIRST task is a fail-fast spike: clone
`lexiforest/curl-impersonate`, run its build to a private prefix, and confirm
that a linked `curl_easy_impersonate("chrome131", ...)` request reaches a
fingerprint endpoint and reports a Chrome JA4 (distinct from default-curl). If it
cannot build here, STOP and reconsider the engine (e.g. a Rust `rquest`/`impit`
path, or a Go `utls` sidecar) before writing the binding.

## Scope (v1)

- Build `libcurl-impersonate` (lexiforest fork) from source, userspace.
- An XS binding: synchronous requests with a chosen impersonate target, returning
  status + headers + body.
- Asynchronous requests via `curl_multi` socket-action driven by external I/O
  watchers (so the proxy can run it on an EV loop).
- HTTP/1.1 + HTTP/2 (curl-impersonate's default). HTTP/3/QUIC and WebSockets are
  explicitly deferred to later sub-projects.
- Impersonate targets: whatever the built curl-impersonate exposes (a set of
  Chrome/Safari/Firefox/Edge versions).

## Build & packaging

Idiomatic Perl split, matching the CPAN Alien pattern:

- **`Alien::curl_impersonate`** -- fetches (pinned version) and builds
  curl-impersonate + its bundled BoringSSL/nghttp2/brotli/zstd to the Alien share
  dir, exposing cflags/libs. Isolates the heavy, slow, platform-sensitive build.
- **`Curl::Impersonate`** -- the XS binding, linking `libcurl-impersonate` via the
  Alien's cflags/libs.

These two are tightly coupled (the Alien exists only to serve the binding), so
they are built together as this one sub-project; the plan orders the Alien build
+ spike first, then the XS. (The Task-1 spike may reveal that an inline build in
`Curl::Impersonate`'s own `Makefile.PL` is simpler than a separate Alien -- if so
the plan collapses them; the default is the Alien split.)

## API

```perl
use Curl::Impersonate;

my $c = Curl::Impersonate->new(impersonate => 'chrome131');   # a built-in target

# synchronous
my $res = $c->get('https://tls.peet.ws/api/all');
#   $res = { status => 200, headers => { 'content-type' => '...' }, body => '...' }
my $res = $c->request(method => 'POST', url => $u, headers => { ... }, body => $bytes);

# asynchronous -- curl_multi driven by caller-supplied I/O readiness (for the
# proxy's EV loop). The exact socket/timer callback surface is fixed in the plan;
# conceptually:
$c->request_async(\%req, sub { my ($res, $err) = @_; ... });

Curl::Impersonate->targets;   # the impersonate target names the built lib supports
```

- `new(impersonate => $target, %opts)` creates a handle and applies
  `curl_easy_impersonate($h, $target, 1)` (1 = also set the browser's default
  headers/order). `%opts`: `timeout`, `verify` (default on), `follow_redirects`.
- `request`/`get` return a response hashref; a `on_body => sub {}` streaming
  option lets the proxy forward bytes without buffering the whole body.
- Errors (connect/resolve/TLS/timeout) are returned as `(undef, $err)` (async) or
  croak/`$res->{error}` (sync) -- never a die deep inside a callback.
- Origin certificate verification stays ON by default; impersonation changes the
  handshake shape, not whether the peer is verified.

## Error handling

- A libcurl error maps to a stable `{ code => $curl_code, message => $str }`; the
  binding never leaves a half-driven multi handle on error.
- Per-request timeout (connect + total).
- The Alien build failing at install degrades to a clear error at `use` time
  (the lib is absent), not a mysterious link error.

## Testing

- **Build/load:** the Task-1 spike; then a smoke test that the XS loads and
  `targets()` is non-empty.
- **Impersonation (the point):** an `xt/` live test (skip without network) that
  GETs a fingerprint endpoint (`tls.peet.ws/api/all` or equivalent) and asserts
  the reported `ja4`/`ja3_hash`/`akamai_hash` correspond to Chrome and DIFFER from
  a default-curl/OpenSSL baseline captured in the same run.
- **Correctness:** GET/POST against an httpbin-style endpoint -- status, response
  headers, and request body/headers round-trip.
- **Async:** the same request through `request_async` on a minimal EV loop
  resolves once with the same result.

## Not in scope

The MITM proxy (sub-project 2: `EV::Proxy::Impersonate`), the EV::WebKit coherence
glue (sub-project 3), HTTP/3/QUIC, and WebSockets. No cookie jar / session state
beyond a single handle in v1 (the proxy manages its own). No attempt to keep the
impersonate target list in sync with EV::WebKit's fingerprint profiles here --
that mapping lives in the integration sub-project.
