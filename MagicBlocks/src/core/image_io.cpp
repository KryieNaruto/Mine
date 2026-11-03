// MagicBlocks —— PNG 读写实现(无第三方依赖)
#include "core/image_io.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace magicblocks {

namespace {

constexpr std::uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

std::uint32_t crc_table[256];
bool crc_ready = false;

void init_crc() {
  if (crc_ready) return;
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_table[n] = c;
  }
  crc_ready = true;
}

std::uint32_t crc32_of(const std::uint8_t* data, std::size_t len) {
  init_crc();
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) c = crc_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32_of(const std::uint8_t* data, std::size_t len) {
  std::uint32_t a = 1, b = 0;
  for (std::size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void put_be32(std::vector<std::uint8_t>* v, std::uint32_t x) {
  v->push_back(static_cast<std::uint8_t>(x >> 24));
  v->push_back(static_cast<std::uint8_t>(x >> 16));
  v->push_back(static_cast<std::uint8_t>(x >> 8));
  v->push_back(static_cast<std::uint8_t>(x));
}

std::uint32_t get_be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void write_chunk(std::vector<std::uint8_t>* out, const char type[5],
                 const std::vector<std::uint8_t>& data) {
  put_be32(out, static_cast<std::uint32_t>(data.size()));
  std::vector<std::uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out->insert(out->end(), body.begin(), body.end());
  put_be32(out, crc32_of(body.data(), body.size()));
}

// ---- zlib:store 模式压缩(无压缩比,但绝对正确、零依赖) ----
std::vector<std::uint8_t> zlib_store(const std::vector<std::uint8_t>& raw) {
  std::vector<std::uint8_t> z;
  z.push_back(0x78);  // CMF: deflate, 32K window
  z.push_back(0x01);  // FLG: 无字典, 最快压缩(与 CMF 组合的校验位合法)
  std::size_t off = 0;
  while (off < raw.size()) {
    const std::size_t block = std::min<std::size_t>(65535, raw.size() - off);
    const bool last = (off + block >= raw.size());
    z.push_back(last ? 1 : 0);  // BFINAL + BTYPE=00 (store)
    z.push_back(static_cast<std::uint8_t>(block & 0xFF));
    z.push_back(static_cast<std::uint8_t>((block >> 8) & 0xFF));
    const std::uint16_t nlen = static_cast<std::uint16_t>(~block);
    z.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
    z.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
             raw.begin() + static_cast<std::ptrdiff_t>(off + block));
    off += block;
  }
  put_be32(&z, adler32_of(raw.data(), raw.size()));
  return z;
}

// ---- zlib 解码:store + 固定/动态 Huffman(够读 PIL/常规工具产出的 PNG) ----
struct BitReader {
  const std::uint8_t* p;
  std::size_t len;
  std::size_t pos = 0;
  std::uint32_t bitbuf = 0;
  int bitcnt = 0;
  bool bad = false;

  bool need(int n) {
    while (bitcnt < n) {
      if (pos >= len) return false;
      bitbuf |= static_cast<std::uint32_t>(p[pos++]) << bitcnt;
      bitcnt += 8;
    }
    return true;
  }
  int get(int n) {
    if (n == 0) return 0;
    if (!need(n)) {
      bad = true;
      return 0;
    }
    const int v = static_cast<int>(bitbuf & ((1u << n) - 1u));
    bitbuf >>= n;
    bitcnt -= n;
    return v;
  }
};

struct Huffman {
  // 用位串长度表做规范 Huffman 解码
  std::vector<int> count;
  std::vector<int> symbol;
};

void build_huffman(Huffman* h, const std::vector<std::uint8_t>& lengths) {
  const int maxbits = 15;
  h->count.assign(static_cast<std::size_t>(maxbits + 1), 0);
  for (auto l : lengths) h->count[l]++;
  h->count[0] = 0;
  std::vector<int> offs(static_cast<std::size_t>(maxbits + 1), 0);
  for (int i = 1; i <= maxbits; ++i) offs[i] = offs[i - 1] + h->count[i - 1];
  h->symbol.assign(lengths.size(), 0);
  for (std::size_t s = 0; s < lengths.size(); ++s) {
    if (lengths[s]) h->symbol[static_cast<std::size_t>(offs[lengths[s]]++)] = static_cast<int>(s);
  }
}

int decode_symbol(BitReader* br, const Huffman& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; ++len) {
    code |= br->get(1);
    if (br->bad) return -1;
    const int cnt = h.count[static_cast<std::size_t>(len)];
    if (code - first < cnt) return h.symbol[static_cast<std::size_t>(index + (code - first))];
    index += cnt;
    first = (first + cnt) << 1;
    code <<= 1;
  }
  return -1;
}

