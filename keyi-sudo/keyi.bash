# keyi bash completion

_keyi()
{
	local cur prev words cword
	_init_completion || return

	local have_e=false
	local done=false
	local cmd_at=0
	local i
	for (( i = 1; i < cword; i++ )); do
		local w="${words[i]}"
		if [[ $w == -e ]]; then
			have_e=true
		elif [[ $w == -* ]]; then
			done=true
			break
		elif [[ $w == = ]]; then
			(( i++ ))
		elif (( i + 1 < cword )) && [[ ${words[i+1]} == = ]]; then
			(( i += 2 ))
		elif [[ $w != *=* ]]; then
			cmd_at=$i
			break
		fi
	done

	if [[ $done == true ]]; then
		COMPREPLY=()
		return
	fi

	if [[ $have_e == true ]]; then
		if [[ $prev == -e ]]; then
			_filedir
		fi
		return
	fi

	if [[ $prev == = || $cur == = ]]; then
		COMPREPLY=()
		return
	fi

	if (( cmd_at == 0 )); then
		mapfile -t COMPREPLY < <(compgen -c -- "$cur")
		return
	fi

	if (( cword == cmd_at )); then
		mapfile -t COMPREPLY < <(compgen -c -- "$cur")
	else
		_command_offset "$cmd_at"
	fi
}

complete -F _keyi keyi

#
# keyi <TAB>			List $PATH commands
# keyi sys<TAB>			Complete to systemctl, sysctl, etc.
# keyi apt <TAB>		Delegate to apt
# keyi apt ins<TAB>		Delegate to apt, complete install
# keyi KEY=VAL <TAB>		Complete commands (env skipped)
# keyi KEY=VAL apt ins<TAB>	Delegate to apt
# keyi KEY=<TAB>		No completion (inside env value)
# keyi KEY=par<TAB>		No completion (inside env value)
# keyi -e <TAB>			Complete file paths
# keyi -e /etc/ho<TAB>		Complete to /etc/hosts etc.
# keyi -e /etc/hosts <TAB>	No completion (file already given)
# keyi -i <TAB>			No completion
# keyi -h <TAB>			No completion
# keyi -v <TAB>			No completion
# keyi -x <TAB>			No completion (unknown option)

# vim: ts=8

