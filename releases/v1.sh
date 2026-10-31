#!/bin/sh
# v1: the first release. Works fine.
# The controller runs this script with the instance's port number as $1.
exec ./bin/demo-service --port "$1" --version v1
