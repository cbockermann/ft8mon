//
// ft8gen -- encode a text message into an FT8 audio waveform.
//
// the inverse of the ft8mon decoder: text -> 77 bits -> +14-bit CRC ->
// LDPC(174,91) -> 79 GFSK symbols -> 12.64 s of audio.
//
//   ft8gen "CQ AB1HL JO31"             # WAV to stdout
//   ft8gen "CQ AB1HL JO31" out.wav     # WAV to a file
//   ft8gen -hz 1500 "W1AW AB1HL -12"   # set the base audio frequency
//
// pipe straight into the decoder for a round-trip test:
//   ft8gen "CQ AB1HL JO31" | ft8mon -file -
//
// supports the standard i3=1 message type: two callsigns plus a grid,
// signal report (optionally R-prefixed), RRR, RR73 or 73, and CQ /
// directed CQ (e.g. "CQ DX ...").
//
// Robert Morris wrote ft8mon; this encoder reuses its building blocks
// (ft8_crc, the LDPC parity-check matrix, gfsk_r).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "util.h"

// from libldpc.c (compiled as C++ in this project, like ft8.cc expects)
extern void ft8_crc(int msg1[], int msglen, int out[14]);
extern int ldpc_check(int codeword[]);
// from arrays.h (compiled into libldpc.c): 83 parity-check rows,
// 1-origin indices into the 174-bit codeword, 0 = unused slot.
extern int Nm[][7];

// character sets, must match unpack.cc exactly.
static const char *C1 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 37
static const char *C2 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";  // 36
static const char *C3 = "0123456789";                            // 10
static const char *C4 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";           // 27

static const int NGBASE = 180 * 180;      // 32400
static const int NTOKENS = 2063592;
static const int MAX22 = 4194304;

static int
tabidx(const char *tab, char c)
{
  const char *p = strchr(tab, c);
  return p ? (int)(p - tab) : -1;
}

static void
die(const std::string &msg)
{
  fprintf(stderr, "ft8gen: %s\n", msg.c_str());
  exit(1);
}

// write len bits of val into a[start..], most-significant bit first.
static void
putbits(int a[], int start, int len, long long val)
{
  for(int i = 0; i < len; i++){
    a[start + i] = (val >> (len - 1 - i)) & 1;
  }
}

// pack a standard callsign (or CQ/DE/QRZ) into a 28-bit value.
static int
packcall(const std::string &callin)
{
  std::string call = callin;
  if(call == "DE") return 0;
  if(call == "QRZ") return 1;
  if(call == "CQ") return 2;

  // format into a 6-char field with the area digit at index 2.
  if(call.size() < 3 || call.size() > 6)
    die("cannot pack callsign '" + call + "'");

  std::string s;
  if(call.size() >= 2 && isdigit((unsigned char)call[1])){
    s = " " + call;        // one-char prefix, e.g. K1ABC -> " K1ABC"
  } else {
    s = call;              // two-char prefix, e.g. AB1CD / UA3FYL
  }
  while(s.size() < 6)
    s += " ";
  if(s.size() != 6 || !isdigit((unsigned char)s[2]))
    die("cannot pack callsign '" + call + "' (nonstandard format)");

  int i0 = tabidx(C1, s[0]);
  int i1 = tabidx(C2, s[1]);
  int i2 = tabidx(C3, s[2]);
  int i3 = tabidx(C4, s[3]);
  int i4 = tabidx(C4, s[4]);
  int i5 = tabidx(C4, s[5]);
  if(i0 < 0 || i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0)
    die("illegal character in callsign '" + call + "'");

  long long x = i0;
  x = x * 36 + i1;
  x = x * 10 + i2;
  x = x * 27 + i3;
  x = x * 27 + i4;
  x = x * 27 + i5;
  x += MAX22;
  x += NTOKENS;
  return (int)x;
}

// directed CQ, e.g. "CQ DX" / "CQ NA": up to 4 letters (space-padded).
static int
pack_directed_cq(const std::string &dir)
{
  if(dir.size() > 4)
    die("directed CQ '" + dir + "' too long");
  std::string s = dir;
  while(s.size() < 4)
    s += " ";
  long long x = 0;
  for(int i = 0; i < 4; i++){
    int j = tabidx(C4, s[i]);
    if(j < 0)
      die("illegal character in directed CQ '" + dir + "'");
    x = x * 27 + j;
  }
  return (int)(1003 + x);
}

