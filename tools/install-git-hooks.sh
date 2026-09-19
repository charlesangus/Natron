#!/bin/bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Use git rev-parse --git-path so linked worktrees resolve to the shared hooks dir.
# Link only pre-commit, not core.hooksPath, because post-commit rewrites the tracked
# Global/GitVersion.h after every commit, which would dirty the tree.
HOOKS_DIR="$(git rev-parse --git-path hooks)"

HOOK_FILE="$HOOKS_DIR/pre-commit"
HOOK_TARGET="../../.git-hooks/pre-commit"
PIN_VERSION="21.1.8"
PIN_INSTALL_CMD="pip install --user --break-system-packages clang-format==$PIN_VERSION"

# Mirrors the hook's own module-first, PATH-fallback resolution so this
# check can't pass on a clang-format the hook would then reject.
check_clang_format() {
	local cf_binary cf_version
	cf_binary="$(python3 -c 'import clang_format, os; print(os.path.join(os.path.dirname(clang_format.__file__), "data", "bin", "clang-format"))' 2>/dev/null)"
	if [ -z "$cf_binary" ] || [ ! -x "$cf_binary" ]; then
		cf_binary="$(command -v clang-format)"
	fi
	if [ -z "$cf_binary" ] || [ ! -x "$cf_binary" ]; then
		return 1
	fi
	cf_version="$("$cf_binary" --version 2>/dev/null)"
	case "$cf_version" in
	*" $PIN_VERSION"*) ;;
	*) return 1 ;;
	esac
	command -v git-clang-format >/dev/null 2>&1
}

uninstall_hook() {
	if [ ! -e "$HOOK_FILE" ] && [ ! -L "$HOOK_FILE" ]; then
		echo "Hook is not installed."
		exit 0
	fi

	if [ ! -L "$HOOK_FILE" ]; then
		echo "Error: $HOOK_FILE exists but is not a symlink."
		exit 1
	fi

	CURRENT_TARGET="$(readlink "$HOOK_FILE")"
	if [ "$CURRENT_TARGET" != "$HOOK_TARGET" ]; then
		echo "Error: $HOOK_FILE points to $CURRENT_TARGET, not $HOOK_TARGET. Not removing."
		exit 1
	fi

	rm "$HOOK_FILE"
	echo "Uninstalled git hook."
	exit 0
}

if [ "${1:-}" = "--uninstall" ]; then
	uninstall_hook
fi

if [ -L "$HOOK_FILE" ]; then
	CURRENT_TARGET="$(readlink "$HOOK_FILE")"
	if [ "$CURRENT_TARGET" = "$HOOK_TARGET" ]; then
		echo "Git hook is already installed."
		if ! check_clang_format; then
			echo "The pinned clang-format the hook needs is not installed; install it with:"
			echo "$PIN_INSTALL_CMD"
		fi
		exit 0
	fi
	echo "Error: $HOOK_FILE is a symlink pointing to $CURRENT_TARGET, not $HOOK_TARGET."
	exit 1
fi

if [ -e "$HOOK_FILE" ]; then
	echo "Error: $HOOK_FILE exists but is not a symlink."
	exit 1
fi

mkdir -p "$HOOKS_DIR"
ln -s "$HOOK_TARGET" "$HOOK_FILE"
echo "Installed git hook."

if ! check_clang_format; then
	echo "The pinned clang-format the hook needs is not installed; install it with:"
	echo "$PIN_INSTALL_CMD"
fi
