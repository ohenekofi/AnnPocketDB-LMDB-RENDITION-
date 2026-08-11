/**
 * annpocketdb Node.js binding
 *
 * Idiomatic JS wrapper over the synchronous N-API addon.
 */

const native = require('bindings')('annpocketdb_native');

class AnnPocketDbError extends Error {
  constructor(message, code) {
    super(message);
    this.code = code;
    this.name = 'AnnPocketDbError';
  }
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

class Cursor {
  constructor(handle) {
    this._handle = handle;
  }

  close() {
    if (this._handle) {
      native.cursorClose(this._handle);
      this._handle = null;
    }
  }

  first() { return native.cursorFirst(this._handle); }
  last() { return native.cursorLast(this._handle); }
  next() { return native.cursorNext(this._handle); }
  prev() { return native.cursorPrev(this._handle); }

  seek(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    return native.cursorSeek(this._handle, key);
  }

  seekExact(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    return native.cursorSeekExact(this._handle, key);
  }

  isValid() {
    return native.cursorValid(this._handle);
  }

  key() {
    return native.cursorKey(this._handle);
  }

  value() {
    return native.cursorValue(this._handle);
  }

  put(key, value, opts = {}) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    if (!Buffer.isBuffer(value)) value = Buffer.from(value);
    let flags = 0;
    if (opts.append) flags |= native.CURSOR_APPEND;
    if (opts.noOverwrite) flags |= native.CURSOR_NO_OVERWRITE;
    native.cursorPut(this._handle, key, value, flags);
  }

  delete() {
    native.cursorDel(this._handle);
  }

  // Make Cursor iterable (for...of)
  *[Symbol.iterator]() {
    if (!this.isValid()) {
      if (!this.first()) return;
    }
    while (this.isValid()) {
      yield { key: this.key(), value: this.value() };
      if (!this.next()) break;
    }
  }
}

// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------

class ReadTxn {
  constructor(handle) {
    this._handle = handle;
    this._closed = false;
  }

  get(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    return native.rtxnGet(this._handle, key) || null;
  }

  cursor() {
    return new Cursor(native.rtxnCursorOpen(this._handle));
  }

  close() {
    if (!this._closed) {
      native.rtxnClose(this._handle);
      this._closed = true;
    }
  }
}

class WriteTxn {
  constructor(handle) {
    this._handle = handle;
    this._done = false;
  }

  put(key, value, overwrite = true) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    if (!Buffer.isBuffer(value)) value = Buffer.from(value);
    native.wtxnPut(this._handle, key, value, overwrite);
  }

  delete(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    native.wtxnDel(this._handle, key);
  }

  get(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    return native.wtxnGet(this._handle, key) || null;
  }

  cursor() {
    return new Cursor(native.wtxnCursorOpen(this._handle));
  }

  commit(syncMode = native.SYNC_DURABLE) {
    if (this._done) throw new Error("Transaction already finished");
    native.wtxnCommit(this._handle, syncMode);
    this._done = true;
  }

  prepare(syncMode = native.SYNC_DURABLE) {
    if (this._done) throw new Error("Transaction already finished");
    native.wtxnPrepare(this._handle, syncMode);
  }

  abort() {
    if (this._done) return;
    native.wtxnAbort(this._handle);
    this._done = true;
  }

  close() {
    if (!this._done) {
      this.abort();
    }
  }
}

// ---------------------------------------------------------------------------
// Env
// ---------------------------------------------------------------------------

class Env {
  constructor(path, opts = {}) {
    this._handle = native.envOpen(path, opts);
    this._closed = false;
  }

  close() {
    if (!this._closed) {
      native.envClose(this._handle);
      this._closed = true;
    }
  }

  get(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    return native.envGet(this._handle, key) || null;
  }

  put(key, value, overwrite = true) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    if (!Buffer.isBuffer(value)) value = Buffer.from(value);
    native.envPut(this._handle, key, value, overwrite);
  }

  delete(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    native.envDel(this._handle, key);
  }

  sync(force = false) {
    native.envSync(this._handle, force);
  }

  stat() {
    return native.envStat(this._handle);
  }

  rollbackPrepared() {
    native.envRollbackPrepared(this._handle);
  }

  putBlob(key, blob, syncMode = native.SYNC_DURABLE) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    if (!Buffer.isBuffer(blob)) blob = Buffer.from(blob);
    native.envPutBlob(this._handle, key, blob, syncMode);
  }

  getBlob(key) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    return native.envGetBlob(this._handle, key) || null;
  }

  deleteBlob(key, syncMode = native.SYNC_DURABLE) {
    if (!Buffer.isBuffer(key)) key = Buffer.from(key);
    native.envDelBlob(this._handle, key, syncMode);
  }

  openChangeFeed(txn, afterToken = 0) {
    return new ChangeFeedIterator(native.changefeedOpenPoll(this._handle, txn._handle, afterToken));
  }

  beginRead() {
    return new ReadTxn(native.rtxnBegin(this._handle));
  }

  beginWrite() {
    return new WriteTxn(native.wtxnBegin(this._handle));
  }
}

// ---------------------------------------------------------------------------
// Changefeed
// ---------------------------------------------------------------------------

class ChangeFeedIterator {
  constructor(handle) {
    this._handle = handle;
    this._closed = false;
  }

  next() {
    if (this._closed) return null;
    return native.changefeedNext(this._handle);
  }

  close() {
    if (!this._closed) {
      native.changefeedClosePoll(this._handle);
      this._closed = true;
    }
  }

  *[Symbol.iterator]() {
    while (true) {
      const ev = this.next();
      if (!ev) break;
      yield ev;
    }
  }
}

module.exports = {
  Env,
  AnnPocketDbError,
  SyncMode: {
    DURABLE: native.SYNC_DURABLE,
    ASYNC: native.SYNC_ASYNC
  },
  EnvFlags: {
    CHANGEFEED: 1 << 9,
    CHANGEFEED_ASYNC: 1 << 10
  }
};
