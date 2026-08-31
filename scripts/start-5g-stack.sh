#!/usr/bin/env bash
# start-5g-stack.sh — deprecated wrapper → native (no Docker)
exec "$(dirname "$0")/start-5g-native.sh" "$@"
