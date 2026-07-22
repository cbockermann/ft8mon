//
// RTL-SDR over TCP (rtl_tcp) input for ft8mon.
// see rtltcp.h for an overview.
//

#include "rtltcp.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// iq2usb() is defined in snd.cc and declared in snd.h; it turns
// baseband I/Q into upper sideband via the phasing method.

//
// tuning layout:
//   the RTL hardware is tuned OFFSET_HZ below the dial frequency, so
//   the DC spike at the hardware centre lands well outside the audio
//   pass band. the NCO then shifts the dial frequency to 0 Hz, after
//   which USB audio appears at 0..a few kHz.
//
static const int OFFSET_HZ = 8000;

// default rtl_tcp sample rate. must be an integer multiple of the
// audio rate. 240000 is a valid RTL rate and 240000/12000 == 20.
static const int DEFAULT_DEV_RATE = 240000;

// low-pass FIR: length and cut-off (Hz). the cut-off is below the
// audio Nyquist (rate_/2) so decimation does not alias, and above the
// FT8 pass band (~3 kHz).
static const int FIR_LEN = 401;
static const double FIR_CUTOFF_HZ = 4000.0;

RTLTCPSoundIn::RTLTCPSoundIn(std::string chan, int rate)
{
  // chan is "host[:port],megahertz", e.g. 192.168.1.50:1234,14.074
  std::string::size_type comma = chan.find(',');
  if(comma == std::string::npos){
    fprintf(stderr, "rtltcp: expected host:port,megahertz, got %s\n", chan.c_str());
    exit(1);
  }
  std::string hostport = chan.substr(0, comma);
  double mhz = atof(chan.substr(comma + 1).c_str());
  if(mhz <= 0){
    fprintf(stderr, "rtltcp: bad frequency in %s\n", chan.c_str());
    exit(1);
  }
  hz_ = (int) llround(mhz * 1000000.0);

  std::string::size_type colon = hostport.rfind(':');
  if(colon == std::string::npos){
    host_ = hostport;
    port_ = 1234; // rtl_tcp default
  } else {
    host_ = hostport.substr(0, colon);
    port_ = atoi(hostport.substr(colon + 1).c_str());
  }
  if(host_.size() == 0 || port_ <= 0){
    fprintf(stderr, "rtltcp: bad host:port in %s\n", chan.c_str());
    exit(1);
  }

  rate_ = (rate == -1) ? 12000 : rate;
  dev_rate_ = DEFAULT_DEV_RATE;
  if((dev_rate_ % rate_) != 0){
    fprintf(stderr, "rtltcp: device rate %d is not a multiple of %d\n",
            dev_rate_, rate_);
    exit(1);
  }
  offset_ = OFFSET_HZ;
  ppm_ = 0;
  fd_ = -1;
  time_ = -1;

  nco_acc_ = 0.0;
  nco_step_ = 2 * M_PI * offset_ / (double) dev_rate_;

  build_filter();
  dl_.assign(h_.size(), std::complex<double>(0, 0));
  dpos_ = 0;
  incount_ = 0;

  n_ = rate_ * 60; // 60-second circular buffer
  buf_ = (std::complex<double> *) malloc(sizeof(std::complex<double>) * n_);
  assert(buf_);
  wi_ = 0;
  ri_ = 0;
  reader_ = 0;
  stop_ = false;
}

RTLTCPSoundIn::~RTLTCPSoundIn()
{
  // stop the reader thread before freeing anything it touches.
  stop_ = true;
  if(fd_ >= 0){
    ::shutdown(fd_, SHUT_RDWR); // unblock a read() in the reader thread
    ::close(fd_);
    fd_ = -1;
  }
  if(reader_){
    reader_->join();
    delete reader_;
    reader_ = 0;
  }
  if(buf_)
    free(buf_);
}

//
// build a low-pass FIR: a windowed sinc with a Blackman-Harris window
// (deep stop band), normalised for unity DC gain.
//
void
RTLTCPSoundIn::build_filter()
{
  int N = FIR_LEN;
  h_.assign(N, 0.0);
  double fc = FIR_CUTOFF_HZ / (double) dev_rate_; // cycles/sample
  double mid = (N - 1) / 2.0;
  double sum = 0;
  for(int i = 0; i < N; i++){
    double x = i - mid;
    double sinc;
    if(x == 0.0){
      sinc = 2 * fc;
    } else {
      sinc = sin(2 * M_PI * fc * x) / (M_PI * x);
    }
    // Blackman-Harris window
    double w = 0.35875
             - 0.48829 * cos(2 * M_PI * i / (N - 1))
             + 0.14128 * cos(4 * M_PI * i / (N - 1))
             - 0.01168 * cos(6 * M_PI * i / (N - 1));
    h_[i] = sinc * w;
    sum += h_[i];
  }
  for(int i = 0; i < N; i++)
    h_[i] /= sum;
}

