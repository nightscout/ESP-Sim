/* The captive-portal request decision, as a pure function: no ESP-IDF, no
 * Arduino, no Wi-Fi. main/captive.c asks it what a request should get and the
 * host test in test/host/ calls the same function.
 *
 * The board's own access point has no internet, so every HTTP probe a phone
 * makes on joining has to be answered the way a captive portal is expected
 * to answer it; otherwise the phone either gives up or never shows the sheet.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PortalAction {
    PORTAL_SERVE_SETUP,            /* the Wi-Fi sheet at /setup */
    PORTAL_REDIRECT_SETUP,         /* / in AP mode before the sheet has been dismissed */
    PORTAL_PROBE_APPLE_HTML,       /* /hotspot-detect.html before dismissal: the sheet */
    PORTAL_PROBE_APPLE_SUCCESS,    /* Apple probes after dismissal: "Success" */
    PORTAL_PROBE_ANDROID_REDIRECT, /* /generate_204, /gen_204 before dismissal: 302 to /setup */
    PORTAL_PROBE_ANDROID_204,      /* Android probes after dismissal: 204, no body */
    PORTAL_PROBE_CONNECTTEST,      /* /connecttest.txt after dismissal: "Microsoft Connect Test" */
    PORTAL_PROBE_NCSI,             /* /ncsi.txt after dismissal: "Microsoft NCSI" */
    PORTAL_PROBE_WINDOWS_REDIRECT, /* Windows probes before dismissal: 302 to /setup */
    PORTAL_PROBE_FWLINK,           /* /fwlink/?...: 302 to the sheet */
    PORTAL_SERVE_APP,              /* anything else: the normal board pages */
} PortalAction;

/* path is the raw request path (a query string is ignored). method is the
 * HTTP method, matched case-insensitively; probes and the sheet are GET only,
 * so HEAD of a probe is an ordinary app request. wifiSaved is 1 when the board
 * has a saved network it is actually serving on; setupCookie is 1 when the
 * `setup=done` cookie is present. Flags are ints so the C test can call this
 * directly. */
PortalAction portalDecide(const char *path, const char *method, int wifiSaved, int setupCookie);

/* A stable name, for logs and test failure messages. */
const char *portalActionName(PortalAction action);

#ifdef __cplusplus
}
#endif
