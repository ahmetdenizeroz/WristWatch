import 'dart:typed_data';
import 'package:image/image.dart' as img;

/// Fast, standalone QOI (Quite OK Image format) encoder and decoder for Dart.
/// Implements the official QOI specification (https://phoboslab.org).
class QoiEncoder {
  static const int _qoiOpIndex = 0x00; // 00xxxxxx
  static const int _qoiOpDiff  = 0x40; // 01xxxxxx
  static const int _qoiOpLuma  = 0x80; // 10xxxxxx
  static const int _qoiOpRun   = 0xc0; // 11xxxxxx
  static const int _qoiOpRgb   = 0xfe; // 11111110
  static const int _qoiOpRgba  = 0xff; // 11111111

  static const List<int> _padding = [0, 0, 0, 0, 0, 0, 0, 1];

  /// Encodes a [img.Image] into QOI byte format (RGB 3-channel mode for album art).
  static Uint8List encodeImage(img.Image image, {bool hasAlpha = false}) {
    final channels = hasAlpha ? 4 : 3;
    final order = hasAlpha ? img.ChannelOrder.rgba : img.ChannelOrder.rgb;
    final bytes = image.getBytes(order: order);
    return encodeRaw(bytes, image.width, image.height, channels: channels);
  }

  /// Encodes raw pixel bytes (RGB or RGBA) into standard QOI format.
  static Uint8List encodeRaw(Uint8List pixels, int width, int height, {int channels = 3}) {
    if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
      throw ArgumentError('Invalid dimensions or channel count for QOI');
    }

    // Maximum possible QOI size: header(14) + pixels*(channels+1) + padding(8)
    final maxOutputSize = 14 + width * height * (channels + 1) + 8;
    final out = Uint8List(maxOutputSize);
    final bd = ByteData.sublistView(out);

    int p = 0;
    // 1. Magic bytes "qoif"
    out[p++] = 0x71; // 'q'
    out[p++] = 0x6f; // 'o'
    out[p++] = 0x69; // 'i'
    out[p++] = 0x66; // 'f'

    // 2. Width & Height (Big-endian uint32)
    bd.setUint32(p, width, Endian.big);
    p += 4;
    bd.setUint32(p, height, Endian.big);
    p += 4;

    // 3. Channels (3=RGB, 4=RGBA) and Colorspace (0=sRGB)
    out[p++] = channels;
    out[p++] = 0; // sRGB with linear alpha

    // 4. Color Index Cache (64 entries of RGBA packed into uint32)
    final index = Uint32List(64);

    int run = 0;
    int prevR = 0;
    int prevG = 0;
    int prevB = 0;
    int prevA = 255;
    int prevPacked = (prevR << 24) | (prevG << 16) | (prevB << 8) | prevA;

    final pxLen = width * height * channels;
    final pxEnd = pxLen - channels;