// pack a grid / report / acknowledgement into 15 bits; sets ir (the
// R-prefix "roger" bit) for R-reports.
static int
packgrid(const std::string &exin, int &ir)
{
  ir = 0;
  std::string ex = exin;

  if(ex.size() == 0)
    return NGBASE + 1;         // blank
  if(ex == "RRR")  return NGBASE + 2;
  if(ex == "RR73") return NGBASE + 3;
  if(ex == "73")   return NGBASE + 4;

  // 4-char Maidenhead grid, e.g. JO31 / FN42.
  if(ex.size() == 4 &&
     ex[0] >= 'A' && ex[0] <= 'R' && ex[1] >= 'A' && ex[1] <= 'R' &&
     isdigit((unsigned char)ex[2]) && isdigit((unsigned char)ex[3])){
    int x1 = ex[0] - 'A';
    int x2 = ex[1] - 'A';
    int x3 = ex[2] - '0';
    int x4 = ex[3] - '0';
    return x1 * (18 * 10 * 10) + x2 * (10 * 10) + x3 * 10 + x4;
  }

  // signal report, optionally R-prefixed: R-12, -07, +03 ...
  if(ex[0] == 'R'){
    ir = 1;
    ex = ex.substr(1);
  }
  if(ex.size() >= 2 && (ex[0] == '+' || ex[0] == '-')){
    int db = atoi(ex.c_str());
    int v = db + 35;
    if(v < 5 || v > NGBASE - 1)
      die("report out of range: " + exin);
    return NGBASE + v;
  }

  die("cannot parse exchange '" + exin + "'");
  return 0; // unreachable
}

// split on whitespace.
static std::vector<std::string>
split(const std::string &s)
{
  std::vector<std::string> out;
  std::string cur;
  for(char c : s){
    if(c == ' ' || c == '\t'){
      if(cur.size()) { out.push_back(cur); cur.clear(); }
    } else {
      cur += c;
    }
  }
  if(cur.size())
    out.push_back(cur);
  return out;
}

// strip a trailing /R (rover) suffix; returns 1 if present.
static int
strip_rover(std::string &call)
{
  if(call.size() > 2 && call.substr(call.size() - 2) == "/R"){
    call = call.substr(0, call.size() - 2);
    return 1;
  }
  return 0;
}

// text -> 77 message bits (i3=1).
static void
pack77(const std::string &msg, int a77[77])
{
  std::vector<std::string> t = split(msg);
  if(t.size() < 2)
    die("need at least two fields, e.g. \"CQ AB1HL JO31\"");

  int call1v, rover1 = 0, call2v, rover2 = 0, ir = 0, gridv;

  int idx; // index of call2 in the token list
  if(t[0] == "CQ" && t.size() >= 4 &&
     t[1].find_first_of("0123456789") == std::string::npos){
    // directed CQ: "CQ DX <call> <grid>"
    call1v = pack_directed_cq(t[1]);
    idx = 2;
  } else {
    std::string c1 = t[0];
    rover1 = strip_rover(c1);
    call1v = packcall(c1);
    idx = 1;
  }

  std::string c2 = t[idx];
  rover2 = strip_rover(c2);
  call2v = packcall(c2);

  std::string ex = (idx + 1 < (int)t.size()) ? t[idx + 1] : "";
  gridv = packgrid(ex, ir);

  int i = 0;
  putbits(a77, i, 28, call1v); i += 28;
  a77[i++] = rover1;
  putbits(a77, i, 28, call2v); i += 28;
  a77[i++] = rover2;
  a77[i++] = ir;
  putbits(a77, i, 15, gridv); i += 15;
  putbits(a77, i, 3, 1); i += 3;   // i3 = 1
  if(i != 77)
    die("internal: packed length != 77");
}

// 77 message bits -> 91 bits (append 14-bit CRC), exactly as the
// decoder's check_crc() computes it.
static void
add_crc(const int a77[77], int a91[91])
{
  int aa[82];
  for(int i = 0; i < 82; i++)
    aa[i] = (i < 77) ? a77[i] : 0;
  int crc[14];
  ft8_crc(aa, 82, crc);
  for(int i = 0; i < 77; i++)
    a91[i] = a77[i];
  for(int i = 0; i < 14; i++)
    a91[77 + i] = crc[i];
}

// GF(2) encode: compute the 83 parity bits so the 174-bit codeword
// satisfies the parity-check matrix. codeword = [91 message | 83 parity].
static void
ldpc_encode(const int a91[91], int a174[174])
{
  const int K = 91, M = 83, N = 174;

  // build the parity-check matrix H (M x N) from Nm.
  static std::vector<std::vector<char>> H;
  if(H.empty()){
    H.assign(M, std::vector<char>(N, 0));
    for(int j = 0; j < M; j++){
      for(int ii = 0; ii < 7; ii++){
        int idx = Nm[j][ii];
        if(idx != 0)
          H[j][idx - 1] = 1; // 1-origin
      }
    }
  }

  // A = H[:, K..N-1]  (M x M);  rhs[j] = sum H[:, 0..K-1] * message.
  std::vector<std::vector<char>> A(M, std::vector<char>(M, 0));
  std::vector<char> b(M, 0);
  for(int j = 0; j < M; j++){
    int r = 0;
    for(int k = 0; k < K; k++)
      r ^= (H[j][k] & a91[k]);
    b[j] = r;
    for(int k = 0; k < M; k++)
      A[j][k] = H[j][K + k];
  }

  // Gauss-Jordan over GF(2): solve A * p = b.
  for(int col = 0; col < M; col++){
    int piv = -1;
    for(int row = col; row < M; row++){
      if(A[row][col]){ piv = row; break; }
    }
    if(piv < 0)
      die("LDPC parity submatrix is singular (unexpected)");
    std::swap(A[piv], A[col]);
    std::swap(b[piv], b[col]);
    for(int row = 0; row < M; row++){
      if(row != col && A[row][col]){
        for(int k = col; k < M; k++)
          A[row][k] ^= A[col][k];
        b[row] ^= b[col];
      }
    }
  }

  for(int i = 0; i < K; i++)
    a174[i] = a91[i];
  for(int i = 0; i < M; i++)
    a174[K + i] = b[i];
}

