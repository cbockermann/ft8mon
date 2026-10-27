//
// decode FT8 from a sound card
//
// Robert Morris, AB1HL
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <vector>
#include <time.h>
#include <string.h>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <stdint.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include "snd.h"
#include "util.h"
#include "unpack.h"
#include "ft8.h"
#include "fft.h"
#ifdef USE_HPSDR
#include "hpsdr.h"
#endif

std::mutex cycle_mu;
volatile int cycle_count;
time_t saved_cycle_start;
std::map<std::string,bool> cycle_already;

//
// optional: emit each decode as a JSON datagram over UDP,
// for downstream processing by another tool.
//
static int json_fd = -1; // connected UDP socket, or -1 if disabled

//
// open a connected UDP socket to host:port for JSON output.
//
static void
json_udp_open(const char *hostport)
{
  std::string hp(hostport);
  std::string::size_type colon = hp.rfind(':');
  if(colon == std::string::npos){
    fprintf(stderr, "-json: expected host:port, got %s\n", hostport);
    exit(1);
  }
  std::string host = hp.substr(0, colon);
  std::string port = hp.substr(colon + 1);
  if(host.size() == 0)
    host = "127.0.0.1";

  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int e = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if(e != 0 || res == 0){
    fprintf(stderr, "-json: cannot resolve %s: %s\n", hostport, gai_strerror(e));
    exit(1);
  }

  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if(fd < 0){
    fprintf(stderr, "-json: socket() failed: %s\n", strerror(errno));
    exit(1);
  }
  // connect() a UDP socket just fixes the default destination.
  if(connect(fd, res->ai_addr, res->ai_addrlen) < 0){
    fprintf(stderr, "-json: connect(%s) failed: %s\n", hostport, strerror(errno));
    exit(1);
  }
  freeaddrinfo(res);

  json_fd = fd;
  fprintf(stderr, "-json: sending decodes as JSON to %s\n", hostport);
}

//
// optional WSJT-X UDP Message Protocol output (-wsjtx host:port).
// emits Heartbeat (0), Status (1) and Decode (2) so GridTracker / JTAlert /
// ft8web and other WSJT-X-aware tools can read ft8mon's decodes directly.
// outbound only; RX-only (no Reply/Halt command side).
//
static int wsjtx_fd = -1;               // connected UDP socket, or -1
static std::string wsjtx_id = "ft8mon"; // instance id, identical in every message
#define WSJTX_VERSION "1.0"

// big-endian / Qt-style serialisation into a byte buffer.
static void w_u8(std::string &b, unsigned v){ b.push_back((char)(v & 0xff)); }
static void w_u32(std::string &b, uint32_t v){
  b.push_back((char)((v >> 24) & 0xff)); b.push_back((char)((v >> 16) & 0xff));
  b.push_back((char)((v >> 8) & 0xff));  b.push_back((char)(v & 0xff));
}
static void w_u64(std::string &b, uint64_t v){
  for(int i = 7; i >= 0; i--) b.push_back((char)((v >> (i*8)) & 0xff));
}
static void w_i32(std::string &b, int32_t v){ w_u32(b, (uint32_t) v); }
static void w_bool(std::string &b, bool v){ b.push_back(v ? 1 : 0); }
static void w_double(std::string &b, double d){ uint64_t u; memcpy(&u, &d, 8); w_u64(b, u); }
// QString: 4-byte big-endian byte length, then UTF-8 bytes (empty => len 0).
static void w_qstr(std::string &b, const std::string &s){
  w_u32(b, (uint32_t) s.size());
  b.append(s);
}
static void w_header(std::string &b, uint32_t type){
  w_u32(b, 0xADBCCBDA); // magic
  w_u32(b, 2);          // schema
  w_u32(b, type);
  w_qstr(b, wsjtx_id);
}
static void wsjtx_send(const std::string &b){
  if(wsjtx_fd >= 0)
    send(wsjtx_fd, b.data(), b.size(), 0);
}

