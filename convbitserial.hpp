#pragma once
#include "gemmbitserial.hpp"

namespace gemmbitserial {

class ConvBitSerialContext {
public:
  uint64_t ifm;         // channels in input
  uint64_t ofm;         // channels in output
  uint64_t in_dim;      // input dimension (assumed to be square)
  uint64_t k;           // convolution kernel size
  uint64_t stride;      // convolution kernel stride
  uint64_t pad;         // padded pixels on each edge

  GEMMContext * gemmctx;  // GEMM context
  BitSerialMatrix * abuf; // buffer for converted activations
  uint64_t aligned_ifm;   // input channels aligned to packing size
  uint64_t packed_ifm;    // input channels after packing
  uint64_t padded_idim;   // padded input dimension (independent from alignment)
  uint64_t out_dim;       // output dimension (assumed to be square)

  template <typename T>
  void importWeights(T * buf) {
    // just call importRegular on the rhs matrix
    gemmctx.rhs.importRegular(buf);
  }

  template <typename T>
  void importActivations(uint8_t * buf) {
    // TODO import with special attention to channel padding
    // call the sliding window (im2row) operator to generate lhs matrix
    im2row(abuf, packed_ifm, in_dim, in_dim, k, stride, pad, ctx.lhs.data);
  }

  // im2row on interleaved channels inspired from DarkNet
  template <typename Dtype>
  inline Dtype im2row_get_pixel(const Dtype *im, const int height, const int width, const int channels,
                          const int row, const int col, const int channel, const int pad)
  {
      const int prow = row - pad;
      const int pcol = col - pad;

      if (prow < 0 || pcol < 0 ||
          prow >= height || pcol >= width) return 0;
      // indexing according to [height][width][channel]
      return im[pcol + width*(prow + height*channel)];
      return im[channel + channels*(pcol + width*prow)];
  }
  template <typename Dtype, typename DtypeOut>
  void im2row(const Dtype* data_im,
       const int channels, const int height, const int width,
       const int ksize, const int stride, const int pad, DtypeOut* data_col)
  {
      int c,h,w;
      const int height_col = (height + 2*pad - ksize) / stride + 1;
      const int width_col = (width + 2*pad - ksize) / stride + 1;
      const int k2 = ksize * ksize;
      // TODO reorder loops for channel-interleaved layout
      int channels_col = channels * ksize * ksize;
      for (c = 0; c < channels_col; ++c) {
          const int w_offset = c % ksize;
          const int h_offset = (c / ksize) % ksize;
          const int c_im = c / k2;
          for (h = 0; h < height_col; ++h) {
              for (w = 0; w < width_col; ++w) {
                  const int im_row = h_offset + h * stride;
                  const int im_col = w_offset + w * stride;
                  const int col_index = c + channels_col * (w + h * width_col);
                  data_col[col_index] = (DtypeOut) (im2row_get_pixel(data_im, height, width, channels,
                          im_row, im_col, c_im, pad));
              }
          }
      }
  }
}

void allocConvBitSerialContext(
  const uint64_t ifm,         // channels in input
  const uint64_t ofm,         // channels in output
  const uint64_t in_dim,      // input dimension (assumed to be square)
  const uint64_t k,           // convolution kernel size
  const uint64_t stride,      // convolution kernel stride
  const uint64_t pad,         // padded pixels on each edge
  const uint64_t ibits,       // bits per input
  const uint64_t wbits        // bits per weight
  const bool isigned          // whether inputs are signed
  const bool wsigned,         // whether weights are signed
) {
  ConvBitSerialContext ctx;
  ctx.ifm = ifm;
  ctx.ofm = ofm;
  ctx.in_dim = in_dim;
  ctx.k = k;
  ctx.stride = stride;
  ctx.pad = pad;
  const uint64_t pack_bits = sizeof(uint64_t);
  // determine the output dimension based on input
  ctx.padded_idim = ctx.in_dim + 2*ctx.pad;
  ctx.out_dim = ((ctx.padded_idim - ctx.k) / ctx.stride) + 1;
  // round up number of input channels to be divisible by packing size
  ctx.aligned_ifm = (ctx.ifm + pack_bits - 1) / pack_bits;
  // number of channels after packing
  ctx.packed_ifm = ctx.aligned_ifm / pack_bits;
  // determine the matrix sizes for the lowered convolution
  // lhs is inputs, rhs is weights
  const uint64_t lhs_rows = ctx.out_dim * ctx.out_dim;
  const uint64_t depth = ctx.aligned_ifm * ctx.k * ctx.k;
  const uint64_t rhs_rows = ofm;
  // allocate the context
  ctx.gemctx = allocateGEMMContext(
    lhs_rows, depth, rhs_rows, ibits, wbits, isigned, wsigned
  );
  // TODO allocate the buffer for converted activations --
  // can we use rows=orig_rows*orig_chans and cols=chans with col alignment
  // to packing size here?
  return ctx;
}

void deallocConvBitSerialContext(ConvBitSerialContext ctx) {
  deallocGEMMContext(ctx.gemmctx);
}

/* Import a matrix with multichannel data, assuming the following data layout:
  [rows][cols][chans]
*/
template <typename PackedT, typename ImportT>
static void importInterleavedMultichanMatrix(
  const ImportT * in,
  uint64_t rows,  // number of rows
  uint64_t cols,  // number of columns
  uint64_t chans  // number of channels, must be divisible by # bits in PackedT
) {
  const unsigned int packBits = sizeof(PackedT)*8;
  // TODO add utility function to create padded v. of tensor if needed
  // channel size must be divisible by packing size. this is necessary to get
  // fast lowering for convolutions, carrying multiply channels by moving
  // around single words without requiring any bit masking.
  assert(chans % packBits == 0);
  assert(rows == this->nrows);
  // treat the innermost two dimensions as a single dimension
  assert(cols * chans == this->ncols);
  this->importRegular(in);
}