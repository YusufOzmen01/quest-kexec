#!/usr/bin/env bash
# Open the BusyBox shell on the kexec target.
exec telnet "${QKX_HOST:-10.42.0.2}" 23
