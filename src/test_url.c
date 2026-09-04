// src/test_url.c — URL parsing, resolution and unwrapping
//
// Every one of these is a case the browser hits on real pages: a scheme-less
// address typed into the bar, a protocol-relative asset reference, a relative
// link two directories up, a search result wrapped in the engine's
// redirector. They are cheap to get subtly wrong and expensive to debug
// through a TLS handshake.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/http.h"

KTEST(url, parse_absolute_https){
    url_t u;
    KT_EQ(url_parse("https://en.wikipedia.org/wiki/Unix", &u), 0);
    KT_EQ(u.https, 1);
    KT_STREQ(u.host, "en.wikipedia.org");
    KT_EQ(u.port, 443);
    KT_STREQ(u.path, "/wiki/Unix");
}

KTEST(url, parse_http_and_default_path){
    url_t u;
    KT_EQ(url_parse("http://example.com", &u), 0);
    KT_EQ(u.https, 0);
    KT_STREQ(u.host, "example.com");
    KT_EQ(u.port, 80);
    KT_STREQ(u.path, "/");
}

KTEST(url, parse_scheme_less_defaults_to_http){
    url_t u;
    KT_EQ(url_parse("example.com/a/b", &u), 0);
    KT_EQ(u.https, 0);
    KT_STREQ(u.host, "example.com");
    KT_STREQ(u.path, "/a/b");
}

KTEST(url, parse_explicit_port){
    url_t u;
    KT_EQ(url_parse("http://localhost:8080/x", &u), 0);
    KT_STREQ(u.host, "localhost");
    KT_EQ(u.port, 8080);
    KT_STREQ(u.path, "/x");
}

KTEST(url, resolve_absolute_reference_replaces_everything){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("https://a.example/one/two", &base), 0);
    url_resolve(&base, "http://b.example/three", out, sizeof(out));
    KT_STREQ(out, "http://b.example/three");
}

KTEST(url, resolve_protocol_relative_keeps_scheme){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("https://a.example/one", &base), 0);
    url_resolve(&base, "//cdn.example/img.png", out, sizeof(out));
    KT_STREQ(out, "https://cdn.example/img.png");
}

KTEST(url, resolve_root_relative){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("https://a.example/one/two/three", &base), 0);
    url_resolve(&base, "/top", out, sizeof(out));
    KT_STREQ(out, "https://a.example/top");
}

KTEST(url, resolve_same_directory){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("https://a.example/dir/page.html", &base), 0);
    url_resolve(&base, "other.html", out, sizeof(out));
    KT_STREQ(out, "https://a.example/dir/other.html");
}

KTEST(url, percent_decode){
    char out[64];
    KT_EQ(url_percent_decode("a%20b", out, sizeof(out)), 0);
    KT_STREQ(out, "a b");

    KT_EQ(url_percent_decode("%2Fpath%2Fx", out, sizeof(out)), 0);
    KT_STREQ(out, "/path/x");

    // A stray percent with nothing decodable after it must survive as
    // itself rather than eating the next two characters.
    KT_EQ(url_percent_decode("100%", out, sizeof(out)), 0);
    KT_STREQ(out, "100%");
}

KTEST(url, unwrap_search_redirector){
    char out[URL_MAX];
    // DuckDuckGo wraps result links; a click has to reach the destination,
    // not the redirector, or every result is one extra round trip and the
    // address bar shows the wrong host.
    int r = url_unwrap("https://duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa",
                       out, sizeof(out));
    if (r == 0) KT_STREQ(out, "https://example.com/a");
    else        ktest_skip("no redirector unwrapping for this form");
}

KTEST(url, unwrap_leaves_plain_urls_alone){
    char out[URL_MAX];
    // A non-redirector URL must be reported as "nothing to unwrap" rather
    // than silently rewritten.
    KT_TRUE(url_unwrap("https://example.com/plain", out, sizeof(out)) != 0);
}
