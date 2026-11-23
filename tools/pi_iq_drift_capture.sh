#!/bin/bash
# ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - registra la frequenza della portante con la Pluto, senza SPECTRUM HD.
# Sul Pi: bash pi_iq_drift_capture.sh SECONDI [LO_HZ]  -> /tmp/pd4pdx_exp_c5iq/<epoch>.bin (65536 campioni IQ int16 a 3 MS/s)
# Solo ricezione. Alla fine rimette la Pluto come l'ha trovata (LO, campionamento, banda).
U=${PLUTO_URI:-ip:192.168.2.1}        # indirizzo della Pluto (predefinito di fabbrica; altrimenti PLUTO_URI=ip:x.x.x.x)
SEC=${1:-330}
LO=${2:-5749900000}
D=/tmp/pd4pdx_exp_c5iq
mkdir -p "$D" && rm -f "$D"/*.bin
old_lo=$(iio_attr -u $U -c ad9361-phy altvoltage0 frequency)
old_sr=$(iio_attr -u $U -i -c ad9361-phy voltage0 sampling_frequency)
old_bw=$(iio_attr -u $U -i -c ad9361-phy voltage0 rf_bandwidth)
echo "prima: LO $old_lo SR $old_sr BW $old_bw"
iio_attr -u $U -c ad9361-phy altvoltage0 frequency "$LO" >/dev/null
iio_attr -u $U -i -c ad9361-phy voltage0 sampling_frequency 3000000 >/dev/null
iio_attr -u $U -i -c ad9361-phy voltage0 rf_bandwidth 1000000 >/dev/null
echo "LO $(iio_attr -u $U -c ad9361-phy altvoltage0 frequency) SR $(iio_attr -u $U -i -c ad9361-phy voltage0 sampling_frequency)"
end=$(( $(date +%s) + SEC ))
n=0
while [ "$(date +%s)" -lt "$end" ]; do
    t=$(date +%s.%N)
    iio_readdev -u $U -b 65536 -s 65536 cf-ad9361-lpc voltage0 voltage1 > "$D/${t:0:14}.bin" 2>/dev/null
    n=$((n + 1))
    sleep 0.7
done
iio_attr -u $U -c ad9361-phy altvoltage0 frequency "$old_lo" >/dev/null
iio_attr -u $U -i -c ad9361-phy voltage0 sampling_frequency "$old_sr" >/dev/null
iio_attr -u $U -i -c ad9361-phy voltage0 rf_bandwidth "$old_bw" >/dev/null
echo "fine: $n catture, Pluto rimessa a LO $(iio_attr -u $U -c ad9361-phy altvoltage0 frequency)"