static void
wsjtx_heartbeat()
{
  std::string b;
  w_header(b, 0);
  w_u32(b, 3);              // maxSchema
  w_qstr(b, WSJTX_VERSION); // version
  w_qstr(b, "");            // revision
  wsjtx_send(b);
}

static void
wsjtx_status(long long dial_hz, bool decoding)
{
  std::string b;
  w_header(b, 1);
  w_u64(b, (uint64_t)(dial_hz > 0 ? dial_hz : 0)); // dialFrequency
  w_qstr(b, "FT8");     // mode
  w_qstr(b, "");        // dxCall
  w_qstr(b, "");        // report
  w_qstr(b, "FT8");     // txMode
  w_bool(b, false);     // txEnabled
  w_bool(b, false);     // transmitting
  w_bool(b, decoding);  // decoding
  w_u32(b, 1500);       // rxDF
  w_u32(b, 1500);       // txDF
  w_qstr(b, "");        // deCall
  w_qstr(b, "");        // deGrid
  w_qstr(b, "");        // dxGrid
  w_bool(b, false);     // txWatchdog
  w_qstr(b, "");        // subMode
  w_bool(b, false);     // fastMode
  w_u8(b, 0);           // specialOpMode
  w_u32(b, 0xFFFFFFFF); // frequencyTolerance
  w_u32(b, 15000);      // trPeriod (ms)
  w_qstr(b, "ft8mon");  // configName
  w_qstr(b, "");        // txMessage
  wsjtx_send(b);
}

static void
wsjtx_decode(uint32_t time_ms, int snr, double dt, uint32_t df,
             const std::string &msg)
{
  std::string b;
  w_header(b, 2);
  w_bool(b, true);     // new
  w_u32(b, time_ms);   // ms since 00:00:00 UTC of the decode period
  w_i32(b, snr);
  w_double(b, dt);
  w_u32(b, df);        // audio offset in Hz
  w_qstr(b, "~");      // mode (FT8 indicator)
  w_qstr(b, msg);
  w_bool(b, false);    // lowConfidence
  w_bool(b, false);    // offAir
  wsjtx_send(b);
}

//
// open a connected UDP socket to host:port for WSJT-X output; a missing
// host defaults to 127.0.0.1 and a missing port to 2237.
//
static void
wsjtx_udp_open(const char *hostport)
{
  std::string hp(hostport);
  std::string::size_type colon = hp.rfind(':');
  std::string host, port;
  if(colon == std::string::npos){
    host = hp; port = "2237";
  } else {
    host = hp.substr(0, colon); port = hp.substr(colon + 1);
  }
  if(host.size() == 0) host = "127.0.0.1";
  if(port.size() == 0) port = "2237";

  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int e = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if(e != 0 || res == 0){
    fprintf(stderr, "-wsjtx: cannot resolve %s: %s\n", hostport, gai_strerror(e));
    exit(1);
  }
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if(fd < 0){
    fprintf(stderr, "-wsjtx: socket() failed: %s\n", strerror(errno));
    exit(1);
  }
  if(connect(fd, res->ai_addr, res->ai_addrlen) < 0){
    fprintf(stderr, "-wsjtx: connect(%s) failed: %s\n", hostport, strerror(errno));
    exit(1);
  }
  freeaddrinfo(res);
  wsjtx_fd = fd;
  fprintf(stderr, "-wsjtx: sending WSJT-X UDP messages to %s:%s\n",
          host.c_str(), port.c_str());

  // announce ourselves right away.
  wsjtx_heartbeat();
  wsjtx_status(0, false);
}

//
// escape a string for inclusion in a JSON value.
//
static std::string
json_escape(const std::string &s)
{
  std::string o;
  for(char c : s){
    switch(c){
    case '"':  o += "\\\""; break;
    case '\\': o += "\\\\"; break;
    case '\n': o += "\\n";  break;
    case '\r': o += "\\r";  break;
    case '\t': o += "\\t";  break;
    default:
      if((unsigned char) c < 0x20){
        char b[8];
        snprintf(b, sizeof(b), "\\u%04x", (unsigned char) c);
        o += b;
      } else {
        o += c;
      }
    }
  }
  return o;
}

