#!/bin/bash
# Copyright 2020 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Usage: ./util/ide-config.sh vscode all all:RO | tee .vscode/c_cpp_properties.json
# This tool needs to be run from the base ec directory.
#
# Future works should be put towards adding new IDE generators and adding
# mechanism for passing IDE specific parameters to the IDE generator.

DEFAULT_IMAGE=RW
INDENT_WIDTH=${INDENT_WIDTH:-1}
INDENT_CHAR=${INDENT_CHAR:-$'\t'}
INCLUDES_DROP_ROOT=${INCLUDES_DROP_ROOT:-false}
PARALLEL_CACHE_FILL=${PARALLEL_CACHE_FILL:-true}
FORCE_INCLUDE_CONFIG_H=${FORCE_INCLUDE_CONFIG_H:-false}
# JOB_BATCH_SIZE is the number of jobs allowed to spawn at any given point.
# This is an inefficient manner of resource management, but at least it
# throttles process creation. Due to the low utilization or each job,
# multiply the number of real processors by 2.
JOB_BATCH_SIZE=${JOB_BATCH_SIZE:-$(($(nproc) * 2))}

MAKE_CACHE_DIR=""

init() {
	MAKE_CACHE_DIR=$(mktemp -t -d ide-config.XXXX)
	mkdir -p "${MAKE_CACHE_DIR}/defines"
	mkdir -p "${MAKE_CACHE_DIR}/includes"
	trap deinit EXIT
}

deinit() {
	rm -rf "${MAKE_CACHE_DIR}"
}

usage() {
	cat <<-HEREDOC
	Usage: ide-config.sh <vscode> [BOARD:IMAGE] [BOARD:IMAGE...]
	Generate a C language configuration for a given IDE and EC board.

	Examples:
	ide-config.sh vscode all:RW all:RO | tee .vscode/c_cpp_properties.json
	ide-config.sh vscode nocturne # implicitly :RW
	ide-config.sh vscode nocturne_fp:RO
	ide-config.sh vscode nocturne:RO hatch:RW
	ide-config.sh vscode all      # implicitly :RW
	HEREDOC
}

# Usage: iprintf <indent-level> <printf-fmt> [printf-args...]
iprintf() {
	local level=$1
	shift

	local n=$((INDENT_WIDTH*level))
	if [[ $n -ne 0 ]]; then
		eval printf '"${INDENT_CHAR}%.0s"' "{1..$n}"
	fi
	# shellcheck disable=SC2059
	printf "$@"
	return $?
}

# Usage: parse-cfg-board <cfg-string>
#
# Example: parse-cfg-board nocturne:RW
parse-cfg-board() {
	local cfg=$1
	# Remove possible :RW or :RO
	local board=${cfg%%:*}
	if [[ -z ${board} ]]; then
		return 1
	fi
	echo "${board}"
}

# Usage: parse-cfg-image <cfg-string>
#
# Example: parse-cfg-image nocturne:RW
# Example: parse-cfg-image nocturne
parse-cfg-image() {
	local cfg=$1

	local board
	if ! board=$(parse-cfg-board "${cfg}"); then
		return 1
	fi
	# Remove known board part
	cfg=${cfg#${board}}
	cfg=${cfg#":"}
	# Use default image if none set
	cfg=${cfg:-${DEFAULT_IMAGE}}

	case ${cfg} in
		RW|RO)
			echo "${cfg}"
			return 0
			;;
		*)
			return 1
			;;
	esac
}

# Usage: make-defines <board> <RO|RW>
make-defines() {
	local board=$1
	local image=$2

	local cache="${MAKE_CACHE_DIR}/defines/${board}-${image}"

	if [[ ! -f "${cache}" ]]; then
		make print-defines BOARD="${board}" BLD="${image}" >"${cache}"
	fi

	cat "${cache}"
}

# Usage: make-includes <board> <RO|RW>
#
# Rerun a newline separated list of include directories relative to the ec's
# root directory.
make-includes() {
	local board=$1
	local image=$2

	local cache="${MAKE_CACHE_DIR}/includes/${board}-${image}"

	if [[ ! -f "${cache}" ]]; then
		make print-includes BOARD="${board}" BLD="${image}" \
		| xargs realpath --relative-to=. \
		| {
			if [[ "${INCLUDES_DROP_ROOT}" == true ]]; then
				grep -v "^\.$"
			else
				cat
			fi
		  } >"${cache}"
	fi

	cat "${cache}"
}

# Usage: make-boards
make-boards() {
	local cache="${MAKE_CACHE_DIR}/boards"

	if [[ ! -f "${cache}" ]]; then
		make print-boards >"${cache}"
	fi

	cat "${cache}"
}

# Usage: <newline-list> |  join <left> <right> <separator>
#
# JSON: includes nocturne_fp RW | join '"' '"' ',\n'
# C:    includes nocturne_fp RW | join '"' '",' '\n'
join() {
	local left=$1
	local right=$2
	local sep=$3

	local first=true
	while read -r line; do
		# JSON is ridiculous for not allowing a trailing ,
		if [[ "${first}" == true ]]; then
			first=false
		else
			printf "%b" "${sep}"
		fi
		printf "%b%s%b" "${left}" "${line}" "${right}"
	done
	echo
}

