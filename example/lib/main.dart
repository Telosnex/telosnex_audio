import 'package:flutter/material.dart';
import 'package:telosnex_audio/telosnex_audio.dart';

void main() {
  // ignore: avoid_print
  print('telosnex_audio native ${nativeVersion()}');
  runApp(const ExampleApp());
}

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        body: Center(child: Text('telosnex_audio ${nativeVersion()}')),
      ),
    );
  }
}
