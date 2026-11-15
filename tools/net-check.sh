#!/usr/bin/bash
echo -n "IIO "
/usr/bin/cat /sys/bus/iio/devices/iio:device0/name
echo
echo -n "BT "
/usr/bin/ls /sys/class/bluetooth
echo
if /usr/bin/timeout 4 /usr/bin/bash -c 'echo >/dev/tcp/1.1.1.1/443'; then
    echo TCP_OK
else
    echo TCP_FAIL
fi
