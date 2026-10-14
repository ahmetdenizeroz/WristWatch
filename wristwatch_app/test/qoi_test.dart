import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as img;
import 'package:wristwatch_app/qoi_encoder.dart';

void main() {
  test('QOI encodes and decodes 120x120 synthetic gradient with 100% fidelity', () {
    const width = 120;
    const height = 120;
    final image = img.Image(width: width, height: height);

    // Create a rich test pattern with gradients, circles, and flat areas
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        int r = (x * 255 / width).round();
        int g = (y * 255 / height).round();
        int b = ((x + y) * 128 / (width + height)).round();

        // Add a circle in the center
        int dx = x - 60;
        int dy = y - 60;
        if (dx * dx + dy * dy < 25 * 25) {
          r = 255;
          g = 215;
          b = 0;
        }

        image.setPixelRgb(x, y, r, g, b);
      }
    }

    final rawBytes = image.getBytes(order: img.ChannelOrder.rgba);
    expect(rawBytes.length, width * height * 4);

    final stopwatch = Stopwatch()..start();
    final qoiBytes = QoiEncoder.encodeImage(image);
    stopwatch.stop();

    print('QOI Encoded in ${stopwatch.elapsedMilliseconds} ms: '
        '${rawBytes.length} raw RGBA bytes -> ${qoiBytes.length} QOI bytes '
        '(${(qoiBytes.length * 100 / rawBytes.length).toStringAsFixed(1)}% of raw size)');

    // Decode and verify 100% lossless match
    final decodedRgba = QoiEncoder.decodeToRgba(qoiBytes, expectedWidth: width, expectedHeight: height);
    expect(decodedRgba.length, rawBytes.length);

    for (int i = 0; i < decodedRgba.length; i += 4) {
      expect(decodedRgba[i], rawBytes[i], reason: 'Red channel mismatch at pixel ${i ~/ 4}');
      expect(decodedRgba[i + 1], rawBytes[i + 1], reason: 'Green channel mismatch at pixel ${i ~/ 4}');
      expect(decodedRgba[i + 2], rawBytes[i + 2], reason: 'Blue channel mismatch at pixel ${i ~/ 4}');
      expect(decodedRgba[i + 3], 255, reason: 'Alpha channel should be 255 at pixel ${i ~/ 4}');
    }
    print('--> 100% LOSSLESS VERIFICATION PASSED: All 14,400 pixels matched perfectly!');
  });
}