# Usage: <content> | encap <header> <footer> [indent]
#
# Encapsulate the content from stdin with a header, footer, and indentation.
encap() {
	local header=$1
	local footer=$2
	local indent=${3:-0}

	iprintf "${indent}" "%b" "${header}"
	while IFS="" read -r line; do
		iprintf $((1+indent)) "%s\n" "${line}"
	done
	iprintf "${indent}" "%b" "${footer}"
}

##########################################################################

# Usage: vscode [cfg...]
#
# Generate the content for one c_cpp_properties.json that contains
# multiple selectable configurations for each board-image pair.
# In VSCode you can select the config in the bottom right, next to the
# "Select Language Mode". You will only see this option when a C/C++ file
# is open. Additionally, you can select a configuration by pressing
# Ctrl-Shift-P and selecting the "C/C++ Select a Configuration..." option.
vscode() {
	local first=true
	{
		for cfg; do
			local board image
			if ! board=$(parse-cfg-board "${cfg}"); then
				echo "Failed to parse board from cfg '${cfg}'"
				return 1
			fi
			if ! image=$(parse-cfg-image "${cfg}"); then
				echo "Failed to parse image from cfg '${cfg}'"
				return 1
			fi
			{
				printf '"name": "%s",\n' "${board}:${image}"
				make-includes "${board}" "${image}" \
					| join '"' '"' ',\n' \
					| encap '"includePath": [\n' '],\n'
				make-defines "${board}" "${image}" \
					| join '"' '"' ',\n' \
					| encap '"defines": [\n' '],\n'

				if [[ "${FORCE_INCLUDE_CONFIG_H}" == true ]]; then
					echo '"include/config.h"' \
						| encap '"forcedInclude": [\n' '],\n'
				fi

				echo '"compilerPath": "/usr/bin/arm-none-eabi-gcc",'
				# echo '"compilerArgs": [],'
				# The macro __STDC_VERSION__ is 201710L,
				# which is c18. The closest is c11.
				echo '"cStandard": "c11",'
				# echo '"cppStandard": "c++17",'
				echo '"intelliSenseMode": "gcc-x64"'
			} | {
				# A single named configuration
				if [[ "${first}" == true ]]; then
					encap '{\n' '}'
				else
					encap ',\n{\n' '}'
				fi
			}
			first=false
		done
		echo
	} \
	| {
		encap '"configurations": [\n' '],\n'
		echo '"version": 4'
	} \
	| encap '{\n' '}\n'
}

# Usage: main <ide> [cfgs...]
main() {
	# Disaply help if no args
	if [[ $# -lt 1 ]]; then
		usage
		exit 1
	fi
	# Search for help flag
	for flag; do
		case ${flag} in
			help|--help|-h)
				usage
				exit 0
				;;
		esac
	done

	local ide=$1
	shift

	# Expand possible "all" cfgs
	local board image
	local -a cfgs=( )
	for cfg; do
		# We parse both board and image to pre-sanatize the input
		if ! board=$(parse-cfg-board "${cfg}"); then
			echo "Failed to parse board from cfg '${cfg}'" >&2
			exit 1
		fi
		if ! image=$(parse-cfg-image "${cfg}"); then
			echo "Failed to parse image from cfg '${cfg}'" >&2
			exit 1
		fi
		# Note "all:*" could be specified multiple times for RO and RW
		# Note "all:*" could be specified with other specific board:images
		if [[ "${board}" == all ]]; then
			local -a allboards=( )
			mapfile -t allboards < <(make-boards)
			cfgs+=( "${allboards[@]/%/:${image}}" )
		else
			cfgs+=( "${cfg}" )
		fi
	done

	# Technically, we have not sanitized the cfgs generated from
	# "all" expression.

	# Make configs unique (and sorted)
	mapfile -t cfgs < <(echo "${cfgs[@]}" | tr ' ' '\n' | sort -u)
	echo "# Generating a config for the following board:images: " >&2
	echo "${cfgs[@]}" | tr ' ' '\n' | sort -u | column >&2

	# Prefill the make cache in parallel
	# This is important because each make request take about 700ms.
	# When running on all boards, this could result in (127*2)*700ms = 3mins.
	if [[ "${PARALLEL_CACHE_FILL}" == true ]]; then
		echo "# Fetching make defines and includes. Please wait." >&2

		# We run into process limits if we launch all processes at the
		# same time, so we must split them in half.
		# This need some jobserver management.

		# Run make for RWs and ROs
		local -i jobs=0
		for cfg in "${cfgs[@]}"; do
			if ! board="$(parse-cfg-board "${cfg}")"; then
				echo "Failed to parse board from cfg '${cfg}'" >&2
				exit 1
			fi
			if ! image="$(parse-cfg-image "${cfg}")"; then
				echo "Failed to parse image from cfg '${cfg}'" >&2
				exit 1
			fi
			make-defines "${board}" "${image}" >/dev/null &
			((++jobs % JOB_BATCH_SIZE == 0)) && wait
			make-includes "${board}" "${image}" >/dev/null &
			((++jobs % JOB_BATCH_SIZE == 0)) && wait
		done
		wait
	fi

	# Run the IDE's generator
	case "${ide}" in
		vscode)
			vscode "${cfgs[@]}" || exit $?
			;;
		*)
			echo "Error - IDE '${ide}' is an unsupported." >&2
			exit 1
			;;
	esac
}

init

# Only start if not being sourced
if [[ "$0" == "${BASH_SOURCE[0]}" ]]; then
	main "$@"
fi
