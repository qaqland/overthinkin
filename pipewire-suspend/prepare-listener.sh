#!/bin/bash

dbus-monitor --system "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'" |
	while read -r line; do
		if echo "$line" | grep -q "boolean true"; then
			./pipewire-suspend.sh 1
		elif echo "$line" | grep -q "boolean false"; then
			./pipewire-suspend.sh 0
		fi
	done