//
// send a 5-byte rtl_tcp command: 1 command byte + 4 big-endian bytes.
//
void
RTLTCPSoundIn::send_cmd(unsigned char cmd, unsigned int param)
{
  unsigned char b[5];
  b[0] = cmd;
  b[1] = (param >> 24) & 0xff;
  b[2] = (param >> 16) & 0xff;
  b[3] = (param >> 8) & 0xff;
  b[4] = param & 0xff;
  if(write(fd_, b, 5) != 5){
    fprintf(stderr, "rtltcp: failed to send command 0x%02x\n", cmd);
  }
}

bool
RTLTCPSoundIn::read_exact(void *buf, int n)
{
  char *p = (char *) buf;
  int got = 0;
  while(got < n){
    int cc = ::read(fd_, p + got, n - got);
    if(cc <= 0)
      return false;
    got += cc;
  }
  return true;
}

//
// connect to rtl_tcp, read the 12-byte greeting, and configure the
// device. returns false on failure.
//
bool
RTLTCPSoundIn::connect_and_configure()
{
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port_);

  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int e = getaddrinfo(host_.c_str(), portstr, &hints, &res);
  if(e != 0 || res == 0){
    fprintf(stderr, "rtltcp: cannot resolve %s: %s\n", host_.c_str(), gai_strerror(e));
    return false;
  }

  fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if(fd_ < 0){
    fprintf(stderr, "rtltcp: socket() failed: %s\n", strerror(errno));
    freeaddrinfo(res);
    return false;
  }
  if(connect(fd_, res->ai_addr, res->ai_addrlen) < 0){
    fprintf(stderr, "rtltcp: connect(%s:%d) failed: %s\n",
            host_.c_str(), port_, strerror(errno));
    ::close(fd_);
    fd_ = -1;
    freeaddrinfo(res);
    return false;
  }
  freeaddrinfo(res);

  int one = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // rtl_tcp greeting: "RTL0" + 4-byte tuner type + 4-byte gain count.
  unsigned char greet[12];
  if(!read_exact(greet, 12) || memcmp(greet, "RTL0", 4) != 0){
    fprintf(stderr, "rtltcp: did not get a valid rtl_tcp greeting\n");
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  // configure. HF (below ~24 MHz) needs direct sampling (Q branch).
  send_cmd(0x09, hz_ < 24000000 ? 2 : 0); // set direct sampling
  send_cmd(0x02, dev_rate_);              // set sample rate
  send_cmd(0x05, ppm_);                   // set frequency correction
  send_cmd(0x03, 0);                      // gain mode: 0 = automatic
  send_cmd(0x01, hz_ - offset_);          // set centre frequency

  fprintf(stderr, "rtltcp: connected to %s:%d, dial %.3f MHz, %s, "
          "dev_rate %d -> audio %d\n",
          host_.c_str(), port_, hz_ / 1000000.0,
          hz_ < 24000000 ? "direct sampling" : "tuner",
          dev_rate_, rate_);
  return true;
}

//
// feed one input I/Q sample through the NCO and the decimating FIR.
//
void
RTLTCPSoundIn::push_sample(std::complex<double> x)
{
  // NCO: multiply by exp(-j*acc) to shift +offset_ down to 0 Hz.
  std::complex<double> lo(cos(nco_acc_), -sin(nco_acc_));
  nco_acc_ += nco_step_;
  if(nco_acc_ > 2 * M_PI)
    nco_acc_ -= 2 * M_PI;
  std::complex<double> mixed = x * lo;

  // push into the FIR delay line.
  dl_[dpos_] = mixed;
  dpos_ = (dpos_ + 1) % (int) dl_.size();
  incount_ += 1;

  // produce one output every dev_rate_/rate_ inputs.
  if((incount_ % (dev_rate_ / rate_)) == 0){
    std::complex<double> acc(0, 0);
    int idx = (dpos_ - 1 + (int) dl_.size()) % (int) dl_.size();
    int L = (int) h_.size();
    for(int k = 0; k < L; k++){
      acc += h_[k] * dl_[idx];
      idx -= 1;
      if(idx < 0)
        idx = L - 1;
    }
    store_output(acc);
  }
}

