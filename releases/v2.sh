#!/bin/sh
# v2: a good update. The canary passes and it rolls out to every instance.
exec ./bin/demo-service --port "$1" --version v2
