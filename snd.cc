#include "snd.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <vector>
#include <string>
#include <string.h>
#include <math.h>
#include "util.h"
#include "fft.h"
#include <portaudio.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>

void
snd_init()
{
  static int inited = 0;

  if(!inited){
    inited = 1;
    int err = Pa_Initialize();
    if(err != paNoError){
      fprintf(stderr, "Pa_Initialize() failed\n");
      exit(1);
    }
  }
}

extern "C" void
airspy_list()
{
#ifdef USE_AIRSPYHF
  int ndev = airspyhf_list_devices(0, 0);
  if(ndev > 0){
    uint64_t serials[20];
    if(ndev > 20)
      ndev = 20;
    airspyhf_list_devices(serials, ndev);
    for(int unit = 0; unit < ndev; unit++){
      airspyhf_device_t *dev = 0;
      if(airspyhf_open_sn(&dev, serials[unit]) == AIRSPYHF_SUCCESS){
        airspyhf_read_partid_serialno_t read_partid_serialno;
        airspyhf_board_partid_serialno_read(dev, &read_partid_serialno);
        printf("Airspy HF+ serial %08X%08X\n",
               read_partid_serialno.serial_no[0],
               read_partid_serialno.serial_no[1]);
      } else {
        fprintf(stderr, "could not open airspyhf unit %d\n", unit);
      }
    }
  }
#endif
}

extern "C" void
hpsdr_list()
{
#ifdef USE_HPSDR
  HPSDR::list();
#endif
}


//
// print a list of sound devices
//
void
snd_list()
{
  snd_init();
  int n = Pa_GetDeviceCount();
  printf("%d sound devices:\n", n);
  for(int di = 0; di < n; di++){
    const PaDeviceInfo *info = Pa_GetDeviceInfo(di);
    if(info == 0)
      continue;
    printf("%d: %s %d/%d ",
           di,
           info->name,
           info->maxInputChannels,
           info->maxOutputChannels);

    PaStreamParameters ip;
    memset(&ip, 0, sizeof(ip));
    ip.device = di;
    ip.channelCount = 1;
    ip.sampleFormat = paInt16;
    ip.suggestedLatency = 0;
    ip.hostApiSpecificStreamInfo = 0;
    int rates[] = { 6000, 8000, 11025, 12000, 16000, 22050, 44100, 48000, 0 };
    for(int ri = 0; rates[ri]; ri++){
      PaError err = Pa_IsFormatSupported(&ip, 0, rates[ri]);
      if(err == paNoError){
        printf("%d ", rates[ri]);
      }
    }

    // printf("-- %f %f ", info->defaultLowOutputLatency, info->defaultHighOutputLatency);

    printf("\n");
  }

  airspy_list();
  hpsdr_list();
}

//
// print avg and peak each second.
//
void
SoundIn::levels()
{
  double max = 0;
  double sum = 0;
  int n = 0;
  double last_t = now();

  while(1){
    double dummy;
    std::vector<double> buf = get(rate(), dummy, 0);
    if(buf.size() == 0)
      usleep(100*1000);
    for(int i = 0; i < (int) buf.size(); i++){
      sum += fabs(buf[i]);
      n += 1;
      if(fabs(buf[i]) > max){
        max = fabs(buf[i]);
      }
      if(n >= rate()){
        printf("avg=%.3f peak=%.3f rate=%.1f\n", sum / n, max, n / (now() - last_t));
        n = 0;
        sum = 0;
        max = 0;
        last_t = now();
      }
    }
  }
}

//
// generic open.
// rate can be -1.
//
SoundIn *
SoundIn::open(std::string card, std::string chan, int rate)
{
  assert(card.size() > 0);
  SoundIn *sin;

  if(isdigit(card[0])){
    sin = new CardSoundIn(atoi(card.c_str()), atoi(chan.c_str()), rate);
  } else if(card == "file"){
    sin = new FileSoundIn(chan, rate);
  } else if(card == "listen"){
    sin = new NetworkSoundIn(chan, rate);
  } else if(card == "rawlisten"){
    sin = new NetworkSoundIn(chan, rate, true);
#ifdef USE_AIRSPYHF
  } else if(card == "airspy"){
    sin = new AirspySoundIn(chan, rate);
#endif
#ifdef USE_HPSDR
  } else if(card == "hpsdr"){
    sin = new HPSDRSoundIn(chan, rate);
#endif
  } else if(card == "cloudsdr"){
    sin = new CloudSoundIn(chan, rate);
#ifdef USE_SDRIP
  } else if(card == "sdrip"){
    sin = new SDRIPSoundIn(chan, rate);
#endif
  } else {
    fprintf(stderr, "SoundIn::open(%s, %s): type not recognized\n", card.c_str(), chan.c_str());
    exit(1);
  }

  return sin;
}
  
int
CardSoundIn::cb(const void *input,
            void *output,
            unsigned long frameCount,
            const struct PaStreamCallbackTimeInfo *timeInfo,
            unsigned long statusFlags, // PaStreamCallbackFlags
            void *userData)
{
  CardSoundIn *sin = (CardSoundIn *) userData;
  const short int *buf = (const short int *) input;

  if(statusFlags != 0){
    // 2 is paInputOverflow
    fprintf(stderr, "CardSoundIn::cb statusFlags 0x%x frameCount %d\n",
            (int)statusFlags, (int)frameCount);
  }

  for(int i = 0; i < (int) frameCount; i++){
    if(((sin->wi_ + 1) % sin->n_) != sin->ri_){
      sin->buf_[sin->wi_] = buf[i*sin->channels_ + sin->chan_];
      sin->wi_ = (sin->wi_ + 1) % sin->n_;
    } else {
      fprintf(stderr, "CardSoundIn::cb buf_ overflow\n");
      break;
    }
  }

  sin->time_ = timeInfo->inputBufferAdcTime + frameCount * (1.0 / sin->rate_);

  return 0;
}