void
RTLTCPSoundIn::store_output(std::complex<double> s)
{
  if(((wi_ + 1) % n_) != ri_){
    buf_[wi_] = s;
    wi_ = (wi_ + 1) % n_;
  }
  // else: buffer full; drop the sample. get(latest=1) drains the
  // buffer each cycle, so this should not happen in practice. only
  // get() advances ri_, to avoid a race with the reader thread.
}

//
// background thread: read the u8 I/Q stream, run the DSP, buffer the
// audio-rate I/Q. reconnects if rtl_tcp drops the connection.
//
void
RTLTCPSoundIn::reader_loop()
{
  std::vector<unsigned char> acc; // leftover bytes (I/Q pairs are 2 bytes)
  unsigned char tmp[16384];

  while(!stop_){
    int cc = ::read(fd_, tmp, sizeof(tmp));
    if(cc <= 0){
      if(stop_)
        break;
      fprintf(stderr, "rtltcp: connection lost, reconnecting...\n");
      ::close(fd_);
      fd_ = -1;
      acc.clear();
      while(!stop_ && !connect_and_configure()){
        sleep(2);
      }
      if(stop_)
        break;
      // reset DSP state for the new stream.
      nco_acc_ = 0.0;
      std::fill(dl_.begin(), dl_.end(), std::complex<double>(0, 0));
      dpos_ = 0;
      incount_ = 0;
      continue;
    }

    acc.insert(acc.end(), tmp, tmp + cc);
    int npair = acc.size() / 2;
    for(int p = 0; p < npair; p++){
      double I = (((int) acc[2*p])     - 127.5) * (1.0 / 127.5);
      double Q = (((int) acc[2*p + 1]) - 127.5) * (1.0 / 127.5);
      push_sample(std::complex<double>(I, Q));
    }
    time_ = now();
    if(npair > 0)
      acc.erase(acc.begin(), acc.begin() + npair * 2);
  }
}

void
RTLTCPSoundIn::start()
{
  if(!connect_and_configure()){
    fprintf(stderr, "rtltcp: could not start\n");
    exit(1);
  }
  reader_ = new std::thread( [this]() { this->reader_loop(); } );
}

int
RTLTCPSoundIn::set_freq(int hz)
{
  hz_ = hz;
  if(fd_ >= 0){
    send_cmd(0x09, hz_ < 24000000 ? 2 : 0);
    send_cmd(0x01, hz_ - offset_);
  }
  return hz;
}

//
// return recent audio samples, converting the buffered I/Q to USB.
// same contract as the other SoundIn::get() implementations.
//
std::vector<double>
RTLTCPSoundIn::get(int n, double &t0, int latest)
{
  if(time_ < 0 && wi_ == ri_){
    t0 = -1;
    return std::vector<double>();
  }

  if(latest){
    while(((wi_ + n_ - ri_) % n_) > n){
      ri_ = (ri_ + 1) % n_;
    }
  }

  // UNIX time of the first sample we are about to return.
  t0 = time_;
  if(wi_ >= ri_){
    t0 -= (wi_ - ri_) * (1.0 / rate_);
  } else {
    t0 -= ((wi_ + n_) - ri_) * (1.0 / rate_);
  }

  std::vector<std::complex<double>> v1;
  while((int) v1.size() < n){
    if(ri_ == wi_)
      break;
    v1.push_back(buf_[ri_]);
    ri_ = (ri_ + 1) % n_;
  }

  if(v1.size() < 2){
    // analytic() (inside iq2usb) needs more than one sample.
    return vreal(v1);
  }

  // pad to a round length so FFT plans can be re-used, then trim back.
  int olen = v1.size();
  int quantum;
  if(olen > rate() * 5){
    quantum = rate();
  } else if(olen > 1000){
    quantum = 1000;
  } else {
    quantum = 100;
  }
  int needed = quantum - (olen % quantum);
  if(needed != quantum)
    v1.resize(v1.size() + needed, 0.0);

  std::vector<double> v2 = iq2usb(v1);
  if((int) v2.size() > olen)
    v2.resize(olen);
  return v2;
}
