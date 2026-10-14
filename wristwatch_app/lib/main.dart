import 'dart:async';
import 'dart:convert';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:volume_controller/volume_controller.dart';
import 'package:notification_listener_service/notification_listener_service.dart';
import 'package:image/image.dart' as img_pkg;
import 'package:wristwatch_app/qoi_encoder.dart';

// BLE UUID Definitions
final Guid serviceUuid = Guid("bb00");
final Guid charWriteUuid = Guid("bb01"); // Volume level (0-100)
final Guid charNotifyUuid = Guid("bb02"); // Volume commands (UP, DOWN, MUTE)
final Guid charTrackUuid = Guid("bb03"); // Track metadata ("Artist - Title")
final Guid charArtUuid = Guid("bb04"); // 120x120 Cover art stream (QOI)
final Guid charAlertUuid = Guid("bb05"); // Pop-up alerts (WhatsApp, SMS, Phone Call)

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const SmartwatchCompanionApp());
}

class SmartwatchCompanionApp extends StatelessWidget {
  const SmartwatchCompanionApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESP32 Smartwatch Companion',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: Colors.deepPurple,
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: const BleScanScreen(),
    );
  }
}

// ==========================================
// SCREEN 1: BLE SCANNER
// ==========================================
class BleScanScreen extends StatefulWidget {
  const BleScanScreen({super.key});

  @override
  State<BleScanScreen> createState() => _BleScanScreenState();
}

class _BleScanScreenState extends State<BleScanScreen> {
  List<ScanResult> _scanResults = [];
  bool _isScanning = false;
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<bool>? _isScanningSub;

  @override
  void initState() {
    super.initState();
    _initBle();
  }

  Future<void> _initBle() async {
    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      if (mounted) setState(() => _scanResults = results);
    });

    _isScanningSub = FlutterBluePlus.isScanning.listen((isScanning) {
      if (mounted) setState(() => _isScanning = isScanning);
    });

    _startScan();
  }

  Future<void> _startScan() async {
    try {
      if (await FlutterBluePlus.isSupported == false) return;
      _scanResults.clear();
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 15));
    } catch (e) {
      debugPrint("Scan error: $e");
    }
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _isScanningSub?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Connect to Smartwatch'),
        actions: [
          IconButton(
            icon: Icon(_isScanning ? Icons.stop : Icons.refresh),
            onPressed: () =>
                _isScanning ? FlutterBluePlus.stopScan() : _startScan(),
          ),
        ],
      ),
      body: _scanResults.isEmpty
          ? Center(
              child: _isScanning
                  ? const CircularProgressIndicator()
                  : const Text('No devices found. Tap refresh to scan.'),
            )
          : ListView.builder(
              itemCount: _scanResults.length,
              itemBuilder: (context, index) {
                final result = _scanResults[index];
                final device = result.device;
                final name = device.platformName.isNotEmpty
                    ? device.platformName
                    : 'Unknown Device';

                return ListTile(
                  leading: const Icon(Icons.watch),
                  title: Text(name),
                  subtitle: Text(
                    '${device.remoteId.str} | RSSI: ${result.rssi} dBm',
                  ),
                  trailing: ElevatedButton(
                    child: const Text('Connect'),
                    onPressed: () {
                      FlutterBluePlus.stopScan();
                      Navigator.push(
                        context,
                        MaterialPageRoute(
                          builder: (context) => WatchSyncScreen(device: device),
                        ),
                      );
                    },
                  ),
                );
              },
            ),
    );
  }
}

// ==========================================
// SCREEN 2: ACTIVE WATCH SYNC (MEDIA + ART)
// ==========================================
class WatchSyncScreen extends StatefulWidget {
  final BluetoothDevice device;

  const WatchSyncScreen({super.key, required this.device});

  @override
  State<WatchSyncScreen> createState() => _WatchSyncScreenState();
}

class _WatchSyncScreenState extends State<WatchSyncScreen> with WidgetsBindingObserver {
  final VolumeController _volumeController = VolumeController.instance;

  BluetoothCharacteristic? _writeChar; // BB01
  BluetoothCharacteristic? _trackChar; // BB03
  BluetoothCharacteristic? _artChar;   // BB04
  BluetoothCharacteristic? _alertChar; // BB05

  StreamSubscription<BluetoothConnectionState>? _connectionStateSub;
  StreamSubscription<List<int>>? _notifySub;
  StreamSubscription? _notificationSub;

  BluetoothConnectionState _connectionState =
      BluetoothConnectionState.disconnected;

