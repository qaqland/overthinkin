#!/bin/bash

export LC_ALL=C

usage() {
	echo "usage: pipewire-suspend {1|0}"
}

SINK_ID=$(wpctl inspect @DEFAULT_AUDIO_SINK@ | head -n1 | awk '{print $2}' | tr -d ',')

if [[ $# -ne 1 ]]; then
	usage >&2
	exit 1
fi

action="$1"

case "$action" in
1)
	command_name="Pause"
	;;
0)
	command_name="Start"
	;;
*)
	usage >&2
	exit 1
	;;
esac

pw-cli send-command "$SINK_ID" "$command_name" '{}'
