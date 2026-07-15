# Curl::Impersonate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Perl XS HTTP client that speaks a chosen real browser's TLS (JA3/JA4) and HTTP/2 (Akamai) fingerprint, by binding `libcurl-impersonate`.

**Architecture:** `Alien::curl_impersonate` builds curl-impersonate (patched curl + bundled BoringSSL/nghttp2/brotli/zstd) from source into its share dir and exposes cflags/libs; `Curl::Impersonate` is an XS binding that links it and wraps a `curl_easy` handle plus `curl_easy_impersonate()`, with a `curl_multi` async path drivable from an EV loop.

**Tech Stack:** Perl 5.10+, XS, ExtUtils::MakeMaker, Alien::Build, curl-impersonate (lexiforest fork), BoringSSL, libcurl API. Go toolchain (present: go1.26) is required by the curl-impersonate build.

## Global Constraints

- Commit author `vividsnow`, no name/email leak, no LLM attribution: `git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "..."`. No `Co-Authored-By`.
- POD is plain ASCII: no unicode, no em-dash -- use `--`.
- No sudo / no system package installs anywhere -- everything builds to a user-writable prefix/share.
- Dist root: `/home/yk/dev/perl-modules/Curl-Impersonate` (spec already committed there). A second dist `/home/yk/dev/perl-modules/Alien-curl_impersonate` holds the Alien.
- Pin the curl-impersonate version (a git tag) so the build is reproducible; do NOT track `main`.
- v1 scope: HTTP/1.1 + HTTP/2 only. No HTTP/3, no WebSockets, no cookie jar.
- Origin certificate verification stays ON by default.

### Verified facts (do not re-derive)

- curl-impersonate (lexiforest) native build: `git clone`, `mkdir build && cd build`, `../configure --prefix=$PREFIX`, `make build`, `make install`. The build auto-downloads + builds BoringSSL/nghttp2/brotli/zstd, then patched curl. Prereqs: `git ninja-build cmake autoconf automake pkg-config libtool ca-certificates curl golang-go bzip2 xz-utils unzip`.
- The impersonation API is `CURLcode curl_easy_impersonate(CURL *handle, const char *target, int default_headers)` -- `target` is a browser name like `"chrome131"`; `default_headers=1` also applies the browser's default header set/order. It configures the whole TLS + HTTP/2 fingerprint on the handle.
- The lexiforest fork ships ONE `libcurl-impersonate.so` (runtime target selection), not per-browser libraries.
- Go 1.26, cmake, autoconf/automake, pkg-config, libtool are available in this environment; `ninja` availability is checked by Task 1.

---

## Task 1: Build spike -- the fail-fast gate

Prove curl-impersonate builds userspace/no-sudo here and that `curl_easy_impersonate` yields a real Chrome JA4. If it can't build or link, STOP and reconsider the engine before writing any binding.

**Files:**
- Create: `xt/build-spike.sh` (the build recipe, kept for the Alien to reuse)
- Create: `xt/spike.c` (a tiny C program linking libcurl-impersonate)

