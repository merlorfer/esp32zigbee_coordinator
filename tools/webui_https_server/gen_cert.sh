#!/usr/bin/env bash
# Generates a local CA + a server cert signed by it, for serving the webUI
# over HTTPS from this PC. Chrome's "Install app" (full PWA) check refuses
# to fire on a page served through a *user-bypassed* certificate warning,
# even though the page itself loads fine and APIs like Web Bluetooth still
# work on it -- so a plain self-signed leaf cert is not enough, the CA
# needs to actually be trusted by the phone.
#
# Workflow:
#   1. This script makes certs/ca-cert.pem (the root, install this on the
#      phone as a trusted CA certificate) and certs/{cert,key}.pem (the
#      leaf cert the server actually presents, signed by that CA).
#   2. Once the phone trusts ca-cert.pem, every leaf cert this script signs
#      is automatically trusted too -- no per-device re-approval needed
#      even if you regenerate the leaf cert later (e.g. IP changed).
#
# Re-run this (the whole thing, CA included) only if you want to start
# over. If only the leaf cert needs regenerating (new IP), that's also
# handled by just re-running this script -- the CA is reused if it already
# exists in certs/.
set -e

cd "$(dirname "$0")"
mkdir -p certs

IPS=$(ipconfig 2>/dev/null | grep -oE "IPv4 Address[^:]*: [0-9.]+" | grep -oE "[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" | grep -v "^169\.254\.")

SAN="subjectAltName=DNS:localhost,IP:127.0.0.1"
for ip in $IPS; do
    SAN="${SAN},IP:${ip}"
done
echo "Leaf cert SAN: ${SAN}"

export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL="*"

if [ ! -f certs/ca-cert.pem ]; then
    echo "Generating local CA (one-time)..."
    openssl req -x509 -newkey rsa:4096 -nodes \
        -keyout certs/ca-key.pem -out certs/ca-cert.pem \
        -days 3650 \
        -subj "/CN=esp32-gateway-webui-bridge-CA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"
else
    echo "Reusing existing CA (certs/ca-cert.pem)."
fi

echo "Generating server key + CSR..."
openssl req -newkey rsa:2048 -nodes \
    -keyout certs/key.pem -out certs/server.csr \
    -subj "/CN=esp32-gateway-webui-bridge"

echo "Signing server cert with the local CA..."
echo "${SAN}" > certs/san.ext
openssl x509 -req -in certs/server.csr \
    -CA certs/ca-cert.pem -CAkey certs/ca-key.pem -CAcreateserial \
    -out certs/cert.pem -days 825 \
    -extfile certs/san.ext

rm -f certs/server.csr certs/san.ext

echo
echo "Done."
echo "  certs/ca-cert.pem  -- install this on the phone as a trusted CA cert"
echo "  certs/cert.pem + certs/key.pem -- what the server presents (signed by the CA above)"
