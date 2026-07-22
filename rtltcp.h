#ifndef rtltcp_h
#define rtltcp_h 1

//
// read from a remote rtl_tcp server (RTL-SDR over TCP) and turn the
// raw I/Q stream into FT8 audio, so a running rtl_tcp can be used as
// a "card":
//
//   ./ft8mon -card rtltcp host:port,megahertz[,up:megahertz]
//
// e.g.  ./ft8mon -card rtltcp 192.168.1.50:1234,14.074
//   with a 125 MHz upconverter:
//       ./ft8mon -card rtltcp 192.168.1.50:1234,14.074,up:125
//
// ft8mon is a plain TCP client here; rtl_tcp does the USB device
// handling. The DSP (a numerically-controlled oscillator to shift the
// dial frequency to baseband, a low-pass FIR, and decimation to the
// audio rate) is done here, without any external DSP library. The
// resulting I/Q is turned into upper sideband by the shared iq2usb().
//

#include <complex>
#include <string>
#include <vector>
#include <thread>
#include "snd.h"

class RTLTCPSoundIn : public SoundIn {
 private:
  std::string host_;
  int port_;
  int hz_;         // dial (USB carrier) frequency, e.g. 14074000
  int rate_;       // audio sample rate, e.g. 12000
  int dev_rate_;   // rtl_tcp sample rate, e.g. 240000 (a multiple of rate_)
  int offset_;     // hardware is tuned offset_ Hz below the dial frequency
  int upconverter_;// upconverter LO in Hz (0 if none); tuner sees dial + LO
  int ppm_;        // frequency correction
  int fd_;         // TCP connection to rtl_tcp, or -1
  double time_;    // UNIX time of most recent audio sample

  // NCO state (mixes the dial frequency down to 0 Hz).
  double nco_acc_;
  double nco_step_;

  // decimating low-pass FIR.
  std::vector<double> h_;                     // filter taps
  std::vector<std::complex<double>> dl_;      // delay line, size h_.size()
  int dpos_;                                  // next write position in dl_
  long long incount_;                         // input samples since start

  // circular buffer of decimated I/Q (audio-rate) samples.
  int n_;
  std::complex<double> *buf_;
  volatile int wi_;
  volatile int ri_;

  std::thread *reader_;
  volatile bool stop_; // set by the destructor to end reader_loop()

  void build_filter();
  bool connect_and_configure();
  void send_cmd(unsigned char cmd, unsigned int param);
  bool read_exact(void *buf, int n);
  void push_sample(std::complex<double> s); // NCO + FIR + decimate
  void store_output(std::complex<double> s); // into circular buffer
  void reader_loop();

 public:
  RTLTCPSoundIn(std::string chan, int rate);
  ~RTLTCPSoundIn();
  void start();
  std::vector<double> get(int n, double &t0, int latest);
  int rate() { return rate_; }
  int set_freq(int hz);
};

#endif