//
// optional extra key/value pairs to add to every JSON datagram,
// e.g. -tag band:40m. pre-formatted as ,"key":"value"... so it can
// be spliced in just before the closing brace.
//
static std::string json_tags;
static bool json_has_band = false; // did the user set an explicit "band" tag?

// dial (carrier) frequency in Hz, taken from an SDR backend that knows
// it; -1 means unknown. when set, each JSON decode also carries
// dial_hz, the absolute rf_hz (dial + audio offset), and a band tag.
static int g_dial_hz = -1;

//
// parse a "key:value" argument from -tag and append it to json_tags.
//
static void
json_add_tag(const char *kv)
{
  std::string s(kv);
  std::string::size_type colon = s.find(':');
  if(colon == std::string::npos){
    fprintf(stderr, "-tag: expected key:value, got %s\n", kv);
    exit(1);
  }
  std::string k = s.substr(0, colon);
  std::string v = s.substr(colon + 1);
  if(k.size() == 0){
    fprintf(stderr, "-tag: empty key in %s\n", kv);
    exit(1);
  }
  if(k == "band")
    json_has_band = true; // user's band tag wins over the auto one
  json_tags += ",\"" + json_escape(k) + "\":\"" + json_escape(v) + "\"";
}

//
// map a dial frequency (Hz) to an amateur band name, or "" if unknown.
//
static const char *
freq_to_band(int hz)
{
  double m = hz / 1e6;
  if(m >= 1.8    && m <= 2.0)    return "160m";
  if(m >= 3.5    && m <= 4.0)    return "80m";
  if(m >= 5.25   && m <= 5.45)   return "60m";
  if(m >= 7.0    && m <= 7.3)    return "40m";
  if(m >= 10.1   && m <= 10.15)  return "30m";
  if(m >= 14.0   && m <= 14.35)  return "20m";
  if(m >= 18.068 && m <= 18.168) return "17m";
  if(m >= 21.0   && m <= 21.45)  return "15m";
  if(m >= 24.89  && m <= 24.99)  return "12m";
  if(m >= 28.0   && m <= 29.7)   return "10m";
  if(m >= 50.0   && m <= 54.0)   return "6m";
  if(m >= 70.0   && m <= 70.5)   return "4m";
  if(m >= 144.0  && m <= 148.0)  return "2m";
  if(m >= 430.0  && m <= 440.0)  return "70cm";
  return "";
}

//
// a91 is 91 bits -- 77 plus the 14-bit CRC.
//
int
hcb(int *a91, double hz0, double hz1, double off,
    const char *comment, double snr, int pass,
    int correct_bits)
{
  std::string msg = unpack(a91);

  cycle_mu.lock();

  if(cycle_already.count(msg) > 0){
    // already decoded this message on this cycle
    cycle_mu.unlock();
    return 1; // 1 => already seen, don't subtract.
  }

  cycle_already[msg] = true;
  cycle_count += 1;

  cycle_mu.unlock();

  struct tm result;
  gmtime_r(&saved_cycle_start, &result);

  printf("%02d%02d%02d %3d %3d %5.2f %6.1f %s\n",
         result.tm_hour,
         result.tm_min,
         result.tm_sec,
         (int)snr,
         correct_bits,
         off - 0.5,
         hz0,
         msg.c_str());
  fflush(stdout);

  if(json_fd >= 0){
    char ts[40];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &result);
    std::string em = json_escape(msg);
    char buf[512];
    // the fixed fields; note there is no closing brace yet.
    snprintf(buf, sizeof(buf),
             "{\"time\":\"%s\",\"unix\":%lld,\"snr\":%d,\"dt\":%.2f,"
             "\"freq\":%.1f,\"correct_bits\":%d,\"msg\":\"%s\"",
             ts, (long long) saved_cycle_start, (int) snr,
             off - 0.5, hz0, correct_bits, em.c_str());
    std::string out = buf;
    // auto frequency tagging when the source knows its dial frequency.
    if(g_dial_hz > 0){
      long long rf = (long long) g_dial_hz + (long long)(hz0 + 0.5);
      const char *band = freq_to_band(g_dial_hz);
      char ab[128];
      if(band[0] != 0 && !json_has_band){
        snprintf(ab, sizeof(ab), ",\"dial_hz\":%d,\"rf_hz\":%lld,\"band\":\"%s\"",
                 g_dial_hz, rf, band);
      } else {
        snprintf(ab, sizeof(ab), ",\"dial_hz\":%d,\"rf_hz\":%lld", g_dial_hz, rf);
      }
      out += ab;
    }
    out += json_tags; // any -tag key/value pairs, or empty
    out += "}\n";
    // one send() == one datagram; atomic, so no lock needed even
    // though hcb() runs on several decoder threads.
    send(json_fd, out.data(), out.size(), 0);
  }

  if(wsjtx_fd >= 0){
    // WSJT-X "time" is ms since 00:00:00 UTC of the decode period.
    uint32_t tms = (uint32_t)(((long long) saved_cycle_start % 86400) * 1000);
    uint32_t df = (uint32_t)(hz0 + 0.5); // audio offset (signal RF minus dial)
    wsjtx_decode(tms, (int) snr, off - 0.5, df, msg);
  }

  return 2; // 2 => new decode, do subtract.
}