CardSoundIn::CardSoundIn(int card, int chan, int rate)
{
  card_ = card;
  chan_ = chan;
  assert(chan_ >= 0 && chan_ <= 1);
  time_ = -1;
  rate_ = rate;
}

//
// read a bunch of recent sound samples.
// read up to n samples, no more.
// return immediately with whatever samples exist,
// perhaps fewer than n.
// return UNIX time of first sample in t0.
// if latest==1, return (up to) the most recent n samples
// and discard any earlier samples.
//
std::vector<double>
CardSoundIn::get(int n, double &t0, int latest)
{
  std::vector<double> v;

  if(time_ < 0 && wi_ == ri_){
    // no input has ever arrived.
    t0 = -1;
    return v;
  }

  if(latest){
    while(((wi_ + n_ - ri_) % n_) > n){
      ri_ = (ri_ + 1) % n_;
    }
  }

  // calculate time of first sample in buf_.
  // XXX there's a race here with cb().
  t0 = time_ + dt_; // UNIX time of last sample in buf_.
  if(wi_ > ri_){
    t0 -= (wi_ - ri_) * (1.0 / rate_);
  } else {
    t0 -= ((wi_ + n_) - ri_) * (1.0 / rate_);
  }

  while((int) v.size() < n){
    if(ri_ == wi_){
      break;
    }
    short x = buf_[ri_];
    v.push_back(x / 32767.0);
    ri_ = (ri_ + 1) % n_;
  }

  return v;
}

void
CardSoundIn::start()
{
  snd_init();

  if(rate_ == -1){
#ifdef __linux__
    // RIGblaster only supports 44100 and 48000.
    rate_ = 48000;
#else
    rate_ = 12000;
#endif
  }

#ifdef __FreeBSD__
  // must read both, otherwise FreeBSD mixes them.
  channels_ = 2;
#else
  if(chan_ == 0){
    channels_ = 1;
  } else {
    channels_ = 2;
  }
#endif

  PaStreamParameters ip;
  memset(&ip, 0, sizeof(ip));
  ip.device = card_;
  ip.channelCount = channels_;
  ip.sampleFormat = paInt16;
  ip.hostApiSpecificStreamInfo = 0;

  // don't set latency to zero; this causes problems on Linux.
  ip.suggestedLatency = Pa_GetDeviceInfo(card_)->defaultLowInputLatency;
  
  PaStream *str = 0;
  PaError err = Pa_OpenStream(&str,
                              &ip,
                              0,
                              rate_,
#ifdef __FreeBSD__
                              128, // framesPerBuffer
#else
                              0, // framesPerBuffer
#endif
                              0,
                              cb,
                              (void*) this);
  if(err != paNoError){
    fprintf(stderr, "Pa_OpenStream(card=%d,rate=%d) failed for input: %s\n",
            card_, rate_, Pa_GetErrorText(err));
    exit(1);
  }

  // allocate a 30-second circular buffer
  n_ = rate_ * 30;
  buf_ = (short *) malloc(sizeof(short) * n_);
  assert(buf_);
  wi_ = 0;
  ri_ = 0;

  err = Pa_StartStream(str);
  if(err != paNoError){
    fprintf(stderr, "Pa_StartStream failed\n");
    exit(1);
  }

  dt_ = now() - Pa_GetStreamTime(str);

}

//
// NetworkSoundIn: read a streamed WAV file from a TCP connection.
//

NetworkSoundIn::NetworkSoundIn(std::string chan, int rate, bool raw)
{
  // chan is "address:port". the address may be empty (":1234")
  // to bind to all interfaces.
  std::string::size_type colon = chan.rfind(':');
  if(colon == std::string::npos){
    fprintf(stderr, "NetworkSoundIn: expected address:port, got %s\n",
            chan.c_str());
    exit(1);
  }
  addr_ = chan.substr(0, colon);
  port_ = atoi(chan.substr(colon + 1).c_str());
  if(port_ <= 0){
    fprintf(stderr, "NetworkSoundIn: bad port in %s\n", chan.c_str());
    exit(1);
  }

  raw_ = raw;
  if(raw_ && rate <= 0){
    fprintf(stderr, "NetworkSoundIn: raw PCM stream needs a sample rate\n");
    exit(1);
  }

  rate_ = rate; // for a WAV stream this is overridden by the header
  channels_ = 1;
  bits_ = 16;
  format_ = 1;
  listen_fd_ = -1;
  fd_ = -1;
  time_ = -1;
  n_ = 0;
  buf_ = 0;
  wi_ = 0;
  ri_ = 0;
  reader_ = 0;
}

//
// read exactly n bytes from the current connection fd_.
// returns false on EOF or error.
//
bool
NetworkSoundIn::read_exact(void *buf, int n)
{
  char *p = (char *) buf;
  int got = 0;
  while(got < n){
    int cc = ::read(fd_, p + got, n - got);
    if(cc <= 0){
      return false;
    }
    got += cc;
  }
  return true;
}

