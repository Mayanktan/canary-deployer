#!/bin/sh
# v3: a buggy update. 40% of real requests fail with HTTP 500, but /health
# still says OK, so health checks alone would never catch it. Watching the
# canary's real traffic does, and the deployment is rolled back.
exec ./bin/demo-service --port "$1" --version v3 --error-rate 40