void
usage()
{
  fprintf(stderr, "Usage: ft8mon -card card channel\n");
#ifdef USE_AIRSPYHF
  fprintf(stderr, "       ft8mon -card airspy serial,mhz\n");
#endif
  fprintf(stderr, "       ft8mon -levels card channel\n");
  fprintf(stderr, "       ft8mon -list\n");
  fprintf(stderr, "       ft8mon -file xxx.wav ...\n");
  fprintf(stderr, "       ft8mon -listen address:port          (WAV stream)\n");
  fprintf(stderr, "       ft8mon -listen address:port rate     (raw s16le mono PCM)\n");
  fprintf(stderr, "  add  -json host:port to also send each decode as JSON over UDP\n");
  fprintf(stderr, "  add  -tag key:value  to add \"key\":\"value\" to each JSON object (repeatable)\n");
  fprintf(stderr, "  add  -wsjtx host:port to also emit WSJT-X UDP messages (default 127.0.0.1:2237)\n");
  exit(1);
}

//
// decode FT8 from a live SoundIn source, once per 15-second cycle.
// used for both a local sound card and a streamed WAV source.
//
void
decode_loop(SoundIn *sin)
{
  int hints[2] = { 2, 0 }; // CQ
  double budget = 5; // compute for this many seconds per cycle

  sin->start();
  int rate = sin->rate();

  // if the source knows its dial frequency (SDR backends do), use it to
  // tag each decode with the absolute frequency and band. an explicit
  // -tag has already been parsed; a -tag band:... still takes priority.
  if(g_dial_hz < 0)
    g_dial_hz = sin->dial_hz();

  while(1){
    // sleep until 14 seconds into the next 15-second cycle.
    double tt = now();
    long long cycle_start = tt - ((long long)tt % 15);

    if(tt - cycle_start >= 14){
      double ttt_start;
      // asking for no more than 15 seconds of samples in order
      // to avoid missing in fftw plan cache.
      // the "1" asks for the most recent 15 seconds of samples,
      // not the oldest buffered. it causes samples before the
      // most recent 15 seconds to be discarded.
      std::vector<double> samples = sin->get(15 * rate, ttt_start, 1);

      // ttt_start is UNIX time of samples[0].
      double ttt_end = ttt_start + samples.size() / rate;
      cycle_start = ((long long) (ttt_end / 15)) * 15;

      // sample # of 0.5 seconds into the 15-second cycle.
      long long nominal_start = samples.size() - rate * (ttt_end - cycle_start - 0.5);

      if(nominal_start >= 0 && nominal_start + 10*rate < (int) samples.size()){
        struct tm result;
        time_t tx = cycle_start;
        gmtime_r(&tx, &result);
        printf("%02d:%02d:%02d decodes: %d\n",
               result.tm_hour,
               result.tm_min,
               result.tm_sec,
               cycle_count);

        // make samples exactly 15 seconds, to make
        // fftw plan caching more effective.
        samples.resize(15 * rate, 0.0);

        cycle_mu.lock();
        cycle_count = 0;
        saved_cycle_start = cycle_start; // for hcb() callback
        cycle_already.clear();
        cycle_mu.unlock();

        // WSJT-X consumers: heartbeat (~15 s) + status (band context).
        wsjtx_heartbeat();
        wsjtx_status(g_dial_hz, true);

        entry(samples.data(), samples.size(), nominal_start, rate,
              150,
              3600, // 2900,
              hints, hints, budget, budget, hcb,
              0, (struct cdecode *) 0);

        wsjtx_status(g_dial_hz, false);
      }

      sleep(2);
    }
    usleep(100 * 1000); // 0.1 seconds
  }
}