//
// parse the WAV header on fd_, filling in rate_, channels_,
// bits_ and format_. leaves fd_ positioned at the first PCM
// sample. returns false on a malformed header.
// the "data" chunk length is ignored, since for a live stream
// it is usually bogus (0, or a maximum value).
//
static unsigned int le32(const unsigned char *p){
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned int le16(const unsigned char *p){
  return p[0] | (p[1] << 8);
}

bool
NetworkSoundIn::read_header()
{
  unsigned char riff[12];
  if(!read_exact(riff, 12))
    return false;
  if(memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0){
    fprintf(stderr, "NetworkSoundIn: not a WAV stream (missing RIFF/WAVE)\n");
    return false;
  }

  int got_fmt = 0;

  // walk the chunks until we reach "data".
  while(1){
    unsigned char ch[8];
    if(!read_exact(ch, 8))
      return false;
    unsigned int sz = le32(ch + 4);

    if(memcmp(ch, "fmt ", 4) == 0){
      unsigned char fmt[16];
      if(sz < 16)
        return false;
      if(!read_exact(fmt, 16))
        return false;
      format_ = le16(fmt + 0);
      channels_ = le16(fmt + 2);
      rate_ = le32(fmt + 4);
      bits_ = le16(fmt + 14);
      got_fmt = 1;
      // skip any extra bytes in the fmt chunk.
      for(unsigned int i = 16; i < sz; i++){
        char junk;
        if(!read_exact(&junk, 1))
          return false;
      }
    } else if(memcmp(ch, "data", 4) == 0){
      if(!got_fmt){
        fprintf(stderr, "NetworkSoundIn: data chunk before fmt chunk\n");
        return false;
      }
      break; // fd_ now points at the PCM samples.
    } else {
      // skip an uninteresting chunk (e.g. LIST, fact).
      for(unsigned int i = 0; i < sz; i++){
        char junk;
        if(!read_exact(&junk, 1))
          return false;
      }
    }
  }

  if(channels_ < 1 || rate_ < 1){
    fprintf(stderr, "NetworkSoundIn: bad WAV parameters (rate=%d chan=%d)\n",
            rate_, channels_);
    return false;
  }
  if(!((format_ == 1 && (bits_ == 8 || bits_ == 16 || bits_ == 24 || bits_ == 32)) ||
       (format_ == 3 && bits_ == 32))){
    fprintf(stderr, "NetworkSoundIn: unsupported WAV format=%d bits=%d\n",
            format_, bits_);
    return false;
  }

  fprintf(stderr, "NetworkSoundIn: WAV stream rate=%d channels=%d bits=%d format=%d\n",
          rate_, channels_, bits_, format_);
  return true;
}

//
// create the listening server socket and start listening.
//
void
NetworkSoundIn::setup_listen()
{
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if(listen_fd_ < 0){
    fprintf(stderr, "NetworkSoundIn: socket() failed: %s\n", strerror(errno));
    exit(1);
  }

  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port_);
  if(addr_.size() == 0 || addr_ == "*" || addr_ == "0.0.0.0"){
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if(inet_pton(AF_INET, addr_.c_str(), &sin.sin_addr) != 1){
    fprintf(stderr, "NetworkSoundIn: bad address %s\n", addr_.c_str());
    exit(1);
  }

  if(bind(listen_fd_, (struct sockaddr *) &sin, sizeof(sin)) < 0){
    fprintf(stderr, "NetworkSoundIn: bind(%s:%d) failed: %s\n",
            addr_.c_str(), port_, strerror(errno));
    exit(1);
  }

  if(listen(listen_fd_, 1) < 0){
    fprintf(stderr, "NetworkSoundIn: listen() failed: %s\n", strerror(errno));
    exit(1);
  }

  fprintf(stderr, "NetworkSoundIn: listening on %s:%d\n",
          addr_.size() ? addr_.c_str() : "0.0.0.0", port_);
}

//
// accept one connection and read its WAV header.
// retries until it gets a connection with a valid header.
//
bool
NetworkSoundIn::accept_and_header()
{
  while(1){
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int c = accept(listen_fd_, (struct sockaddr *) &peer, &plen);
    if(c < 0){
      if(errno == EINTR)
        continue;
      fprintf(stderr, "NetworkSoundIn: accept() failed: %s\n", strerror(errno));
      return false;
    }

    // low latency for streamed audio.
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    fd_ = c;
    fprintf(stderr, "NetworkSoundIn: accepted connection from %s:%d\n",
            inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

    if(raw_){
      // no header: the caller specified the format.
      fprintf(stderr, "NetworkSoundIn: raw PCM stream rate=%d channels=%d bits=%d\n",
              rate_, channels_, bits_);
      return true;
    }

    if(read_header()){
      return true;
    }

    // bad header; drop this client and wait for another.
    ::close(fd_);
    fd_ = -1;
  }
}

//
// convert one frame's channel-0 sample to a double in [-1, 1].
//
static double
net_sample(const unsigned char *p, int format, int bits)
{
  if(format == 3 && bits == 32){
    float f;
    memcpy(&f, p, 4);
    return f;
  } else if(bits == 8){
    // 8-bit WAV PCM is unsigned, centred at 128.
    return (((int) p[0]) - 128) / 128.0;
  } else if(bits == 16){
    short s = (short) (p[0] | (p[1] << 8));
    return s / 32768.0;
  } else if(bits == 24){
    int v = p[0] | (p[1] << 8) | (p[2] << 16);
    if(v & 0x800000)
      v |= ~0xffffff; // sign-extend
    return v / 8388608.0;
  } else if(bits == 32){
    int v = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned) p[3] << 24);
    return v / 2147483648.0;
  }
  return 0;
}