**Interfaces:**
- Produces: a built curl-impersonate at `$HOME/.cache/curl-impersonate-prefix` (or the dist's `.tmp`), the confirmed build recipe, and the confirmed `curl_easy_impersonate` signature/target names.

- [ ] **Step 1: Check prerequisites**

Run:
```bash
for t in git ninja cmake autoconf automake pkg-config libtool go bzip2 xz unzip cc; do
  command -v "$t" >/dev/null 2>&1 && echo "ok   $t" || echo "MISS $t"
done
```
Expected: all `ok`. If any is `MISS` and cannot be provided without sudo, STOP -- record which, and reconsider (e.g. a Rust `rquest` engine) before proceeding.

- [ ] **Step 2: Write the build recipe**

Create `xt/build-spike.sh`:
```bash
#!/bin/sh
# Build curl-impersonate (lexiforest) to a user prefix. No sudo.
set -eu
VER="${CI_VERSION:-v1.2.0}"          # pin a real tag; Task 1 confirms the exact latest tag
PREFIX="${CI_PREFIX:-$PWD/.tmp/ci-prefix}"
SRC="$PWD/.tmp/curl-impersonate"
mkdir -p "$PWD/.tmp"
[ -d "$SRC" ] || git clone --depth 1 --branch "$VER" https://github.com/lexiforest/curl-impersonate.git "$SRC"
cd "$SRC"
rm -rf build && mkdir build && cd build
../configure --prefix="$PREFIX"
make build
make install
echo "PREFIX=$PREFIX"
ls -1 "$PREFIX/lib"/libcurl-impersonate* "$PREFIX/include/curl/curl.h" 2>&1
```
(Task-1 runner: first check the real latest tag at https://github.com/lexiforest/curl-impersonate/releases and set `CI_VERSION` to it.)

- [ ] **Step 3: Run the build (the slow gate)**

Run:
```bash
cd /home/yk/dev/perl-modules/Curl-Impersonate
chmod +x xt/build-spike.sh
CI_VERSION=<latest-tag> time sh xt/build-spike.sh 2>&1 | tail -30
```
Expected: a `$PREFIX/lib/libcurl-impersonate.so*` and `$PREFIX/include/curl/curl.h` exist. This is a long build (BoringSSL). If it fails, capture the error and STOP -- diagnose (missing prereq, Go/cmake version, network) before continuing.

- [ ] **Step 4: Write a C linking spike**

Create `xt/spike.c`:
```c
#include <curl/curl.h>
#include <stdio.h>
static size_t sink(char *p, size_t s, size_t n, void *u){ (void)p;(void)u; return s*n; }
int main(void){
    CURL *h = curl_easy_init();
    if (!h) { fprintf(stderr, "init failed\n"); return 2; }
    CURLcode rc = curl_easy_impersonate(h, "chrome131", 1);
    printf("impersonate rc=%d (%s)\n", rc, curl_easy_strerror(rc));
    curl_easy_setopt(h, CURLOPT_URL, "https://tls.peet.ws/api/all");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink);
    rc = curl_easy_perform(h);
    printf("perform rc=%d (%s)\n", rc, curl_easy_strerror(rc));
    long code = 0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    printf("http=%ld\n", code);
    curl_easy_cleanup(h);
    return rc == CURLE_OK ? 0 : 1;
}
```

- [ ] **Step 5: Compile + run the C spike (prove the API links AND impersonates)**

Run:
```bash
cd /home/yk/dev/perl-modules/Curl-Impersonate
PREFIX="$PWD/.tmp/ci-prefix"
cc -o .tmp/spike xt/spike.c -I"$PREFIX/include" -L"$PREFIX/lib" -lcurl-impersonate \
   -Wl,-rpath,"$PREFIX/lib"
LD_LIBRARY_PATH="$PREFIX/lib" ./.tmp/spike
```
Expected: `impersonate rc=0`, `perform rc=0`, `http=200`. `curl_easy_impersonate` linking proves the fork's API is present; a 200 from `tls.peet.ws` proves it handshakes.

- [ ] **Step 6: Confirm the fingerprint is actually Chrome's (the whole point)**

Run:
```bash
cd /home/yk/dev/perl-modules/Curl-Impersonate
PREFIX="$PWD/.tmp/ci-prefix"
# impersonated JA4 via the built curl-impersonate binary:
LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/curl-impersonate" -s --impersonate chrome131 https://tls.peet.ws/api/all | perl -0777 -ne 'print "impersonated ja4: $1\n" if /"ja4":"([^"]+)"/'
# baseline (system curl / OpenSSL) for contrast:
curl -s https://tls.peet.ws/api/all | perl -0777 -ne 'print "system-curl ja4: $1\n" if /"ja4":"([^"]+)"/'
```
Expected: the two `ja4` strings DIFFER, and the impersonated one begins `t13d` with a Chrome-shaped cipher/extension count (not the OpenSSL one). This is the go/no-go: if the JA4 is identical to system curl, impersonation isn't working -- STOP.

- [ ] **Step 7: Commit the spike recipe**

```bash
cd /home/yk/dev/perl-modules/Curl-Impersonate
git add xt/build-spike.sh xt/spike.c docs/superpowers/plans/2026-07-15-curl-impersonate.md
git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "spike: build curl-impersonate userspace + confirm Chrome JA4"
```
(Add `.tmp/` to `.gitignore` first so the build output is not committed.)

---

## Task 2: Alien::curl_impersonate -- reproducible build as a share

Wrap the proven recipe in an `Alien::Build` dist so `Curl::Impersonate` gets `cflags`/`libs` without re-deriving the build.

**Files:**
- Create: `/home/yk/dev/perl-modules/Alien-curl_impersonate/Makefile.PL`
- Create: `/home/yk/dev/perl-modules/Alien-curl_impersonate/alienfile`
- Create: `/home/yk/dev/perl-modules/Alien-curl_impersonate/lib/Alien/curl_impersonate.pm`
- Test: `/home/yk/dev/perl-modules/Alien-curl_impersonate/t/00-alien.t`

**Interfaces:**
- Consumes: the build recipe from Task 1.
- Produces: `Alien::curl_impersonate` (an `Alien::Base` subclass) with `->cflags`, `->libs`, `->dynamic_libs`, `->runtime_prop->{prefix}`.

- [ ] **Step 1: Write the alienfile**

Create `Alien-curl_impersonate/alienfile`:
```perl
use alienfile;

# Always build from source (no system libcurl-impersonate exists).
share {
    my $ver = 'v1.2.0';   # keep in sync with the version Task 1 confirmed
    start_url "https://github.com/lexiforest/curl-impersonate.git";
    plugin 'Fetch::Git' => (tag => $ver);
    plugin 'Build::Autoconf';   # curl-impersonate's ./configure
    build [
        'mkdir -p build',
        'cd build && ../configure --prefix=%{.install.prefix}',
        'cd build && %{make} build',
        'cd build && %{make} install',
    ];
    plugin 'Gather::IsolateDynamic';
    gather [
        # expose the include + lib we installed
        [ '%{pkgconf} --exists libcurl' ],
    ];
    ffi { symbol_prefix 'curl_' }
}
```
(If `Fetch::Git`/`Build::Autoconf` prove awkward for this build, the fallback -- recorded from Task 1 -- is a `plugin 'Build::CommandSequence'` running `xt/build-spike.sh`'s exact steps against `%{.install.prefix}`. The Task-1 spike already proved those commands, so this is a mechanical translation.)

- [ ] **Step 2: Write Makefile.PL + the module**

Create `Alien-curl_impersonate/Makefile.PL`:
```perl
use v5.10; use strict; use warnings;
use ExtUtils::MakeMaker;
use Alien::Base::Wrapper ();
WriteMakefile(
    NAME             => 'Alien::curl_impersonate',
    VERSION_FROM     => 'lib/Alien/curl_impersonate.pm',
    ABSTRACT         => 'Build/find libcurl-impersonate (browser-fingerprint curl)',
    AUTHOR           => 'vividsnow',
    LICENSE          => 'perl_5',
    MIN_PERL_VERSION => '5.010',
    CONFIGURE_REQUIRES => { 'Alien::Build::MM' => '0', 'Alien::Base::Wrapper' => '0' },
    BUILD_REQUIRES   => { 'Alien::Build::MM' => '0' },
    TEST_REQUIRES    => { 'Test::More' => '0', 'Test::Alien' => '0' },
    PREREQ_PM        => { 'Alien::Base' => '0' },
);
# Alien::Build::MM glue:
package main;
```
Create `Alien-curl_impersonate/lib/Alien/curl_impersonate.pm`:
```perl
package Alien::curl_impersonate;
use v5.10; use strict; use warnings;
use parent 'Alien::Base';
our $VERSION = '0.01';
1;
```
(The canonical `Alien::Build::MM` `WriteMakefile` incantation -- `my $abmm = Alien::Build::MM->new; WriteMakefile($abmm->mm_args(...))` plus the `postamble`/`clean`/`realclean` overrides -- is standard boilerplate; follow the `Alien::Build::MM` synopsis exactly.)

- [ ] **Step 3: Write the failing test**

Create `Alien-curl_impersonate/t/00-alien.t`:
```perl
use v5.10; use strict; use warnings;
use Test::More;
use Test::Alien;
use Alien::curl_impersonate;

alien_ok 'Alien::curl_impersonate';
my $xs = do { local $/; <DATA> };
xs_ok { xs => $xs }, with_subtest {
    my ($mod) = @_;
    ok $mod->has_impersonate, 'curl_easy_impersonate symbol is linkable';
};
done_testing;
__DATA__
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include <curl/curl.h>
MODULE = TA_MODULE PACKAGE = TA_MODULE
int has_impersonate(klass)
    const char *klass
  CODE:
    /* link-time proof: reference the symbol */
    RETVAL = (curl_easy_impersonate != 0);
  OUTPUT:
    RETVAL
```

- [ ] **Step 4: Build + test the Alien**

Run:
```bash
cd /home/yk/dev/perl-modules/Alien-curl_impersonate
perl Makefile.PL && make 2>&1 | tail -15
prove -lv t/00-alien.t
```
Expected: the Alien builds curl-impersonate to its share and `t/00-alien.t` links `curl_easy_impersonate` (PASS). This build is slow (BoringSSL) -- allow time.

- [ ] **Step 5: Commit**

```bash
cd /home/yk/dev/perl-modules/Alien-curl_impersonate && git init -q 2>/dev/null
printf '/blib/\n/Makefile\n/MYMETA.*\n/pm_to_blib\n_alien/\n.tmp/\n' > .gitignore
git add -A
git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "Alien::curl_impersonate: build libcurl-impersonate from source"
```

---

## Task 3: Curl::Impersonate skeleton -- link the Alien, load clean

**Files:**
- Create: `Makefile.PL`, `lib/Curl/Impersonate.pm`, `Impersonate.xs`, `.gitignore`
- Test: `t/00-load.t`

**Interfaces:**
- Consumes: `Alien::curl_impersonate` (cflags/libs).
- Produces: a loadable `Curl::Impersonate` with `->new` returning a blessed handle (no request logic yet).

- [ ] **Step 1: Write Makefile.PL linking the Alien**

Create `Makefile.PL`:
```perl
use v5.10; use strict; use warnings;
use ExtUtils::MakeMaker;
use Alien::Base::Wrapper qw( Alien::curl_impersonate !export );
WriteMakefile(
    NAME             => 'Curl::Impersonate',
    VERSION_FROM     => 'lib/Curl/Impersonate.pm',
    ABSTRACT         => 'HTTP client that impersonates a browser TLS/HTTP2 fingerprint',
    AUTHOR           => 'vividsnow',
    LICENSE          => 'perl_5',
    MIN_PERL_VERSION => '5.010',
    CONFIGURE_REQUIRES => { 'Alien::Base::Wrapper' => 0, 'Alien::curl_impersonate' => 0 },
    PREREQ_PM        => { 'Alien::curl_impersonate' => 0 },
    TEST_REQUIRES    => { 'Test::More' => 0 },
    Alien::Base::Wrapper->mm_args,   # injects the Alien's CCFLAGS/LIBS/INC
);
```

- [ ] **Step 2: Write the module + minimal XS**

Create `lib/Curl/Impersonate.pm`:
```perl
package Curl::Impersonate;
use v5.10; use strict; use warnings;
our $VERSION = '0.01';
require XSLoader;
XSLoader::load('Curl::Impersonate', $VERSION);
1;
```
Create `Impersonate.xs`:
```c
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include <curl/curl.h>

typedef struct { CURL *easy; } ci_t;

MODULE = Curl::Impersonate    PACKAGE = Curl::Impersonate    PREFIX = ci_

BOOT:
    curl_global_init(CURL_GLOBAL_DEFAULT);

SV *
ci_new(klass, ...)
    const char *klass
  PREINIT:
    ci_t *self;
    SV *ref;
  CODE:
    Newxz(self, 1, ci_t);
    self->easy = curl_easy_init();
    if (!self->easy) { Safefree(self); croak("Curl::Impersonate: curl_easy_init failed"); }
    ref = newSV(0);
    sv_setref_pv(ref, klass, (void *) self);
    RETVAL = ref;
  OUTPUT:
    RETVAL

void
ci_DESTROY(self_sv)
    SV *self_sv
  PREINIT:
    ci_t *self;
  CODE:
    self = (ci_t *) SvIV((SV *) SvRV(self_sv));
    if (self) { if (self->easy) curl_easy_cleanup(self->easy); Safefree(self); }
```

- [ ] **Step 3: Write the failing test**

Create `t/00-load.t`:
```perl
use v5.10; use strict; use warnings;
use Test::More;
use_ok('Curl::Impersonate');
my $c = Curl::Impersonate->new;
isa_ok($c, 'Curl::Impersonate', 'new returns a handle');
done_testing;
```

- [ ] **Step 4: Build + test**

Run:
```bash
cd /home/yk/dev/perl-modules/Curl-Impersonate
perl Makefile.PL && make 2>&1 | tail -8
prove -lv t/00-load.t
```
Expected: compiles against the Alien's headers/libs, loads, `new` returns a blessed handle. PASS.

- [ ] **Step 5: Commit**

```bash
printf '/blib/\n/Makefile\n/MYMETA.*\n/pm_to_blib\n*.o\n*.c\nImpersonate.c\n.tmp/\n' > .gitignore
git add Makefile.PL lib/Curl/Impersonate.pm Impersonate.xs t/00-load.t .gitignore
git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "Curl::Impersonate: skeleton XS linking Alien::curl_impersonate"
```

---

## Task 4: Synchronous request + impersonation

**Files:**
- Modify: `Impersonate.xs`, `lib/Curl/Impersonate.pm`
- Test: `t/10-request.t`, `xt/20-fingerprint.t`

**Interfaces:**
- Consumes: the skeleton `ci_t`.
- Produces:
  - `Curl::Impersonate->new(impersonate => $target, %opt)` where `%opt`: `timeout` (seconds), `verify` (bool, default 1), `follow_redirects` (bool).
  - `$c->request(method => $m, url => $u, headers => \%h, body => $bytes)` -> `{ status => $int, headers => \%h, body => $bytes }` or `{ error => $str, code => $int }`.
  - `$c->get($url)` = `request(method => 'GET', url => $url)`.

- [ ] **Step 1: Write the failing tests**

Create `t/10-request.t`:
```perl
use v5.10; use strict; use warnings;
use Test::More;
use Curl::Impersonate;

my $host = $ENV{CI_TEST_HTTPBIN};   # e.g. a local httpbin; skip if absent
plan skip_all => 'set CI_TEST_HTTPBIN to an httpbin-like base URL' unless $host;

my $c = Curl::Impersonate->new(impersonate => 'chrome131', timeout => 20);
my $r = $c->get("$host/get");
is($r->{status}, 200, 'GET 200');
like($r->{headers}{'content-type'} // '', qr/json/i, 'response headers parsed');
like($r->{body}, qr/"url"/, 'body returned');

my $p = $c->request(method => 'POST', url => "$host/post", headers => { 'X-Test' => 'hi' }, body => 'abc=1');
is($p->{status}, 200, 'POST 200');
like($p->{body}, qr/"abc":\s*"1"/, 'POST body echoed');
like($p->{body}, qr/"X-Test":\s*"hi"/, 'request header sent');
done_testing;
```
Create `xt/20-fingerprint.t` (the point -- live, network-gated):
```perl
use v5.10; use strict; use warnings;
use Test::More;
use Curl::Impersonate;
plan skip_all => 'network fingerprint test; set CI_LIVE=1 to run' unless $ENV{CI_LIVE};

my $c = Curl::Impersonate->new(impersonate => 'chrome131', timeout => 20);
my $r = $c->get('https://tls.peet.ws/api/all');
is($r->{status}, 200, 'fingerprint endpoint reachable');
my ($ja4) = $r->{body} =~ /"ja4":"([^"]+)"/;
ok($ja4, "got a ja4 ($ja4)");
like($ja4, qr/^t13d/, 'JA4 is a TLS 1.3 client (Chrome-shaped)');
# contrast: a NON-impersonated handle would differ. Assert the akamai h2 hash is present too.
like($r->{body}, qr/"akamai_hash":"[0-9a-f]+/, 'HTTP/2 (Akamai) fingerprint present');
done_testing;
```

- [ ] **Step 2: Run them (RED)**

Run: `CI_TEST_HTTPBIN=... prove -lv t/10-request.t`
Expected: FAIL -- `Can't locate object method "get"`.

- [ ] **Step 3: Implement request in XS**

Add to `Impersonate.xs` (before `ci_DESTROY`). This adds a header-collect + body-collect and a `ci_request` that takes url/method/headers/body and returns a hashref; plus `ci_impersonate`:
```c
struct buf { char *data; size_t len; };
static size_t body_cb(char *p, size_t s, size_t n, void *u){
    struct buf *b = u; size_t add = s*n;
    Renew(b->data, b->len + add + 1, char);
    memcpy(b->data + b->len, p, add); b->len += add; b->data[b->len] = 0;
    return add;
}
static size_t hdr_cb(char *p, size_t s, size_t n, void *u){
    HV *h = u; size_t len = s*n; char *c = memchr(p, ':', len);
    if (c) {
        size_t klen = c - p; const char *v = c + 1; size_t vlen = (p + len) - v;
        while (vlen && (*v==' '||*v=='\t')) { v++; vlen--; }
        while (vlen && (v[vlen-1]=='\r'||v[vlen-1]=='\n'||v[vlen-1]==' ')) vlen--;
        SV *key = newSVpvn(p, klen); (void) sv_2mortal(key);
        /* lower-case the header name for stable lookup */
        for (char *k = SvPVX(key); *k; k++) *k = tolower((unsigned char)*k);
        (void) hv_store(h, SvPVX(key), klen, newSVpvn(v, vlen), 0);
    }
    return len;
}
```
```c
MODULE = Curl::Impersonate    PACKAGE = Curl::Impersonate

int
ci_impersonate(self_sv, target, default_headers = 1)
    SV *self_sv
    const char *target
    int default_headers
  PREINIT:
    ci_t *self;
  CODE:
    self = (ci_t *) SvIV((SV *) SvRV(self_sv));
    RETVAL = (int) curl_easy_impersonate(self->easy, target, default_headers);
  OUTPUT:
    RETVAL

SV *
ci_request(self_sv, method, url, headers_hv, body_sv)
    SV *self_sv
    const char *method
    const char *url
    SV *headers_hv
    SV *body_sv
  PREINIT:
    ci_t *self; struct buf body = {0,0}; HV *rh; HV *res; CURLcode rc;
    struct curl_slist *hlist = NULL; long code = 0;
  CODE:
    self = (ci_t *) SvIV((SV *) SvRV(self_sv));
    curl_easy_setopt(self->easy, CURLOPT_URL, url);
    curl_easy_setopt(self->easy, CURLOPT_CUSTOMREQUEST, method);
    if (SvOK(body_sv) && SvCUR(body_sv)) {
        curl_easy_setopt(self->easy, CURLOPT_POSTFIELDS, SvPVX(body_sv));
        curl_easy_setopt(self->easy, CURLOPT_POSTFIELDSIZE, (long) SvCUR(body_sv));
    }
    if (SvROK(headers_hv) && SvTYPE(SvRV(headers_hv))==SVt_PVHV) {
        HV *hv = (HV*) SvRV(headers_hv); HE *e; hv_iterinit(hv);
        while ((e = hv_iternext(hv))) {
            STRLEN kl; char *k = HePV(e, kl); const char *v = SvPV_nolen(HeVAL(e));
            SV *line = sv_2mortal(newSVpvf("%s: %s", k, v));
            hlist = curl_slist_append(hlist, SvPVX(line));
        }
        curl_easy_setopt(self->easy, CURLOPT_HTTPHEADER, hlist);
    }
    rh = (HV*) sv_2mortal((SV*) newHV());
    curl_easy_setopt(self->easy, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(self->easy, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(self->easy, CURLOPT_HEADERFUNCTION, hdr_cb);
    curl_easy_setopt(self->easy, CURLOPT_HEADERDATA, rh);
    rc = curl_easy_perform(self->easy);
    if (hlist) curl_slist_free_all(hlist);
    res = (HV*) sv_2mortal((SV*) newHV());
    if (rc != CURLE_OK) {
        (void) hv_stores(res, "error", newSVpv(curl_easy_strerror(rc), 0));
        (void) hv_stores(res, "code",  newSViv((int) rc));
    } else {
        curl_easy_getinfo(self->easy, CURLINFO_RESPONSE_CODE, &code);
        (void) hv_stores(res, "status",  newSViv(code));
        (void) hv_stores(res, "headers", newRV_inc((SV*) rh));
        (void) hv_stores(res, "body",    body.data ? newSVpvn(body.data, body.len) : newSVpvs(""));
    }
    if (body.data) Safefree(body.data);
    RETVAL = newRV_inc((SV*) res);
  OUTPUT:
    RETVAL
```
Add `#include <ctype.h>` at the top of the XS.

- [ ] **Step 4: Implement the Perl API layer**

Add to `lib/Curl/Impersonate.pm` (before `1;`):
```perl
sub new {
    my ($class, %opt) = @_;
    my $self = _new($class);   # XS ci_new
    $self->_setopt_defaults(\%opt);
    if (defined $opt{impersonate}) {
        my $rc = $self->impersonate($opt{impersonate}, $opt{default_headers} // 1);
        Carp::croak("Curl::Impersonate: unknown impersonate target '$opt{impersonate}' (rc=$rc)") if $rc != 0;
    }
    return $self;
}
use Carp ();
1;
```
And a small `_setopt_defaults` in XS applying `timeout`/`verify`/`follow_redirects` via `curl_easy_setopt` (CURLOPT_TIMEOUT, CURLOPT_SSL_VERIFYPEER/HOST, CURLOPT_FOLLOWLOCATION). Provide it:
```c
void
_setopt_defaults(self_sv, opt_hv)
    SV *self_sv
    SV *opt_hv
  PREINIT:
    ci_t *self; HV *o;
  CODE:
    self = (ci_t *) SvIV((SV *) SvRV(self_sv));
    o = (HV*) SvRV(opt_hv);
    { SV **t = hv_fetchs(o, "timeout", 0);
      if (t && SvOK(*t)) curl_easy_setopt(self->easy, CURLOPT_TIMEOUT, (long) SvIV(*t)); }
    { SV **v = hv_fetchs(o, "verify", 0); long on = (v && SvOK(*v)) ? (SvTRUE(*v)?1:0) : 1;
      curl_easy_setopt(self->easy, CURLOPT_SSL_VERIFYPEER, on);
      curl_easy_setopt(self->easy, CURLOPT_SSL_VERIFYHOST, on ? 2L : 0L); }
    { SV **f = hv_fetchs(o, "follow_redirects", 0);
      if (f && SvTRUE(*f)) curl_easy_setopt(self->easy, CURLOPT_FOLLOWLOCATION, 1L); }
```
And add `get`:
```perl
sub get { my ($s, $url) = @_; $s->request(method => 'GET', url => $url) }
sub request {
    my ($s, %a) = @_;
    return $s->_request($a{method} // 'GET', $a{url}, $a{headers} // {}, $a{body});
}
```
(Rename the XS `ci_request` export to `_request` in the Perl call, or keep names aligned -- match whichever the XS exposes.)

- [ ] **Step 5: Build + test (GREEN)**

Run:
```bash
make 2>&1 | tail -8
CI_TEST_HTTPBIN=<httpbin-base> prove -lv t/10-request.t
CI_LIVE=1 prove -lv xt/20-fingerprint.t
```
Expected: `t/10-request.t` PASS (GET/POST, headers, body); `xt/20-fingerprint.t` PASS (status 200, a `t13d...` JA4, an akamai hash). If no httpbin is reachable, stand up a local one or use `https://tls.peet.ws/api/all` for the GET-only checks.

- [ ] **Step 6: Commit**

```bash
git add Impersonate.xs lib/Curl/Impersonate.pm t/10-request.t xt/20-fingerprint.t
git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "Curl::Impersonate: sync request/response + impersonate; live JA4 test"
```

---

## Task 5: targets() -- enumerate impersonation profiles

**Files:** Modify `Impersonate.xs`, `lib/Curl/Impersonate.pm`; Test `t/30-targets.t`

**Interfaces:** Produces `Curl::Impersonate->targets` -> sorted list of supported target names.

- [ ] **Step 1: Write the failing test**

Create `t/30-targets.t`:
```perl
use v5.10; use strict; use warnings;
use Test::More;
use Curl::Impersonate;
my @t = Curl::Impersonate->targets;
ok(scalar(@t) >= 1, 'at least one impersonate target');
ok((grep { /^chrome/ } @t), 'a chrome target exists');
# a made-up target must be rejected by new()
eval { Curl::Impersonate->new(impersonate => 'notabrowser999') };
like($@, qr/unknown impersonate target/, 'bad target croaks');
done_testing;
```

- [ ] **Step 2: Run (RED)**

Run: `prove -lv t/30-targets.t` -- Expected: FAIL (`targets` undefined).

- [ ] **Step 3: Implement targets in XS**

curl-impersonate exposes the target list; use its API if present, else the documented static set. Add to XS:
```c
void
targets(klass)
    const char *klass
  PPCODE:
    /* lexiforest exposes curl_easy_impersonate_target_list() returning a
       NULL-terminated char**; if unavailable at build time, fall back to a
       compiled-in list confirmed in Task 1. */
#ifdef CURL_IMPERSONATE_HAS_TARGET_LIST
    {
        const char * const *t = curl_easy_impersonate_target_list();
        for (; t && *t; t++) mXPUSHp(*t, strlen(*t));
    }
#else
    {
        static const char *fallback[] = { "chrome131","chrome124","chrome120",
            "safari18_0","firefox133","edge131", NULL };
        const char **t = fallback;
        for (; *t; t++) mXPUSHp(*t, strlen(*t));
    }
#endif
```
Task-5 runner: in Task 1, check whether `curl_easy_impersonate_target_list` exists in the built headers (`grep target_list $PREFIX/include/curl/*.h`); define `-DCURL_IMPERSONATE_HAS_TARGET_LIST` in Makefile.PL's `DEFINE` if so, and replace the fallback list with the ACTUAL targets the built lib reports.

- [ ] **Step 4: Build + test (GREEN)**

Run: `make && prove -lv t/30-targets.t` -- Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Impersonate.xs t/30-targets.t
git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "Curl::Impersonate: targets() + reject unknown target"
```

---

## Task 6: Async via curl_multi (EV-drivable)

The proxy needs concurrent upstream requests. Expose a `curl_multi`-backed async path whose socket/timer readiness is driven by the caller's event loop.

**Files:** Modify `Impersonate.xs`, `lib/Curl/Impersonate.pm`; Test `t/40-async.t`

**Interfaces:**
- Produces: `Curl::Impersonate->multi` -> a multi handle object with:
  - `$m->add($req_handle, \%req, $cb)` -- queue a request; `$cb->($res, $err)` fires once on completion.
  - `$m->socket_action($fd, $ev_bitmask)` and `$m->timeout_ms` -- the caller's EV io/timer watchers call these; `$m->set_socket_callback($cb)` / `set_timer_callback($cb)` register what to (re)arm.
  - `$m->perform_blocking` -- a convenience loop (for tests/simple use) that runs its own select until all queued requests finish.

- [ ] **Step 1: Write the failing test (blocking convenience path first)**

Create `t/40-async.t`:
```perl
use v5.10; use strict; use warnings;
use Test::More;
use Curl::Impersonate;
plan skip_all => 'set CI_LIVE=1' unless $ENV{CI_LIVE};

my $m = Curl::Impersonate->multi;
my %got;
for my $n (1..2) {
    my $c = Curl::Impersonate->new(impersonate => 'chrome131', timeout => 20);
    $m->add($c, { method => 'GET', url => 'https://tls.peet.ws/api/clean' }, sub {
        my ($res, $err) = @_; $got{$n} = $err ? "ERR:$err" : $res->{status};
    });
}
$m->perform_blocking;
is($got{1}, 200, 'request 1 completed');
is($got{2}, 200, 'request 2 completed');
done_testing;
```

- [ ] **Step 2: Run (RED)**

Run: `CI_LIVE=1 prove -lv t/40-async.t` -- Expected: FAIL (`multi` undefined).

- [ ] **Step 3: Implement the multi handle in XS**

Add a `ci_multi_t { CURLM *multi; }` package `Curl::Impersonate::Multi` with `curl_multi_init`/`curl_multi_cleanup`, an `add` that stashes the per-request `(callback SV, response-buffers)` in the easy handle's `CURLOPT_PRIVATE`, and `perform_blocking` using `curl_multi_perform` + `curl_multi_poll` + draining `curl_multi_info_read` for `CURLMSG_DONE` (invoking the stored callback with the built response hashref). Full socket_action/timer surface (`CURLMOPT_SOCKETFUNCTION`/`CURLMOPT_TIMERFUNCTION`) is added the same way, calling back into Perl SVs the caller registers.

```c
/* sketch of the DONE drain in perform_blocking (complete it fully):
 *   while (still_running) {
 *     curl_multi_perform(m->multi, &still_running);
 *     curl_multi_poll(m->multi, NULL, 0, 100, NULL);
 *     CURLMsg *msg; int q;
 *     while ((msg = curl_multi_info_read(m->multi, &q))) {
 *       if (msg->msg == CURLMSG_DONE) {
 *         CURL *e = msg->easy_handle; struct req *r; curl_easy_getinfo(e, CURLINFO_PRIVATE, &r);
 *         build response HV from r->body/r->headers + msg->data.result;
 *         call r->cb with (res_or_undef, err_or_undef);
 *         curl_multi_remove_handle(m->multi, e);
 *       }
 *     }
 *   }
 */
```
(This is the standard libcurl multi pattern; implement `add`/`perform_blocking` fully, and the socket-action/timer callbacks for the EV path. A `req` struct carries the callback SV + the two `struct buf`s reused from Task 4.)

- [ ] **Step 4: Perl wrappers**

Add to `lib/Curl/Impersonate.pm`:
```perl
sub multi { require Curl::Impersonate::Multi; Curl::Impersonate::Multi->_new }
```
with the `Curl::Impersonate::Multi` package (thin over the XS multi object).

- [ ] **Step 5: Build + test (GREEN)**

Run: `make && CI_LIVE=1 prove -lv t/40-async.t` -- Expected: both requests complete with status 200.

- [ ] **Step 6: Commit**

```bash
git add Impersonate.xs lib/Curl/Impersonate.pm t/40-async.t
git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "Curl::Impersonate: curl_multi async (blocking loop + EV-drivable socket-action)"
```

---

## Task 7: POD, Changes, MANIFEST, dist metadata

**Files:** Modify `lib/Curl/Impersonate.pm` (POD); Create `Changes`, `MANIFEST`, `README.md`

- [ ] **Step 1: Add POD**

Document `new` (with `impersonate`/`timeout`/`verify`/`follow_redirects`), `get`, `request` (and the response/error hashref shape), `targets`, `multi` + the async surface, and a SYNOPSIS. State the ceiling honestly: this impersonates the TLS + HTTP/2 connection fingerprint; it is an HTTP client, not a full browser, and h3/WebSockets are out of scope in v1. Note it requires `Alien::curl_impersonate` (a from-source BoringSSL build at install).

- [ ] **Step 2: Changes + README + MANIFEST**

Write `Changes` (0.01 initial), a short `README.md`, and generate `MANIFEST` (`make manifest`; ensure a `MANIFEST.SKIP` excludes `.tmp/`, `blib/`, `*.o`, the generated `Impersonate.c`).

- [ ] **Step 3: Full test pass**

Run:
```bash
make && prove -lv t/
CI_LIVE=1 prove -lv xt/
```
Expected: all `t/` pass; `xt/` (live) pass when network is available.

- [ ] **Step 4: Commit**

```bash
git add lib/Curl/Impersonate.pm Changes README.md MANIFEST MANIFEST.SKIP
git -c user.name=vividsnow -c user.email=vividsnow@pm.me commit -m "Curl::Impersonate: POD, Changes, README, MANIFEST"
```

---

## Self-review notes

- **Spec coverage:** the build (Task 1 spike + Task 2 Alien), the XS binding (Tasks 3-5), sync request/response + impersonate + verify-on (Task 4), targets (Task 5), async curl_multi for the proxy (Task 6), errors/timeout (Task 4's `_setopt_defaults` + the `{error,code}` return), POD/ceiling (Task 7). All spec sections map to a task.
- **The linchpin is isolated up front:** Task 1 is a pure fail-fast gate -- if curl-impersonate does not build userspace/no-sudo, or the JA4 does not change, everything stops there before any binding is written.
- **Type consistency:** `ci_t`/`ci_request`/`ci_impersonate`/`_setopt_defaults` names are used identically across tasks; the response hashref shape (`{status,headers,body}` / `{error,code}`) is fixed in Task 4 and reused in Task 6; `impersonate($target,$default_headers)` returns a CURLcode int (0 = ok) consistently.
- **Known soft spots to resolve during implementation (flagged, not hidden):** the exact `Alien::Build::MM` boilerplate (Task 2) and the full `curl_multi` socket-action/timer callback bodies (Task 6) are standard libcurl/Alien patterns given in sketch form -- the implementer completes them against the libcurl-multi and Alien::Build::MM references; each has a test that fails loudly until correct. The real target list (Task 5) and the pinned curl-impersonate tag (Tasks 1-2) are confirmed empirically in Task 1.
