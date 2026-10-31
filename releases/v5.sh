#!/bin/sh
# v5: a good update that takes 5 seconds to start. Traffic only reaches an
# instance once it passes its health check, so users never hit it too early.
exec ./bin/demo-service --port "$1" --version v5 --boot-seconds 5