//
// background thread: read PCM from the connection and append to
// the circular buffer. re-accepts a new connection if the current
// one closes, so ft8mon keeps running as a persistent server.
//
void
NetworkSoundIn::reader_loop()
{
  int frame_bytes = channels_ * (bits_ / 8);
  std::vector<unsigned char> acc; // leftover partial-frame bytes

  while(1){
    unsigned char tmp[8192];
    int cc = ::read(fd_, tmp, sizeof(tmp));
    if(cc <= 0){
      // connection closed; wait for the next client.
      fprintf(stderr, "NetworkSoundIn: connection closed, waiting for a new one\n");
      ::close(fd_);
      fd_ = -1;
      acc.clear();
      int saved_rate = rate_, saved_ch = channels_, saved_bits = bits_;
      if(!accept_and_header()){
        return;
      }
      if(rate_ != saved_rate || channels_ != saved_ch || bits_ != saved_bits){
        fprintf(stderr, "NetworkSoundIn: new stream format differs from the first; "
                "keeping original rate=%d channels=%d bits=%d\n",
                saved_rate, saved_ch, saved_bits);
        rate_ = saved_rate;
        channels_ = saved_ch;
        bits_ = saved_bits;
        frame_bytes = channels_ * (bits_ / 8);
      }
      continue;
    }

    acc.insert(acc.end(), tmp, tmp + cc);

    int nframes = acc.size() / frame_bytes;
    for(int f = 0; f < nframes; f++){
      // channel 0 is the first sample in the frame.
      double s = net_sample(&acc[f * frame_bytes], format_, bits_);
      if(((wi_ + 1) % n_) != ri_){
        buf_[wi_] = s;
        wi_ = (wi_ + 1) % n_;
      } else {
        // buffer full: drop the oldest sample to keep the newest.
        ri_ = (ri_ + 1) % n_;
        buf_[wi_] = s;
        wi_ = (wi_ + 1) % n_;
      }
    }
    // UNIX time of the most recent sample we just stored.
    time_ = now();

    if(nframes > 0){
      acc.erase(acc.begin(), acc.begin() + nframes * frame_bytes);
    }
  }
}

void
NetworkSoundIn::start()
{
  setup_listen();

  // block until the first client connects and sends a valid header,
  // so that rate() returns the real sample rate to the main loop.
  if(!accept_and_header()){
    fprintf(stderr, "NetworkSoundIn: could not accept a connection\n");
    exit(1);
  }

  // allocate a 30-second circular buffer.
  n_ = rate_ * 30;
  buf_ = (double *) malloc(sizeof(double) * n_);
  assert(buf_);
  wi_ = 0;
  ri_ = 0;

  reader_ = new std::thread( [this]() { this->reader_loop(); } );
}

//
// read a bunch of recent sound samples. same semantics as
// CardSoundIn::get(): return up to n samples, or fewer if that
// is all that is buffered; if latest==1, discard all but the
// most recent n samples. t0 is the UNIX time of the first
// returned sample.
//
std::vector<double>
NetworkSoundIn::get(int n, double &t0, int latest)
{
  std::vector<double> v;

  if(time_ < 0 && wi_ == ri_){
    // no input has ever arrived.
    t0 = -1;
    return v;
  }

  if(latest){
    while(((wi_ + n_ - ri_) % n_) > n){
      ri_ = (ri_ + 1) % n_;
    }
  }

  // UNIX time of the first sample in buf_ (buf_[ri_]).
  t0 = time_; // time of most recent sample, buf_[wi_-1].
  if(wi_ > ri_){
    t0 -= (wi_ - ri_) * (1.0 / rate_);
  } else {
    t0 -= ((wi_ + n_) - ri_) * (1.0 / rate_);
  }

  while((int) v.size() < n){
    if(ri_ == wi_){
      break;
    }
    v.push_back(buf_[ri_]);
    ri_ = (ri_ + 1) % n_;
  }

  return v;
}

//
// convert I/Q to USB via the phasing method.
// uses FFTs over the whole of a[], so it's slow.
// and the results are crummy at the start and end.
//
std::vector<double>
iq2usb(const std::vector<std::complex<double>> &a)
{
#if 0
  int slop = 10000;
  std::vector<std::complex<double>> a(aa.size() + 2*slop);
  for(int i = 0; i < (int)a.size(); i++){
    if(i - slop >= 0 && i - slop < (int) aa.size()){
      a[i] = aa[i-slop];
    } else {
      a[i] = aa[i % slop];
    }
  }
#endif
  
  std::vector<double> ii = vreal(analytic(vreal(a), "snd::iq2usb_i"));
  std::vector<double> qq = vimag(analytic(vimag(a), "snd::iq2usb_q"));
  std::vector<double> ssb(ii.size());
  for(int i = 0; i < (int) ii.size(); i++){
    ssb[i] = ii[i] - qq[i];
  }

#if 0
  ssb.erase(ssb.begin(), ssb.begin()+slop);
  ssb.resize(aa.size());
#endif
  
  return ssb;
}

