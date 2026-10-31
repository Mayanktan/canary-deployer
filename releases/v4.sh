#!/bin/sh
# v4: an unstable update. Crashes after 30 requests, so the canary dies
# under real traffic and the deployment is rolled back.
exec ./bin/demo-service --port "$1" --version v4 --crash-after 30
