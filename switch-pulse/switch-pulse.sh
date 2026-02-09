#!/bin/bash

PW_SERVICE=(pipewire.socket pipewire.service pipewire-pulse.socket pipewire-pulse.service)
PA_SERVICE=(pulseaudio.socket pulseaudio.service)

systemctl --user daemon-reload

ps aux | grep -E 'pulseaudio|pipewire'

if systemctl --user is-active pipewire.service ; then
	STOP_MASK=("${PW_SERVICE[@]}")
	START_ENABLE=("${PA_SERVICE[@]}")
else
	STOP_MASK=("${PA_SERVICE[@]}")
	START_ENABLE=("${PW_SERVICE[@]}")
fi

for service in "${STOP_MASK[@]}"; do
	systemctl --user stop "$service"
	systemctl --user mask "$service"
done

for service in "${START_ENABLE[@]}"; do
	systemctl --user unmask "$service"
	systemctl --user enable --now "$service"
done

systemctl --user restart org.dde.session.Daemon1.service

ps aux | grep -E 'pulseaudio|pipewire'