#ifdef USE_AIRSPYHF
//
// chan argument is serial[,megahertz].
// e.g. 3B52EB5DAC35398D,14.074
// or -,megahertz
// or just serial #
//
AirspySoundIn::AirspySoundIn(std::string chan, int rate)
{
  device_ = 0;
  //hz_ = 1000000.0 * atof(chan.c_str());
  hz_ = 10 * 1000 * 1000;
  air_rate_ = 192 * 1000;;
  time_ = -1;
  count_ = 0;
  strcpy(hostname_, "???");
  gethostname(hostname_, sizeof(hostname_));
  hostname_[sizeof(hostname_)-1] = '\0';

  if(rate == -1){
    rate_ = 12000;
  } else {
    rate_ = rate;
  }

  int comma = chan.find(",");
  if(comma >= 0){
    hz_ = atof(chan.c_str() + comma + 1) * 1000000.0;
  }

  if(comma == 0 || chan.size() < 1 || chan[0] == '-'){
    if(airspyhf_open(&device_) != AIRSPYHF_SUCCESS ) {
      fprintf(stderr, "airspyhf_open() failed\n");
      exit(1);
    }
  } else {
    // open by Airspy HF+ serial number.
    uint64_t serial = strtoull(chan.c_str(), 0, 16);
    if(airspyhf_open_sn(&device_, serial) != AIRSPYHF_SUCCESS ) {
      fprintf(stderr, "airspyhf_open_sn(%llx) failed\n", (unsigned long long) serial);
      exit(1);
    }
  }
  
  serial_ = get_serial();

  if (airspyhf_set_samplerate(device_, air_rate_) != AIRSPYHF_SUCCESS) {
    fprintf(stderr, "airspyhf_set_samplerate(%d) failed\n", air_rate_);
    exit(1);
  }

#if 0
  if (airspyhf_set_hf_lna(device_, 1) != AIRSPYHF_SUCCESS) {
    fprintf(stderr, "airspyhf_set_hf_lna(1) failed\n");
    exit(1);
  }
#endif

#if 0
  if (airspyhf_set_hf_att(device_, 5) != AIRSPYHF_SUCCESS) {
    fprintf(stderr, "airspyhf_set_hf_att(5) failed\n");
    exit(1);
  }
#endif

  // allocate a 60-second circular buffer
  n_ = rate_ * 60;
  buf_ = (std::complex<double> *) malloc(sizeof(std::complex<double>) * n_);
  assert(buf_);
  wi_ = 0;
  ri_ = 0;

  // Liquid DSP filter + resampler to convert 192000 to 6000.
  int h_len = estimate_req_filter_len(0.01, 60.0);
  float h[h_len];
  double cutoff = (rate_ / (double) air_rate_) / 2.0;
  liquid_firdes_kaiser(h_len, cutoff, 60.0, 0.0, h);

  assert((air_rate_ % rate_) == 0);
  filter_ = firfilt_crcf_create(h, h_len);
}

unsigned long long
AirspySoundIn::get_serial()
{
  airspyhf_read_partid_serialno_t sn;
  airspyhf_board_partid_serialno_read(device_, &sn);
  unsigned long long x;
  x = ((unsigned long long) sn.serial_no[0]) << 32;
  x |= (unsigned long long) sn.serial_no[1];
  return x;
}

int
AirspySoundIn::set_freq(int hz)
{
  if(hz > 31000000 && hz < 60000000){
    // Airspy HF+ specifications say 0.5kHz .. 31 MHz and 60..260 MHz.
    // Thus not the 6-meter band.
    fprintf(stderr, "airspy %llx: unsupported frequency %d\n", serial_, hz);
  }

  int ret = airspyhf_set_freq(device_, hz);
  if(ret != AIRSPYHF_SUCCESS){
    fprintf(stderr, "airspyhf_set_freq(%d) failed.\n", hz);
    exit(1);
  }
  hz_ = hz;
  
  return hz;
}

void
AirspySoundIn::start()
{
  int ret = airspyhf_start(device_, cb1, this);
  if(ret != AIRSPYHF_SUCCESS){
    fprintf(stderr, "airspyhf_start() failed.\n");
    exit(1);
  }

  set_freq(hz_);
}

int
AirspySoundIn::cb1(airspyhf_transfer_t *transfer)
{
  AirspySoundIn *sin = (AirspySoundIn *) transfer->ctx;
  return sin->cb2(transfer);
}

int
AirspySoundIn::cb2(airspyhf_transfer_t *transfer)
{
  // transfer->sample_count
  // transfer->samples
  // transfer->ctx
  // transfer->dropped_samples
  // each sample is an I/Q pair of 32-bit floats

  if(transfer->dropped_samples){
    fprintf(stderr, "airspy %s %llx (%.3f MHz) dropped_samples %d, sample_count %d\n",
            hostname_,
            serial_,
            hz_ / 1000000.0,
            (int)transfer->dropped_samples,
            (int)transfer->sample_count);
  }

  time_ = now();

  airspyhf_complex_float_t *buf = transfer->samples;

  FILE *fp = 0;
  
  if(0){
    fp = fopen("airspy.dat", "a");
    assert(fp);
  }

  for(int i = 0; i < (int)(transfer->sample_count + transfer->dropped_samples); i++){
    // low-pass filter, preparatory to rate reduction.
    liquid_float_complex x, y;
    if(i < transfer->sample_count){
      x.real = buf[i].re;
      x.imag = buf[i].im;
    } else {
      x.real = x.imag = 0;
    }

    if(fp){
      double dd;
      dd = x.real;
      fwrite(&dd, sizeof(dd), 1, fp);
      dd = x.imag;
      fwrite(&dd, sizeof(dd), 1, fp);
    }
    
    firfilt_crcf_push(filter_, x);
    firfilt_crcf_execute(filter_, &y);

    if((count_ % (air_rate_ / rate_)) == 0){
      if(((wi_ + 1) % n_) != ri_){
        // XXX what is the range of buf[i].re? 0..1?
        buf_[wi_] = std::complex<double>(y.real, y.imag);
        wi_ = (wi_ + 1) % n_;
      } else {
#if 0
        fprintf(stderr, "AirspySoundIn::cb buf_ overflow, serial=%llx\n",
                serial_);
#endif
        break;
      }
    }

    count_ += 1;
  }

  if(fp)
    fclose(fp);

  return 0;
}