int
main(int argc, char *argv[])
{
  int hints[2] = { 2, 0 }; // CQ
  double budget = 5; // compute for this many seconds per cycle

  extern int fftw_type;
  fftw_type = FFTW_ESTIMATE; // rather than FFTW_MEASURE

  extern int nthreads;
  nthreads = 4; // multi-core

  // pull an optional "-json host:port" out of argv, wherever it
  // appears, so it can be combined with any input source. the
  // remaining arguments are then dispatched exactly as before.
  {
    int w = 1;
    for(int r = 1; r < argc; ){
      if(strcmp(argv[r], "-json") == 0 && r + 1 < argc){
        json_udp_open(argv[r+1]);
        r += 2;
      } else if(strcmp(argv[r], "-wsjtx") == 0 && r + 1 < argc){
        wsjtx_udp_open(argv[r+1]);
        r += 2;
      } else if(strcmp(argv[r], "-tag") == 0 && r + 1 < argc){
        json_add_tag(argv[r+1]);
        r += 2;
      } else {
        argv[w++] = argv[r++];
      }
    }
    argc = w;
  }

  if(argc == 4 && strcmp(argv[1], "-card") == 0){
    int wanted_rate = 12000;
    SoundIn *sin = SoundIn::open(argv[2], argv[3], wanted_rate);
    decode_loop(sin);
  } else if(argc == 3 && strcmp(argv[1], "-listen") == 0){
    // read a streamed WAV file from a TCP connection.
    // argv[2] is "address:port"; the WAV header sets the sample rate.
    SoundIn *sin = SoundIn::open("listen", argv[2], -1);
    decode_loop(sin);
  } else if(argc == 4 && strcmp(argv[1], "-listen") == 0){
    // read a headerless raw signed-16-bit little-endian mono PCM
    // stream from a TCP connection. argv[2] is "address:port" and
    // argv[3] is the sample rate (e.g. 12000).
    SoundIn *sin = SoundIn::open("rawlisten", argv[2], atoi(argv[3]));
    decode_loop(sin);
  } else if(argc == 4 && strcmp(argv[1], "-levels") == 0){
    SoundIn *sin = SoundIn::open(argv[2], argv[3], 12000);
    sin->start();
    sin->levels();
  } else if(argc >= 3 && strcmp(argv[1], "-file") == 0){
    for(int ii = 2; ii < argc; ii++){
      // the .wav file should start at an even 15-second boundary.
      // "-" means read a WAV from stdin (e.g. ft8gen ... | ft8mon -file -).
      int rate;
      std::vector<double> s;
      if(strcmp(argv[ii], "-") == 0){
        s = readwav_stdin(rate);
      } else {
        s = readwav(argv[ii], rate);
      }
      entry(s.data(), s.size(), 0.5 * rate, rate,
            150,
            3600, // 2900,
            hints, hints, budget, budget, hcb,
            0, (struct cdecode *) 0);
    }
    extern void fft_stats();
    // fft_stats();
  } else if(argc == 2 && strcmp(argv[1], "-list") == 0){
    snd_list();
  } else {
    usage();
  }
}
