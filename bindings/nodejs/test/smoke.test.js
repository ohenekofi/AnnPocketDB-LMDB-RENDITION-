const fs = require('fs');
const path = require('path');
const { Env, AnnPocketDbError } = require('../index');

describe('AnnPocketDb Node.js Binding Smoke Test', () => {
  let dbPath;

  beforeEach(() => {
    dbPath = path.join(__dirname, 'testdb_' + Math.random().toString(36).substring(7));
    fs.mkdirSync(dbPath, { recursive: true });
  });

  afterEach(() => {
    fs.rmSync(dbPath, { recursive: true, force: true });
  });

  test('open / close', () => {
    const env = new Env(dbPath);
    env.close();
  });

  test('basic put / get / delete', () => {
    const env = new Env(dbPath);
    
    env.put('hello', 'world');
    
    const val = env.get('hello');
    expect(val).toBeInstanceOf(Buffer);
    expect(val.toString()).toBe('world');
    
    const missing = env.get('missing');
    expect(missing).toBeNull();
    
    env.delete('hello');
    expect(env.get('hello')).toBeNull();
    
    env.close();
  });

  test('write transaction', () => {
    const env = new Env(dbPath);
    
    const txn = env.beginWrite();
    txn.put('k1', 'v1');
    txn.put('k2', 'v2');
    
    // read-your-own-writes
    expect(txn.get('k1').toString()).toBe('v1');
    
    txn.commit();
    
    expect(env.get('k2').toString()).toBe('v2');
    
    env.close();
  });

  test('write transaction abort', () => {
    const env = new Env(dbPath);
    
    const txn = env.beginWrite();
    txn.put('ephemeral', 'data');
    txn.abort();
    
    expect(env.get('ephemeral')).toBeNull();
    
    env.close();
  });

  test('cursor iteration', () => {
    const env = new Env(dbPath);
    
    const txn = env.beginWrite();
    txn.put('a', 'v:a');
    txn.put('b', 'v:b');
    txn.put('c', 'v:c');
    txn.commit();
    
    const rtxn = env.beginRead();
    const cur = rtxn.cursor();
    
    const collected = [];
    for (const item of cur) {
      collected.push({ k: item.key.toString(), v: item.value.toString() });
    }
    
    expect(collected).toEqual([
      { k: 'a', v: 'v:a' },
      { k: 'b', v: 'v:b' },
      { k: 'c', v: 'v:c' }
    ]);
    
    cur.close();
    rtxn.close();
    env.close();
  });

  test('stat', () => {
    const env = new Env(dbPath);
    env.put('k', 'v');
    const s = env.stat();
    expect(s.pageSize).toBeGreaterThan(0);
    expect(s.entries).toBeGreaterThan(0);
    env.close();
  });

  test('2pc', () => {
    const env = new Env(dbPath);
    const txn = env.beginWrite();
    txn.put('2pc_k', '2pc_v');
    txn.prepare();
    expect(env.get('2pc_k').toString()).toBe('2pc_v');
    env.rollbackPrepared();
    env.close();
  });

  test('blob storage', () => {
    const env = new Env(dbPath);
    const payload = Buffer.from('large_blob_payload');
    env.putBlob('b1', payload);
    expect(env.getBlob('b1').toString()).toBe('large_blob_payload');
    env.deleteBlob('b1');
    expect(env.getBlob('b1')).toBeNull();
    env.close();
  });

  test('changefeed', () => {
    const { EnvFlags } = require('../index');
    const env = new Env(dbPath, { flags: EnvFlags.CHANGEFEED });
    env.put('cfk', 'cfv');
    env.delete('cfk');

    const rtxn = env.beginRead();
    const cf = env.openChangeFeed(rtxn);
    const events = [];
    for (const ev of cf) {
      events.push(ev);
    }
    cf.close();
    rtxn.close();

    expect(events.length).toBe(2);
    expect(events[0].key.toString()).toBe('cfk');
    expect(events[0].type).toBe(1); // insert
    expect(events[1].type).toBe(3); // delete
    env.close();
  });
});