// UNIX time of first sample in t0.
std::vector<double>
AirspySoundIn::get(int n, double &t0, int latest)
{
  std::vector<double> nothing;

  if(time_ < 0 && wi_ == ri_){
    // no input has ever arrived.
    t0 = -1;
    return nothing;
  }

  if(latest){
    while(((wi_ + n_ - ri_) % n_) > n){
      ri_ = (ri_ + 1) % n_;
    }
  }

  // calculate time of first sample in buf_.
  // XXX there's a race here with cb().
  t0 = time_; // time of last sample in buf_.
  if(wi_ >= ri_){
    t0 -= (wi_ - ri_) * (1.0 / rate_);
  } else {
    t0 -= ((wi_ + n_) - ri_) * (1.0 / rate_);
  }

  std::vector<std::complex<double>> v1;
  while((int) v1.size() < n){
    if(ri_ == wi_){
      break;
    }
    v1.push_back(buf_[ri_]);
    ri_ = (ri_ + 1) % n_;
  }

  if(v1.size() < 2){
    // analytic() demands more than one sample.
    return vreal(v1);
  } else {
    // pad to increase chances that we can re-use an FFT plan.
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
    if(needed != quantum){
      v1.resize(v1.size() + needed, 0.0);
    }
    std::vector<double> v2 = iq2usb(v1);
    if((int) v2.size() > olen){
      v2.resize(olen);
    }
    return v2;
  }
}
#endif

#if 0
void
eval_filter(float h[], int h_len, int rate)
{
  for(double hz = 4000; hz < rate / 2 && hz < 8000; hz += 20){
    firfilt_crcf ff = firfilt_crcf_create(h, h_len);
    double phase = 0;
    double sum = 0;
    int nn = 100 * h_len;
    for(int i = 0; i < nn; i++){
      liquid_float_complex x, y;
      x.real = cos(phase);
      x.imag = sin(phase);
      phase += hz * 2 * M_PI / rate;
      firfilt_crcf_push(ff, x);
      firfilt_crcf_execute(ff, &y);
      sum += sqrt(y.real*y.real + y.imag*y.imag);
    }
    sum /= nn;
    printf("%.1f %f\n", hz, sum);
    fflush(stdout);
    firfilt_crcf_destroy(ff);
  }
}
#endif

#ifdef USE_HPSDR
HPSDRSoundIn::HPSDRSoundIn(std::string chan, int rate)
{
  assert(rate > 0);
  rate_ = rate;

  int comma = chan.find(",");
  if(comma >= 0){
    hz_ = atof(chan.c_str() + comma + 1) * 1000000.0;
  } else {
    fprintf(stderr, "HPSDR needs chan IPAddress,MHz or just ,MHz\n");
    exit(1);
  }

  if(comma == 0 || chan.size() < 1 || chan[0] == '-'){
    // open any HPSDR found on the ethernet.
    sdr_ = HPSDR::open();
    unit_ = sdr_->allocate_unit(rate_);
  } else {
    // open an HPSDR at a specified IP address.
    assert(0);
  }

  sdr_->set_freq(unit_, hz_);
  
  time_ = -1;
}

void
HPSDRSoundIn::start()
{
}

int
HPSDRSoundIn::set_freq(int hz)
{
  hz_ = hz;
  sdr_->set_freq(unit_, hz);
  return hz;
}

//
// read waiting HPSDR input, filter, down-sample, buffer.
//
void
HPSDRSoundIn::absorb()
{
  std::vector<std::complex<double>> v;

  sdr_->get(unit_, v);
  if(v.size() == 0)
    return;

  if(0){
    FILE *fp = fopen("hpsdr.dat", "a");
    assert(fp);
    for(int i = 0; i < (int) v.size(); i++){
      double x = v[i].real();
      fwrite(&x, sizeof(x), 1, fp);
      x = v[i].imag();
      fwrite(&x, sizeof(x), 1, fp);
    }
    fclose(fp);
  }

  time_ = now();

  buf_.insert(buf_.begin(), v.begin(), v.end());

  if((int) buf_.size() > 100 * rate_){
    fprintf(stderr, "HPSDRSoundIn: buf_ too big\n");
    buf_.clear();
  }
}

//
// convert from I/Q to upper sideband.
// sample rate has already been reduced (e.g. from 48000 to 12000).
// modifies v1[].
//
std::vector<double>
HPSDRSoundIn::usb(std::vector<std::complex<double>> &v1)
{
  if(v1.size() < 2){
    // analytic() demands more than one sample.
    return vreal(v1);
  } else {
    // pad to increase chances that we can re-use an FFT plan.
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
    if(needed != quantum){
      v1.resize(v1.size() + needed, 0.0);
    }
    std::vector<double> v2 = iq2usb(v1);
    if((int) v2.size() > olen){
      v2.resize(olen);
    }
    return v2;
  }
}

//
// return I/Q samples.
//
std::vector<std::complex<double>>
HPSDRSoundIn::get_iq(int n, double &t0, int latest)
{
  // read and rate-reduce HPSDR input, place in buf_.
  absorb();

  if(time_ < 0 && buf_.size() == 0){
    // no input has ever arrived.
    t0 = -1;
    return std::vector<std::complex<double>>();
  }

  if(latest && (int) buf_.size() > n){
    buf_.erase(buf_.begin(), buf_.begin() + (buf_.size() - n));
  }

  // time of first sample in buf_.
  t0 = time_ - buf_.size() / (double) rate_;

  int nn = std::min(n, (int) buf_.size());
  std::vector<std::complex<double>> v1(buf_.begin(), buf_.begin() + nn);
  buf_.erase(buf_.begin(), buf_.begin() + nn);

  return v1;
}

