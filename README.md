# ft8mon
Demodulate the FT8 WSJT-X protocol of Taylor and Franke.
Input from a sound card via portaudio.
Written in C++, using FFTW.
Runs on Linux, MacOS, and FreeBSD.

To compile:

```
  make
```

To get a list of sound card numbers:

```
  ./ft8mon -list
```

To listen to sound card X, left channel:

```
  ./ft8mon -card X 0
```

You should see output like this:

```
094445  -6 -0.7 1083 JA2KGH CM5VVC EL92
094445 -15  0.1 2584 FG5GH   N4EFS R-07
094445 -10  0.3  601 WA4PT  VK5MRD RRR 
094445 -19  0.0  820 CQ   DX WD6DBM CM97
094445 -15 -0.0 2398 CQ VK3HGQ QF33
094445 -15 -0.4 1227 AB1HL  NA7K   -10
```

The columns are HHMMSS, SNR, time offset, audio frequency, and the message.

To read input from a recorded WAV file:

```
  ./ft8mon -file xxx.wav
```

To read a live WAV stream from a TCP connection, ft8mon can act as a
server. It creates a listening socket on the given address and port,
accepts one connection, reads the WAV header to learn the sample rate,
and then decodes the streamed audio as it arrives:

```
  ./ft8mon -listen 0.0.0.0:8073
```

Use an empty address (e.g. `:8073`) to listen on all interfaces. If the
sender disconnects, ft8mon keeps listening and accepts the next
connection. Any source that writes a WAV header (8/16/24/32-bit PCM or
32-bit float) followed by streamed samples will work, for example:

```
  sox -d -t wav - | nc some-host 8073   # stream the default input
```

To feed decodes to another tool, add `-json host:port` to any of the
commands above. ft8mon then sends each decode as a JSON object in its
own UDP datagram, in addition to the normal output:

```
  ./ft8mon -listen :8073 -json 127.0.0.1:9000
```

Each datagram looks like:

```
  {"time":"2026-07-21T22:04:30Z","unix":1784671470,"snr":-27,
   "dt":1.86,"freq":2574.1,"correct_bits":128,"msg":"CQ DX AB1HL FN42"}
```

UDP is fire-and-forget, so no receiver needs to be running and a
missed packet just drops one decode. A minimal consumer is
`nc -ul 9000` or a few lines of Python.

For Airspy HF+ Discovery support, install the airspyhf
and liquid dsp libraries, and uncomment the relevant lines in the
Makefile. For RFspace SDR-IP, NetSDR, CloudIQ, and CloudSDR
support, install liquid dsp and edit the Makefile. Similarly
for the Apache ANAN-7000dle. Then try commands like these:

```
  ./ft8mon -card airspy ,14.074
  ./ft8mon -card hpsdr 192.168.3.100,14.074
  ./ft8mon -card sdrip 192.168.3.100,14.074
```

Robert Morris,
AB1HL
