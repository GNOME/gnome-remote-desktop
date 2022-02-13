#!/usr/bin/env bash

export WLOG_LEVEL=DEBUG
export G_MESSAGES_DEBUG=all
dbus-run-session -- $TEST_SRCDIR/tests/run-rdp-tests.py