// 174 bits -> 79 FSK-8 symbols (Costas sync + Gray-coded data),
// mirrors the decoder's recode().
static std::vector<int>
to_symbols(const int a174[174])
{
  int costas[] = { 3, 1, 4, 0, 6, 5, 2 };
  int graymap[] = { 0, 1, 3, 2, 5, 6, 4, 7 };
  std::vector<int> sym;
  int i174 = 0;
  for(int i = 0; i < 79; i++){
    if(i < 7){
      sym.push_back(costas[i]);
    } else if(i >= 36 && i < 43){
      sym.push_back(costas[i - 36]);
    } else if(i >= 72){
      sym.push_back(costas[i - 72]);
    } else {
      int s = (a174[i174] << 2) | (a174[i174 + 1] << 1) | a174[i174 + 2];
      i174 += 3;
      sym.push_back(graymap[s]);
    }
  }
  return sym;
}

// write 16-bit PCM mono WAV to a FILE*; header sizes are exact because
// we know the sample count up front, so it works on a pipe (no seek).
static void
write_wav(FILE *fp, const std::vector<double> &samples, int rate)
{
  double mx = 0;
  for(double v : samples)
    mx = std::max(mx, fabs(v));
  if(mx <= 0)
    mx = 1;

  int n = samples.size();
  unsigned int datasize = n * 2;
  auto w32 = [&](unsigned int v){ unsigned char b[4] = {(unsigned char)v,(unsigned char)(v>>8),(unsigned char)(v>>16),(unsigned char)(v>>24)}; fwrite(b,1,4,fp); };
  auto w16 = [&](unsigned int v){ unsigned char b[2] = {(unsigned char)v,(unsigned char)(v>>8)}; fwrite(b,1,2,fp); };

  fwrite("RIFF", 1, 4, fp); w32(36 + datasize); fwrite("WAVE", 1, 4, fp);
  fwrite("fmt ", 1, 4, fp); w32(16); w16(1); w16(1);
  w32(rate); w32(rate * 2); w16(2); w16(16);
  fwrite("data", 1, 4, fp); w32(datasize);
  for(double v : samples){
    double s = (v / mx) * 0.9 * 32767.0;
    int iv = (int)lround(s);
    if(iv > 32767) iv = 32767;
    if(iv < -32768) iv = -32768;
    w16((unsigned short)(short)iv);
  }
  fflush(fp);
}

int
main(int argc, char *argv[])
{
  double base_hz = 1500.0;
  std::string msg, outfile = "-";
  std::vector<std::string> pos;

  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "-hz") == 0 && i + 1 < argc){
      base_hz = atof(argv[++i]);
    } else {
      pos.push_back(argv[i]);
    }
  }
  if(pos.size() < 1){
    fprintf(stderr, "usage: ft8gen [-hz basehz] \"message\" [outfile|-]\n");
    exit(1);
  }
  msg = pos[0];
  if(pos.size() >= 2)
    outfile = pos[1];

  // encode chain.
  int a77[77];
  pack77(msg, a77);
  int a91[91];
  add_crc(a77, a91);
  int a174[174];
  ldpc_encode(a91, a174);

  // internal sanity: the codeword must satisfy all 83 parity checks.
  int score = ldpc_check(a174);
  if(score != 83)
    die("internal: encoded codeword fails LDPC check (" + std::to_string(score) + "/83)");

  std::vector<int> sym = to_symbols(a174);

  // GFSK: 6.25 Hz tone spacing, 0.16 s/symbol at 12000 sps.
  int rate = 12000;
  int symsamples = rate * 16 / 100; // 1920
  double spacing = 6.25;
  std::vector<double> gwin = gfsk_window(symsamples, 2.0);
  std::vector<double> samples = gfsk_r(sym, base_hz, base_hz, spacing,
                                       rate, symsamples, 0.0, gwin);

  FILE *fp;
  if(outfile == "-"){
    fp = stdout;
  } else {
    fp = fopen(outfile.c_str(), "wb");
    if(!fp)
      die("cannot open " + outfile);
  }
  write_wav(fp, samples, rate);
  if(fp != stdout)
    fclose(fp);

  fprintf(stderr, "ft8gen: \"%s\" -> %d samples (%.2f s) at %d Hz base, LDPC ok\n",
          msg.c_str(), (int)samples.size(), samples.size() / (double)rate, (int)base_hz);
  return 0;
}