//
// read a bunch of recent sound samples.
// read up to n samples, no more.
// return immediately with whatever samples exist,
// perhaps fewer than n.
// return UNIX time of first sample in t0.
// if latest==1, return (up to) the most recent n samples
// and discard any earlier samples.
// converts from I/Q to upper sideband.
//
std::vector<double>
HPSDRSoundIn::get(int n, double &t0, int latest)
{
  std::vector<std::complex<double>> v1 = get_iq(n, t0, latest);
  if(v1.size() == 0){
    std::vector<double> nothing;
    return nothing;
  }
  std::vector<double> rrr = usb(v1);
  return rrr;
}
#endif

// chan should be e.g. 192.168.3.140
CloudSoundIn::CloudSoundIn(std::string chan, int rate)
{
  printf("%d\n", rate);
  assert(rate == 8000);
  chan_ = chan;
  sdr_ = new CloudSDR();
}

void
CloudSoundIn::start()
{
  sdr_->open(chan_);
}

//
// read a bunch of recent sound samples.
// read up to n samples, no more.
// return immediately with whatever samples exist,
// perhaps fewer than n.
// return UNIX time of first sample in t0.
// if latest==1, return (up to) the most recent n samples
// and discard any earlier samples.
//
std::vector<double>
CloudSoundIn::get(int n, double &t0, int latest)
{
  double lastt;
  std::vector<short> v0 = sdr_->get(lastt);

  int skip = 0;
  if(latest && n > 0){
    if((int) v0.size() > n){
      skip = v0.size() - n;
    }
  } else if(latest){
    assert(0);
  } else if(n > 0){
    assert(0);
  }

  std::vector<double> v1(v0.size() - skip);
  for(int i = 0; i < (int) v1.size(); i++){
    v1[i] = v0[i+skip];
  }
  
  t0 = lastt - (v1.size() / (double) rate());
  return v1;
}

int
CloudSoundIn::set_freq(int hz)
{
  sdr_->set_frequency(hz);
  return hz;
}

#if USE_SDRIP
// chan should be e.g. 192.168.3.140
SDRIPSoundIn::SDRIPSoundIn(std::string chan, int rate)
{
  rate_ = rate;
  chan_ = chan;
  sdr_ = new SDRIP(rate);
}

void
SDRIPSoundIn::start()
{
  sdr_->open(chan_);
}

//
// read waiting SDRIP input, filter, down-sample, buffer.
//
void
SDRIPSoundIn::absorb()
{
  std::vector<std::complex<double>> v;

  sdr_->get(v);
  if(v.size() == 0)
    return;

  time_ = now();

  buf_.insert(buf_.begin(), v.begin(), v.end());

  if((int) buf_.size() > 100 * rate_){
    fprintf(stderr, "SDRIPSoundIn: buf_ too big\n");
    buf_.clear();
  }
}

//
// convert from I/Q to upper sideband.
// (sdrip.cc has already reduced the rate (if needed).)
// modifies v1[].
//
std::vector<double>
SDRIPSoundIn::usb(std::vector<std::complex<double>> &v1)
{
  if(v1.size() < 2){
    // analytic() demands more than one sample.
    return vreal(v1);
  } else {
    // pad to increase chances that we can re-use an FFT plan.
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
    if(needed != quantum){
      v1.resize(v1.size() + needed, 0.0);
    }
    std::vector<double> v2 = iq2usb(v1);
    if((int) v2.size() > olen){
      v2.resize(olen);
    }
    return v2;
  }
}

//
// return I/Q samples.
//
std::vector<std::complex<double>>
SDRIPSoundIn::get_iq(int n, double &t0, int latest)
{
  // read waiting SDRIP input, place in buf_.
  absorb();

  if(time_ < 0 && buf_.size() == 0){
    // no input has ever arrived.
    t0 = -1;
    return std::vector<std::complex<double>>();
  }

  if(latest && (int) buf_.size() > n){
    buf_.erase(buf_.begin(), buf_.begin() + (buf_.size() - n));
  }

  // time of first sample in buf_.
  t0 = time_ - buf_.size() / (double) rate_;

  int nn = std::min(n, (int) buf_.size());
  std::vector<std::complex<double>> v1(buf_.begin(), buf_.begin() + nn);
  buf_.erase(buf_.begin(), buf_.begin() + nn);

  return v1;
}

//
// read a bunch of recent sound samples.
// read up to n samples, no more.
// return immediately with whatever samples exist,
// perhaps fewer than n.
// return UNIX time of first sample in t0.
// if latest==1, return (up to) the most recent n samples
// and discard any earlier samples.
//
std::vector<double>
SDRIPSoundIn::get(int n, double &t0, int latest)
{
  std::vector<std::complex<double>> v1 = get_iq(n, t0, latest);
  if(v1.size() == 0){
    std::vector<double> nothing;
    return nothing;
  }
  std::vector<double> rrr = usb(v1);
  return rrr;
}

int
SDRIPSoundIn::set_freq(int hz)
{
  sdr_->set_frequency(hz);
  return hz;
}
#endif