bool inflate_zlib(const std::uint8_t* in, std::size_t in_len, std::vector<std::uint8_t>* out) {
  if (in_len < 6) return false;
  if ((in[0] & 0x0F) != 8) return false;  // 只支持 deflate
  BitReader br{in + 2, in_len - 2, 0, 0, 0, false};

  static const int lenbase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const int lenext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const int distbase[] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                 33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const int distext[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

  Huffman lenfix{}, distfix{};
  std::vector<std::uint8_t> lf(288), df(30);
  for (int i = 0; i < 144; ++i) lf[static_cast<std::size_t>(i)] = 8;
  for (int i = 144; i < 256; ++i) lf[static_cast<std::size_t>(i)] = 9;
  for (int i = 256; i < 280; ++i) lf[static_cast<std::size_t>(i)] = 7;
  for (int i = 280; i < 288; ++i) lf[static_cast<std::size_t>(i)] = 8;
  for (int i = 0; i < 30; ++i) df[static_cast<std::size_t>(i)] = 5;
  build_huffman(&lenfix, lf);
  build_huffman(&distfix, df);

  bool last_block = false;
  while (!last_block) {
    last_block = br.get(1) != 0;
    const int type = br.get(2);
    if (br.bad) return false;
    if (type == 0) {
      br.bitcnt = 0;  // 丢弃残余位
      br.bitbuf = 0;
      if (br.pos + 4 > br.len) return false;
      const std::uint32_t len = br.p[br.pos] | (br.p[br.pos + 1] << 8);
      br.pos += 4;
      if (br.pos + len > br.len) return false;
      out->insert(out->end(), br.p + br.pos, br.p + br.pos + len);
      br.pos += len;
    } else if (type == 1 || type == 2) {
      Huffman lh{}, dh{};
      if (type == 1) {
        lh = lenfix;
        dh = distfix;
      } else {
        const int hlit = br.get(5) + 257;
        const int hdist = br.get(5) + 1;
        const int hclen = br.get(4) + 4;
        static const int order[] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        std::vector<std::uint8_t> cl(19, 0);
        for (int i = 0; i < hclen; ++i) cl[static_cast<std::size_t>(order[i])] = static_cast<std::uint8_t>(br.get(3));
        Huffman clh{};
        build_huffman(&clh, cl);
        std::vector<std::uint8_t> lens;
        lens.reserve(static_cast<std::size_t>(hlit + hdist));
        while (static_cast<int>(lens.size()) < hlit + hdist) {
          const int sym = decode_symbol(&br, clh);
          if (sym < 0) return false;
          if (sym < 16) {
            lens.push_back(static_cast<std::uint8_t>(sym));
          } else if (sym == 16) {
            if (lens.empty()) return false;
            const std::uint8_t prev = lens.back();
            const int rep = 3 + br.get(2);
            for (int i = 0; i < rep; ++i) lens.push_back(prev);
          } else if (sym == 17) {
            const int rep = 3 + br.get(3);
            for (int i = 0; i < rep; ++i) lens.push_back(0);
          } else {
            const int rep = 11 + br.get(7);
            for (int i = 0; i < rep; ++i) lens.push_back(0);
          }
        }
        if (static_cast<int>(lens.size()) < hlit + hdist) return false;
        std::vector<std::uint8_t> ll(lens.begin(), lens.begin() + hlit);
        std::vector<std::uint8_t> dl(lens.begin() + hlit, lens.begin() + hlit + hdist);
        build_huffman(&lh, ll);
        build_huffman(&dh, dl);
      }
      for (;;) {
        const int sym = decode_symbol(&br, lh);
        if (sym < 0) return false;
        if (sym < 256) {
          out->push_back(static_cast<std::uint8_t>(sym));
        } else if (sym == 256) {
          break;
        } else {
          const int li = sym - 257;
          if (li < 0 || li >= 29) return false;
          const int length = lenbase[li] + br.get(lenext[li]);
          const int ds = decode_symbol(&br, dh);
          if (ds < 0 || ds >= 30) return false;
          const int dist = distbase[ds] + br.get(distext[ds]);
          if (dist <= 0 || static_cast<std::size_t>(dist) > out->size()) return false;
          const std::size_t start = out->size() - static_cast<std::size_t>(dist);
          for (int i = 0; i < length; ++i) out->push_back((*out)[start + static_cast<std::size_t>(i)]);
        }
        if (br.bad) return false;
      }
    } else {
      return false;
    }
  }
  return !br.bad;
}

int paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

}  // namespace

