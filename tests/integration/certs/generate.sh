#!/bin/sh
set -e
cd "$(dirname "$0")"
openssl req -x509 -nodes -newkey rsa:2048 \
  -keyout localhost.key -out localhost.crt \
  -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
