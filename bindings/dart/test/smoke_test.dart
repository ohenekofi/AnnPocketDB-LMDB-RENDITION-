import 'dart:io';
import 'dart:typed_data';
import 'package:test/test.dart';
import 'package:annpocketdb/annpocketdb.dart';

Uint8List toBytes(String s) => Uint8List.fromList(s.codeUnits);
String fromBytes(Uint8List b) => String.fromCharCodes(b);

void main() {
  late Directory tempDir;

  setUp(() {
    tempDir = Directory.systemTemp.createTempSync('apdb_test_');
  });

  tearDown(() {
    if (tempDir.existsSync()) {
      tempDir.deleteSync(recursive: true);
    }
  });

  test('open and close', () {
    final env = Env.open(tempDir.path);
    env.close();
  });

  test('basic put / get / delete', () {
    final env = Env.open(tempDir.path);

    env.put(toBytes('hello'), toBytes('world'));
    
    final val = env.get(toBytes('hello'));
    expect(val, isNotNull);
    expect(fromBytes(val!), equals('world'));

    final missing = env.get(toBytes('missing'));
    expect(missing, isNull);

    env.delete(toBytes('hello'));
    expect(env.get(toBytes('hello')), isNull);

    env.close();
  });

  test('write transaction', () {
    final env = Env.open(tempDir.path);

    final txn = env.beginWrite();
    txn.put(toBytes('k1'), toBytes('v1'));
    txn.put(toBytes('k2'), toBytes('v2'));

    // Read your own writes
    final val1 = txn.get(toBytes('k1'));
    expect(val1, isNotNull);
    expect(fromBytes(val1!), equals('v1'));

    txn.commit();

    final val2 = env.get(toBytes('k2'));
    expect(val2, isNotNull);
    expect(fromBytes(val2!), equals('v2'));

    env.close();
  });

  test('write transaction abort', () {
    final env = Env.open(tempDir.path);

    final txn = env.beginWrite();
    txn.put(toBytes('ephemeral'), toBytes('data'));
    txn.abort();

    expect(env.get(toBytes('ephemeral')), isNull);

    env.close();
  });

  test('read cursor iteration', () {
    final env = Env.open(tempDir.path);

    final wtxn = env.beginWrite();
    wtxn.put(toBytes('a'), toBytes('1'));
    wtxn.put(toBytes('b'), toBytes('2'));
    wtxn.put(toBytes('c'), toBytes('3'));
    wtxn.commit();

    final rtxn = env.beginRead();
    final cur = rtxn.openCursor();

    expect(cur.first(), isTrue);
    expect(fromBytes(cur.key()!), equals('a'));
    expect(fromBytes(cur.value()!), equals('1'));

    expect(cur.next(), isTrue);
    expect(fromBytes(cur.key()!), equals('b'));
    expect(fromBytes(cur.value()!), equals('2'));

    expect(cur.next(), isTrue);
    expect(cur.next(), isFalse);

    cur.close();
    rtxn.close();
    env.close();
  });

  test('2pc', () {
    final env = Env.open(tempDir.path);
    final txn = env.beginWrite();
    txn.put(toBytes('2pc_k'), toBytes('2pc_v'));
    txn.prepare();
    expect(fromBytes(env.get(toBytes('2pc_k'))!), equals('2pc_v'));
    env.rollbackPrepared();
    env.close();
  });

  test('blob storage', () {
    final env = Env.open(tempDir.path);
    final blob = toBytes('large_blob_payload');
    env.putBlob(toBytes('b1'), blob);
    expect(fromBytes(env.getBlob(toBytes('b1'))!), equals('large_blob_payload'));
    env.deleteBlob(toBytes('b1'));
    expect(env.getBlob(toBytes('b1')), isNull);
    env.close();
  });

  test('changefeed', () {
    final env = Env.open(tempDir.path, flags: 1 << 9); // APDB_ENV_CHANGEFEED
    env.put(toBytes('cfk'), toBytes('cfv'));
    env.delete(toBytes('cfk'));

    final rtxn = env.beginRead();
    final cf = env.openChangeFeed(rtxn);
    final events = <ChangeEvent>[];
    while (true) {
      final ev = cf.next();
      if (ev == null) break;
      events.add(ev);
    }
    cf.close();
    rtxn.close();

    expect(events.length, equals(2));
    expect(fromBytes(events[0].key), equals('cfk'));
    expect(events[0].type, equals(1)); // Insert
    expect(events[1].type, equals(3)); // Delete
    env.close();
  });
}
