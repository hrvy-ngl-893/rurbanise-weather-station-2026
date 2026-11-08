#pragma once
// Admin authentication: salted PBKDF2-HMAC-SHA256 in NVS, random cookie sessions,
// login lockout. The password itself is never stored or sent back.
#include <ESPAsyncWebServer.h>
#include <mbedtls/md.h>
#include "state.h"

struct Sess {
  char tok[33];
  uint32_t exp;
};
static Sess sess[4];
static uint8_t failCount = 0;
static uint32_t lockUntil = 0;
static uint32_t setupOpenUntil = 0;

static void hmac256(const uint8_t* key, size_t kl, const uint8_t* msg, size_t ml, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, kl, msg, ml, out);
}

static void pbkdf2(const char* pw, const uint8_t* salt, size_t sl, uint32_t iter, uint8_t out[32]) {
  uint8_t s[24];
  memcpy(s, salt, sl);           // sl <= 20
  s[sl] = 0; s[sl + 1] = 0; s[sl + 2] = 0; s[sl + 3] = 1;
  uint8_t u[32], t[32];
  size_t pl = strlen(pw);
  hmac256((const uint8_t*)pw, pl, s, sl + 4, u);
  memcpy(t, u, 32);
  for (uint32_t i = 1; i < iter; i++) {
    hmac256((const uint8_t*)pw, pl, u, 32, u);
    for (int j = 0; j < 32; j++) t[j] ^= u[j];
  }
  memcpy(out, t, 32);
}

static bool authHasPassword() {
  Preferences p;
  p.begin("auth", false);
  bool h = p.isKey("hash");
  p.end();
  return h;
}

static void authSetPassword(const String& pw) {
  uint8_t salt[16], h[32];
  esp_fill_random(salt, sizeof salt);
  pbkdf2(pw.c_str(), salt, sizeof salt, PBKDF2_ITER, h);
  Preferences p;
  p.begin("auth", false);
  p.putBytes("salt", salt, sizeof salt);
  p.putBytes("hash", h, sizeof h);
  p.end();
}

static bool authVerify(const String& pw) {
  uint8_t salt[16], stored[32], h[32];
  Preferences p;
  p.begin("auth", false);
  size_t a = p.getBytes("salt", salt, sizeof salt);
  size_t b = p.getBytes("hash", stored, sizeof stored);
  p.end();
  if (a != sizeof salt || b != sizeof stored) return false;
  pbkdf2(pw.c_str(), salt, sizeof salt, PBKDF2_ITER, h);
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= h[i] ^ stored[i];
  return diff == 0;
}

static void authClearAll() {
  Preferences p;
  p.begin("auth", false);
  p.remove("hash");
  p.remove("salt");
  p.end();
  memset(sess, 0, sizeof sess);
}

static bool setupOpen() {
  if (!authHasPassword()) return true;
  return setupOpenUntil && (int32_t)(setupOpenUntil - millis()) > 0;
}

static bool authLocked() { return lockUntil && (int32_t)(lockUntil - millis()) > 0; }

// Returns 0 = ok (token filled), 1 = bad password, 2 = locked
static int authLogin(const String& pw, String& tokOut) {
  if (authLocked()) return 2;
  if (!authVerify(pw)) {
    if (++failCount >= 5) { failCount = 0; lockUntil = millis() + 300000UL; }
    return 1;
  }
  failCount = 0;
  uint8_t r[16];
  esp_fill_random(r, sizeof r);
  char tok[33];
  for (int i = 0; i < 16; i++) sprintf(tok + i * 2, "%02x", r[i]);
  int slot = 0;
  for (int i = 0; i < 4; i++) {
    if (!sess[i].tok[0] || (int32_t)(millis() - sess[i].exp) > 0) { slot = i; break; }
    if ((int32_t)(sess[i].exp - sess[slot].exp) < 0) slot = i;
  }
  strcpy(sess[slot].tok, tok);
  sess[slot].exp = millis() + SESSION_TOKEN_TTL_MS;
  tokOut = tok;
  return 0;
}

static String cookieSid(AsyncWebServerRequest* r) {
  if (!r->hasHeader("Cookie")) return "";
  String c = r->header("Cookie");
  int i = c.indexOf("sid=");
  if (i < 0) return "";
  i += 4;
  int e = c.indexOf(';', i);
  return e < 0 ? c.substring(i) : c.substring(i, e);
}

static bool authCheck(AsyncWebServerRequest* r) {
  String s = cookieSid(r);
  if (s.length() != 32) return false;
  for (int i = 0; i < 4; i++) {
    if (sess[i].tok[0] && s == sess[i].tok) {
      if ((int32_t)(millis() - sess[i].exp) > 0) { sess[i].tok[0] = 0; return false; }
      sess[i].exp = millis() + SESSION_TOKEN_TTL_MS;
      return true;
    }
  }
  return false;
}

static void authLogout(AsyncWebServerRequest* r) {
  String s = cookieSid(r);
  for (int i = 0; i < 4; i++)
    if (sess[i].tok[0] && s == sess[i].tok) sess[i].tok[0] = 0;
}
