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
