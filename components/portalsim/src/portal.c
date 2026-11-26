/* The captive-portal decision. Portable C: no ESP-IDF and no libc calls.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "portal.h"

/* Small, allocation-free string helpers: keep this file pure so the host test
 * builds it with plain cc. */

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* True when path is exactly want, or want followed by a query/fragment. */
static int path_is(const char *path, const char *want)
{
    for (; *want; path++, want++) {
        if (*path != *want) {
            return 0;
        }
    }
    return *path == 0 || *path == '?' || *path == '#';
}

/* True when path starts with want and want ends on a component boundary. */
static int path_under(const char *path, const char *want)
{
    for (; *want; path++, want++) {
        if (*path != *want) {
            return 0;
        }
    }
    return *path == 0 || *path == '/' || *path == '?' || *path == '#';
}

static int method_is(const char *method, const char *want)
{
    for (; *want; method++, want++) {
        if (lower(*method) != lower(*want)) {
            return 0;
        }
    }
    return *method == 0;
}

PortalAction portalDecide(const char *path, const char *method, int wifiSaved, int setupCookie)
{
    if (!path) {
        path = "";
    }
    const int get = method && method_is(method, "GET");
    /* Once the reader has a network or has dismissed the sheet, the OS probes
     * must report "no portal": that is the same condition as the / redirect. */
    const int noPortal = wifiSaved || setupCookie;

    /* The OS probes only answer GET. A POST or HEAD to one of these paths
     * falls through to the normal app (which will 404 or handle it) instead of
     * being mistaken for a probe. */
    if (get) {
        if (path_is(path, "/hotspot-detect.html")) {
            return noPortal ? PORTAL_PROBE_APPLE_SUCCESS : PORTAL_PROBE_APPLE_HTML;
        }
        if (path_is(path, "/library/test/success.html")) {
            return PORTAL_PROBE_APPLE_SUCCESS;
        }
        if (path_is(path, "/generate_204") || path_is(path, "/gen_204")) {
            return noPortal ? PORTAL_PROBE_ANDROID_204 : PORTAL_PROBE_ANDROID_REDIRECT;
        }
        if (path_is(path, "/connecttest.txt")) {
            return noPortal ? PORTAL_PROBE_CONNECTTEST : PORTAL_PROBE_WINDOWS_REDIRECT;
        }
        if (path_is(path, "/ncsi.txt")) {
            return noPortal ? PORTAL_PROBE_NCSI : PORTAL_PROBE_WINDOWS_REDIRECT;
        }
        if (path_under(path, "/fwlink")) {
            return noPortal ? PORTAL_SERVE_APP : PORTAL_PROBE_FWLINK;
        }

        if (path_is(path, "/setup")) {
            return PORTAL_SERVE_SETUP;
        }
        if (path_is(path, "/")) {
            /* In AP mode the root is the sheet, until the reader has dismissed it. */
            if (!wifiSaved && !setupCookie) {
                return PORTAL_REDIRECT_SETUP;
            }
            return PORTAL_SERVE_APP;
        }
        return PORTAL_SERVE_APP;
    }

    /* A non-GET request to /setup is never redirected: the router only has GET
     * /setup, so it is an ordinary app request too. */
    return PORTAL_SERVE_APP;
}

const char *portalActionName(PortalAction action)
{
    switch (action) {
    case PORTAL_SERVE_SETUP: return "serve_setup";
    case PORTAL_REDIRECT_SETUP: return "redirect_setup";
    case PORTAL_PROBE_APPLE_HTML: return "probe_apple_html";
    case PORTAL_PROBE_APPLE_SUCCESS: return "probe_apple_success";
    case PORTAL_PROBE_ANDROID_REDIRECT: return "probe_android_redirect";
    case PORTAL_PROBE_ANDROID_204: return "probe_android_204";
    case PORTAL_PROBE_CONNECTTEST: return "probe_connecttest";
    case PORTAL_PROBE_NCSI: return "probe_ncsi";
    case PORTAL_PROBE_WINDOWS_REDIRECT: return "probe_windows_redirect";
    case PORTAL_PROBE_FWLINK: return "probe_fwlink";
    case PORTAL_SERVE_APP: return "serve_app";
    }
    return "unknown";
}
