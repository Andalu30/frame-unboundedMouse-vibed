#!/bin/sh
# SPDX-License-Identifier: MIT
# This is NOT an installer. It only prints a message and exits with an error.
# It exists because the README "offers" the popular `curl … | bash` one-liner, and
# piping a stranger's script into your shell is a habit worth breaking.
# The real install steps are in README.md ("Build and install").
#
# Everything is inside main(), which is only called on the last line. A download cut
# short halfway therefore runs nothing. Real installers should do this too.

main() {
    if [ -t 1 ]; then
        bold=$(printf '\033[1m') red=$(printf '\033[31m') green=$(printf '\033[32m') reset=$(printf '\033[0m')
    else
        bold='' red='' green='' reset=''
    fi

    printf '%s\n' "Installing mouselaser..."
    printf '%s\n' "  [1/3] Fetching driver ............ just kidding."
    printf '\n'
    printf '%s\n' "${bold}${red}Stop. Nothing was installed, and nothing on your system was touched.${reset}"
    printf '\n'
    if [ -t 0 ]; then
        printf '%s\n' "You ran this script from a file. Hopefully you read it first. Good."
    else
        printf '%s\n' "You just piped a script from the internet straight into your shell, without reading it."
    fi
    printf '%s\n' "This one only prints text. The next one might not. On a Steam Frame, a script running"
    printf '%s\n' "as your user could:"
    printf '%s\n' "  - read every key you type: the steamos user is in the 'input' group;"
    printf '%s\n' "  - ask for your password with sudo: the steamos user is in the 'wheel' group;"
    printf '%s\n' "  - register a SteamVR driver, which then runs inside vrserver with your whole VR session."
    printf '%s\n' "And with curl | bash:"
    printf '%s\n' "  - you never see what runs, and a server can tell curl from a browser and send it"
    printf '%s\n' "    something different from what you'd read on the web page;"
    printf '%s\n' "  - a connection that drops halfway runs half a script."
    printf '\n'
    printf '%s\n' "${bold}${green}What to do instead${reset}"
    printf '%s\n' "  1. Download first:   curl -fsSLO <url>"
    printf '%s\n' "  2. Read it:          less quick-install.sh"
    printf '%s\n' "  3. Run it only if you understand it."
    printf '\n'
    printf '%s\n' "For this project, the real steps are in the README: clone the repo, read"
    printf '%s\n' "src/driver.cpp and install.sh, build with cmake, then run ./install.sh."
    printf '%s\n' "https://github.com/Andalu30/frame-unboundedMouse-vibed#build-and-install"
    printf '\n'
    printf '%s\n' "${bold}And remember: this project is vibecoded.${reset} As much as I tried to be engineery"
    printf '%s\n' "about it, don't take it on a 'trust me, bro'. Understand what you run before you run it."
    exit 1
}

main "$@"
