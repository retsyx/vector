#!/bin/sh
# Name: Vector Shell
#
# Runs shell scripts that a USB host drops into vector-shell/in on user storage.
# The host uploads in/NAME.sh, placeholder files out/NAME.out and out/NAME.rc,
# then in/NAME.go. The server runs NAME.sh once, writes its output over
# out/NAME.out, then writes "STATUS LENGTH" over out/NAME.rc. It writes into the
# host's files in place and leaves deletion to the host.
#
# While USB is connected, /mnt/us (an fsp FUSE view) blocks, so the server works
# in /mnt/base-us, the partition behind it that the MTP server also uses.
# Scripts get the exchange folder as $VECTOR_SHELL and should avoid /mnt/us.
#
# Tapping the scriptlet (re)starts the server in the background. The server
# stops when the host uploads vector-shell/stop, after an hour without commands,
# or at reboot. It keeps the screensaver off while it runs and logs to
# out/server.log.

if [ -d /mnt/base-us/documents ]; then
    root=/mnt/base-us/vector-shell
else
    root=/mnt/us/vector-shell
fi
state=/tmp/vector-shell
server=$state/serve.sh
log=$root/out/server.log

mkdir -p "$root/in" "$root/out" "$state/done" || exit 1

stopped=
for proc in /proc/[0-9]*; do
    case "$(tr '\0' ' ' <"$proc/cmdline" 2>/dev/null)" in
    "sh /tmp/vector-shell"*) kill "${proc#/proc/}" 2>/dev/null && stopped="$stopped ${proc#/proc/}" ;;
    esac
done
if [ -n "$stopped" ]; then
    echo "Stopped the running Vector Shell (pid$stopped)."
    sleep 2
fi

cat >"$server" <<'SERVE'
root=$1
state=/tmp/vector-shell
log=$root/out/server.log
idle_limit=3600
export VECTOR_SHELL=$root

note() { echo "$(date '+%F %T') [$$] $*" >>"$log"; }

finish() {
    lipc-set-prop com.lab126.powerd preventScreenSaver 0
    note "stopped: $1"
}

trap 'note "ignored SIGHUP"' HUP
trap 'finish SIGTERM; exit 143' TERM
trap 'finish SIGINT; exit 130' INT
note "started in $root; $(id)"

rm -f "$root/stop"
lipc-set-prop com.lab126.powerd preventScreenSaver 1 || note "could not hold off the screensaver"

# Commands left from before this start are not run.
for go in "$root"/in/*.go; do
    [ -e "$go" ] || break
    name=${go##*/}
    name=${name%.go}
    touch "$state/done/$name"
    note "skipped stale $name"
done

last=$(date +%s)
beat=0
while [ ! -e "$root/stop" ]; do
    for go in "$root"/in/*.go; do
        [ -e "$go" ] || break
        name=${go##*/}
        name=${name%.go}
        [ -e "$state/done/$name" ] && continue
        touch "$state/done/$name"
        note "run $name"
        (cd / && exec sh "$root/in/$name.sh") </dev/null >"$state/$name.out" 2>&1
        rc=$?
        length=$(wc -c <"$state/$name.out")
        dd if="$state/$name.out" of="$root/out/$name.out" conv=notrunc 2>/dev/null
        rm -f "$state/$name.out"
        printf '%s %s\n' "$rc" "$length" | dd of="$root/out/$name.rc" conv=notrunc 2>/dev/null
        note "done $name: status $rc, $length bytes"
        last=$(date +%s)
    done
    now=$(date +%s)
    if [ $((now - last)) -ge "$idle_limit" ]; then
        finish "idle for an hour"
        exit 0
    fi
    if [ $((now - beat)) -ge 60 ]; then
        beat=$now
        note "alive; power $(lipc-get-prop com.lab126.powerd state 2>&1)"
    fi
    sleep 1
done
rm -f "$root/stop"
finish "stop file"
SERVE

echo "$(date '+%F %T') scriptlet run by: $(tr '\0' ' ' <"/proc/$PPID/cmdline")" >>"$log"
# Detach so the server does not depend on the scriptlet launcher app staying loaded.
setsid sh "$server" "$root" </dev/null >>"$log" 2>&1 &
echo "Vector Shell started."