bool write_png_rgba8(const std::string& path, const ImageRgba8& img, std::string* err) {
  if (img.width == 0 || img.height == 0) {
    if (err) *err = "write_png: 尺寸为 0";
    return false;
  }
  if (img.pixels.size() != img.byte_size()) {
    if (err) *err = "write_png: 像素缓冲大小与尺寸不符";
    return false;
  }

  // 扫描线:每行前置 filter 字节 0
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(img.height) * (1u + static_cast<std::size_t>(img.width) * 4u));
  for (std::uint32_t y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const auto* row = img.pixels.data() + static_cast<std::size_t>(y) * img.width * 4u;
    raw.insert(raw.end(), row, row + static_cast<std::size_t>(img.width) * 4u);
  }

  std::vector<std::uint8_t> out;
  out.insert(out.end(), std::begin(kPngSig), std::end(kPngSig));

  std::vector<std::uint8_t> ihdr;
  put_be32(&ihdr, img.width);
  put_be32(&ihdr, img.height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // color type RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  write_chunk(&out, "IHDR", ihdr);
  write_chunk(&out, "IDAT", zlib_store(raw));
  write_chunk(&out, "IEND", {});

  std::ofstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "write_png: 无法打开 " + path;
    return false;
  }
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!f) {
    if (err) *err = "write_png: 写入失败 " + path;
    return false;
  }
  return true;
}

bool read_png_rgba8(const std::string& path, ImageRgba8* out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "read_png: 无法打开 " + path;
    return false;
  }
  std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
  if (buf.size() < 8 || std::memcmp(buf.data(), kPngSig, 8) != 0) {
    if (err) *err = "read_png: PNG magic 不匹配 " + path;
    return false;
  }

  std::uint32_t w = 0, h = 0;
  int bit_depth = 0, color_type = 0;
  std::vector<std::uint8_t> idat;
  std::size_t pos = 8;
  while (pos + 8 <= buf.size()) {
    const std::uint32_t len = get_be32(&buf[pos]);
    if (pos + 12 + len > buf.size()) {
      if (err) *err = "read_png: chunk 越界";
      return false;
    }
    const char* type = reinterpret_cast<const char*>(&buf[pos + 4]);
    const std::uint8_t* data = &buf[pos + 8];
    if (std::memcmp(type, "IHDR", 4) == 0) {
      w = get_be32(data);
      h = get_be32(data + 4);
      bit_depth = data[8];
      color_type = data[9];
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), data, data + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    pos += 12 + len;
  }

  if (bit_depth != 8 || (color_type != 6 && color_type != 2)) {
    if (err) {
      *err = "read_png: 只支持 8bit RGBA/RGB,得到 bit_depth=" + std::to_string(bit_depth) +
             " color_type=" + std::to_string(color_type);
    }
    return false;
  }
  const int channels = (color_type == 6) ? 4 : 3;

  std::vector<std::uint8_t> raw;
  if (!inflate_zlib(idat.data(), idat.size(), &raw)) {
    if (err) *err = "read_png: IDAT 解压失败";
    return false;
  }
  const std::size_t stride = static_cast<std::size_t>(w) * static_cast<std::size_t>(channels);
  if (raw.size() < static_cast<std::size_t>(h) * (stride + 1)) {
    if (err) *err = "read_png: 解压后数据长度不足";
    return false;
  }

  std::vector<std::uint8_t> img(stride * h);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t filter = raw[static_cast<std::size_t>(y) * (stride + 1)];
    const std::uint8_t* src = &raw[static_cast<std::size_t>(y) * (stride + 1) + 1];
    std::uint8_t* dst = &img[static_cast<std::size_t>(y) * stride];
    const std::uint8_t* prev = (y > 0) ? &img[static_cast<std::size_t>(y - 1) * stride] : nullptr;
    for (std::size_t x = 0; x < stride; ++x) {
      const int a = (x >= static_cast<std::size_t>(channels)) ? dst[x - channels] : 0;
      const int b = prev ? prev[x] : 0;
      const int c = (prev && x >= static_cast<std::size_t>(channels)) ? prev[x - channels] : 0;
      int v = src[x];
      switch (filter) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: v += paeth(a, b, c); break;
        default:
          if (err) *err = "read_png: 未知 filter " + std::to_string(filter);
          return false;
      }
      dst[x] = static_cast<std::uint8_t>(v & 0xFF);
    }
  }

  out->width = w;
  out->height = h;
  out->pixels.assign(static_cast<std::size_t>(w) * h * 4u, 255);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint8_t* s = &img[(static_cast<std::size_t>(y) * w + x) * static_cast<std::size_t>(channels)];
      std::uint8_t* d = &out->pixels[(static_cast<std::size_t>(y) * w + x) * 4u];
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = (channels == 4) ? s[3] : 255;
    }
  }
  return true;
}

std::size_t count_differing_pixels(const ImageRgba8& a, const ImageRgba8& b, int tolerance) {
  if (a.width != b.width || a.height != b.height) {
    return static_cast<std::size_t>(a.width) * a.height;
  }
  std::size_t bad = 0;
  for (std::size_t i = 0; i + 3 < a.pixels.size(); i += 4) {
    for (int c = 0; c < 4; ++c) {
      const int d = static_cast<int>(a.pixels[i + static_cast<std::size_t>(c)]) -
                    static_cast<int>(b.pixels[i + static_cast<std::size_t>(c)]);
      if ((d > tolerance) || (-d > tolerance)) {
        ++bad;
        break;
      }
    }
  }
  return bad;
}

}  // namespace magicblocks
