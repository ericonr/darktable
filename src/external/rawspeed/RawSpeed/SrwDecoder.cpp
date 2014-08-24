#include "StdAfx.h"
#include "SrwDecoder.h"
#include "TiffParserOlympus.h"
#include "ByteStreamSwap.h"

#if defined(__unix__) || defined(__APPLE__) 
#include <stdlib.h>
#endif
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2010 Klaus Post

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

    http://www.klauspost.com
*/

namespace RawSpeed {

SrwDecoder::SrwDecoder(TiffIFD *rootIFD, FileMap* file):
RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 3;
  b = NULL;
}

SrwDecoder::~SrwDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
  if (NULL != b)
    delete b;
  b = NULL;
}

RawImage SrwDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty())
    ThrowRDE("Srw Decoder: No image data found");

  TiffIFD* raw = data[0];

  int compression = raw->getEntry(COMPRESSION)->getInt();
  int bits = raw->getEntry(BITSPERSAMPLE)->getInt();

  if (32769 != compression && 32770 != compression && 32772 != compression)
    ThrowRDE("Srw Decoder: Unsupported compression");

  if (32769 == compression)
  {
    bool bit_order = false;  // Default guess
    map<string,string>::iterator msb_hint = hints.find("msb_override");
    if (msb_hint != hints.end())
      bit_order = (0 == (msb_hint->second).compare("true"));
    this->decodeUncompressed(raw, bit_order ? BitOrder_Jpeg : BitOrder_Plain);
    return mRaw;
  }

  if (32770 == compression)
  {
    if (!raw->hasEntry ((TiffTag)40976)) {
      bool bit_order = (bits == 12);  // Default guess
      map<string,string>::iterator msb_hint = hints.find("msb_override");
      if (msb_hint != hints.end())
        bit_order = (0 == (msb_hint->second).compare("true"));
      this->decodeUncompressed(raw, bit_order ? BitOrder_Jpeg : BitOrder_Plain);
      return mRaw;
    } else {
      uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
      if (nslices != 1)
        ThrowRDE("Srw Decoder: Only one slice supported, found %u", nslices);
      try {
        decodeCompressed(raw);
      } catch (RawDecoderException& e) {
        mRaw->setError(e.what());
      }
      return mRaw;
    }
  }
  if (32772 == compression)
  {
    uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
    if (nslices != 1)
      ThrowRDE("Srw Decoder: Only one slice supported, found %u", nslices);
    try {
      decodeCompressed2(raw, bits);
    } catch (RawDecoderException& e) {
      mRaw->setError(e.what());
    }
    return mRaw;
  }
  ThrowRDE("Srw Decoder: Unsupported compression");
  return mRaw;
}
// Decoder for compressed srw files (NX300 and later)
void SrwDecoder::decodeCompressed( TiffIFD* raw )
{
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  const uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 compressed_offset = raw->getEntry((TiffTag)40976)->getInt();

  if (NULL != b)
    delete b;
  if (getHostEndianness() == little)
    b = new ByteStream(mFile->getData(0), mFile->getSize());
  else
    b = new ByteStreamSwap(mFile->getData(0), mFile->getSize());
  b->setAbsoluteOffset(compressed_offset);

  for (uint32 y = 0; y < height; y++) {
    uint32 line_offset = offset + b->getInt();
    if (line_offset >= mFile->getSize())
      ThrowRDE("Srw decoder: Offset outside image file, file probably truncated.");
    int len[4];
    for (int i = 0; i < 4; i++)
      len[i] = y < 2 ? 7 : 4;
    BitPumpMSB32 bits(mFile->getData(line_offset),mFile->getSize() - line_offset);
    int op[4];
    ushort16* img = (ushort16*)mRaw->getData(0, y);
    ushort16* img_up = (ushort16*)mRaw->getData(0, max(0, (int)y - 1));
    ushort16* img_up2 = (ushort16*)mRaw->getData(0, max(0, (int)y - 2));
    // Image is arranged in groups of 16 pixels horizontally
    for (uint32 x = 0; x < width; x += 16) {
			bits.fill();
      bool dir = !!bits.getBitNoFill();
      for (int i = 0; i < 4; i++)
        op[i] = bits.getBitsNoFill(2);
      for (int i = 0; i < 4; i++) {
        switch (op[i]) {
          case 3: len[i] = bits.getBits(4);
            break;
          case 2: len[i]--;
            break;
          case 1: len[i]++;
        }
        if (len[i] < 0)
          ThrowRDE("Srw Decompressor: Bit length less than 0.");
        if (len[i] > 16)
          ThrowRDE("Srw Decompressor: Bit Length more than 16.");
      }
      if (dir) {
        // Upward prediction
        // First we decode even pixels
        for (int c = 0; c < 16; c += 2) {
          int b = len[(c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + img_up[c];
        }
        // Now we decode odd pixels
        // Why on earth upward prediction only looks up 1 line above
        // is beyond me, it will hurt compression a deal.
        for (int c = 1; c < 16; c += 2) {
          int b = len[2 | (c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + img_up2[c];
        }
      } else {
        // Left to right prediction
        // First we decode even pixels
        int pred_left = x ? img[-2] : 128;
        for (int c = 0; c < 16; c += 2) {
          int b = len[(c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + pred_left;
        }
        // Now we decode odd pixels
        pred_left = x ? img[-1] : 128;
        for (int c = 1; c < 16; c += 2) {
          int b = len[2 | (c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + pred_left;
        }
      }
      bits.checkPos();
      img += 16;
      img_up += 16;
      img_up2 += 16;
    }
  }

  // Swap red and blue pixels to get the final CFA pattern
  for (uint32 y = 0; y < height-1; y+=2) {
    ushort16* topline = (ushort16*)mRaw->getData(0, y);
    ushort16* bottomline = (ushort16*)mRaw->getData(0, y+1);
    for (uint32 x = 0; x < width-1; x += 2) {
      ushort16 temp = topline[1];
      topline[1] = bottomline[0];
      bottomline[0] = temp;
      topline += 2;
      bottomline += 2;
    }
  }
}

// Decoder for compressed srw files (NX3000 and later)
void SrwDecoder::decodeCompressed2( TiffIFD* raw, int bits)
{
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  const ushort16 tab[14] = { 0x304,0x307,0x206,0x205,0x403,0x600,0x709,
                             0x80a,0x90b,0xa0c,0xa0d,0x501,0x408,0x402 };
  ushort16 huff[1026], vpred[2][2] = {{0,0},{0,0}}, hpred[2];

  uint32 n = 0;
  huff[n] = 10;
  for (uint32 i=0; i < 14; i++)
    for(int32 c = 0; c < (1024 >> (tab[i] >> 8)); c++)
      huff[++n] = tab[i];

  ByteStream input(mFile->getData(offset), mFile->getSize());
  getbithuff(input, -1, NULL);

  for (uint32 y = 0; y < height; y++) {
    ushort16* img = (ushort16*)mRaw->getData(0, y);
    for (uint32 x = 0; x < width; x++) {
      int32 diff = ljpegDiff(input, huff);
      if (x < 2)
        hpred[x] = vpred[y & 1][x] += diff;
      else
        hpred[x & 1] += diff;
      img[x] = hpred[x & 1];
      if (img[x] >> bits)
        ThrowRDE("SRW: Error: decoded value out of bounds");
    }
  }
}

int32 SrwDecoder::ljpegDiff (ByteStream &input, ushort16 *huff)
{
  int32 len, diff;

  len = getbithuff(input, *huff, huff+1);
  if (len == 16)
    return -32768;
  diff = getbithuff(input, len, NULL);
  if (len && (diff & (1 << (len-1))) == 0)
    diff -= (1 << len) - 1;
  return diff;
}

uint32 SrwDecoder::getbithuff (ByteStream &input, int nbits, ushort16 *huff)
{
  static unsigned bitbuf=0;
  static int vbits=0, reset=0;
  uint32 c;

  if (nbits > 25) return 0;
  if (nbits < 0)
    return bitbuf = vbits = reset = 0;
  if (nbits == 0 || vbits < 0) return 0;
  // The <1000 comparison is spurious and will always return true as the value 
  // is a byte converted to uint32. It used to be != EOF in dcraw when using fgetc
  // but doesn't make sense with getByte as that will throw an exception instead
  while (!reset && vbits < nbits && (c = ((uint32)input.getByte())) < 1000 &&
    !(reset = 0 && c == 0xff && ((uint32) input.getByte()))) {
    bitbuf = (bitbuf << 8) + (uchar8) c;
    vbits += 8;
  }
  c = bitbuf << (32-vbits) >> (32-nbits);
  if (huff) {
    vbits -= huff[c] >> 8;
    c = (uchar8) huff[c];
  } else
    vbits -= nbits;
  if (vbits < 0)
    ThrowRDE("CRW: Asking for bits we don't have");
  return c;
}



void SrwDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("Srw Support check: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("SRW Support: Make name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void SrwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("SRW Meta Decoder: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  if (!this->checkCameraSupported(meta, make, model, "") && !data.empty() && data[0]->hasEntry(CFAREPEATPATTERNDIM)) {
    const unsigned short* pDim = data[0]->getEntry(CFAREPEATPATTERNDIM)->getShortArray();
    iPoint2D cfaSize(pDim[1], pDim[0]);
    if (cfaSize.x != 2 && cfaSize.y != 2)
      ThrowRDE("SRW Decoder: Unsupported CFA pattern size");

    const uchar8* cPat = data[0]->getEntry(CFAPATTERN)->getData();
    if (cfaSize.area() != data[0]->getEntry(CFAPATTERN)->count)
      ThrowRDE("SRW Decoder: CFA pattern dimension and pattern count does not match: %d.");

    for (int y = 0; y < cfaSize.y; y++) {
      for (int x = 0; x < cfaSize.x; x++) {
        uint32 c1 = cPat[x+y*cfaSize.x];
        CFAColor c2;
        switch (c1) {
            case 0:
              c2 = CFA_RED; break;
            case 1:
              c2 = CFA_GREEN; break;
            case 2:
              c2 = CFA_BLUE; break;
            default:
              c2 = CFA_UNKNOWN;
              ThrowRDE("SRW Decoder: Unsupported CFA Color.");
        }
        mRaw->cfa.setColorAt(iPoint2D(x, y), c2);
      }
    }
    //printf("Camera CFA: %s\n", mRaw->cfa.asString().c_str());
  }

  int iso = 0;
  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  setMetaData(meta, make, model, "", iso);
}


} // namespace RawSpeed
