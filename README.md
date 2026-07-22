# ft8mon

A decoder for the FT8 digital mode of the WSJT-X software by Joe Taylor
(K1JT) and Steve Franke (K9AN). It listens continuously, decodes every
15-second FT8 cycle, and prints the received messages. Written in C++,
using FFTW.

This is an extended version of the original **ft8mon by Robert Morris
(AB1HL)**. All of the FT8 signal processing — the FFT front end, the
LDPC and ordered-statistics decoding, and the message unpacking — is his
work. This fork keeps that core unchanged and adds new ways to get audio
*into* ft8mon and to get decodes *out* of it (see below). Full credit for
the decoder goes to Robert Morris; any bugs in the additions are mine.

Runs on Linux, macOS, and FreeBSD.

## What's new in this version

* **Read a live audio stream over TCP** (`-listen`), so ft8mon can run on
  a different machine than the receiver — either as a WAV stream or as a
  raw PCM stream.
* **Emit decodes as JSON over UDP** (`-json`), so another program can
  process the decodes instead of you reading the terminal.
* **Label the JSON output** (`-tag`), so several ft8mon instances (for
  example one per band) can send to the same collector and still be told
  apart.

None of these change the default behaviour: without the new options,
ft8mon works exactly as before.

## Building

```
  make
```

The core needs FFTW and libsndfile, and portaudio for sound-card input.
Optional SDR backends (see the end of this file) need additional
libraries and are enabled by editing the `Makefile`.

## Reading audio

### From a sound card

List the available sound-card numbers:

```
  ./ft8mon -list
```

Then decode from card `X`, left channel:

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

The columns are HHMMSS, SNR, time offset, audio frequency, and the
message.

### From a recorded WAV file

```
  ./ft8mon -file xxx.wav
```

The file should start on an even 15-second boundary.

### From a WAV stream over TCP (`-listen`)

ft8mon can act as a server and take its audio from a network connection.
It creates a listening socket on the given address and port, accepts one
connection, reads the WAV header to learn the sample rate, and then
decodes the streamed audio as it arrives:

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

### From a raw PCM stream over TCP (`-listen … rate`)

If you add a sample rate, the stream is treated as headerless, signed
16-bit little-endian **mono** PCM at that rate, rather than a WAV file.
This suits sources that produce plain PCM, such as an RTL-SDR pipeline,
without having to wrap it in WAV first:

```
  ./ft8mon -listen :8073 12000
```

## Sending decodes out

### As JSON over UDP (`-json`)

Add `-json host:port` to any of the commands above and ft8mon will send
each decode as one JSON object in its own UDP datagram, in addition to
the normal terminal output:

```
  ./ft8mon -listen :8073 -json 127.0.0.1:9000
```

Each datagram looks like:

```
  {"time":"2026-07-21T22:04:30Z","unix":1784671470,"snr":-27,
   "dt":1.86,"freq":2574.1,"correct_bits":128,"msg":"CQ DX AB1HL FN42"}
```

The fields are the decode time (UTC and Unix epoch), signal-to-noise
ratio, time offset, audio frequency in Hz, the number of correct bits,
and the decoded message.

UDP is fire-and-forget, so no receiver needs to be running and a missed
packet just drops one decode. A minimal consumer is `nc -ul 9000` or a
few lines of Python. (Messages whose text begins with `i3=` are decodes
of a message type ft8mon does not format, and are usually false decodes
of noise; they can be filtered out downstream.)

### Labelling the JSON (`-tag`)

When running several ft8mon instances that all send to the same
collector, add one or more `-tag key:value` options to label each
stream. Each tag is added as a `"key":"value"` pair to every JSON
object:

```
  ./ft8mon -listen :8073 -json host:9000 -tag band:40m -tag site:home
```

yields:

```
  {..., "msg":"CQ DX AB1HL FN42", "band":"40m", "site":"home"}
```

Persistence and message brokers (MQTT, Kafka) are intentionally *not*
built in: ft8mon stays dependency-free, and a small separate program can
bridge the UDP JSON stream to whatever transport or database you like.

## SDR backends

For Airspy HF+ Discovery support, install the airspyhf and liquid dsp
libraries, and uncomment the relevant lines in the `Makefile`. For
RFspace SDR-IP, NetSDR, CloudIQ, and CloudSDR support, install liquid
dsp and edit the `Makefile`. Similarly for the Apache ANAN-7000dle. Then
try commands like these:

```
  ./ft8mon -card airspy ,14.074
  ./ft8mon -card hpsdr 192.168.3.100,14.074
  ./ft8mon -card sdrip 192.168.3.100,14.074
```

## Credits

Original ft8mon and all of the FT8 decoding: **Robert Morris, AB1HL**.
FT8 protocol: Joe Taylor (K1JT) and Steve Franke (K9AN). Network
streaming input and JSON output added in this fork.
