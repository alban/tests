#!/bin/sh
# Send 8 concurrent requests, each with its own W3C traceparent.
for i in $(seq 8); do
  curl -s -H "traceparent: 00-$(openssl rand -hex 16)-$(openssl rand -hex 8)-01" \
    "http://localhost:8000/?x=$i" &
done
wait
echo