SoundOut *
SoundOut::open(const std::string card, const std::string chan, int rate)
{
  assert(card.size() > 0);
  SoundOut *sout;

  if(isdigit(card[0])){
    assert(chan == "0");
    sout = new CardSoundOut(atoi(card.c_str()), rate);
#ifdef USE_HPSDR
  } else if(card == "hpsdr"){
    sout = new HPSDRSoundOut(chan, rate);
#endif
  } else {
    fprintf(stderr, "SoundOut::open(%s): type not recognized\n",
            card.c_str());
    exit(1);
  }

  return sout;
}

#ifdef USE_HPSDR
HPSDRSoundOut::HPSDRSoundOut(const std::string chan, int rate)
{
  rate_ = rate;
  hz_ = 0;

  // open any HPSDR found on the ethernet.
  sdr_ = HPSDR::open();
}

void
HPSDRSoundOut::start()
{
}

void
HPSDRSoundOut::write(const std::vector<double> &v)
{
  sdr_->tx_real(hz_, v, rate_);
}
#endif

CardSoundOut::CardSoundOut(int card, int rate)
{
  card_ = card;
  rate_ = rate;
}

void
CardSoundOut::start()
{
  snd_init();

  PaStreamParameters op;
  memset(&op, 0, sizeof(op));
  op.device = card_;
  op.channelCount = 1;
  op.sampleFormat = paInt16;
  // latency must be the same as for input card.
  op.suggestedLatency = Pa_GetDeviceInfo(card_)->defaultLowInputLatency;
  op.hostApiSpecificStreamInfo = 0;
  
  str_ = 0;
  PaError err = Pa_OpenStream(&str_,
                              0,
                              &op,
                              rate_,
                              0, // framesPerBuffer
                              0,
                              0,
                              (void*) 0);
  if(err != paNoError){
    fprintf(stderr, "Pa_OpenStream(card=%d,rate=%d) failed for output: %s\n",
            card_, rate_, Pa_GetErrorText(err));
    exit(1);
  }

  err = Pa_StartStream(str_);
  if(err != paNoError){
    fprintf(stderr, "Pa_StartStream failed\n");
    exit(1);
  }

}

//
// Pa_WriteStream may block, but it does have some internal buffering.
// the amount seems to be controlled by suggestedLatency.
//
void
CardSoundOut::write(const std::vector<short int> &v)
{
  PaError err = Pa_WriteStream(str_, v.data(), v.size());
  if(err != paNoError && err != paOutputUnderflowed){
    fprintf(stderr, "Pa_WriteStream failed %d %s\n", err, Pa_GetErrorText(err));
    exit(1);
  }
}

void
CardSoundOut::write(const std::vector<double> &v)
{
  std::vector<short int> vv(v.size());
  for(int i = 0; i < (int) v.size(); i++){
    if(v[i] > 1.0){
      fprintf(stderr, "CardSoundOut::write() oops %f\n", v[i]);
    }
    vv[i] = v[i] * 16380;
  }
  write(vv);
}

//
// functions for Python to call via ctypes.
//

extern "C" {
  void *ext_snd_in_open(const char *card, const char *chan, int rate);
  int ext_snd_in_read(void *, double *, int, double *);
  int ext_snd_in_freq(void *, int);
  void *ext_snd_out_open(const char *card, const char *chan, int rate);
  int ext_snd_out_freq(void *, int);
  void ext_snd_out_write(void *, double *, int);
}

void *
ext_snd_in_open(const char *card, const char *chan, int rate)
{
  SoundIn *sin = SoundIn::open(card, chan, rate);
  sin->start();
  return (void *) sin;
}

//
// reads up to maxout samples.
// non-blocking.
// *tm will be set to UNIX time of last sample in out[].
// return value is number of samples written to out[].
//
int
ext_snd_in_read(void *thing, double *out, int maxout, double *tm)
{
  SoundIn *sin = (SoundIn *) thing;
  double t0; // time of first sample.

  // the "1" argument to get() means return the latest maxout samples,
  // and discard samples older than that!

  int n;
  if(sin->has_iq()){
    // return I/Q pairs.
    std::vector<std::complex<double>> v = sin->get_iq(maxout / 2, t0, 1);
    assert((int)v.size()*2 <= maxout);
    for(int i = 0; i*2 < maxout && i < (int) v.size(); i++){
      out[i*2+0] = v[i].real();
      out[i*2+1] = v[i].imag();
    }
    *tm = t0 + v.size() * (1.0 / sin->rate()); // time of last sample.
    n = v.size() * 2;
  } else {
    // return ordinary audio samples.
    std::vector<double> v = sin->get(maxout, t0, 1);

    assert((int) v.size() <= maxout);
    for(int i = 0; i < maxout && i < (int) v.size(); i++){
      out[i] = v[i];
    }
    *tm = t0 + v.size() / (double)sin->rate(); // time of last sample.
    n = v.size();
  }

  return n;
}

int
ext_snd_in_freq(void *thing, int hz)
{
  SoundIn *sin = (SoundIn *) thing;
  int x = sin->set_freq(hz);
  return x;
}

//
// open a sound card or HPSDR for output.
//
void *
ext_snd_out_open(const char *card, const char *chan, int rate)
{
  SoundOut *sout = SoundOut::open(card, chan, rate);
  sout->start();
  return (void *) sout;
}

int
ext_snd_out_freq(void *thing, int hz)
{
  SoundOut *sout = (SoundOut *) thing;
  sout->set_freq(hz);
  return hz;
}

void
ext_snd_out_write(void *thing, double *buf, int n)
{
  SoundOut *sout = (SoundOut *) thing;
  std::vector<double> v(buf, buf + n);
  sout->write(v);
}