  double _currentVolume = 0.5;
  double _previousVolumeBeforeMute = 0.5;
  String _lastCommand = "None";
  String _currentTrackInfo = "No Track Playing";
  Uint8List? _currentCoverArtBytes;
  bool _isArtSending = false;
  double _artSendProgress = 0.0;
  bool _isServicesDiscovered = false;
  bool _isNotificationPermissionGranted = false;
  bool _isServiceBound = false;
  String _lastDetectedMediaApp = "None";
  int? _pendingVolumeToSend;
  int? _lastSentVolume;
  bool _isVolumeSyncing = false;

  final TextEditingController _testTextController = TextEditingController();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _initDevice();
    _initMediaNotificationListener();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      // Re-verify permission state & re-scan when returning from Android Settings
      _checkNotificationPermission();
      _scanActiveNotifications();
    }
  }

  Future<void> _checkNotificationPermission() async {
    try {
      final granted = await NotificationListenerService.isPermissionGranted();
      final bool? isBound = await const MethodChannel('x-slayer/notifications_channel')
          .invokeMethod('isServiceConnected');
      debugPrint("--> [PERMISSION CHECK] granted=$granted, isBound=$isBound");
      if (mounted) {
        setState(() {
          _isNotificationPermissionGranted = granted;
          _isServiceBound = isBound ?? false;
        });
      }
    } catch (e) {
      debugPrint("Permission check error: $e");
    }
  }

  /// Forces Android to rebind to NotificationListener after app rebuild / install
  Future<void> _forceReconnectListener() async {
    try {
      debugPrint("--> [REBIND] Forcing service reconnect & rebind...");
      await const MethodChannel('x-slayer/notifications_channel')
          .invokeMethod('reconnectService');
      await const MethodChannel('x-slayer/notifications_channel')
          .invokeMethod('forceRequestRebind');
      await Future.delayed(const Duration(milliseconds: 500));
      await _checkNotificationPermission();
      await _scanActiveNotifications();
    } catch (e) {
      debugPrint("--> Reconnect listener error: $e");
    }
  }

  /// Filters notifications so ONLY actual media players (e.g. VLC, Spotify)
  /// are processed, explicitly ignoring communication / messenger apps (WhatsApp, Telegram, etc.)
  bool _isSupportedMediaPlayer(String pkg) {
    if (pkg.isEmpty) return false;

    // Explicit blacklist for messengers, social apps, SMS, emails, system dialers
    const excludedApps = [
      'whatsapp',
      'telegram',
      'signal',
      'messenger',
      'discord',
      'slack',
      'sms',
      'mms',
      'message',
      'contact',
      'viber',
      'skype',
      'wechat',
      'twitter',
      'instagram',
      'facebook',
      'tiktok',
      'snapchat',
      'mail',
      'gmail',
      'outlook',
      'dialer',
      'phone',
      'call',
    ];

    for (final excluded in excludedApps) {
      if (pkg.contains(excluded)) return false;
    }

    // Explicit whitelist of media players
    return pkg.contains('vlc') ||
        pkg.contains('videolan') ||
        pkg.contains('spotify') ||
        pkg.contains('music') ||
        pkg.contains('audio') ||
        pkg.contains('player') ||
        pkg.contains('sound');
  }

  /// Identifies system phone dialer and incoming call notifications
  bool _isPhoneCallNotification(String pkg) {
    return pkg.contains('dialer') ||
        pkg.contains('telecom') ||
        pkg.contains('incallui') ||
        pkg.contains('phone');
  }

  /// Identifies missed call notifications (English & Turkish)
  bool _isMissedCallNotification(String title, String content) {
    final combined = "$title $content".toLowerCase();
    return combined.contains("missed call") ||
        combined.contains("missed") ||
        combined.contains("cevapsız arama") ||
        combined.contains("cevapsız") ||
        combined.contains("kaçırılan arama") ||
        combined.contains("kaçırılan") ||
        combined.contains("call declined") ||
        combined.contains("arama reddedildi") ||
        combined.contains("reddedildi");
  }

  /// Proactively queries ongoing notifications for currently playing music (VLC, Spotify, etc.)
  Future<void> _scanActiveNotifications() async {
    try {
      final List<dynamic>? active = await const MethodChannel('x-slayer/notifications_channel')
          .invokeMethod('getActiveNotifications');
      debugPrint("--> [SCAN ACTIVE] Found ${active?.length ?? 0} active notifications");
      if (active != null) {
        for (final item in active) {
          if (item is Map) {
            final pkg = (item['packageName'] ?? '').toString().toLowerCase();
            final t = (item['title'] ?? '').toString().trim();
            final c = (item['content'] ?? '').toString().trim();
            debugPrint("--> Active item: pkg='$pkg' | title='$t' | content='$c'");
            final isMusic = _isSupportedMediaPlayer(pkg);
            if (isMusic && (t.isNotEmpty || c.isNotEmpty)) {
              String meta = (t.isNotEmpty && c.isNotEmpty)
                  ? "$c - $t"
                  : (t.isNotEmpty ? t : c);
              debugPrint("--> [FOUND RUNNING TRACK]: $meta (from $pkg)");
              if (mounted) {
                setState(() {
                  _currentTrackInfo = meta;
                  _lastDetectedMediaApp = pkg;
                });
              }
              _sendTrackInfoToWatch(meta);
              break;
            }
          }
        }
      }
    } catch (e) {
      debugPrint("--> Scan active notifications error: $e");
    }
  }

  Future<void> _initDevice() async {
    _volumeController.showSystemUI = true;
    _currentVolume = await _volumeController.getVolume();
    if (mounted) setState(() {});

    _volumeController.addListener((volume) {
      if (mounted) setState(() => _currentVolume = volume);
      _syncVolumeToWatch((volume * 100).round());
    });

    _connectionStateSub = widget.device.connectionState.listen((state) {
      if (mounted) setState(() => _connectionState = state);
      if (state == BluetoothConnectionState.connected &&
          !_isServicesDiscovered) {
        _discoverServices();
      }
    });

    try {
      await widget.device.connect(license: License.nonprofit, autoConnect: false);
      // High connection priority for low-latency (11.25 - 15ms interval)
      await widget.device.requestConnectionPriority(
        connectionPriorityRequest: ConnectionPriority.high,
      );
      // Request high MTU for ultra-fast album art transfer
      await widget.device.requestMtu(512);
    } catch (e) {
      debugPrint("Connect error: $e");
    }
  }

  /// Listen to VLC, Spotify, & Android Media Notifications
  Future<void> _initMediaNotificationListener() async {
    try {
      bool isGranted = await NotificationListenerService.isPermissionGranted();
      debugPrint("--> [NOTIFICATION PERMISSION INITIAL] isGranted: $isGranted");
      if (mounted) setState(() => _isNotificationPermissionGranted = isGranted);

      if (!isGranted) {
        await NotificationListenerService.requestPermission();
      } else {
        await _forceReconnectListener();
      }

      _notificationSub = NotificationListenerService.notificationsStream.listen((
        event,
      ) {
        final pkg = event.packageName.toLowerCase();
        final t = event.title.trim();
        final c = event.content.trim();

        // Print EVERY incoming notification to logcat/terminal
        debugPrint(
          "--> [NOTIFICATION] Pkg: '$pkg' | Title: '$t' | Content: '$c' | hasArt: ${event.largeIcon != null} | isOngoing: ${event.onGoing} | hasRemoved: ${event.hasRemoved}",
        );

        if (mounted) {
          setState(() {
            _isServiceBound = true;
            _isNotificationPermissionGranted = true;
          });
        }

        // 1. Phone Call Alerts (Incoming Call, Missed Call, or Call Ended)
        if (_isPhoneCallNotification(pkg)) {
          if (event.hasRemoved) {
            debugPrint("--> [PHONE CALL NOTIFICATION REMOVED] Sending CALL_END");
            _sendAlertToWatch("CALL_END");
            return;
          }

          if (_isMissedCallNotification(t, c)) {
            debugPrint("--> [MISSED CALL DETECTED]: Title='$t' | Content='$c'");
            // Dismiss any active call takeover first
            _sendAlertToWatch("CALL_END");

            String caller = t;
            String details = c;
            final lowerT = t.toLowerCase();
            if (lowerT.contains("missed") || lowerT.contains("cevapsız") || lowerT.contains("kaçırılan")) {
              caller = c.isNotEmpty ? c : "Unknown Caller";
              details = t;
            }
            if (details.isEmpty) details = "Missed Call";

            _sendAlertToWatch("MISSED|$caller|$details");
            return;
          }

          // Active Incoming Call
          if (t.isNotEmpty) {
            debugPrint("--> [INCOMING CALL ALERT]: Caller='$t', Info='$c'");
            _sendAlertToWatch("CALL|$t|${c.isNotEmpty ? c : "Incoming call..."}");
          }
          return;
        }

        if (event.hasRemoved) return;

        // 2. Media Player Notifications (VLC, Spotify, etc.)
        if (_isSupportedMediaPlayer(pkg)) {
          if (mounted) setState(() => _lastDetectedMediaApp = pkg);
          String meta = "";
          if (t.isNotEmpty && c.isNotEmpty) {
            meta = "$c - $t"; // e.g. "Artist - Song"
          } else if (t.isNotEmpty) {
            meta = t;
          } else if (c.isNotEmpty) {
            meta = c;
          }

          if (meta.isNotEmpty && meta != _currentTrackInfo) {
            debugPrint("--> [TRACK UPDATED]: $meta");
            setState(() => _currentTrackInfo = meta);
            _sendTrackInfoToWatch(meta);
          }

          // Check for album cover art
          if (event.largeIcon != null && event.largeIcon!.isNotEmpty) {
            final artBytes = event.largeIcon!;
            final isSameArt = _currentCoverArtBytes != null &&
                _currentCoverArtBytes!.length == artBytes.length;
            if (!isSameArt && !_isArtSending) {
              debugPrint("--> [ALBUM ART FOUND]: ${artBytes.length} bytes from $pkg");
              _sendCoverArtToWatch(artBytes);
            }
          }
          return;
        }

        // 3. WhatsApp Messages
        if (pkg.contains("whatsapp")) {
          if (t.isEmpty || c.isEmpty) return;
          final lowerT = t.toLowerCase().trim();
          final lowerC = c.toLowerCase().trim();

          // A. Ignore app summary notifications ("WhatsApp", "WhatsApp Business")
          if (lowerT == "whatsapp" || lowerT == "whatsapp business") {
            debugPrint("--> [WHATSAPP SUMMARY IGNORED]: Title='$t' | Content='$c'");
            return;
          }

          // B. Ignore "X messages from Y chats" or "X new messages" summary text
          final isSummaryContent = RegExp(r'^\d+\s+(new\s+)?(messages?|mesaj|chats?|sohbet)', caseSensitive: false).hasMatch(lowerC) ||
              lowerC.contains("messages from") ||
              lowerC.contains("sohbetten") ||
              lowerC.contains("yeni mesaj");
          if (isSummaryContent) {
            debugPrint("--> [WHATSAPP MULTI-CHAT SUMMARY IGNORED]: Title='$t' | Content='$c'");
            return;
          }

          // C. Filter out background sync & service notifications
          if (lowerC.contains("checking for new messages") ||
              lowerC.contains("web is currently active") ||
              lowerC.contains("backup in progress") ||
              lowerC.contains("whatsapp web")) {
            return;
          }

          debugPrint("--> [WHATSAPP POPUP ALERT]: Sender='$t' | Message='$c'");
          _sendAlertToWatch("WA|$t|$c");
          return;
        }

        // 4. SMS / MMS Messages
        if (pkg.contains("messaging") || pkg.contains("mms") || (pkg.contains("sms") && !pkg.contains("call"))) {
          if (t.isEmpty || c.isEmpty) return;
          debugPrint("--> [SMS POPUP ALERT]: Sender='$t' | Message='$c'");
          _sendAlertToWatch("SMS|$t|$c");
          return;
        }
      });

      // Query active notification right away
      await _scanActiveNotifications();
    } catch (e) {
      debugPrint("Notification listener error: $e");
    }
  }

  Future<void> _discoverServices() async {
    _isServicesDiscovered = true;
    try {
      List<BluetoothService> services = await widget.device.discoverServices();

      for (var service in services) {
        if (_matchUuid(service.uuid, serviceUuid)) {
          for (var char in service.characteristics) {
            if (_matchUuid(char.uuid, charWriteUuid)) {
              _writeChar = char;
            } else if (_matchUuid(char.uuid, charNotifyUuid)) {
              await _setupNotifyListener(char);
            } else if (_matchUuid(char.uuid, charTrackUuid)) {
              _trackChar = char;
            } else if (_matchUuid(char.uuid, charArtUuid)) {
              _artChar = char;
            } else if (_matchUuid(char.uuid, charAlertUuid)) {
              _alertChar = char;
            }
          }
        }
      }

      // Initial sync of current volume
      await _syncVolumeToWatch((_currentVolume * 100).round());
      if (_currentTrackInfo != "No Track Playing") {
        await _sendTrackInfoToWatch(_currentTrackInfo);
      }
      if (mounted) setState(() {});
    } catch (e) {
      debugPrint("Discovery error: $e");
    }
  }

  bool _matchUuid(Guid a, Guid b) {
    return a == b ||
        a.str128.toLowerCase().contains(b.str128.toLowerCase()) ||
        a.toString().toLowerCase().contains(b.toString().toLowerCase());
  }

  Future<void> _setupNotifyListener(BluetoothCharacteristic char) async {
    await char.setNotifyValue(true);
    _notifySub = char.onValueReceived.listen((data) {
      if (data.isEmpty) return;
      final command = utf8.decode(data).trim().toUpperCase();
      if (mounted) setState(() => _lastCommand = command);
      _handleWatchCommand(command);
    });
  }

  Future<void> _handleWatchCommand(String command) async {
    const double volumeStep = 0.05;
    double newVolume = _currentVolume;

    switch (command) {
      case "UP":
        newVolume = (_currentVolume + volumeStep).clamp(0.0, 1.0);
        break;
      case "DOWN":
        newVolume = (_currentVolume - volumeStep).clamp(0.0, 1.0);
        break;
      case "MUTE":
        if (_currentVolume > 0.0) {
          _previousVolumeBeforeMute = _currentVolume;
          newVolume = 0.0;
        } else {
          newVolume = _previousVolumeBeforeMute > 0
              ? _previousVolumeBeforeMute
              : 0.3;
        }
        break;
      default:
        return;
    }

    _currentVolume = newVolume;
    _volumeController.setVolume(newVolume);
    await _syncVolumeToWatch((newVolume * 100).round());
    if (mounted) setState(() {});
  }

  /// Sends Volume percentage (0-100) to BB01 with queue dropping
  Future<void> _syncVolumeToWatch(int percentage) async {
    if (_writeChar == null) return;
    if (_lastSentVolume == percentage) return;

    _pendingVolumeToSend = percentage;
    if (_isVolumeSyncing) return;

    _isVolumeSyncing = true;
    while (_pendingVolumeToSend != null) {
      final target = _pendingVolumeToSend!;
      _pendingVolumeToSend = null;

      if (_lastSentVolume == target) break;

      try {
        await _writeChar!.write(
          utf8.encode(target.toString()),
          withoutResponse: true,
        );
        _lastSentVolume = target;
      } catch (e) {
        debugPrint("Error writing to BB01: $e");
      }

      // Small pacing pause matching high-speed BLE connection interval
      if (_pendingVolumeToSend != null) {
        await Future.delayed(const Duration(milliseconds: 15));
      }
    }
    _isVolumeSyncing = false;
  }

  /// Sends Track Metadata (Song Title & Artist) to BB03 (Instant Write Without Response)
  Future<void> _sendTrackInfoToWatch(String trackInfo) async {
    if (_trackChar == null) return;
    try {
      await _trackChar!.write(utf8.encode(trackInfo), withoutResponse: true);
    } catch (e) {
      debugPrint("Error writing to BB03: $e");
    }
  }

  /// Sends Pop-up Alert (WhatsApp, SMS, Phone Call) to BB05
  Future<void> _sendAlertToWatch(String alertPayload) async {
    if (_alertChar == null) {
      debugPrint("--> [ALERT BB05] Characteristic not discovered yet!");
      return;
    }
    try {
      await _alertChar!.write(utf8.encode(alertPayload), withoutResponse: true);
      debugPrint("--> [ALERT SENT TO WATCH]: $alertPayload");
    } catch (e) {
      debugPrint("Error writing alert to BB05: $e");
    }
  }

  /// Encodes cover art to 120x120 100% Lossless QOI and streams to Watch over BB04 in high-speed chunks
  Future<void> _sendCoverArtToWatch(Uint8List rawBytes) async {
    if (_artChar == null || _isArtSending) return;

    setState(() {
      _isArtSending = true;
      _artSendProgress = 0.0;
    });

    try {
      // 1. Decode image & resize to 120x120
      final decoded = img_pkg.decodeImage(rawBytes);
      if (decoded == null) {
        debugPrint("--> [COVER ART] Failed to decode image for QOI encoding");
        if (mounted) setState(() => _isArtSending = false);
        return;
      }

      final resized = img_pkg.copyResize(
        decoded,
        width: 120,
        height: 120,
        interpolation: img_pkg.Interpolation.linear,
      );

      // Keep preview image for phone UI (lossless PNG)
      _currentCoverArtBytes = Uint8List.fromList(img_pkg.encodePng(resized));

      final stopwatch = Stopwatch()..start();
      // Encode to 100% lossless QOI (Quite OK Image format)
      final qoiBytes = QoiEncoder.encodeImage(resized);
      final encodeMs = stopwatch.elapsedMilliseconds;

      debugPrint("--> [QOI LOSSLESS COMPRESSION]: ${rawBytes.length} raw bytes -> ${qoiBytes.length} QOI bytes in $encodeMs ms (120x120)");

      // 2. Send Start Header Packet: [0xAA, 0x55, 120, 120]
      await _artChar!.write([0xAA, 0x55, 120, 120], withoutResponse: true);
      await Future.delayed(const Duration(milliseconds: 3));

      // 3. Stream QOI chunks (480 bytes each)
      const int chunkSize = 480;
      final int totalChunks = (qoiBytes.length / chunkSize).ceil();

      for (int i = 0; i < totalChunks; i++) {
        int start = i * chunkSize;
        int end = (start + chunkSize > qoiBytes.length) ? qoiBytes.length : start + chunkSize;
        final chunk = qoiBytes.sublist(start, end);

        await _artChar!.write(chunk, withoutResponse: true);

        if (mounted) {
          setState(() => _artSendProgress = (i + 1) / totalChunks);
        }

        // Minimal high-speed pacing delay
        await Future.delayed(const Duration(milliseconds: 3));
      }

      // 4. Send Finish Packet: [0xAA, 0xFF]
      await _artChar!.write([0xAA, 0xFF], withoutResponse: true);
      stopwatch.stop();
      debugPrint("--> [QOI] 120x120 COVER ART STREAMED IN ${stopwatch.elapsedMilliseconds} ms (${qoiBytes.length} bytes, $totalChunks chunks)!");

      if (mounted) {
        setState(() {
          _isArtSending = false;
          _artSendProgress = 1.0;
        });
      }
    } catch (e) {
      debugPrint("Error streaming cover art: $e");
      if (mounted) setState(() => _isArtSending = false);
    }
  }

  /// Generates a stylish 120x120 test image with gradient & shapes
  Future<void> _sendTestCoverArt() async {
    final recorder = ui.PictureRecorder();
    final canvas = Canvas(recorder, const Rect.fromLTWH(0, 0, 120, 120));

    // Background gradient
    final paint = Paint()
      ..shader = ui.Gradient.linear(
        const Offset(0, 0),
        const Offset(120, 120),
        [Colors.purple.shade900, Colors.deepOrangeAccent],
      );
    canvas.drawRect(const Rect.fromLTWH(0, 0, 120, 120), paint);

    // Decorative circle
    final circlePaint = Paint()..color = Colors.amberAccent;
    canvas.drawCircle(const Offset(60, 50), 28, circlePaint);

    // Dark horizon bar
    final barPaint = Paint()..color = Colors.indigo.shade900;
    canvas.drawRect(const Rect.fromLTWH(0, 80, 120, 40), barPaint);

    // Accent lines
    final linePaint = Paint()
      ..color = Colors.cyanAccent
      ..strokeWidth = 2;
    canvas.drawLine(const Offset(0, 95), const Offset(120, 95), linePaint);
    canvas.drawLine(const Offset(0, 108), const Offset(120, 108), linePaint);

    final picture = recorder.endRecording();
    final img = await picture.toImage(120, 120);
    final byteData = await img.toByteData(format: ui.ImageByteFormat.png);
    if (byteData != null) {
      final pngBytes = byteData.buffer.asUint8List();
      _sendCoverArtToWatch(pngBytes);
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _volumeController.removeListener();
    _notifySub?.cancel();
    _notificationSub?.cancel();
    _connectionStateSub?.cancel();
    _testTextController.dispose();
    widget.device.disconnect();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final isConnected = _connectionState == BluetoothConnectionState.connected;
    final percentage = (_currentVolume * 100).round();

    return Scaffold(
      appBar: AppBar(
        title: Text(
          widget.device.platformName.isNotEmpty
              ? widget.device.platformName
              : 'Smartwatch Control',
        ),
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(20.0),
        child: Column(
          children: [
            // Status Card
            Card(
              child: ListTile(
                leading: Icon(
                  isConnected
                      ? Icons.bluetooth_connected
                      : Icons.bluetooth_disabled,
                  color: isConnected ? Colors.green : Colors.red,
                  size: 32,
                ),
                title: Text(isConnected ? 'Connected to Watch' : 'Disconnected'),
                subtitle: Text(
                  'BLE: ${widget.device.remoteId.str} | Command: $_lastCommand',
                ),
              ),
            ),
            const SizedBox(height: 12),

            // Notification Access & Media Listener Diagnostics Card
            Card(
              color: (_isNotificationPermissionGranted && _isServiceBound)
                  ? Colors.green.shade900.withValues(alpha: 0.3)
                  : Colors.orange.shade900.withValues(alpha: 0.4),
              child: Padding(
                padding: const EdgeInsets.all(12.0),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Icon(
                          (_isNotificationPermissionGranted && _isServiceBound)
                              ? Icons.notifications_active
                              : Icons.notification_important,
                          color: (_isNotificationPermissionGranted && _isServiceBound)
                              ? Colors.greenAccent
                              : Colors.orangeAccent,
                          size: 26,
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                (_isNotificationPermissionGranted && _isServiceBound)
                                    ? 'Notification Service: ACTIVE & BOUND'
                                    : (!_isNotificationPermissionGranted
                                        ? 'Notification Permission: NOT GRANTED'
                                        : 'Notification Listener: UNBOUND / IDLE'),
                                style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 13),
                              ),
                              Text(
                                'Last Player: $_lastDetectedMediaApp',
                                style: const TextStyle(fontSize: 12, color: Colors.white70),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 10),
                    Wrap(
                      spacing: 8,
                      runSpacing: 6,
                      children: [
                        ElevatedButton.icon(
                          icon: const Icon(Icons.sync, size: 16),
                          label: const Text('Sync Playing VLC'),
                          style: ElevatedButton.styleFrom(
                            backgroundColor: Colors.deepPurple,
                            foregroundColor: Colors.white,
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: () async {
                            await _scanActiveNotifications();
                            if (context.mounted) {
                              ScaffoldMessenger.of(context).showSnackBar(
                                SnackBar(
                                  content: Text('Scanned for music. Current: $_currentTrackInfo'),
                                  duration: const Duration(seconds: 2),
                                ),
                              );
                            }
                          },
                        ),
                        OutlinedButton.icon(
                          icon: const Icon(Icons.refresh, size: 16),
                          label: const Text('Reconnect Listener'),
                          style: OutlinedButton.styleFrom(
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: () async {
                            await _forceReconnectListener();
                            if (context.mounted) {
                              ScaffoldMessenger.of(context).showSnackBar(
                                SnackBar(
                                  content: Text('Rebound listener. Bound: $_isServiceBound'),
                                  duration: const Duration(seconds: 2),
                                ),
                              );
                            }
                          },
                        ),
                        OutlinedButton.icon(
                          icon: const Icon(Icons.settings, size: 16),
                          label: const Text('Settings'),
                          style: OutlinedButton.styleFrom(
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: () async {
                            await NotificationListenerService.requestPermission();
                            await _checkNotificationPermission();
                          },
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 16),

            // Track & 120x120 Cover Art Card
            Card(
              color: Colors.deepPurple.shade900.withValues(alpha: 0.3),
              child: Padding(
                padding: const EdgeInsets.all(16.0),
                child: Column(
                  children: [
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        // 120x120 Cover Art Preview
                        Container(
                          width: 80,
                          height: 80,
                          decoration: BoxDecoration(
                            color: Colors.black45,
                            borderRadius: BorderRadius.circular(8),
                            border: Border.all(color: Colors.deepPurpleAccent),
                          ),
                          child: _currentCoverArtBytes != null
                              ? ClipRRect(
                                  borderRadius: BorderRadius.circular(8),
                                  child: Image.memory(
                                    _currentCoverArtBytes!,
                                    fit: BoxFit.cover,
                                  ),
                                )
                              : const Center(
                                  child: Icon(
                                    Icons.album,
                                    size: 40,
                                    color: Colors.white38,
                                  ),
                                ),
                        ),
                        const SizedBox(width: 14),

                        // Track Info
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              const Row(
                                children: [
                                  Icon(Icons.music_note, color: Colors.amber, size: 18),
                                  SizedBox(width: 4),
                                  Text(
                                    'Now Playing on Watch:',
                                    style: TextStyle(fontWeight: FontWeight.bold, fontSize: 13),
                                  ),
                                ],
                              ),
                              const SizedBox(height: 6),
                              Text(
                                _currentTrackInfo,
                                maxLines: 2,
                                overflow: TextOverflow.ellipsis,
                                style: const TextStyle(
                                  fontSize: 15,
                                  fontWeight: FontWeight.w600,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),

                    if (_isArtSending) ...[
                      const SizedBox(height: 14),
                      LinearProgressIndicator(value: _artSendProgress),
                      const SizedBox(height: 4),
                      Text(
                        'Sending 120x120 Art to Watch... ${(_artSendProgress * 100).toInt()}%',
                        style: const TextStyle(fontSize: 12, color: Colors.cyanAccent),
                      ),
                    ],

                    const SizedBox(height: 12),
                    // Button to manually test Cover Art stream
                    SizedBox(
                      width: double.infinity,
                      child: OutlinedButton.icon(
                        icon: const Icon(Icons.image),
                        label: const Text('Send Test 120x120 Cover Art'),
                        onPressed: isConnected && !_isArtSending
                            ? _sendTestCoverArt
                            : null,
                      ),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 16),

            // Pop-Up Notification Testing Card
            Card(
              color: Colors.blueGrey.shade900.withValues(alpha: 0.4),
              child: Padding(
                padding: const EdgeInsets.all(14.0),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Row(
                      children: [
                        Icon(Icons.notifications_active, color: Colors.greenAccent, size: 18),
                        SizedBox(width: 8),
                        Text(
                          'Pop-Up Notifications Preview',
                          style: TextStyle(fontWeight: FontWeight.bold, fontSize: 14),
                        ),
                      ],
                    ),
                    const SizedBox(height: 8),
                    const Text(
                      'Tap to preview pop-up alert windows on the watch display:',
                      style: TextStyle(fontSize: 12, color: Colors.white70),
                    ),
                    const SizedBox(height: 10),
                    Wrap(
                      spacing: 8,
                      runSpacing: 8,
                      children: [
                        ElevatedButton.icon(
                          icon: const Icon(Icons.chat, size: 16),
                          label: const Text('Test WhatsApp'),
                          style: ElevatedButton.styleFrom(
                            backgroundColor: const Color(0xFF25D366),
                            foregroundColor: Colors.white,
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: isConnected
                              ? () => _sendAlertToWatch("WA|John Doe|Hey! Are you still at the lab? Let me know when you head out.")
                              : null,
                        ),
                        ElevatedButton.icon(
                          icon: const Icon(Icons.sms, size: 16),
                          label: const Text('Test SMS'),
                          style: ElevatedButton.styleFrom(
                            backgroundColor: Colors.blue.shade700,
                            foregroundColor: Colors.white,
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: isConnected
                              ? () => _sendAlertToWatch("SMS|Mom|Dinner is ready! Call me when you get this message.")
                              : null,
                        ),
                        ElevatedButton.icon(
                          icon: const Icon(Icons.call, size: 16),
                          label: const Text('Test Call'),
                          style: ElevatedButton.styleFrom(
                            backgroundColor: Colors.red.shade700,
                            foregroundColor: Colors.white,
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: isConnected
                              ? () => _sendAlertToWatch("CALL|Sarah Connor|+1 (555) 019-2834")
                              : null,
                        ),
                        ElevatedButton.icon(
                          icon: const Icon(Icons.phone_missed, size: 16),
                          label: const Text('Test Missed Call'),
                          style: ElevatedButton.styleFrom(
                            backgroundColor: Colors.deepOrange.shade800,
                            foregroundColor: Colors.white,
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: isConnected
                              ? () => _sendAlertToWatch("MISSED|Sarah Connor|Missed Call (1)")
                              : null,
                        ),
                        OutlinedButton.icon(
                          icon: const Icon(Icons.call_end, size: 16),
                          label: const Text('End / Dismiss'),
                          style: OutlinedButton.styleFrom(
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                            textStyle: const TextStyle(fontSize: 12),
                          ),
                          onPressed: isConnected
                              ? () => _sendAlertToWatch("CALL_END")
                              : null,
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 20),

            // Volume Percentage & Slider
            Text(
              '$percentage%',
              style: Theme.of(context).textTheme.displayMedium?.copyWith(
                fontWeight: FontWeight.bold,
                color: Colors.deepPurpleAccent,
              ),
            ),
            Slider(
              value: _currentVolume,
              min: 0.0,
              max: 1.0,
              divisions: 20,
              onChanged: (val) {
                setState(() => _currentVolume = val);
                _volumeController.setVolume(val);
                _syncVolumeToWatch((val * 100).round());
              },
            ),

            const SizedBox(height: 16),

            // Manual Text Input for Immediate Testing
            TextField(
              controller: _testTextController,
              decoration: InputDecoration(
                labelText: 'Test Track Metadata',
                hintText: 'e.g. Queen - Bohemian Rhapsody',
                suffixIcon: IconButton(
                  icon: const Icon(Icons.send),
                  onPressed: () {
                    final txt = _testTextController.text.trim();
                    if (txt.isNotEmpty) {
                      setState(() => _currentTrackInfo = txt);
                      _sendTrackInfoToWatch(txt);
                    }
                  },
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
