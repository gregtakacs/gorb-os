#!/usr/bin/env python3
"""signing_probe.py — prove the filed signing values work, without a release run.

    python3 tools/signing_probe.py <SIGNING_SECRETS_DIR>

Reads the files signing_secrets.sh keeps (one per secret, named as the secret) and asks
each vendor directly, so a wrong account name, profile, password or key shows up in
seconds instead of as a failed job half an hour into a release:

  Azure  a token for gorb-os-ci, then one test signature of a throwaway digest from the
         named account and profile (Azure answers a wrong account or profile, or a missing
         Certificate Profile Signer role, with a bare 403)
  Apple  the .p12 opens with the password and holds the identity's certificate, still in
         date; the notary API key is accepted (a list of past submissions)

No value is printed. Signing a digest of the bytes "orb-signing-probe" signs nothing
anyone can use. Exit 0 when every probe that has its values passes; 1 otherwise. Needs
the cryptography package for the Apple half (pip install cryptography).
"""
import base64
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

AZURE = ("AZURE_TENANT_ID", "AZURE_CLIENT_ID", "AZURE_CLIENT_SECRET",
         "AZURE_SIGNING_ENDPOINT", "AZURE_SIGNING_ACCOUNT", "AZURE_CERT_PROFILE")
APPLE = ("MACOS_CERT_P12", "MACOS_CERT_PASSWORD", "MACOS_SIGN_IDENTITY",
         "APPLE_API_KEY_P8", "APPLE_API_KEY_ID", "APPLE_API_ISSUER")


def load(d, names):
    out = {}
    for n in names:
        p = os.path.join(d, n)
        if not os.path.isfile(p) or os.path.getsize(p) == 0:
            return None, n
        with open(p) as f:
            out[n] = f.read().strip()
    return out, None


def http(req):
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def azure(v):
    form = urllib.parse.urlencode({
        "grant_type": "client_credentials", "client_id": v["AZURE_CLIENT_ID"],
        "client_secret": v["AZURE_CLIENT_SECRET"],
        "scope": "https://codesigning.azure.net/.default"}).encode()
    code, body = http(urllib.request.Request(
        f"https://login.microsoftonline.com/{v['AZURE_TENANT_ID']}/oauth2/v2.0/token", form))
    if code != 200:
        err = json.loads(body or b"{}").get("error", "?")
        return f"no token for the app ({code} {err}): check AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET (expired?)"
    tok = json.loads(body)["access_token"]
    digest = base64.b64encode(hashlib.sha256(b"orb-signing-probe").digest()).decode()
    url = (f"{v['AZURE_SIGNING_ENDPOINT'].rstrip('/')}/codesigningaccounts/{v['AZURE_SIGNING_ACCOUNT']}"
           f"/certificateprofiles/{v['AZURE_CERT_PROFILE']}:sign?api-version=2023-06-15-preview")
    code, _ = http(urllib.request.Request(
        url, json.dumps({"signatureAlgorithm": "RS256", "digest": digest}).encode(),
        {"Authorization": "Bearer " + tok, "Content-Type": "application/json"}))
    if code in (200, 202):
        return None
    if code == 403:
        return ("the sign request was refused (403): check AZURE_SIGNING_ACCOUNT and AZURE_CERT_PROFILE "
                "against the portal, and that gorb-os-ci holds Artifact Signing Certificate Profile Signer")
    if code == 404:
        return "no such account or profile at that endpoint (404): check AZURE_SIGNING_ENDPOINT's region"
    return f"the sign request answered {code}"


def apple(v):
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
        from cryptography.hazmat.primitives.serialization import pkcs12
    except ImportError:
        return "needs the cryptography package (pip install cryptography)"
    try:
        key, cert, _ = pkcs12.load_key_and_certificates(
            base64.b64decode(v["MACOS_CERT_P12"]), v["MACOS_CERT_PASSWORD"].encode())
    except Exception:
        return "MACOS_CERT_P12 does not open with MACOS_CERT_PASSWORD"
    if key is None or cert is None:
        return "MACOS_CERT_P12 holds no key and certificate pair"
    cn = cert.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)
    if not cn or cn[0].value != v["MACOS_SIGN_IDENTITY"]:
        return "MACOS_SIGN_IDENTITY is not the .p12 certificate's name (codesign would find no identity)"
    left = (cert.not_valid_after_utc.timestamp() - time.time()) / 86400
    if left < 0:
        return "the Developer ID certificate has expired: renew it (RELEASING.md)"
    try:
        ak = serialization.load_pem_private_key(base64.b64decode(v["APPLE_API_KEY_P8"]), None)
    except Exception:
        return "APPLE_API_KEY_P8 is not a readable .p8 key"
    b64 = lambda x: base64.urlsafe_b64encode(x).rstrip(b"=")
    now = int(time.time())
    head = b64(json.dumps({"alg": "ES256", "kid": v["APPLE_API_KEY_ID"], "typ": "JWT"}).encode())
    claims = b64(json.dumps({"iss": v["APPLE_API_ISSUER"], "iat": now, "exp": now + 600,
                             "aud": "appstoreconnect-v1"}).encode())
    r, s = decode_dss_signature(ak.sign(head + b"." + claims, ec.ECDSA(hashes.SHA256())))
    jwt = (head + b"." + claims + b"." + b64(r.to_bytes(32, "big") + s.to_bytes(32, "big"))).decode()
    code, _ = http(urllib.request.Request(
        "https://appstoreconnect.apple.com/notary/v2/submissions?limit=1",
        headers={"Authorization": "Bearer " + jwt}))
    if code == 401:
        return "Apple refused the notary key (401): check APPLE_API_KEY_ID, APPLE_API_ISSUER and the .p8"
    if code != 200:
        return f"Apple's notary API answered {code}"
    if left < 30:
        print(f"  note     the Developer ID certificate expires in {int(left)} days")
    return None


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit("usage: signing_probe.py <SIGNING_SECRETS_DIR>")
    d, bad = sys.argv[1], 0
    for name, names, probe in (("Windows (Azure)", AZURE, azure), ("macOS (Apple)", APPLE, apple)):
        v, missing = load(d, names)
        if v is None:
            print(f"  skip     {name}: {missing} is not filed")
            continue
        err = probe(v)
        if err:
            print(f"  FAIL     {name}: {err}")
            bad = 1
        else:
            print(f"  works    {name}")
    sys.exit(bad)


if __name__ == "__main__":
    main()
