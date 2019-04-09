#!/usr/bin/bash

dbus-run-session -- xvfb-run -s '+iglx -noreset' $TEST_SRCDIR/tests/run-vnc-tests.py