    for (int pxPos = 0; pxPos < pxLen; pxPos += channels) {
      final r = pixels[pxPos + 0];
      final g = pixels[pxPos + 1];
      final b = pixels[pxPos + 2];
      final a = (channels == 4) ? pixels[pxPos + 3] : 255;

      final packed = (r << 24) | (g << 16) | (b << 8) | a;

      if (packed == prevPacked) {
        run++;
        if (run == 62 || pxPos == pxEnd) {
          out[p++] = _qoiOpRun | (run - 1);
          run = 0;
        }
      } else {
        if (run > 0) {
          out[p++] = _qoiOpRun | (run - 1);
          run = 0;
        }

        final indexPos = (r * 3 + g * 5 + b * 7 + a * 11) & 63;

        if (index[indexPos] == packed) {
          out[p++] = _qoiOpIndex | indexPos;
        } else {
          index[indexPos] = packed;

          if (a == prevA) {
            // Signed 8-bit wraparound difference
            final vr = ((r - prevR + 128) & 0xFF) - 128;
            final vg = ((g - prevG + 128) & 0xFF) - 128;
            final vb = ((b - prevB + 128) & 0xFF) - 128;

            final vgR = ((vr - vg + 128) & 0xFF) - 128;
            final vgB = ((vb - vg + 128) & 0xFF) - 128;

            if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
              out[p++] = _qoiOpDiff | ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2);
            } else if (vgR > -9 && vgR < 8 && vg > -33 && vg < 32 && vgB > -9 && vgB < 8) {
              out[p++] = _qoiOpLuma | (vg + 32);
              out[p++] = ((vgR + 8) << 4) | (vgB + 8);
            } else {
              out[p++] = _qoiOpRgb;
              out[p++] = r;
              out[p++] = g;
              out[p++] = b;
            }
          } else {
            out[p++] = _qoiOpRgba;
            out[p++] = r;
            out[p++] = g;
            out[p++] = b;
            out[p++] = a;
          }
        }
      }

      prevR = r;
      prevG = g;
      prevB = b;
      prevA = a;
      prevPacked = packed;
    }

    // 5. Append 8-byte QOI padding
    for (int i = 0; i < 8; i++) {
      out[p++] = _padding[i];
    }

    // Return exact slice of encoded bytes
    return Uint8List.sublistView(out, 0, p);
  }

  /// Decodes QOI bytes back into raw RGBA pixels (width * height * 4).
  /// Useful for testing, validation, and previewing.
  static Uint8List decodeToRgba(Uint8List qoiBytes, {required int expectedWidth, required int expectedHeight}) {
    if (qoiBytes.length < 14 + 8) {
      throw FormatException('QOI data too short');
    }

    // Check magic
    if (qoiBytes[0] != 0x71 || qoiBytes[1] != 0x6f || qoiBytes[2] != 0x69 || qoiBytes[3] != 0x66) {
      throw FormatException('Invalid QOI magic header');
    }

    final bd = ByteData.sublistView(qoiBytes);
    final width = bd.getUint32(4, Endian.big);
    final height = bd.getUint32(8, Endian.big);

    if (width != expectedWidth || height != expectedHeight) {
      throw FormatException('Dimensions mismatch: got ${width}x$height, expected ${expectedWidth}x$expectedHeight');
    }

    final totalPixels = width * height;
    final out = Uint8List(totalPixels * 4);
    final index = Uint32List(64);

    int r = 0, g = 0, b = 0, a = 255;
    int p = 14;
    final chunksEnd = qoiBytes.length - 8;
    int pixelIdx = 0;
    int run = 0;

    while (pixelIdx < totalPixels) {
      if (run > 0) {
        run--;
      } else if (p < chunksEnd) {
        final b1 = qoiBytes[p++];

        if (b1 == _qoiOpRgb) {
          r = qoiBytes[p++];
          g = qoiBytes[p++];
          b = qoiBytes[p++];
        } else if (b1 == _qoiOpRgba) {
          r = qoiBytes[p++];
          g = qoiBytes[p++];
          b = qoiBytes[p++];
          a = qoiBytes[p++];
        } else if ((b1 & 0xc0) == _qoiOpIndex) {
          final cached = index[b1 & 0x3f];
          r = (cached >> 24) & 0xff;
          g = (cached >> 16) & 0xff;
          b = (cached >> 8) & 0xff;
          a = cached & 0xff;
        } else if ((b1 & 0xc0) == _qoiOpDiff) {
          r = (r + ((b1 >> 4) & 0x03) - 2) & 0xff;
          g = (g + ((b1 >> 2) & 0x03) - 2) & 0xff;
          b = (b + ( b1       & 0x03) - 2) & 0xff;
        } else if ((b1 & 0xc0) == _qoiOpLuma) {
          final b2 = qoiBytes[p++];
          final vg = (b1 & 0x3f) - 32;
          r = (r + vg - 8 + ((b2 >> 4) & 0x0f)) & 0xff;
          g = (g + vg) & 0xff;
          b = (b + vg - 8 +  (b2       & 0x0f)) & 0xff;
        } else if ((b1 & 0xc0) == _qoiOpRun) {
          run = (b1 & 0x3f);
        }

        final packed = (r << 24) | (g << 16) | (b << 8) | a;
        index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = packed;
      } else {
        break;
      }

      final outPos = pixelIdx * 4;
      out[outPos + 0] = r;
      out[outPos + 1] = g;
      out[outPos + 2] = b;
      out[outPos + 3] = a;
      pixelIdx++;
    }

    return out;
  }
}

